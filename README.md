# NIDE

**NIDE** adalah singkatan rekursif dari **"NIDE Isn't a Desktop"**. Meskipun
namanya begitu, NIDE adalah compositor Wayland kecil dan sederhana, dibangun
dengan [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots) 0.20 dan
dikonfigurasi penuh memakai Lua.

Konfigurasi yang dipakai proyek ini ada di satu file:
[`config/config.lua`](config/config.lua). Panduan lengkap pemakaian dan
konfigurasi ada di [`Docs.txt`](Docs.txt).

## Fitur

- Compositor Wayland native berbasis wlroots.
- Jendela **floating/dynamic**, bukan tiling.
- Tanpa dekorasi dari compositor: tidak ada title bar, border, atau tombol
  jendela. Aplikasi juga tidak diminta menggambar client-side decoration.
- Konfigurasi keybinding dengan Lua, dibaca dari `~/.config/nide/config.lua`.
- Hot reload: menyimpan file config langsung memperbarui binding tanpa restart.
- `Super+W` membuka terminal, `Super+C` menutup jendela yang sedang fokus.
- `Super+klik-kiri` untuk memindahkan jendela, `Super+klik-kanan` untuk
  mengubah ukuran.
- Clipboard dan primary selection (paste dengan klik tengah) didukung.
- Satu binary kecil, satu file C, tanpa dependency toolkit.

## Kebutuhan

Arch Linux:

```sh
sudo pacman -S --needed base-devel wlroots0.20 lua54
```

Distribusi lain perlu header pengembangan wlroots **0.20**, Lua **5.4**,
wayland-server, dan xkbcommon.

## Build

```sh
make
./nide --check -c config/config.lua
```

Pasang ke sistem (opsional):

```sh
sudo make install PREFIX=/usr
mkdir -p ~/.config/nide
cp config/config.lua ~/.config/nide/config.lua
```

`PREFIX=/usr` diperlukan agar display manager menemukan
`/usr/share/wayland-sessions/nide.desktop`. Tanpa itu, file terpasang di
`/usr/local` dan sesi NIDE mungkin tidak muncul di SDDM.

## Menjalankan

Dari TTY (backend DRM/libinput):

```sh
./nide -c ~/.config/nide/config.lua
```

Setelah terpasang, sesi `NIDE` juga muncul di display manager (SDDM, GDM,
dsbx, dll.) lewat file `nide.desktop`.

Uji coba bersarang di dalam sesi X11/XWayland (aman, tidak mengganggu sesi
utama):

```sh
Xephyr :2 -screen 1280x800 &
DISPLAY=:2 WLR_BACKENDS=x11 WLR_RENDERER=pixman \
  WLR_RENDERER_ALLOW_SOFTWARE=1 ./nide -c config/config.lua
```

Atau langsung di sesi X11 yang sedang berjalan:

```sh
WLR_BACKENDS=x11 WLR_RENDERER=pixman WLR_RENDERER_ALLOW_SOFTWARE=1 \
  ./nide -c config/config.lua
```

Jendela compositor akan muncul sebagai jendela 1024x768. Klik jendela itu,
lalu tekan `Super+W`.

> Untuk uji di X11, `WLR_BACKENDS=x11` wajib dipakai. Backend Wayland bersarang
> tidak tersedia di paket wlroots Arch.

## Konfigurasi

Urutan pencarian file konfigurasi:

1. `-c PATH`
2. `$NIDE_CONFIG`
3. `$XDG_CONFIG_HOME/nide/config.lua`
4. `~/.config/nide/config.lua`
5. `./config.lua`

Kalau tidak ada file config, NIDE memakai binding bawaan:

```lua
nide.bind("super+w", "spawn", "foot")
nide.bind("super+c", "close")
nide.bind("super+shift+q", "quit")
```

Contoh config:

```lua
local terminal = "foot"

nide.bind("super+w", "spawn", terminal)
nide.bind("super+c", "close")
nide.bind("super+shift+q", "quit")
```

Formatnya:

```lua
nide.bind("modifier+tombol", "aksi", "argumen")
```

Modifier yang dikenal: `shift`, `ctrl`, `alt`, `super` (boleh digabung dengan
`+`). Nama tombol mengikuti nama keysym XKB, misalnya `w`, `Return`, `space`,
`Left`, `1`, `bracketleft`.

Aksi:

| Aksi | Argumen | Efek |
|---|---|---|
| `spawn` | perintah shell | menjalankan program |
| `close` | — | menutup jendela yang fokus |
| `quit` | — | keluar dari compositor |

`nide.log(...)` bisa dipakai untuk mencetak pesan dari config:

```lua
nide.log("NIDE siap")
```

NIDE memantau file config aktif dan memuat ulang binding otomatis saat file
disimpan. Jendela yang sedang terbuka tidak terpengaruh. Config yang error atau
tidak punya keybinding diabaikan, config lama tetap dipakai. Kalau NIDE dimulai
tanpa file config (mode bawaan), restart sekali setelah membuat file config.

## Dekorasi dan perilaku jendela

NIDE sengaja tidak menggambar dekorasi apa pun. Agar aplikasi juga tidak
menggambar client-side decoration (CSD), NIDE meminta mode *server-side* tetapi
tidak menggambar title bar/border. Hasilnya jendela benar-benar polos.

Konsekuensinya:

- Tidak ada tombol close/minimize/maximize di jendela. Gunakan `Super+C`.
- Pindahkan jendela dengan `Super+klik-kiri`, ubah ukuran dengan
  `Super+klik-kanan`.
- Jendela baru diletakkan di tengah dengan cascade kecil.
- Fokus berpindah saat jendela diklik atau jendela baru dibuka.

## Batasan

- Hanya aplikasi Wayland native. XWayland tidak diaktifkan, jadi aplikasi X11
  tidak bisa dijalankan.
- Belum ada layer-shell, jadi bar/panel Wayland (Waybar, dll.) belum didukung.
- Belum ada maximize, fullscreen, minimize, atau animasi.
- Input method (IME) belum didukung.
- Satu konfigurasi output; belum ada pengaturan posisi multi-monitor.

## Struktur

```
src/main.c        compositor + integrasi Lua
config/config.lua contoh konfigurasi
Docs.txt          panduan lengkap pemakaian dan konfigurasi
Makefile
nide.desktop      entri sesi Wayland untuk display manager
```

## Versi

NIDE memakai [Semantic Versioning](https://semver.org/lang/id/) dengan format
`MAJOR.MINOR.PATCH`:

- `MAJOR` naik saat ada perubahan yang tidak kompatibel.
- `MINOR` naik saat ada fitur baru yang tetap kompatibel.
- `PATCH` naik saat ada perbaikan bug yang kompatibel.

Selama versi masih `0.x`, API config dianggap belum stabil. Perubahan yang
mematahkan config lama akan menaikkan `MINOR`, misalnya `0.2.0` menjadi
`0.3.0`, dan dicatat di [`CHANGELOG.md`](CHANGELOG.md).

Versi saat ini: **0.2.0**.
