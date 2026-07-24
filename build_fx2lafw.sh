#!/bin/bash
# Build fx2lafw firmware from sigrok-firmware-fx2lafw submodule using sdcc.
# Produces 15 .fw files in sigrok-firmware-fx2lafw/hw/*/.
#
# Usage:
#   bash build_fx2lafw.sh
#
# Prerequisites:
#   - git submodule update --init --recursive (already done by clone --recursive)
#   - sdcc 4.5.0+ (MSYS2: pacman -S mingw-w64-x86_64-sdcc)
set -e

# Resolve script directory (works regardless of CWD)
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
SRC_DIR="$SCRIPT_DIR/sigrok-firmware-fx2lafw"

if [ ! -d "$SRC_DIR" ]; then
    echo "ERROR: sigrok-firmware-fx2lafw submodule not initialized."
    echo "Run: git submodule update --init --recursive"
    exit 1
fi

if ! command -v sdcc >/dev/null 2>&1; then
    echo "ERROR: sdcc not found in PATH."
    echo "MSYS2: pacman -S mingw-w64-x86_64-sdcc"
    echo "Linux: apt install sdcc  (or dnf install sdcc)"
    echo "macOS: brew install sdcc"
    exit 1
fi

cd "$SRC_DIR"

SDCC=sdcc
SDAS=sdas8051
SDAR=sdar
MAKEBIN=makebin

AS_INC="-Iinclude"
C_INC="-Iinclude -Ifx2lib/include"
LINK_FLAGS='--code-size 0x1c00 --xram-size 0x0200 --xram-loc 0x1c00 -Wl"-b DSCR_AREA=0x1e00" -Wl"-b INT2JT=0x1f00"'
LINK_FLAGS_SCOPE='--code-size 0x3c00 --xram-size 0x0100 --xram-loc 0x3c00 -Wl"-b DSCR_AREA=0x3d00" -Wl"-b INT2JT=0x3f00"'

echo "=== Step 1: Build fx2lib objects ==="
for f in fx2lib/lib/delay.c fx2lib/lib/eputils.c fx2lib/lib/gpif.c \
         fx2lib/lib/i2c.c fx2lib/lib/serial.c fx2lib/lib/setupdat.c; do
    out="${f%.c}.rel"
    echo "  SDCC $f"
    $SDCC -mmcs51 $C_INC -c "$f" -o "$out"
done

echo "  SDAS fx2lib/lib/int4av.a51"
$SDAS -glos $AS_INC fx2lib/lib/int4av.rel fx2lib/lib/int4av.a51
echo "  SDAS fx2lib/lib/usbav.a51"
$SDAS -glos $AS_INC fx2lib/lib/usbav.rel fx2lib/lib/usbav.a51

echo "=== Step 2: Build fx2lib interrupt objects ==="
for f in fx2lib/lib/interrupts/*.c; do
    out="${f%.c}.rel"
    echo "  SDCC $f"
    $SDCC -mmcs51 $C_INC -c "$f" -o "$out"
done

echo "=== Step 3: Create fx2lib libs ==="
$SDAR -rc fx2lib/lib/fx2.lib fx2lib/lib/delay.rel fx2lib/lib/eputils.rel \
    fx2lib/lib/gpif.rel fx2lib/lib/i2c.rel fx2lib/lib/int4av.rel \
    fx2lib/lib/serial.rel fx2lib/lib/setupdat.rel fx2lib/lib/usbav.rel
$SDAR -rc fx2lib/lib/interrupts/ints.lib fx2lib/lib/interrupts/*.rel

echo "=== Step 4: Build fx2lafw core objects ==="
$SDCC -mmcs51 $C_INC -c fx2lafw.c -o fx2lafw.rel
$SDCC -mmcs51 $C_INC -c gpif-acquisition.c -o gpif-acquisition.rel

FX2LAFW_OBJS="fx2lafw.rel gpif-acquisition.rel"
FX2LIB_LIBS="fx2lib/lib/fx2.lib fx2lib/lib/interrupts/ints.lib"

build_la() {
    local hw="$1"
    local name="${hw#hw/}"
    echo "  SDAS $hw/dscr.a51"
    $SDAS -glos $AS_INC "$hw/dscr.rel" "$hw/dscr.a51"
    echo "  SDCC LINK $hw"
    eval $SDCC -mmcs51 $LINK_FLAGS -o "$hw/fx2lafw-${name}.ihx" \
        "$hw/dscr.rel" $FX2LAFW_OBJS $FX2LIB_LIBS
    echo "  MAKEBIN $hw"
    $MAKEBIN -p < "$hw/fx2lafw-${name}.ihx" > "$hw/fx2lafw-${name}.fw"
}

build_scope() {
    local hw="$1"
    local name="${hw#hw/}"
    echo "  SDAS $hw/dscr.a51"
    $SDAS -glos $AS_INC "$hw/dscr.rel" "$hw/dscr.a51"
    echo "  SDCC $hw/fw.c"
    $SDCC -mmcs51 $C_INC -c "$hw/fw.c" -o "$hw/fw.rel"
    echo "  SDCC LINK $hw"
    eval $SDCC -mmcs51 $LINK_FLAGS_SCOPE -o "$hw/fx2lafw-${name}.ihx" \
        "$hw/dscr.rel" "$hw/fw.rel" $FX2LIB_LIBS
    echo "  MAKEBIN $hw"
    $MAKEBIN -p < "$hw/fx2lafw-${name}.ihx" > "$hw/fx2lafw-${name}.fw"
}

echo "=== Step 5: Build LA firmware (9 logic analyzer devices) ==="
build_la hw/braintechnology-usb-lps
build_la hw/cwav-usbeeax
build_la hw/cwav-usbeedx
build_la hw/cwav-usbeesx
build_la hw/cwav-usbeezx
build_la hw/cypress-fx2
build_la hw/saleae-logic
build_la hw/sigrok-fx2-8ch
build_la hw/sigrok-fx2-16ch

echo "=== Step 6: Build scope firmware (6 scope devices) ==="
build_scope hw/hantek-6022be
build_scope hw/hantek-6022bl
build_scope hw/hantek-pso2020
build_scope hw/instrustar-isds205b
build_scope hw/sainsmart-dds120
build_scope hw/yixingdianzi-mdso

echo "=== Done. Generated .fw files: ==="
ls -la hw/*/fx2lafw-*.fw
