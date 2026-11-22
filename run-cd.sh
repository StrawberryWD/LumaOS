#!/usr/bin/env bash
# run-cd.sh - boot the LIVE CD (Day 59-63): no hard disk, just lumaos-5.0.iso
#   ./run-cd.sh            GUI (BGA)        ./run-cd.sh vmware   SVGA 32bpp display
#   ./run-cd.sh --hd       also attach lumaos-5.0.img as a hard disk (still boots from the CD)
# Build the ISO first with ./build.sh (it writes lumaos-5.0.iso next to the .img).
set -e
cd "$(dirname "$0")"
VGA=std; HD=""
for a in "$@"; do
    case "$a" in vmware) VGA=vmware;; --hd) HD="-drive format=raw,file=lumaos-5.0.img,if=ide,index=0";; esac
done
[ -f lumaos-5.0.iso ] || { echo "lumaos-5.0.iso not found - run ./build.sh first"; exit 1; }
exec qemu-system-i386 -m 256 -vga "$VGA" $HD \
    -drive format=raw,file=lumaos-5.0.iso,if=ide,media=cdrom,index=2 -boot d
