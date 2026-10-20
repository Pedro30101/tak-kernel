#!/bin/bash
set -e
cd /mnt/c/Users/lacer/AppData/Local/Temp/tak-kernel-login-20261013

mkdir -p build/grub/i386-pc
cp -a /usr/lib/grub/i386-pc/. build/grub/i386-pc/
cp /usr/lib/grub/i386-pc/boot.img build/grub/boot.img

grub-mkimage -d build/grub/i386-pc -O i386-pc -o build/grub/core.img -p '(hd0,msdos2)/boot/grub' biosdisk part_msdos part_gpt fat multiboot2 normal configfile echo test true all_video video video_fb vbe gfxterm font boot
echo "core.img built: $(ls -la build/grub/core.img)"

rm -rf build/stage
cp -r initrd build/stage
mkdir -p build/stage/apps
mkdir -p build/stage/sys/grub
cp build/grub/boot.img build/grub/core.img build/stage/sys/grub/ 2>/dev/null || true
cp build/grub/BOOTX64.EFI build/stage/sys/grub/ 2>/dev/null || true
tar --format=ustar --owner=0 --group=0 -C build/stage -cf build/initrd.tar .
echo "initrd.tar built: $(ls -la build/initrd.tar)"

rm -rf build/iso
mkdir -p build/iso/boot/grub
cp build/kernel.elf build/iso/boot/kernel.elf
cp build/initrd.tar build/iso/boot/initrd.tar
cp boot/grub/grub.cfg build/iso/boot/grub/grub.cfg
grub-mkrescue -d build/grub -o build/astros.iso build/iso
echo "ISO built: $(ls -la build/astros.iso)"
echo "SHA-256: $(sha256sum build/astros.iso | cut -d' ' -f1)"
