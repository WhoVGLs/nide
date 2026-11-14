# Changelog

All notable changes to NIDE are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/).

## [0.2.0] "Jendela" - 2026-11-14

First release under the NIDE name. The project was previously called wlua.

### Added

- Config hot reload using inotify: bindings reload automatically when the
  config file is saved, including atomic saves (rename) used by vim and nano.
- Invalid configs, configs without keybindings, and deleted configs do not
  replace the active config; errors are logged.
- `CHANGELOG.md` and Semantic Versioning policy.

### Changed

- Project, binary, and Wayland session entry renamed to NIDE / `nide`.
- Lua API changed from `wl.*` to `nide.*` without a compatibility alias.
- Config path is now `~/.config/nide/config.lua`; environment variable is
  `NIDE_CONFIG`.
- Install paths are now `/usr/bin/nide`, `/usr/share/nide/config.lua`, and
  `/usr/share/wayland-sessions/nide.desktop`.
- `XDG_CURRENT_DESKTOP` is now `nide`.
- Versions follow SemVer, starting at `0.2.0`.
- Documentation, source comments, and user-facing messages translated to
  English.

## [0.1.0] - 2026-11-13

Initial release under the name wlua.

- Floating Wayland compositor based on wlroots 0.20.
- No compositor-side decorations (no title bar, border, or window buttons).
- Lua-configured keybindings.
- `Super+W` opens a terminal, `Super+C` closes the focused window.
- Clipboard and primary selection support.
