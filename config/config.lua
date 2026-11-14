-- Konfigurasi NIDE
-- Salin file ini ke ~/.config/nide/config.lua, lalu sesuaikan.
--
-- Format binding:
--   nide.bind("modifier+tombol", "aksi", "argumen")
--
-- Modifier: shift, ctrl, alt, super (bisa digabung dengan +).
-- Aksi     : spawn (jalankan perintah), close (tutup jendela fokus), quit.

-- Ganti dengan terminal Wayland favoritmu: foot, kitty, alacritty, dll.
local terminal = "dolphin"
local file = "dolphin"

-- Super+W membuka terminal.
nide.bind("super+w", "spawn", terminal)

-- Super+F membuka File Exploler
nide.bind("super+f", "spawn", file)

-- Super+C menutup aplikasi/jendela yang sedang fokus.
nide.bind("super+c", "close")

-- Tambahan: keluar dari compositor.
nide.bind("super+shift+q", "quit")

-- Kamu juga bisa menambah binding lain, misalnya:
-- nide.bind("super+Return", "spawn", terminal)
-- nide.bind("super+d", "spawn", "fuzzel")
