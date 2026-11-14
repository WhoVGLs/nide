-- NIDE configuration
-- Copy this file to ~/.config/nide/config.lua and adjust it.
--
-- Binding format:
--   nide.bind("modifier+key", "action", "argument")
--
-- Modifiers: shift, ctrl, alt, super (can be combined with +).
-- Actions  : spawn (run a command), close (close the focused window), quit.

-- Replace with your favorite Wayland terminal: foot, kitty, alacritty, etc.
local terminal = "dolphin"
local file = "dolphin"

-- Super+W opens a terminal.
nide.bind("super+w", "spawn", terminal)

-- Super+F opens the file manager.
nide.bind("super+f", "spawn", file)

-- Super+C closes the focused application/window.
nide.bind("super+c", "close")

-- Optional: quit the compositor.
nide.bind("super+shift+q", "quit")

-- You can add more bindings, for example:
-- nide.bind("super+Return", "spawn", terminal)
-- nide.bind("super+d", "spawn", "fuzzel")
