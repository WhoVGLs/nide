# Changelog

All notable changes to NIDE are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/).

## [0.3.1] - 2026-11-15

### Changed

- Edge snapping now shows a live preview while dragging. The left and right
  edges preview the matching half of the output, and the top edge previews
  fullscreen. The window is resized only after the mouse button is released.
- Snapped windows now resist detaching. Pushing toward the snapped edge never
  detaches the window. Pulling inward or vertically requires 64 pixels of
  travel, visualized by a pressure gauge on the snapped edge.
- Saved floating geometry is kept on-screen, so leaving fullscreen or maximize
  cannot restore a window outside the output.

### Fixed

- Snapped windows detached after almost any pointer movement because the old
  16-pixel breakout threshold accepted movement toward the edge.
- Releasing a window just after detaching could immediately snap it back to
  the same edge.

## [0.3.0] - 2026-11-15

### Added

- Edge snapping: dragging a window to the left or right edge snaps it to that
  half of the output. Pushing further toward the edge breaks the snap and
  returns the window to floating mode.
- `maximize` action with a configurable margin around the window. The default
  is 8 pixels and is set with `nide.maximize_margin` in the Lua config.
- `fullscreen` action, bound to `Super+S` by default.
- `Super+M` is now the default maximize binding.
- Maximize, fullscreen, and snap states remember the previous floating
  geometry and restore it when the window returns to floating.
- Application-initiated maximize and fullscreen requests are supported,
  including F11 fullscreen in browsers and games.

### Fixed

- Application fullscreen requests could freeze because the compositor
  scheduled a configure without setting the fullscreen state or output size.
  NIDE now applies the requested state and size before configuring the client.

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
