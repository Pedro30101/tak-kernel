CROSS ?= x86_64-elf-
CC := $(CROSS)gcc
HOSTCC ?= gcc
AS := $(CROSS)as
LD := $(CROSS)gcc
QEMU ?= qemu-system-x86_64
QEMU_NET ?= -nic user,model=e1000

CFLAGS := -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding \
          -fno-stack-protector -fno-pie -fno-pic -m64 -mcmodel=small \
          -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-80387 -Isrc -Isrc/inc
LDFLAGS := -T linker.ld -ffreestanding -nostdlib -no-pie -m64 -Wl,--build-id=none
BUILD := build

CSRC := $(filter-out src/stbimg.c src/term.c,$(wildcard src/*.c))
OBJ := $(patsubst src/%.c,$(BUILD)/%.o,$(CSRC)) $(BUILD)/boot.o $(BUILD)/isr.o $(BUILD)/stbimg.o $(BUILD)/font_smooth.o
HDR := $(wildcard src/*.h)

.PHONY: all iso run clean apps grubimgs grub-bfrus-modules
all: $(BUILD)/kernel.elf

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/boot.o: src/boot.s | $(BUILD)
	$(AS) --64 $< -o $@

$(BUILD)/isr.o: src/isr.s | $(BUILD)
	$(AS) --64 $< -o $@

$(BUILD)/font_smooth.o: src/font_smooth.s src/font_smooth.bin | $(BUILD)
	$(AS) --64 $< -o $@

$(BUILD)/stbimg.o: src/stbimg.c src/stb_image.h | $(BUILD)
	$(CC) $(CFLAGS) -Wno-error -w -c $< -o $@

$(BUILD)/%.o: src/%.c $(HDR) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kernel.elf: $(OBJ) linker.ld
	$(LD) $(LDFLAGS) $(OBJ) -lgcc -o $@

clean:
	rm -rf $(BUILD)

ISO_ROOT := $(BUILD)/iso

OBJCOPY := $(CROSS)objcopy
UCFLAGS := -std=gnu11 -O2 -Wall -Wextra -ffreestanding -fno-pie -fno-pic -fno-stack-protector \
           -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-80387 -mcmodel=small -nostdlib -static \
           -fno-asynchronous-unwind-tables -Isdk
APPS := $(patsubst apps/%.c,%,$(wildcard apps/*.c))
APPTE := $(patsubst %,$(BUILD)/apps/%.te,$(APPS))

$(BUILD)/apps/%.elf: apps/%.c sdk/te.c sdk/tw32.c sdk/te.h sdk/tw32.h sdk/tw_font.h sdk/te.ld | $(BUILD)
	mkdir -p $(BUILD)/apps
	$(CC) $(UCFLAGS) -Wl,-T,sdk/te.ld -Wl,--build-id=none $< sdk/te.c sdk/tw32.c -o $@

$(BUILD)/apps/%.te: $(BUILD)/apps/%.elf
	$(OBJCOPY) -O binary $< $@

apps: $(APPTE)

# CD boot modules: no biosdisk (causes int13_cdrom 64-bit LBA failure on CD-ROM),
# no bfrus (custom module, needs GRUB source tree to build),
# use iso9660 instead to read from the CD filesystem
CDMOD := iso9660 fat multiboot2 normal configfile echo test true all_video video video_fb vbe gfxterm font boot
# Hard disk boot modules
GRUBMOD := biosdisk part_msdos part_gpt fat bfrus multiboot2 normal configfile echo test true all_video video video_fb vbe gfxterm font boot
EFIMOD := part_msdos part_gpt fat bfrus multiboot2 normal configfile all_video efi_gop echo test true boot
GRUB_MKIMAGE ?= grub-mkimage
GRUBDIR := /usr/lib/grub
GRUB_SOURCE ?=
GRUB_BFRUS_READY := $(BUILD)/grub-modules/.ready

$(GRUB_BFRUS_READY): boot/grub/bfrus.c src/bfrus_format.h tools/build-grub-bfrus.sh
	@test -n "$(GRUB_SOURCE)" || { echo "Set GRUB_SOURCE to an extracted GRUB source tree (2.14 recommended)." >&2; exit 1; }
	sh tools/build-grub-bfrus.sh "$(GRUB_SOURCE)" "$(CURDIR)"
	touch $@

grub-bfrus-modules: $(GRUB_BFRUS_READY)

$(BUILD)/grub/core.img: Makefile $(GRUB_BFRUS_READY) | $(BUILD)
	mkdir -p $(BUILD)/grub
	mkdir -p $(BUILD)/grub/i386-pc
	cp -a $(GRUBDIR)/i386-pc/. $(BUILD)/grub/i386-pc/
	cp $(BUILD)/grub-modules/i386-pc/bfrus.mod $(BUILD)/grub/i386-pc/bfrus.mod
	$(GRUB_MKIMAGE) -d $(BUILD)/grub/i386-pc -O i386-pc -o $@ -p '(hd0,msdos2)/boot/grub' $(GRUBMOD)
	cp $(GRUBDIR)/i386-pc/boot.img $(BUILD)/grub/boot.img

$(BUILD)/grub/BOOTX64.EFI: Makefile $(GRUB_BFRUS_READY) | $(BUILD)
	mkdir -p $(BUILD)/grub
	mkdir -p $(BUILD)/grub/x86_64-efi
	cp -a $(GRUBDIR)/x86_64-efi/. $(BUILD)/grub/x86_64-efi/
	cp $(BUILD)/grub-modules/x86_64-efi/bfrus.mod $(BUILD)/grub/x86_64-efi/bfrus.mod
	$(GRUB_MKIMAGE) -d $(BUILD)/grub/x86_64-efi -O x86_64-efi -o $@ -p '(hd0,msdos2)/boot/grub' $(EFIMOD)

grubimgs: $(BUILD)/grub/core.img $(BUILD)/grub/BOOTX64.EFI

$(BUILD)/initrd.tar: $(shell find initrd -type f 2>/dev/null) $(APPTE) | $(BUILD)
	rm -rf $(BUILD)/stage
	cp -r initrd $(BUILD)/stage
	mkdir -p $(BUILD)/stage/apps
	for f in $(APPTE); do cp $$f $(BUILD)/stage/apps/; done
	mkdir -p $(BUILD)/stage/sys/grub
	-cp $(BUILD)/grub/boot.img $(BUILD)/grub/core.img $(BUILD)/grub/BOOTX64.EFI $(BUILD)/stage/sys/grub/ 2>/dev/null
	tar --format=ustar --owner=0 --group=0 -C $(BUILD)/stage -cf $@ .

iso: $(BUILD)/kernel.elf $(BUILD)/initrd.tar
	rm -rf $(ISO_ROOT)
	mkdir -p $(ISO_ROOT)/boot/grub
	cp $(BUILD)/kernel.elf $(ISO_ROOT)/boot/kernel.elf
	cp $(BUILD)/initrd.tar $(ISO_ROOT)/boot/initrd.tar
	cp boot/grub/grub.cfg $(ISO_ROOT)/boot/grub/grub.cfg
	mkdir -p $(BUILD)/grub/i386-pc
	cp -a $(GRUBDIR)/i386-pc/. $(BUILD)/grub/i386-pc/
	$(GRUB_MKIMAGE) -d $(BUILD)/grub/i386-pc -O i386-pc -c boot/grub/grub.cfg -o $(ISO_ROOT)/boot/grub/eltorito.img -p "(cd)/boot/grub" $(CDMOD)
	xorriso -as mkisofs \
		-b boot/grub/eltorito.img \
		-no-emul-boot \
		-boot-load-size 4 \
		-boot-info-table \
		-o $(BUILD)/astros.iso \
		$(ISO_ROOT)

run: iso
	$(QEMU) -m 512 -cdrom $(BUILD)/astros.iso -vga std -serial stdio $(QEMU_NET)
.PRECIOUS: $(BUILD)/apps/%.elf
