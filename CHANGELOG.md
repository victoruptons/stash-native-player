# Changelog

All notable changes to Stash Native Player are listed here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
The app and the plugin share one version number.

## [Unreleased]

### Fixed

- macOS: the libmpv bundled in the app could not be loaded on macOS 15.4 and
  later (duplicate `LC_RPATH`), so the app fell back to Homebrew's mpv, or had
  no video player at all on Macs without it.

## [0.2.1] - 2026-11-22

### Fixed

- In fill-window mode the settings menu showed a check mark next to every
  speed / audio / subtitle choice instead of only the selected one.
- Opening another video after the previous one had played to the end could
  leave the player paused while the play/pause button showed "Pause"; the
  button then did nothing visible. (mpv ignores an unpause while a finished
  file is still open, so the new video never started. Seen with mpv 0.42 on
  macOS.)

## [0.2.0] - 2026-11-22

### Added

- macOS: trackpad swipes go back / forward between pages, following System
  Settings > Trackpad > "Swipe between pages" (two-finger swipes tracked like
  Safari, or three-finger swipes). Swipes over the video are left to the
  player.

## [0.1.1] - 2026-11-22

### Fixed

- macOS: the window uses the standard traffic-light buttons on the left
  (over a transparent title bar) instead of Windows-style buttons, and Chinese
  text uses PingFang TC.

## [0.1.0] - 2026-11-22

First release.

### Added

- Desktop app that shows Stash's own web UI and plays videos with an embedded
  mpv player (hardware decoding, almost any codec/container, no live
  transcoding).
- Stash UI plugin (`StashNativePlayer`) that replaces the scene player inside
  the app: seek bar with sprite thumbnails and marker ticks, speed / audio /
  subtitle menus, resume position and play count syncing, fill-window mode.
- Frameless window with its own toolbar (back / forward / reload, server
  switcher), Windows Snap and FancyZones support, mouse-wheel seeking.
- Live subtitles: local whisper speech recognition with optional translation
  to Traditional Chinese by a local LLM (llama.cpp).
- Whole-video subtitles saved in Stash as `.srt` captions next to the video,
  through the plugin's `snp_captions.py`; generated one by one from a queue
  (the 「產生字幕」 tag), with a one-click "queue all videos without subtitles".
- Skipping videos that already have embedded subtitle tracks, burned-in
  subtitles (Windows OCR), or speech in chosen languages.
- Subtitle settings dialog: models folder and separate translation models for
  live and pre-made subtitles.
- macOS (Apple Silicon) build, not yet tested on a Mac.
