CROSS ?= x86_64-elf-
CC := $(CROSS)gcc
HOSTCC ?= gcc
AS := $(CROSS)as
LD := $(CROSS)gcc
GRUB_MKRESCUE ?= grub-mkrescue
QEMU ?= qemu-system-x86_64

CFLAGS := -std=gnu11 -O2 -Wall -Wextra -Werror -ffreestanding \
          -fno-stack-protector -fno-pie -fno-pic -m64 -mcmodel=small \
          -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-80387 -Isrc -Isrc/inc
LDFLAGS := -T linker.ld -ffreestanding -nostdlib -no-pie -m64 -Wl,--build-id=none
BUILD := build

CSRC := $(filter-out src/stbimg.c,$(wildcard src/*.c))
OBJ := $(patsubst src/%.c,$(BUILD)/%.o,$(CSRC)) $(BUILD)/boot.o $(BUILD)/isr.o $(BUILD)/stbimg.o
HDR := $(wildcard src/*.h)

.PHONY: all iso run clean apps grubimgs test-auth
all: $(BUILD)/kernel.elf

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/boot.o: src/boot.s | $(BUILD)
	$(AS) --64 $< -o $@

$(BUILD)/isr.o: src/isr.s | $(BUILD)
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

$(BUILD)/auth_test: tests/auth_test.c src/auth.c src/auth.h src/vfs.h src/tak.h | $(BUILD)
	$(HOSTCC) -std=gnu11 -O2 -Wall -Wextra -Werror -DAUTH_TEST -Isrc $< -o $@

test-auth: $(BUILD)/auth_test
	$(BUILD)/auth_test

GRUBMOD := biosdisk part_msdos part_gpt fat multiboot2 normal configfile echo test true all_video video video_fb vbe gfxterm font boot
EFIMOD := part_msdos part_gpt fat multiboot2 normal configfile all_video efi_gop echo test true boot
GRUB_MKIMAGE ?= grub-mkimage
GRUBDIR := /usr/lib/grub

$(BUILD)/grub/core.img: Makefile | $(BUILD)
	mkdir -p $(BUILD)/grub
	$(GRUB_MKIMAGE) -O i386-pc -o $@ -p '(hd0,msdos1)/boot/grub' $(GRUBMOD)
	cp $(GRUBDIR)/i386-pc/boot.img $(BUILD)/grub/boot.img

$(BUILD)/grub/BOOTX64.EFI: Makefile | $(BUILD)
	mkdir -p $(BUILD)/grub
	$(GRUB_MKIMAGE) -O x86_64-efi -o $@ -p /boot/grub $(EFIMOD)

grubimgs: $(BUILD)/grub/core.img $(BUILD)/grub/BOOTX64.EFI

$(BUILD)/initrd.tar: $(shell find initrd -type f 2>/dev/null) $(APPTE) grubimgs | $(BUILD)
	rm -rf $(BUILD)/stage
	cp -r initrd $(BUILD)/stage
	mkdir -p $(BUILD)/stage/apps
	for f in $(APPTE); do cp $$f $(BUILD)/stage/apps/; done
	mkdir -p $(BUILD)/stage/sys/grub
	-cp $(BUILD)/grub/* $(BUILD)/stage/sys/grub/
	tar --format=ustar --owner=0 --group=0 -C $(BUILD)/stage -cf $@ .

iso: $(BUILD)/kernel.elf $(BUILD)/initrd.tar
	rm -rf $(ISO_ROOT)
	mkdir -p $(ISO_ROOT)/boot/grub
	cp $(BUILD)/kernel.elf $(ISO_ROOT)/boot/kernel.elf
	cp $(BUILD)/initrd.tar $(ISO_ROOT)/boot/initrd.tar
	cp boot/grub/grub.cfg $(ISO_ROOT)/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $(BUILD)/astros.iso $(ISO_ROOT)

run: iso
	$(QEMU) -m 512 -cdrom $(BUILD)/astros.iso -vga std -serial stdio
.PRECIOUS: $(BUILD)/apps/%.elf
