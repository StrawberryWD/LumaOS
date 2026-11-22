#!/usr/bin/env bash
# run.sh — LumaOS 5.0  Standard QEMU launch
#
# DAY1 (LumaOS 5.0 roadmap - Week 1, Day 1):
#   if=floppy -> if=ide
#   The image is now presented to QEMU as the primary IDE disk instead
#   of a floppy, since Week 1 introduces a real IDE PIO driver
#   (kernel/drivers/block/ide.c) that talks to ports 0x1F0-0x1F7 as
#   ATA primary channel. A floppy-attached image would never appear
#   there. MBR/loader/kernel layout on the image itself is unchanged;
#   only how QEMU exposes the backing image changes.
#
# STAGE1: Updated for BGA 800x450 graphics.
#   QEMU's default VGA device already supports the Bochs VBE interface
#   (BGA ports 0x01CE/0x01CF) when you use -device VGA or the default
#   VGA.  No extra flags are needed for BGA — QEMU exposes it
#   automatically.  The LFB will be at the PCI BAR0 of the VGA device.
#
#   IMPORTANT: We also pass -vga std (explicitly) which uses the Bochs
#   VGA model in QEMU and maps the LFB at 0xE0000000 on i386 guests.
#
#   Keys: Ctrl+Alt+G = release mouse | Ctrl+Alt+Q = quit QEMU

# DAY43 PRE-2: the kernel now carries a VMware SVGA II driver next to the
#   Bochs BGA one and picks whichever display device it finds. Choose the
#   emulated card from here:
#       ./run.sh                     -> -vga std     (Bochs BGA, 8bpp)
#       LUMAOS_VGA=vmware ./run.sh   -> -vga vmware  (VMware SVGA II, 32bpp)
#   The same lumaos-5.0.img boots on both (VMware Workstation/Player too).
# DAY43 PRE-4: shorthand  ./run.sh vmware  (32bpp)  |  ./run.sh std  (BGA 8bpp).
#   The top bar shows which one is active ("SVGA 32bpp" / "BGA 8bpp").

set -euo pipefail
VGA="${1:-${LUMAOS_VGA:-std}}"   # DAY43 PRE-4: ./run.sh vmware  == LUMAOS_VGA=vmware ./run.sh

IMG="lumaos-5.0.img"
if [ ! -f "$IMG" ]; then
    echo "Error: $IMG not found — run ./build.sh first"
    exit 1
fi

echo "=== LumaOS 5.0 — QEMU standard mode (BGA 800x450) ==="
echo "  Image  : $IMG"
echo "  Memory : 256 MB"
echo "  Disk   : IDE primary (if=ide)"
echo "  VGA    : $VGA (std = Bochs BGA 8bpp, vmware = VMware SVGA II 32bpp)"
echo "  Keys   : Ctrl+Alt+G = release mouse"
echo ""

qemu-system-i386 \
    -machine type=pc \
    -m 256 \
    -vga "$VGA" \
    -drive format=raw,file="$IMG",if=ide \
    -device intel-hda \
    -device hda-duplex \
    -serial stdio \
    -accel kvm 2>/dev/null \
    || \
qemu-system-i386 \
    -machine type=pc \
    -m 256 \
    -vga "$VGA" \
    -drive format=raw,file="$IMG",if=ide \
    -device intel-hda \
    -device hda-duplex \
    -serial stdio