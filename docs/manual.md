# NIDE — Complete Guide

**Version:** 0.4.0

**Document:** November 15, 2026

This document explains how to build, run, use, and configure NIDE. It is
written for beginners; you do not need to understand C or Wayland to use NIDE
day to day.

## Table of Contents

1. [What is NIDE?](#1-what-is-nide)
2. [Basic terms](#2-basic-terms)
3. [Build and install](#3-build-and-install)
4. [Running NIDE](#4-running-nide)
5. [Daily use](#5-daily-use)
6. [Configuration](#6-configuration)
7. [Window behavior and appearance](#7-window-behavior-and-appearance)
8. [Command-line options and environment](#8-command-line-options-and-environment)
9. [Limitations](#9-limitations)
10. [Troubleshooting](#10-troubleshooting)
11. [Quick reference](#11-quick-reference)

## 1. What is NIDE?

NIDE stands for "NIDE Isn't a Desktop" (a recursive acronym, like GNU).
Despite the name, NIDE is technically a small wlroots-based Wayland compositor
that acts as a minimal desktop environment. Think of it as a very minimal
"desktop":

- Floating windows, not tiled.
- Plain windows: no title bar, border, or close button.
- All keybindings are configured in a Lua file.
- The config reloads automatically when the file is saved (hot reload).
- A built-in launcher (Super+Tab) searches applications and switches between
  open windows, so no separate dock or taskbar is needed.
- Just one small binary and one config file.

NIDE is NOT an application that can run inside KDE as a regular window
manager. When run as a session, NIDE replaces the KDE desktop. To try it
inside KDE without logging out, run it "nested" inside Xephyr (see section
4.3).

## 2. Basic terms

**Compositor** — The program that manages the screen, windows, keyboard, and
mouse. On modern Linux, the compositor is also the window manager.

**Floating** — Windows can be placed and resized freely instead of being
arranged automatically in a grid.

**Focus** — The window currently receiving keyboard input. Click a window to
focus it.

**Keybinding** — A key combination that runs an action, for example Super+W
to open a terminal.

**Spawn** — Launch a new program from inside the compositor.

**Nested** — Running NIDE inside a window on an existing desktop, usually with
Xephyr. Useful for testing.

## 3. Build and install

Requirements on Arch Linux:

```sh
sudo pacman -S --needed base-devel wlroots0.20 lua54 cairo
```

Other distributions need the wlroots 0.20 development headers, Lua 5.4,
wayland-server, xkbcommon, and Cairo.

Build from the project directory:

```sh
cd ~/Projects/nide
make
```

Check the configuration without starting the compositor:

```sh
./nide --check -c config/config.lua
```

Install system-wide:

```sh
sudo make install PREFIX=/usr
```

Use `PREFIX=/usr` so the binary and session file are in locations scanned by
the display manager:

```text
/usr/bin/nide
/usr/share/wayland-sessions/nide.desktop
/usr/share/nide/config.lua          (template, not the active config)
```

Copy the config to your personal directory:

```sh
mkdir -p ~/.config/nide
cp config/config.lua ~/.config/nide/config.lua
```

Uninstall:

```sh
sudo make uninstall PREFIX=/usr
```

Note: if you install without `PREFIX=/usr`, the session file goes to
`/usr/local/share/wayland-sessions`. Some display managers do not scan that
directory, so `PREFIX=/usr` is safer.

## 4. Running NIDE

There are several ways to run NIDE. Choose the one that fits your needs.

### 4.1 Full session from the greeter (normal use)

Prerequisites: you have run `sudo make install PREFIX=/usr` and copied the
config to `~/.config/nide/config.lua`.

1. Log out of your current desktop session.
2. On the login screen (SDDM), open the session menu. It is usually in the
   bottom-left or bottom-right corner.
3. Select "NIDE" and log in as usual.

After logging in, NIDE becomes your desktop. Every application you launch
becomes a Wayland client of NIDE.

If NIDE does not appear in the session list:

```sh
ls -l /usr/share/wayland-sessions/nide.desktop
which nide
```

Both must exist. If the file was just installed but does not appear yet,
restart SDDM (this logs out the current session):

```sh
sudo systemctl restart sddm
```

### 4.2 From a TTY

This is useful if you do not use a display manager or want to test directly
on the hardware.

1. Switch to a TTY with Ctrl+Alt+F3 (F4, F5, etc. also work).
2. Log in as a regular user.
3. Run:

```sh
cd ~/Projects/nide
./nide -c ~/.config/nide/config.lua
```

Or, if already installed:

```sh
nide -c ~/.config/nide/config.lua
```

Quit NIDE with Super+Shift+Q.

### 4.3 Nested inside KDE/X11 with Xephyr (for testing)

This is the safest way to try NIDE without logging out. Xephyr creates a
virtual X11 display; NIDE runs as a window inside it.

If Xephyr is not installed:

```sh
sudo pacman -S xorg-server-xephyr
```

Then run:

```sh
cd ~/Projects/nide

Xephyr :5 -screen 1280x800 &
sleep 1

DISPLAY=:5 WLR_BACKENDS=x11 WLR_RENDERER=pixman \
  WLR_RENDERER_ALLOW_SOFTWARE=1 ./nide -c config/config.lua
```

Notes:

- Do not use :0, :1, or :2. On KDE, those display numbers are used by
  XWayland.
- If ":5 is already active", use :6, :7, etc.
- After the Xephyr window appears, CLICK inside it first.
- NIDE keybindings (Super+W, Super+C, Super+M, Super+S, Super+Tab, and
  Super+Shift+Q) only work after the Xephyr window has focus.

If the Super key does not reach NIDE, a KDE global shortcut may be grabbing
it first (for example, Super+W for Overview or Super+Tab for task switching).
For nested testing, temporarily change the binding to another combination in
the config, or test in a full session (greeter/TTY) that has no KDE shortcuts.

When done, quit NIDE with Super+Shift+Q, then close the Xephyr window.

### 4.4 Directly as an XWayland window

If you do not want to use Xephyr:

```sh
cd ~/Projects/nide
WLR_BACKENDS=x11 WLR_RENDERER=pixman \
  WLR_RENDERER_ALLOW_SOFTWARE=1 ./nide -c config/config.lua
```

NIDE appears as a 1024x768 window. Click it before pressing keybindings. As
with the nested method, KDE global shortcuts may grab the Super combinations.

Environment variables used for nested testing:

- `WLR_BACKENDS=x11` — force the X11 backend
- `WLR_RENDERER=pixman` — software renderer
- `WLR_RENDERER_ALLOW_SOFTWARE=1` — allow the software renderer

If the screen is black or rendering fails, make sure the last two variables
are set.

## 5. Daily use

**New windows** — Appear centered on the monitor where the cursor is, then
shift by 32 pixels so they do not overlap exactly.

**Focus** — Click a window to focus it. New windows are focused automatically.
Click an empty area to release keyboard focus.

**Launcher** — Press Super+Tab to open the launcher overlay. It opens on the
**Open Windows** tab when at least one window is running, otherwise on
**All Apps**. Both tabs use a 5x3 grid with 15 results per page.

- **Open Windows** lists each open window separately. The badge shows how many
  windows that application has open. Selecting a tile raises and focuses that
  window.
- **All Apps** lists applications discovered from XDG `.desktop` files and
  shows a badge when that application already has windows open. Launching a
  terminal application uses `foot -e`.
- Type to search the current tab's names and window titles. Use the arrow keys
  to move, Enter to launch or focus, Tab to switch tabs, Page Up/Page Down or
  the mouse wheel to change pages, and Esc or Super+Tab to close.
- Click a tile or tab to use it. Clicking outside the panel closes the
  launcher. The page arrows and dots are also clickable.
- Launcher icons are generated from colored initials; application icon files
  are not loaded yet.

**Moving windows** — Hold Super, left-click anywhere on the window, and drag.
Release the mouse button to finish.

**Resizing** — Hold Super, right-click the window, and drag. The size changes
from the bottom-right corner. Release the mouse button to finish.

**Snapping** — Hold Super, left-click a window, and drag the pointer to the
left or right edge of the screen. A preview shows the half of the screen the
window will occupy, but the window itself does not change size until you
release the mouse button. Drag the pointer to the top edge instead to preview
fullscreen. Release to apply the preview, or move the pointer away from the
edge to cancel it. The pointer triggers the preview, so you do not have to push
the entire window to the edge.

Once a window is snapped, it resists accidental detaching. Pushing toward the
snapped edge never releases it. Pull inward or vertically at least 64 pixels
to break the snap; a pressure gauge on the snapped edge shows how close you
are to the threshold. The window returns to its previous floating size and
stays fully on-screen.

**Maximizing** — Super+M. The window fills the output but keeps a small margin
around it, so it is not the same as fullscreen. Press Super+M again to restore
the previous floating size and position.

**Fullscreen** — Super+S toggles fullscreen, which fills the entire output
without a margin. Applications can also request fullscreen themselves, for
example F11 in a browser or game. Press the application's shortcut again to
leave it.

**Closing windows** — Super+C. This sends a proper close request to the
application, not a forced kill. Applications with a "save file?" dialog can
show it.

**Quitting NIDE** — Super+Shift+Q. All applications lose their Wayland
connection and close. Save your work first.

**Clipboard** — Normal copy/paste is supported. Middle-click paste (primary
selection) is also supported.

**What is not available** — No close/minimize/maximize buttons, no traditional
Alt+Tab, no dock/panel, and no workspaces. Use Super+C to close, Super+M to
maximize, and Super+S for fullscreen. Use the launcher's Open Windows tab to
switch between open windows. Focus also changes by clicking or when a new
window opens.

## 6. Configuration

### 6.1 Config file location

Config lookup order when NIDE starts:

1. Path from the `-c` option
2. `NIDE_CONFIG` environment variable
3. `$XDG_CONFIG_HOME/nide/config.lua`
4. `~/.config/nide/config.lua`
5. `./config.lua` (the directory where NIDE is started)

If you use `-c`, that file must exist. If `NIDE_CONFIG` is set, that file must
also exist. For the next three locations, NIDE uses the first readable file.

For normal use, put the config at:

```text
~/.config/nide/config.lua
```

The `config/config.lua` file in the project directory is only an
example/template. Changing it does not automatically change the greeter
session unless you run NIDE with `-c config/config.lua` or copy it to
`~/.config/nide/config.lua`.

Recommended workflow:

```sh
# edit your personal config
nano ~/.config/nide/config.lua

# check it without starting the compositor
cd ~/Projects/nide
./nide --check -c ~/.config/nide/config.lua

# nested test with the same config
DISPLAY=:5 WLR_BACKENDS=x11 ... ./nide -c ~/.config/nide/config.lua
```

### 6.2 Default config and important notes

If NO config file is found, NIDE uses the default bindings:

```lua
nide.bind("super+w", "spawn", "foot")
nide.bind("super+c", "close")
nide.bind("super+m", "maximize")
nide.bind("super+s", "fullscreen")
nide.bind("super+tab", "launcher")
nide.bind("super+shift+q", "quit")
```

IMPORTANT: the default bindings are used only when no config file exists at
all. If a config file is found, its contents are used as-is; the default
bindings are NOT added automatically. So if the config only contains Super+W,
then Super+C, Super+M, Super+S, Super+Tab, and Super+Shift+Q do not exist.
Super+Tab still has a small fallback so the launcher can always be reached,
but adding the binding explicitly is recommended.

Always include at least one quit binding, for example:

```lua
nide.bind("super+shift+q", "quit")
```

Without it, you can get stuck inside NIDE. Emergency exit: switch to a TTY
with Ctrl+Alt+F3, log in, then run:

```sh
pkill -x nide
```

### 6.3 Basic command syntax

Format:

```lua
nide.bind("combination", "action", "argument")
```

Examples:

```lua
nide.bind("super+w", "spawn", "foot")
nide.bind("super+c", "close")
nide.bind("super+m", "maximize")
nide.bind("super+s", "fullscreen")
nide.bind("super+tab", "launcher")
nide.bind("super+shift+q", "quit")
```

Rules:

- Two arguments for actions without an argument (close, maximize,
  fullscreen, launcher, quit); three arguments for spawn.
- The combination and action name are written as strings (use quotes).
- The last part of the combination is the key name. The preceding parts are
  modifiers.
- Modifier and key names are case-insensitive.
- Modifiers must match exactly. The binding "super+w" does not activate if
  you press Super+Shift+W. Caps Lock and Num Lock do not interfere.
- If two bindings use the same combination, the last one wins.
- At most 64 bindings.
- A combination without a modifier is technically possible, for example
  "F12", but that key is always swallowed by the compositor. Prefer always
  using a modifier for regular keys.

Useful Lua basics:

- Single-line comment: `-- write a comment here`
- Variable: `local terminal = "foot"`
- String: `"foot"` or `'foot'`
- Newlines do not need semicolons.

The config file runs as full Lua code with the standard library. This means a
config can run commands and access files. Only use configs you wrote yourself
or trust.

### 6.4 Available modifiers

```text
shift
ctrl        (alias: control)
alt         (alias: mod1)
super       (alias: win, logo, mod4)
```

Can be combined with plus signs, in any order:

```lua
nide.bind("super+shift+q", "quit")
nide.bind("ctrl+alt+t", "spawn", "foot")
nide.bind("super+ctrl+left", "spawn", "echo example")
```

"Super" is the Windows/Logo key on PC keyboards.

### 6.5 Key names

Key names follow XKB keysym names. Common ones:

```text
Letters      : a b c ... z
Digits       : 0 1 2 ... 9
Enter        : Return
Space        : space
Tab          : Tab
Escape       : Escape
Backspace    : BackSpace
Delete       : Delete
Arrows       : Left Right Up Down
Home/End     : Home End
Page         : Page_Up Page_Down
Function     : F1 F2 ... F12
Punctuation  : minus equal comma period slash backslash
               semicolon apostrophe grave bracketleft bracketright
Multimedia   : XF86AudioRaiseVolume, XF86AudioLowerVolume,
               XF86AudioMute, XF86AudioPlay, XF86MonBrightnessUp
```

If you are unsure of a key name, use `wev` (Wayland event viewer):

```sh
sudo pacman -S wev
wev
```

Press the key you want to use, then look for "keysym" in the output. That name
is what you write in the config.

### 6.6 Available actions

**spawn** — Runs a shell command. The third argument is required. Example:
`nide.bind("super+w", "spawn", "foot")`. The command runs through
`/bin/sh -c`, so arguments, pipes, and shell variables are allowed. Example:
`nide.bind("super+b", "spawn", "firefox --new-window")`.

**close** — Asks the focused application to close its window. No argument
needed. If no window is focused, nothing happens.

**maximize** — Maximizes the focused window while keeping a margin around it.
No argument needed. The margin is set by `nide.maximize_margin` (default 8
pixels) and also applies to snapped windows. Pressing the binding again
restores the previous floating size and position.

**fullscreen** — Toggles fullscreen for the focused window. No argument needed.
Fullscreen fills the entire output without a margin. Pressing the binding
again returns the window to its previous state (floating, maximized, or
snapped). Application-initiated fullscreen, such as F11 in a browser or game,
works the same way.

**launcher** — Opens or closes the built-in launcher overlay. No argument
needed. The launcher searches applications from XDG `.desktop` files and lists
open windows in two tabs. The default binding is Super+Tab.

**quit** — Quits the compositor. No argument needed. All Wayland applications
are disconnected and closed. Save your work first.

There are no other actions. Windows are moved and resized with the mouse
(Super+click). Tiling and workspaces are not available.

### 6.7 Complete config example

Everyday example:

```lua
-- Your favorite terminal. Change it to one you have installed.
local terminal = "foot"

-- Open a terminal.
nide.bind("super+w", "spawn", terminal)

-- Open the built-in launcher.
nide.bind("super+tab", "launcher")

-- Close the focused window.
nide.bind("super+c", "close")

-- Maximize and fullscreen.
nide.bind("super+m", "maximize")
nide.bind("super+s", "fullscreen")

-- Gap around maximized and snapped windows, in pixels.
nide.maximize_margin = 8

-- Quit the compositor.
nide.bind("super+shift+q", "quit")

-- Print a message to the log when the config is loaded (optional).
nide.log("NIDE config loaded, version " .. nide.version)
```

Example with several applications:

```lua
local terminal = "foot"

nide.bind("super+w", "spawn", terminal)
nide.bind("super+b", "spawn", "firefox")
nide.bind("super+f", "spawn", "nautilus")
nide.bind("super+t", "spawn", terminal .. " --title=new-terminal")
nide.bind("super+c", "close")
nide.bind("super+m", "maximize")
nide.bind("super+s", "fullscreen")
nide.bind("super+tab", "launcher")
nide.maximize_margin = 12
nide.bind("super+shift+q", "quit")
```

Multimedia key example (requires wpctl/PipeWire):

```lua
nide.bind("XF86AudioRaiseVolume", "spawn",
    "wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%+")
nide.bind("XF86AudioLowerVolume", "spawn",
    "wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-")
nide.bind("XF86AudioMute", "spawn",
    "wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle")
```

If a command fails, NIDE does not show a notification. Run the command in a
terminal first to make sure the program exists. For example:

```sh
command -v foot
command -v fuzzel
```

### 6.8 Debugging config

`nide.log(...)` prints messages to the NIDE log. It accepts multiple values
separated by commas:

```lua
nide.log("terminal =", terminal, "version =", nide.version)
```

`nide.version` contains the NIDE version string.

When run from a terminal, logs are visible immediately. Add `-d` for more
detailed wlroots logging:

```sh
./nide -d -c ~/.config/nide/config.lua
```

When run from the greeter/SDDM, logs are usually in:

```text
~/.local/share/sddm/wayland-session.log
```

and:

```sh
journalctl -b -u sddm
```

### 6.9 Checking config and hot reload

Check the config at any time without disturbing the session:

```sh
cd ~/Projects/nide
./nide --check -c ~/.config/nide/config.lua
```

Example of valid output:

```text
Config: /home/user/.config/nide/config.lua
Maximize margin: 8
Keybindings (6):
  super+w              -> spawn foot
  super+c              -> close
  super+m              -> maximize
  super+s              -> fullscreen
  super+tab            -> launcher
  super+shift+q        -> quit
```

If there is a Lua error, NIDE prints the filename and error message,
including the line number. Fix it and check again.

Hot reload:

NIDE watches the active config file. After the file is saved, new bindings
take effect immediately without a restart. Open windows keep running, and a
changed `nide.maximize_margin` is applied to open maximized and snapped
windows.

If a config file exists, startup logs show the watched path:

```text
hot reload active for /home/user/.config/nide/config.lua
```

On a successful reload:

```text
config reloaded: /home/user/.config/nide/config.lua (6 keybindings)
```

Rules and behavior:

- Only the active config file is watched. If NIDE is started with
  `-c config/config.lua`, that file is watched, not
  `~/.config/nide/config.lua`.
- If the file is deleted or moved, the previous config stays active.
- If the file is saved but contains a Lua error, the reload is cancelled and
  the previous config stays active. The error appears in the log; fix it and
  save again.
- A config with 0 keybindings is rejected. It is still recommended to always
  have a quit binding so you cannot get locked in.
- If NIDE starts with no config file at all (default mode), no file is
  watched. Create a config and restart once.
- Editors that save by renaming (vim, nano, and similar) are supported.

Viewing hot reload logs:

- From a terminal: logs are visible immediately.
- From an SDDM session: look for "hot reload" or "reloaded" in
  `~/.local/share/sddm/wayland-session.log`.

If reload is not detected, check the path you edited and the path in the log.
As a fallback, restart NIDE with Super+Shift+Q and run it again.

### 6.10 If the config is broken and you cannot log in

If the config has an error, NIDE refuses to start and logging in from the
greeter may return to the login screen. Fix it from a TTY:

1. Ctrl+Alt+F3, log in.
2. Check the error:

```sh
nide --check -c ~/.config/nide/config.lua
```

3. Edit the config:

```sh
nano ~/.config/nide/config.lua
```

4. If needed, restore the default example:

```sh
mkdir -p ~/.config/nide
cp ~/Projects/nide/config/config.lua ~/.config/nide/config.lua
```

5. Check again, then return to the greeter with Ctrl+Alt+F1 or F2.

## 7. Window behavior and appearance

**No decorations** — NIDE asks applications to use server-side decorations,
but NIDE itself does not draw a title bar, border, or buttons. Applications
also do not draw their own decorations. The result is completely plain
windows.

Consequences:

- No close button on windows. Use Super+C.
- No maximize or fullscreen button. Use Super+M and Super+S.
- No title bar to drag. Use Super+left-click.
- No resize handle. Use Super+right-click (bottom-right corner).

**Placement** — New windows are centered on the output where the cursor is.
Each new window is shifted 32 pixels from the previous one (cascade),
repeating every 8 windows. Positions are kept at least 8 pixels from the
screen edge.

**Focus** — New windows are focused immediately. Click another window to
switch. Clicking an empty area releases keyboard focus.

**Launcher** — The launcher is a centered overlay with a dimmed background,
a search field, two tabs, a 5x3 tile grid, count badges, and pagination
controls. Open Windows tiles update when windows open or close. All Apps reads
XDG `.desktop` files the first time the launcher opens; restart NIDE to pick
up newly installed applications. Application icons are colored initials for
now, not icon files from the desktop entries.

**Maximize and fullscreen** — Super+M maximizes the focused window with a
margin from `nide.maximize_margin` (default 8 pixels). Saving a changed margin
applies it to open maximized and snapped windows. Super+S toggles true
fullscreen, which fills the output without a margin. Applications can also
request maximize or fullscreen; NIDE honors those requests, so F11 fullscreen
in browsers and games works. Maximize, fullscreen, and snapping remember the
previous floating geometry and restore it when the window returns to floating.

**Snapping** — While moving a floating window, drag the pointer to the left or
right edge of the output. A translucent preview shows the half-screen target
while the window is still floating. Drag the pointer to the top edge to preview
fullscreen instead. The preview uses `nide.maximize_margin` for side targets;
fullscreen has no margin. The window is resized only when you release the mouse
button. The preview follows the pointer rather than the window edge, so large
windows are easy to snap.

A snapped window stays attached when you push toward its edge. Pull inward or
vertically at least 64 pixels to detach it. A pressure gauge on the snapped
edge fills as you pull; when it is full, the window returns to floating mode.
This prevents a snap from breaking on small pointer movements.

**Multi-monitor** — Outputs are detected automatically and arranged by
wlroots. There is no manual monitor position configuration yet. New windows
appear on the monitor where the cursor is.

**Clipboard** — Clipboard and primary selection are supported as long as the
source application is still alive. There is no clipboard manager.

**Scale/HiDPI** — There is no scale or fractional scaling configuration yet.
The display follows the output as-is.

## 8. Command-line options and environment

Command-line options:

```text
-c, --config PATH   use a specific config file
-C, --check         check the config and exit
-d, --debug         wlroots debug logging (more output)
-v, --version       print the version
-h, --help          print help
```

Examples:

```sh
./nide --check -c config/config.lua
./nide -d -c ~/.config/nide/config.lua
./nide --version
```

`--version` prints the NIDE version, for example:

```text
nide 0.4.0
```

Relevant environment variables:

**NIDE_CONFIG** — Config path, used when `-c` is not given.

**XDG_CONFIG_HOME** — Config base directory. If set, NIDE looks for
`$XDG_CONFIG_HOME/nide/config.lua` first.

**WLR_BACKENDS=x11** — For nested testing on X11/Xephyr.

**WLR_RENDERER=pixman** — Software renderer for nested testing.

**WLR_RENDERER_ALLOW_SOFTWARE=1** — Allow the software renderer.

NIDE also sets environment variables for spawned applications:

```text
WAYLAND_DISPLAY      NIDE's Wayland socket
XDG_CURRENT_DESKTOP  set to "nide"
```

Do not set `WAYLAND_DISPLAY` manually when running NIDE; NIDE creates its own
socket and passes it to child applications.

## 9. Limitations

This is a minimal project, not a replacement for a full desktop. Not yet
available:

- XWayland. X11 applications cannot run.
- layer-shell. Bars/panels such as Waybar and layer-shell-based notifications
  are not supported. Use the launcher's Open Windows tab instead of a dock or
  taskbar.
- Minimize, animations, shadows, and decorations.
- A traditional Alt+Tab switcher and workspaces. The launcher's Open Windows
  tab provides window switching.
- Launcher icon files. Icons are generated from colored initials.
- Tiling or automatic layout management; only edge snapping (left/right
  halves and top fullscreen) is available.
- Manual multi-monitor position/scale configuration.
- Input method (IME) for languages that require composition.
- Idle management, screen lock, and automatic DPMS.
- Touch, tablet, and gestures.
- Color, border, or cursor theme configuration.

If you need these features, NIDE can serve as a foundation for development,
but they are not available yet.

## 10. Troubleshooting

**"./nide: No such file or directory"**

You are not in the project directory. Run:

```sh
cd ~/Projects/nide
./nide -c config/config.lua
```

**"Server is already active for display :5"**

The display number is already in use. Choose another number, for example :6
or :7. On KDE, :0, :1, and :2 are used by XWayland, so do not use them.

**"failed to create backend" or "failed to open DRM device"**

The DRM backend cannot be used. Common causes:

- Another graphical session is already active.
- No seat/logind permission.
- Trying DRM inside a Wayland session without the X11 backend.

For desktop testing, use `WLR_BACKENDS=x11` and Xephyr as described in
section 4.3.

**Super+W does not open a terminal in Xephyr**

- Make sure the Xephyr window has been clicked/focused.
- Make sure the binding exists in the config being used (check with
  `--check`).
- Make sure foot is installed: `command -v foot`.
- A KDE global shortcut may be grabbing Super+W. Try changing the binding to
  alt+w temporarily, or test in a full session.

**Super+Tab does not open the launcher**

- Make sure the NIDE output or Xephyr window has keyboard focus.
- Super+Tab is also a built-in fallback, but adding
  `nide.bind("super+tab", "launcher")` to the config is recommended.
- A desktop environment global shortcut may grab Super+Tab when testing
  nested. Rebind the action to another combination, for example
  `nide.bind("super+space", "launcher")`.
- If the All Apps tab is empty, check that `.desktop` files exist in
  `~/.local/share/applications` or `/usr/share/applications` and that they are
  not marked `Hidden=true` or `NoDisplay=true`.

**Terminal does not appear and there is no error message**

The spawn command runs through the shell; if the program does not exist, the
error only goes to the NIDE log. Check the log and run the command manually in
a terminal.

**"failed to load config ..."**

There is a Lua error or an invalid action/combination name. Run:

```sh
./nide --check -c ~/.config/nide/config.lua
```

The error message shows the problematic line.

**No close/quit binding**

A found config replaces the default bindings. Add:

```lua
nide.bind("super+c", "close")
nide.bind("super+shift+q", "quit")
```

**Trapped in NIDE without a quit binding**

Switch to a TTY with Ctrl+Alt+F3, log in, then:

```sh
pkill -x nide
```

If running nested, close the Xephyr window or stop the NIDE process from
another terminal.

**Logging in from the greeter returns to the login screen**

Check:

```sh
~/.local/share/sddm/wayland-session.log
journalctl -b -u sddm
nide --check -c ~/.config/nide/config.lua
which nide
```

Make sure the binary is at `/usr/bin/nide` and the config is valid.

**X11 applications do not run**

Expected. NIDE does not enable XWayland. Use Wayland-native applications. For
applications with both backends, force Wayland, for example:

```sh
GDK_BACKEND=wayland application
QT_QPA_PLATFORM=wayland application
```

**Windows have no title bar or close button**

That is NIDE's design. Use Super+C to close and Super+left-click to move.

**Config was changed but has no effect**

The active config reloads automatically, so make sure:

- The file was actually saved to disk, not just kept in the editor buffer.
- The file you edited matches the path in the "hot reload active" log.
- There are no config errors in the log; if there are, the previous config
  stays active.
- If the session started without a config file (default mode), create a config
  and restart once.

If it is still not detected, restart with Super+Shift+Q and run it again, or
log out and back in from the greeter.

**Black screen in Xephyr**

Make sure you use:

```sh
WLR_BACKENDS=x11
WLR_RENDERER=pixman
WLR_RENDERER_ALLOW_SOFTWARE=1
```

Then click the Xephyr window. Also check the NIDE log in the terminal.

## 11. Quick reference

Build:

```sh
cd ~/Projects/nide
make
```

Check config:

```sh
./nide --check -c ~/.config/nide/config.lua
```

After the config is saved, NIDE reloads bindings automatically. No restart is
needed; open windows keep running.

Install for the greeter:

```sh
sudo make install PREFIX=/usr
mkdir -p ~/.config/nide
cp config/config.lua ~/.config/nide/config.lua
```

Nested test:

```sh
Xephyr :5 -screen 1280x800 &
DISPLAY=:5 WLR_BACKENDS=x11 WLR_RENDERER=pixman \
  WLR_RENDERER_ALLOW_SOFTWARE=1 ./nide -c config/config.lua
```

Default bindings (only when no config file exists):

```text
Super+W          open foot
Super+C          close focused window
Super+M          maximize with margin
Super+S          toggle fullscreen
Super+Tab        open/close the launcher
Super+Shift+Q    quit NIDE
```

Launcher keys:

```text
Type             search the current tab
Arrow keys       move the selection
Enter            launch app or focus window
Tab              switch Open Windows / All Apps
Page Up/Down     change page
Mouse wheel      change page
Esc / Super+Tab  close
```

Mouse:

```text
Super+left-click   move window; drag the pointer to a side edge for a
                   half-screen preview, or to the top edge for fullscreen
Super+right-click  resize from bottom-right corner
```

Minimal config example:

```lua
local terminal = "foot"

nide.bind("super+w", "spawn", terminal)
nide.bind("super+c", "close")
nide.bind("super+m", "maximize")
nide.bind("super+s", "fullscreen")
nide.bind("super+tab", "launcher")
nide.maximize_margin = 8
nide.bind("super+shift+q", "quit")
```

We hope this helps. Start with a minimal config, check it with `--check`, test
nested, and install it for the greeter when you are comfortable.
