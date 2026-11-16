# NIDE

**NIDE** is a recursive acronym for **"NIDE Isn't a Desktop"**. Despite the
name, NIDE is a small, simple Wayland compositor built with
[wlroots](https://gitlab.freedesktop.org/wlroots/wlroots) 0.20 and configured
entirely with Lua.

Current version: **NIDE 0.4.0**.

## What is NIDE?

NIDE is a Wayland compositor: the program that manages application windows,
keyboard and mouse input, and what appears on screen. NIDE is intentionally
small and simple:

- Windows float and have no decorations, so applications look plain.
- Almost everything is driven from the keyboard.
- A built-in launcher searches applications and lists open windows, replacing
  the need for a dock, taskbar, or separate application menu.
- Behavior and keybindings are configured in a single Lua file.
- Config changes take effect immediately without a restart.

NIDE is not a full desktop. It has no panel, dock, workspaces, or animations.
It is a good fit if you want a lightweight, fast Wayland environment that is
easy to customize with Lua.

The project configuration lives in [`config/config.lua`](config/config.lua),
and the complete guide is in [`docs/manual.md`](docs/manual.md).

## Features

- Native Wayland compositor based on wlroots.
- **Floating/dynamic** windows, not tiling.
- No compositor-side decorations: no title bar, border, or window buttons.
  Applications are also not asked to draw client-side decorations.
- Lua keybinding configuration, read from `~/.config/nide/config.lua`.
- Hot reload: saving the config file updates bindings without a restart.
- `Super+W` opens a terminal, `Super+C` closes the focused window.
- `Super+Tab` opens the built-in launcher. It has two tabs: **Open Windows**
  switches between running windows, while **All Apps** searches applications
  discovered from XDG `.desktop` files and marks ones that are already running.
  Results are sorted alphabetically and shown as application logos with names in
  a 5-column grid with a side scrollbar, keyboard and mouse navigation, and
  search.
- `Super+M` maximizes with a configurable margin; `Super+S` toggles fullscreen.
- Edge snapping with a live preview: while moving a window, drag the pointer to
  the left or right screen edge to preview that half of the screen, or to the
  top edge to preview fullscreen. The window changes only when the mouse
  button is released.
- Snapped windows resist detaching: drag inward or vertically at least 64
  pixels to break the snap. A pressure gauge shows the progress, and pushing
  toward the snapped edge never detaches the window.
- Application fullscreen requests work, including F11 in browsers and games.
- `Super+left-click` moves a window, `Super+right-click` resizes it.
- Clipboard and primary selection (middle-click paste) are supported.
- One small binary and one C file. Cairo is used only to render the launcher.

## Requirements

Arch Linux:

```sh
sudo pacman -S --needed base-devel wlroots0.20 lua54 cairo
```

Other distributions need the wlroots **0.20** development headers, Lua **5.4**,
wayland-server, xkbcommon, and Cairo.

## Build

```sh
make
./nide --check -c config/config.lua
```

Install system-wide (optional):

```sh
sudo make install PREFIX=/usr
mkdir -p ~/.config/nide
cp config/config.lua ~/.config/nide/config.lua
```

`PREFIX=/usr` is required so the display manager can find
`/usr/share/wayland-sessions/nide.desktop`. Without it, files are installed
under `/usr/local` and the NIDE session may not appear in SDDM.

## Running

From a TTY (DRM/libinput backend):

```sh
./nide -c ~/.config/nide/config.lua
```

After installation, the `NIDE` session also appears in display managers
(SDDM, GDM, etc.) through `nide.desktop`.

Nested test inside an X11/XWayland session (safe, does not disturb your main
session):

```sh
Xephyr :2 -screen 1280x800 &
DISPLAY=:2 WLR_BACKENDS=x11 WLR_RENDERER=pixman \
  WLR_RENDERER_ALLOW_SOFTWARE=1 ./nide -c config/config.lua
```

Or directly inside a running X11 session:

```sh
WLR_BACKENDS=x11 WLR_RENDERER=pixman WLR_RENDERER_ALLOW_SOFTWARE=1 \
  ./nide -c config/config.lua
```

The compositor window appears as a 1024x768 window. Click it, then press
`Super+W`.

> For X11 testing, `WLR_BACKENDS=x11` is required. The nested Wayland backend
> is not available in the Arch wlroots package.

## Configuration

Config file lookup order:

1. `-c PATH`
2. `$NIDE_CONFIG`
3. `$XDG_CONFIG_HOME/nide/config.lua`
4. `~/.config/nide/config.lua`
5. `./config.lua`

If no config file exists, NIDE uses the default bindings:

```lua
nide.bind("super+w", "spawn", "foot")
nide.bind("super+c", "close")
nide.bind("super+m", "maximize")
nide.bind("super+s", "fullscreen")
nide.bind("super+tab", "launcher")
nide.bind("super+shift+q", "quit")
```

Example config:

```lua
local terminal = "foot"

nide.bind("super+w", "spawn", terminal)
nide.bind("super+c", "close")
nide.bind("super+m", "maximize")
nide.bind("super+s", "fullscreen")
nide.bind("super+tab", "launcher")
nide.bind("super+shift+q", "quit")

-- Gap in pixels around maximized and snapped windows. Default: 8.
nide.maximize_margin = 8
```

Format:

```lua
nide.bind("modifier+key", "action", "argument")
```

Recognized modifiers: `shift`, `ctrl`, `alt`, `super` (can be combined with
`+`). Key names follow XKB keysym names, for example `w`, `Return`, `space`,
`Left`, `1`, `bracketleft`.

Actions:

| Action | Argument | Effect |
|---|---|---|
| `spawn` | shell command | runs a program |
| `close` | — | closes the focused window |
| `maximize` | — | maximizes with a configurable margin |
| `fullscreen` | — | toggles fullscreen |
| `launcher` | — | opens or closes the built-in launcher |
| `quit` | — | quits the compositor |

`nide.maximize_margin` sets the gap in pixels around maximized windows. It
also applies to snapped windows. The default is `8`; fullscreen always fills
the output without a margin. Saving a changed margin applies it to open
maximized and snapped windows.

```lua
nide.maximize_margin = 12
```

`nide.log(...)` can be used to print messages from the config:

```lua
nide.log("NIDE ready")
```

NIDE watches the active config file and reloads bindings automatically when
the file is saved. Open windows keep running, and a changed
`nide.maximize_margin` is applied to open maximized and snapped windows.
Configs with errors or no keybindings are ignored, and the previous config
stays active. If NIDE starts without a config file (default mode), restart it
once after creating a config.

## Launcher

Press `Super+Tab` to open the launcher overlay. It opens in **Open Windows**
mode when at least one window is running, otherwise in **All Apps** mode.

- Both tabs show a 5-column grid of application logos and names, sorted
  alphabetically. A scrollbar on the right side shows the current position;
  click or drag it to scroll. The mouse wheel and `Page Up`/`Page Down` also
  scroll.
- **Open Windows** lists each open window individually. Selecting an entry
  raises and focuses that window.
- **All Apps** lists applications found in XDG application directories. It
  runs the application's `.desktop` `Exec` command, using `foot -e` when the
  entry has `Terminal=true`. A small dot marks an application with one open
  window; a numeric badge shows the count when it has more.
- Type to search the current tab's names and window titles. Use the arrow keys
  to move, `Enter` to open/focus, `Tab` to switch tabs, and `Esc` or
  `Super+Tab` to close.
- The panel uses a translucent glass look so the desktop remains visible
  behind it. Applications are shown with their PNG icon from the `.desktop`
  entry when one can be found; otherwise a colored initial is used.
- Clicking an entry or tab works as expected; clicking outside the panel
  closes the launcher.

The action name is `launcher`, so it can be rebound:

```lua
nide.bind("super+d", "launcher")
```

## Decorations and window behavior

NIDE deliberately draws no decorations. To keep applications from drawing
client-side decorations (CSD), NIDE requests server-side mode but does not draw
a title bar or border. The result is completely plain windows.

Consequences:

- There are no close/minimize/maximize buttons. Use `Super+C` to close,
  `Super+M` to maximize, and `Super+S` to toggle fullscreen.
- Move windows with `Super+left-click`, resize with `Super+right-click`.
- While moving a window, drag the pointer to the left or right screen edge to
  preview and snap it into that half of the output. Drag the pointer to the top
  edge to preview fullscreen. A snapped window resists detaching until you
  pull it 64 pixels away.
- Maximize keeps a small margin; fullscreen fills the output.
- Maximizing, snapping, or fullscreening a window remembers its previous
  floating geometry and restores it when the window returns to floating.
- New windows are centered with a small cascade.
- Focus changes when a window is clicked or a new window opens.

## Limitations

- Wayland-native applications only. XWayland is not enabled, so X11
  applications cannot run.
- No layer-shell, so Wayland bars/panels (Waybar, etc.) are not supported.
  Use the launcher's **Open Windows** tab instead of a dock or taskbar.
- No minimize, animations, shadows, or decorations.
- No input method (IME) support.
- Launcher icons use PNG files from common icon theme locations (hicolor and
  oxygen); applications that only ship SVG icons fall back to colored initials.
- Single output configuration; no manual multi-monitor positioning.

## Layout

```
src/main.c        compositor + Lua integration
config/config.lua example configuration
docs/manual.md    complete usage and configuration guide
Makefile
nide.desktop      Wayland session entry for display managers
```

## Versioning

NIDE uses [Semantic Versioning](https://semver.org/) with the
`MAJOR.MINOR.PATCH` format:

- `MAJOR` increases for incompatible changes.
- `MINOR` increases for new backward-compatible features.
- `PATCH` increases for backward-compatible bug fixes.

While the version is still `0.x`, the config API is considered unstable.
New features and changes that break existing configs increase `MINOR`; bug
fixes increase `PATCH`. Every release is documented in
[`CHANGELOG.md`](CHANGELOG.md).

Current version: **0.4.0**.
