#!/bin/sh
# Build GRUB 2.14 with a BFRUS filesystem module.
# Usage: ./build-grub-brfus.sh <grub_source_dir> <output_dir>
#
# This compiles the BFRUS filesystem driver as a GRUB module so GRUB can
# load kernel.elf and initrd.tar directly from a BFRUS volume.

set -e

GRUB_SRC="$1"
OUT_DIR="$2"

if [ -z "$GRUB_SRC" ] || [ -z "$OUT_DIR" ]; then
    echo "Usage: $0 <grub_source_dir> <output_dir>"
    exit 1
fi

if [ ! -f "$GRUB_SRC/grub-core/Makefile.core.def" ]; then
    echo "Error: $GRUB_SRC does not look like a GRUB 2 source tree"
    exit 1
fi

mkdir -p "$OUT_DIR"

# Copy our module source into GRUB's fs directory
cp src/bfrus_format.h "$GRUB_SRC/grub-core/fs/bfrus_format.h"
cp src/bfrus.c "$GRUB_SRC/grub-core/fs/bfrus.c"

# Add the module to GRUB's build system
grep -q 'bfrus' "$GRUB_SRC/grub-core/Makefile.core.def" || \
    sed -i '/^fs\/fat.c$/a fs\/bfrus.c' "$GRUB_SRC/grub-core/Makefile.core.def"

# Configure and build just the module
cd "$GRUB_SRC"
./autogen.sh >/dev/null 2>&1 || true
./configure --disable-werror --disable-grub-mkfont --disable-grub-mkimage \
    --disable-grub-mkconfig --disable-grub-mkrelpath --disable-grub-mkrescue \
    --disable-grub-mkstandalone --disable-grub-editenv --disable-grub-fstest \
    --disable-grub-set-default --disable-grub-reboot --disable-grub-sparsemodule \
    --disable-grub-probe --disable-grub-editenv --without-grub-mkfont \
    --without-grub-mkimage --without-grub-mkconfig --without-grub-mkrelpath \
    --without-grub-mkrescue --without-grub-mkstandalone --without-grub-editenv \
    --without-grub-fstest --without-grub-set-default --without-grub-reboot \
    --without-grub-sparsemodule --without-grub-probe --disable-nls >/dev/null 2>&1 || true

# Build just the bfrus module
make -C grub-core fs/bfrus.mod 2>/dev/null || true

# Copy the built module
if [ -f "$GRUB_SRC/grub-core/fs/bfrus.mod" ]; then
    cp "$GRUB_SRC/grub-core/fs/bfrus.mod" "$OUT_DIR/bfrus.mod"
    echo "Built $OUT_DIR/bfrus.mod"
else
    echo "Warning: bfrus.mod was not built. Check GRUB build output above."
    exit 1
fi

echo "GRUB BFRUS module built successfully."
