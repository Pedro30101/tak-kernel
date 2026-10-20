#!/bin/sh
set -eu

if [ "$#" -ne 2 ] || [ ! -f "$1/configure" ]; then
    echo "usage: $0 GRUB_SOURCE PROJECT_ROOT" >&2
    exit 2
fi

grub_source=$(cd "$1" && pwd)
project_root=$(cd "$2" && pwd)
grub_def="$grub_source/grub-core/Makefile.core.def"

cp "$project_root/boot/grub/bfrus.c" "$grub_source/grub-core/fs/bfrus.c"
cp "$project_root/src/bfrus_format.h" "$grub_source/grub-core/fs/bfrus_format.h"
if ! grep -q 'name = bfrus;' "$grub_def"; then
    printf '\nmodule = {\n  name = bfrus;\n  common = fs/bfrus.c;\n};\n' >> "$grub_def"
fi

build_module() {
    target=$1
    platform=$2
    destination=$3
    build_dir="$project_root/build/grub-bfrus/$destination"
    mkdir -p "$build_dir"
    cd "$build_dir"
    "$grub_source/configure" --target="$target" --with-platform="$platform" --disable-werror
    make -j2 grub-core/fs/bfrus.mod
    mkdir -p "$project_root/build/grub-modules/$destination"
    cp grub-core/fs/bfrus.mod "$project_root/build/grub-modules/$destination/bfrus.mod"
}

build_module i386 pc i386-pc
build_module x86_64 efi x86_64-efi
