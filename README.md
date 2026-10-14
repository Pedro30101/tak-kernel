# Tak Kernel – **0.9.6 BETA**

> **Astros** – a lightweight desktop environment that boots directly from the kernel (GRUB‑multiboot2).

---

## Table of Contents

1. [Overview](#overview)  
2. [Build & Run](#build--run)  
3. [Project Layout](#project-layout)  
4. [Versioning](#versioning)  
5. [Features](#features)  
6. [Installation (Tak Installer)](#installation-tak-installer)  
7. [User‑Facing Applications](#user‑facing-applications)  
8. [Configuration & Settings](#configuration--settings)  
9. [Development Notes](#development-notes)  
10. [License](#license)

---

## Overview

The **Tak kernel** is a small, monolithic x86_64 OS with an integrated **Astros** desktop environment. It boots through **GRUB**, loads an initramfs, and presents a glass‑styled UI inspired by Ubuntu GNOME, Windows Aero, and Liquid Glass.

Key goals:

- **Minimal footprint** – < 5 MiB ISO, runs comfortably in QEMU/VirtualBox.
- **Extensible UI** – `tw32` window library, taskbar, start‑menu, Settings app.
- **Embedded scripting** – tiny Python interpreter and **TIDE Studio** IDE for `.py` and `.c` files.
- **Media support** – audio & video playback (wav, mp3, ogg, mp4, mkv) and image viewers (jpeg, png, webp, svg).

The kernel version constant (`TAK_VERSION`) is defined in `src/kernel.c` and is **0.9.6 BETA**.

---

## Build & Run

```bash
# Inside the repo root (WSL Ubuntu recommended)
sudo apt update
sudo apt install -y build-essential grub-common grub-pc-bin \
    grub-efi-amd64-bin xorriso mtools qemu-system-x86

# Build the ISO
make CROSS=x86_64-linux-gnu- iso        # produces build/astros.iso

# Run it in QEMU (adjust memory as you like)
qemu-system-x86_64 -m 512 -cdrom build/astros.iso -display gtk
```

For **VirtualBox**, attach the ISO to a new VM and start it.

---

## Project Layout

```
src/
│   boot.s           – entry point, multiboot2 header
│   kernel.c         – core kernel, version constant
│   memory.c, cpu.c  – phys‑mem, paging, scheduler
│   input.c          – PS/2 keyboard & mouse handling
│   gfx.c, wm.c      – framebuffer, glass rendering, taskbar
│   install.c        – Tak Installer (partition method UI)
│   apps.c           – file‑type dispatcher
│   media.c          – audio/video player (wav, mp3, ogg, mp4, mkv)
│   view.c           – image viewer (jpeg, png, webp, svg)
│   tide.c           – TIDE Studio IDE (Python / C)
│   ...              – other utilities (taskmgr, settings, etc.)
inc/
│   *.h              – public headers for the SDK
sdk/
│   tw32/            – window library used by user apps
apps/
│   sample .te apps  – hello, paint, calc, snake
```

The **`README.md`** in the repository root (this file) provides the public documentation; internal docs are in `doc/` (currently empty).

---

## Versioning

The kernel version is stored in a single source location:

```c
/* src/kernel.c */
static const char TAK_VERSION[] = "0.9.6 BETA";
```

All UI screens (wallpaper, system‑info dialogs, Settings pages) read this constant via the kernel’s API, so the new version appears automatically throughout the OS.

> **If you ever need to bump the version again, edit only the line above.** No other file contains the version string.

---

## Features

- **GRUB boot** – multiboot2 header, BIOS/UEFI support.
- **Astros DE** – glass‑styled windows, movable taskbar (top/left/right/bottom), start‑menu, Settings.
- **Tw32 window library** – `tw_open`, `tw_flip`, `tw_draw*` functions for apps.
- **Executable format** – `.te` (TE02 format; code is read-only/executable and data is writable/non-executable).
- **Embedded Python** – operand stack, arithmetic, conditionals, loops, `print`.
- **TIDE Studio IDE** – edit/run `.py` and `.c` files, save to the in‑kernel VFS.
- **Media Player** – playback of wav, mp3, ogg, mp4, mkv (stub PCM for unsupported codecs).
- **Image Viewer** – jpeg, png, webp, svg (placeholder for missing decoders).
- **Task Manager** – list of running processes, kill button.
- **Installer** – three partitioning modes (Wipe Whole Drive, Automatic, Manual), UI tip for VirtualBox.
- **Installed-system login** – first boot creates one local account; later boots require its password. The live installer remains accessible without an account.
- **Cursor** – custom GNOME/KDE‑style graphic (fallback generated if decoder missing).

---

## Installation (Tak Installer)

The installer runs as page 2 of `install.c`:

1. **Choose drive** (detects IDE/PATA disks).
2. **Select partition method** (radio buttons for the three options).
3. **Confirm** → installer formats the disk (if “Wipe Whole Drive”) and copies the OS.
4. **Finish** → system reboots into Astros.

A short tip appears when the VM reports GPU vendor `0x80EE` (VirtualBox), reminding the user to enable the IDE controller.

---

## User‑Facing Applications

| App | Description | Launch command (`apps.c`) |
|-----|-------------|---------------------------|
| **File Explorer** | Browse VFS, open files with associated app. | `open /` |
| **Image Viewer** | View JPEG, PNG, WebP, SVG. | `open image.webp` |
| **Media Player** | Play audio/video (wav/mp3/ogg/mp4/mkv). | `open song.mp3` |
| **TIDE Studio** | Edit/run `.py` or `.c` files. | `open script.py` |
| **Task Manager** | Show processes, terminate. | `taskmgr` |
| **Settings** | Change wallpaper, task‑bar position, screen resolution, etc. | `settings` |
| **Terminal** | Minimal shell on the kernel console. | `term` |

All apps are loaded from the initramfs; new `.te` binaries can be added to `apps/` and compiled with `make apps`.

---

## Configuration & Settings

- **Wallpaper** – stored as `/wallpaper.bmp` in the VFS; change via Settings → “Wallpaper”.
- **System Info dialog** – reads `TAK_VERSION` and displays it with the build date (hard‑coded in `kernel.c`).
- **Task‑bar Position** – `settings` writes a small config file (`/etc/astros.conf`) that the WM reads on boot.

---

## Development Notes

- **Memory Management** – simple bump allocator + page‑frame bitmap (`mem.c`).
- **User‑process W^X** – bootstrap enables EFER.NXE so non-executable user data and stack pages are honored by the CPU.
- **No networking** – the kernel is deliberately offline for security.
- **Login credential storage** – a per-install PBKDF2-HMAC-SHA-256 verifier with a random salt is saved on the installed FAT volume; plaintext passwords are not stored. Account setup requires CPU RDRAND support.
- **Login security scope** – the login screen gates desktop input, but most built-in applications still execute in kernel context, and the current FAT volume is not encrypted. This is not yet a security boundary against physical/offline disk access; process isolation and encrypted storage remain future work.
- **Network hardware discovery** – `net-info` lists PCI network controllers for VM bring-up diagnostics; it does not enable networking or send packets.
- **GPU drivers** – only VGA/Bochs/QEMU framebuffer is used; minimal PCI detection.
- **User-app file access** – `.te` application file reads, writes, and directory listings are limited to `/home`.
- **User-app memory protection** – TE02 code pages are read-only/executable; data, heap, and stack pages are writable/non-executable. Older TE01 apps must be rebuilt with the current SDK.
- **Future work** – add AHCI/NVMe driver, USB HID input, real audio decoding (ALSA/OSS), networking stack.

---

## License

The project is released under the **MIT License** – see `LICENSE` file for details.

---

### Quick Script to Regenerate This README

Save the following as `generate_readme.sh` and run it (`bash generate_readme.sh`). It will overwrite any existing `README.md` with the content above.

```bash
#!/usr/bin/env bash
cat > README.md <<'EOF'
# Tak Kernel – **0.9.6 BETA**

> **Astros** – a lightweight desktop environment that boots directly from the kernel (GRUB‑multiboot2).

---
... (full markdown omitted for brevity) ...
EOF

echo "README.md generated."
```

Replace the `...` with the full markdown from this file if you edit it manually.

---

### Next Steps

1. **Create a private repository on GitHub** (via the website or `gh repo create tak-kernel --private`).
2. **Add the remote** to this local folder (`git remote add origin <URL>`).
3. **Commit & push**:
   ```bash
   git init
   git add .
   git commit -m "Initial commit: Tak kernel 0.9.6 BETA"
   git branch -M main
   git push -u origin main
   ```
4. Verify the repo is **private** (Settings → Manage access).

If you need me to run any of the Git commands for you, I’ll require a GitHub personal access token (PAT) with `repo` scope or an SSH key that has write access to the target repository.

Feel free to provide the token securely, or run the commands yourself and let me know if anything fails.
