"""Tests for plugin/StashNativePlayer/snp_captions.py, with Stash faked out.

Run from the repository root:  python -m unittest discover -s plugin/tests -v
"""

import importlib.util
import io
import json
import os
import sys
import tempfile
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "..", "StashNativePlayer", "snp_captions.py")


def load_plugin():
    spec = importlib.util.spec_from_file_location("snp_captions", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class FakeStash:
    """Answers the two GraphQL calls the plugin makes."""

    def __init__(self, video):
        self.video = video
        self.scanned = []

    def __call__(self, conn, query, variables=None):
        if "findScene" in query:
            return {"findScene": {"files": [{"path": self.video}]}}
        if "metadataScan" in query:
            self.scanned.extend(variables["input"]["paths"])
            return {"metadataScan": "7"}
        raise AssertionError("unexpected query: " + query)


class SnpCaptionsTest(unittest.TestCase):
    def setUp(self):
        self.plugin = load_plugin()
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.video = os.path.join(self.tmp.name, "My Video.mp4")
        open(self.video, "wb").close()
        self.stash = FakeStash(self.video)
        self.plugin.gql = self.stash
        self.plugin.RECORD = os.path.join(self.tmp.name, "snp_written.json")

    def save(self, **args):
        args.setdefault("scene_id", "25")
        args.setdefault("lang", "zh")
        args.setdefault("srt", "1\n00:00:01,000 --> 00:00:02,000\n你好\n")
        return self.plugin.op_save({}, args)

    def caption(self, lang="zh"):
        return os.path.join(self.tmp.name, "My Video.%s.srt" % lang)

    def read(self, path):
        with open(path, encoding="utf-8") as f:
            return f.read()

    def test_save_writes_next_to_video_and_scans_it(self):
        out = self.save()
        self.assertTrue(out["saved"])
        self.assertEqual(out["path"], self.caption())
        self.assertEqual(self.read(self.caption()), "1\n00:00:01,000 --> 00:00:02,000\n你好\n")
        self.assertEqual(self.stash.scanned, [self.caption()])
        self.assertEqual(out["scan_job"], "7")
        # no temporary files left behind
        self.assertEqual(sorted(os.listdir(self.tmp.name)), ["My Video.mp4", "My Video.zh.srt", "snp_written.json"])

    def test_replaces_its_own_file(self):
        self.save(srt="old\n")
        out = self.save(srt="new\n")
        self.assertTrue(out["saved"])
        self.assertEqual(self.read(self.caption()), "new\n")

    def test_never_replaces_a_users_file(self):
        with open(self.caption("en"), "w", encoding="utf-8") as f:
            f.write("mine\n")
        out = self.save(lang="en", srt="generated\n")
        self.assertFalse(out["saved"])
        self.assertEqual(out["reason"], "exists")
        self.assertEqual(self.read(self.caption("en")), "mine\n")
        self.assertEqual(self.stash.scanned, [])

    def test_overwrite_flag_replaces_a_users_file(self):
        with open(self.caption("en"), "w", encoding="utf-8") as f:
            f.write("mine\n")
        self.assertTrue(self.save(lang="en", srt="generated\n", overwrite=True)["saved"])
        self.assertEqual(self.read(self.caption("en")), "generated\n")

    def test_rejects_bad_language_codes(self):
        for lang in ("zh-TW", "", "x", "english", "../zh"):
            with self.subTest(lang=lang), self.assertRaises(RuntimeError):
                self.save(lang=lang)

    def test_rejects_empty_subtitles(self):
        with self.assertRaises(RuntimeError):
            self.save(srt="  \n")

    def test_check_reports_folder(self):
        out = self.plugin.op_check({}, {"scene_id": "25"})
        self.assertEqual(out["video"], self.video)
        self.assertEqual(out["folder"], self.tmp.name)
        self.assertTrue(out["writable"])

    def run_main(self, payload):
        stdin = io.TextIOWrapper(io.BytesIO(json.dumps(payload).encode("utf-8")), encoding="utf-8")
        stdout = io.TextIOWrapper(io.BytesIO(), encoding="utf-8")
        with mock.patch.object(sys, "stdin", stdin), mock.patch.object(sys, "stdout", stdout):
            self.plugin.main()
            return json.loads(stdout.buffer.getvalue().decode("utf-8"))

    def test_main_runs_an_operation(self):
        out = self.run_main({"server_connection": {}, "args": {"op": "save", "scene_id": "25", "lang": "zh", "srt": "1\n字幕\n"}})
        self.assertTrue(out["output"]["saved"])

    def test_main_reports_errors(self):
        out = self.run_main({"args": {"op": "nope"}})
        self.assertIn("unknown op", out["error"])


if __name__ == "__main__":
    unittest.main()
