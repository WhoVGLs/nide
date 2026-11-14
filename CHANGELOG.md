# Changelog

Semua perubahan penting pada NIDE dicatat di file ini.

Format mengikuti [Keep a Changelog](https://keepachangelog.com/id/1.1.0/),
dan versi mengikuti [Semantic Versioning](https://semver.org/lang/id/).

## [0.2.0] "Jendela" - 2026-11-14

Rilis pertama dengan nama NIDE. Sebelumnya proyek ini bernama wlua.

### Added

- Hot reload config memakai inotify: binding dimuat ulang otomatis saat file
  config disimpan, termasuk penyimpanan atomic (rename) ala vim dan nano.
- Config invalid, config tanpa keybinding, atau config yang dihapus tidak
  mengganti config aktif; error dicatat ke log.
- `CHANGELOG.md` dan kebijakan Semantic Versioning.

### Changed

- Nama proyek, binary, dan entri sesi Wayland menjadi NIDE / `nide`.
- API Lua berubah dari `wl.*` menjadi `nide.*` tanpa alias kompatibilitas.
- Path config menjadi `~/.config/nide/config.lua`; environment menjadi
  `NIDE_CONFIG`.
- Path install menjadi `/usr/bin/nide`, `/usr/share/nide/config.lua`, dan
  `/usr/share/wayland-sessions/nide.desktop`.
- `XDG_CURRENT_DESKTOP` menjadi `nide`.
- Versi mengikuti SemVer dan dimulai dari `0.2.0`.

## [0.1.0] - 2026-11-13

Rilis awal dengan nama wlua.

- Compositor Wayland floating berbasis wlroots 0.20.
- Tanpa dekorasi dari compositor (tanpa title bar, border, atau tombol jendela).
- Keybinding dikonfigurasi dengan Lua.
- `Super+W` membuka terminal, `Super+C` menutup jendela yang fokus.
- Dukungan clipboard dan primary selection.
