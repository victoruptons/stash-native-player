"""Stash Native Player: save generated subtitles next to a scene's video.

Runs on the Stash server (raw plugin interface; only the standard library).
The desktop app calls it with the runPluginOperation mutation:

    args = {"op": "save", "scene_id": "25", "lang": "zh", "srt": "1\\n00:00:01,000 --> ...",
            "overwrite": false}
    args = {"op": "check", "scene_id": "25"}

"save" writes <video name>.<lang>.srt in the video's folder and scans that one
file, so that Stash lists it as a caption of the scene (served by
/scene/<id>/caption, shown by Stash's own player too).

An existing caption file is replaced only if this plugin wrote it (recorded in
snp_written.json next to this script) or "overwrite" is true, so captions
you put there yourself are never touched.
"""

import json
import os
import re
import sys
import tempfile
import urllib.request

LANG_RE = re.compile(r"^[a-z]{2,3}$")  # Stash only accepts ISO 639 base codes ("zh", not "zh-TW")
RECORD = os.path.join(os.path.dirname(os.path.abspath(__file__)), "snp_written.json")


def gql(conn, query, variables=None):
    host = conn.get("Host") or "localhost"
    if host in ("0.0.0.0", "::", ""):
        host = "localhost"
    url = "%s://%s:%s/graphql" % (conn.get("Scheme") or "http", host, conn.get("Port") or 9999)
    req = urllib.request.Request(
        url,
        data=json.dumps({"query": query, "variables": variables or {}}).encode("utf-8"),
        headers={"Content-Type": "application/json", "Accept": "application/json"},
    )
    cookie = conn.get("SessionCookie")
    if cookie and cookie.get("Name"):
        req.add_header("Cookie", "%s=%s" % (cookie["Name"], cookie.get("Value", "")))
    ctx = None
    if url.startswith("https"):
        import ssl

        ctx = ssl._create_unverified_context()  # the server talking to itself
    with urllib.request.urlopen(req, timeout=60, context=ctx) as r:
        res = json.loads(r.read().decode("utf-8"))
    if res.get("errors"):
        raise RuntimeError("; ".join(e.get("message", "?") for e in res["errors"]))
    return res["data"]


def video_path(conn, scene_id):
    data = gql(conn, "query($id: ID!) { findScene(id: $id) { files { path } } }", {"id": str(scene_id)})
    scene = data.get("findScene")
    if not scene or not scene.get("files"):
        raise RuntimeError("scene %s has no file" % scene_id)
    return scene["files"][0]["path"]


def load_record():
    try:
        with open(RECORD, encoding="utf-8") as f:
            return set(json.load(f))
    except (OSError, ValueError):
        return set()


def save_record(paths):
    try:
        with open(RECORD, "w", encoding="utf-8") as f:
            json.dump(sorted(paths), f, indent=0)
    except OSError:
        pass


def caption_path(video, lang):
    return os.path.splitext(video)[0] + "." + lang + ".srt"


def op_check(conn, args):
    video = video_path(conn, args["scene_id"])
    folder = os.path.dirname(video)
    return {"video": video, "folder": folder, "writable": os.access(folder, os.W_OK)}


def op_save(conn, args):
    lang = str(args.get("lang", "")).lower()
    if not LANG_RE.match(lang):
        raise RuntimeError("bad language code %r" % lang)
    srt = args.get("srt")
    if not isinstance(srt, str) or not srt.strip():
        raise RuntimeError("no subtitles given")
    video = video_path(conn, args["scene_id"])
    path = caption_path(video, lang)
    record = load_record()
    if os.path.exists(path) and path not in record and not args.get("overwrite"):
        return {"path": path, "saved": False, "reason": "exists"}

    folder = os.path.dirname(path)
    fd, tmp = tempfile.mkstemp(prefix=".snp-", suffix=".srt.tmp", dir=folder)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as f:
            f.write(srt)
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise
    record.add(path)
    save_record(record)

    # Scan just this file: Stash then attaches it to the scene as a caption.
    job = gql(
        conn,
        "mutation($input: ScanMetadataInput!) { metadataScan(input: $input) }",
        {"input": {"paths": [path]}},
    ).get("metadataScan")
    return {"path": path, "saved": True, "scan_job": job}


def main():
    # Always UTF-8, whatever the server's locale (Windows defaults to a code page).
    raw = sys.stdin.buffer.read().decode("utf-8")
    inp = json.loads(raw) if raw.strip() else {}
    conn = inp.get("server_connection") or {}
    args = inp.get("args") or {}
    ops = {"save": op_save, "check": op_check}
    try:
        op = ops.get(args.get("op"))
        if not op:
            raise RuntimeError("unknown op %r" % args.get("op"))
        out = {"output": op(conn, args)}
    except Exception as e:  # reported back to the app as the operation's error
        out = {"error": "%s: %s" % (type(e).__name__, e)}
    sys.stdout.buffer.write(json.dumps(out).encode("utf-8"))
    sys.stdout.flush()


if __name__ == "__main__":
    main()
