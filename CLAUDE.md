# Stash Native Player — notes for Claude

C++17 / Qt 6.8.3 (Qt Quick + WebEngine + WebChannel) desktop app that shows
Stash's web UI and plays video in an embedded libmpv, plus a Stash UI plugin in
`plugin/StashNativePlayer/` (`native-player.js`, `native-player.css`,
`snp_captions.py`). The plugin controls the player through the QWebChannel
`bridge` object (`app/src/bridge.cpp`).

## Rules (must follow)

1. **GitHub account: only `victoruptons`**, never the owner's main account.
   - Remote: `git@github-alt:victoruptons/stash-native-player.git`. `github-alt`
     is an SSH host alias in `~/.ssh/config` pointing at
     `~/.ssh/id_ed25519_victoruptons` (IdentitiesOnly).
   - Repo-local git identity only (never `--global`, never touch global git
     config): `user.name = victoruptons`,
     `user.email = 123921734+victoruptons@users.noreply.github.com`.
   - Don't use `gh` / HTTPS credentials of another account to push.
2. **The owner's real Stash server has NSFW content**: never screenshot it,
   never read or describe its content. Test only against the separate test
   Stash server with synthetic media (below).
3. **Ask before every commit and push.** Release only when the owner says so.

## Build (macOS, arm64) — mirrors the `macos` job in `.github/workflows/build.yml`

```bash
brew install cmake ninja mpv dylibbundler
# Qt 6.8.3 clang_64 is in ~/Qt (installed with aqtinstall):
#   aqt install-qt mac desktop 6.8.3 clang_64 -m qtwebengine qtwebchannel qtpositioning -O ~/Qt
cmake -S app -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSNP_BUILD_TESTS=ON \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_PREFIX_PATH=$HOME/Qt/6.8.3/macos \
  -DMPV_INCLUDE_DIR=$(brew --prefix)/include
cmake --build build
```

- **macOS 26 SDK has no AGL framework**, but Qt 6.8.3's
  `~/Qt/6.8.3/macos/lib/cmake/Qt6/FindWrapOpenGL.cmake` links `-framework AGL`
  (fixed in later Qt). The local copy is patched to skip AGL (original saved as
  `FindWrapOpenGL.cmake.orig`). CI uses macos-15 and is unaffected. Reinstalling
  Qt brings the error back (`ld: framework 'AGL' not found`).
- The dev build loads libmpv from `/opt/homebrew/lib/libmpv.2.dylib`
  (`app/src/mpvlib.cpp`, which tries `Contents/Frameworks` first and then
  Homebrew, silently).
- Local packaged build (same as the CI `Package` step): Release build in
  `build-release/`, then with `~/Qt/6.8.3/macos/bin` on PATH run
  `macdeployqt`, copy + `dylibbundler` libmpv, remove duplicate `LC_RPATH`s,
  ad-hoc `codesign --force --deep --sign -`. Copy to `/Applications` with
  `ditto`. Check it uses the bundled libmpv:
  `vmmap <pid> | grep libmpv` must show `.../Contents/Frameworks/libmpv.2.dylib`,
  not `/opt/homebrew/...`.
- dylibbundler adds a duplicate `LC_RPATH` to libmpv. dyld rejects that only
  for dylibs built for macOS 26+ (minos 26.0 — e.g. Homebrew bottles on this
  Mac); CI's macos-15 libmpv (minos 15.0) loads anyway. `build.yml` removes
  duplicates and fails the build if any is left (since v0.2.2).
- Windows builds: see the `windows` job in the same workflow (shinchiro libmpv).

## Tests

```bash
ctest --test-dir build --output-on-failure          # needs -DSNP_BUILD_TESTS=ON
python3 -m unittest discover -s plugin/tests        # plugin (snp_captions.py)
node --check plugin/StashNativePlayer/native-player.js   # CI also does this
```

## Running against the test Stash server (macOS)

Test server lives outside the repo in `~/Dev/stash-test/`:
- `bin/stash-macos` (official Stash v0.32.0 release binary)
- `data/config.yml` — 127.0.0.1:**9998**, own database/generated/cache,
  `plugins_path: data/plugins`, library = `media/`
- `media/` — synthetic videos only (ffmpeg `testsrc2` + macOS `say` speech +
  sine, hand-written `.srt`; one HEVC/AC3 MKV). Make more the same way.

```bash
cd ~/Dev/stash-test/data && ../bin/stash-macos --config ~/Dev/stash-test/data/config.yml --nobrowser
# rescan after adding media:
curl -s http://127.0.0.1:9998/graphql -H 'Content-Type: application/json' \
  -d '{"query":"mutation{metadataScan(input:{scanGenerateSprites:true,scanGenerateCovers:true})}"}'
```

After changing the plugin, copy it and reload the page (or reload plugins):

```bash
cp -R plugin/StashNativePlayer ~/Dev/stash-test/data/plugins/
curl -s http://127.0.0.1:9998/graphql -H 'Content-Type: application/json' -d '{"query":"mutation{reloadPlugins}"}'
```

Run the app (always pass `--server` so it never opens the real server):

```bash
SNP_DEBUG=1 QTWEBENGINE_REMOTE_DEBUGGING=9223 \
  ./build/StashNativePlayer.app/Contents/MacOS/StashNativePlayer --server http://127.0.0.1:9998
```

- `SNP_DEBUG=1` — logs every mpv property the bridge receives and every state
  map it sends (`[bridge] mpv ...` / `[bridge] state ...`).
- `SNP_MPV_LOG=v` — mpv log.
- `SNP_NO_GPU_RASTER=1` — skip the Chromium GPU-rasterization flags.
- `QTWEBENGINE_REMOTE_DEBUGGING=9223` — Chrome DevTools Protocol at
  `http://127.0.0.1:9223/json`; drive the page with `Runtime.evaluate` and
  `Input.dispatchMouseEvent` / `Input.dispatchKeyEvent` (real input events
  also trigger the controls' auto-show; `element.click()` does not).
- `--server <url>` — use that server for this run only.

Screenshots (test server only): `screencapture -x -o -l<windowid>` captures
just the app window (window id via `CGWindowListCopyWindowInfo` for the app's
PID). Needs Screen Recording permission for the Claude app. CDP
`Page.captureScreenshot` shows Chromium's output, not what Qt put on screen.
The window is Retina (2x).

## Architecture notes

- Pause state path: mpv `pause` property → `Bridge::onPropertyChanged` →
  `stateChanged` (WebChannel) → `onState` in `native-player.js` →
  `playback.paused` → control bar `btn(FA.faPause / FA.faPlay)`.
- mpv runs with `keep-open=yes`, `idle=yes`. At EOF mpv sets `pause=yes` and
  then ignores `pause=no` (no property event). `Bridge` remembers the requested
  pause state (`m_wantPaused`) and re-applies it in `onFileLoaded`.
- Qt Quick is forced to OpenGL (mpv render API); on macOS that is a legacy
  2.1 context, so mpv logs "GLSL version too old" and VideoToolbox interop
  warnings. Known, not yet addressed.
- macOS-only code: `app/src/winframe_mac.mm` (transparent title bar with
  traffic lights, trackpad swipe back/forward via `canSwipe` /
  `swipeNavigate` in `Main.qml`).
- Windows-only features (no need to test on Mac): Windows OCR burned-in
  subtitle detection (`screenocr.cpp`), Snap / FancyZones (`winframe.cpp`).

## Release (only when the owner says so)

1. `CHANGELOG.md`: rename `## [Unreleased]` to `## [X.Y.Z] - YYYY-MM-DD`
   (add a fresh empty Unreleased section if wanted).
2. `app/CMakeLists.txt`: `project(... VERSION X.Y.Z)`.
3. `plugin/StashNativePlayer/StashNativePlayer.yml`: `version: X.Y.Z`.
4. Commit ("Release X.Y.Z"), `git tag -a vX.Y.Z -m "vX.Y.Z"`,
   `git push origin main vX.Y.Z`.
5. `.github/workflows/release.yml` builds Windows + macOS and publishes the
   GitHub release.

App and plugin share one version number. Every user-visible change goes into
the Unreleased section of `CHANGELOG.md`.
