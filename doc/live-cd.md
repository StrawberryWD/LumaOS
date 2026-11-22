# LumaOS live CD (Day 59-63)

```
BIOS ──El Torito No-Emulation──▶ cdboot.bin (0x7C00, 2048 B)         Day 59
        │  reads KERNEL.BIN by the Boot Info Table (LBA/len), 16 sectors per INT 13h AH=42h
        ▼
     kernel @0x10000  ◀── boot info @0x600: "LBI1", drive, media=1, kernel/LMFS LBA+len, ISO sectors
        │  kmain: boot_is_cd() → skip the hard-disk self-tests                                  Day 61
        ▼
     live_init():                                                                               Day 62/63
        ATAPI IDENTIFY PACKET on 4 IDE positions → the drive whose LBA 16 PVD matches the boot info
        ISO 9660 mount  → CD0  (read only)   → FONTS/SYS8X8.FNT, ETC/LUMAOS.CFG
        BOOT/LMFS.IMG   → RAM 0x600000 (ramdisk) → LMFS mount → RAM0 (writable, current root)
        GUI up → live_apply_config(): resolution, theme, desktop icons
```

ISO layout (tools/mkiso.py, built by build.sh): `BOOT/CDBOOT.BIN`, `BOOT/KERNEL.BIN`, `BOOT/LMFS.IMG`,
`FONTS/SYS8X8.FNT`, `ETC/LUMAOS.CFG`.

Run it: `qemu-system-i386 -cdrom lumaos-5.0.iso -boot d` (no hard disk needed). Shell: `mounts`, `cdinfo`,
`config`, `font`, `ls CD0/ETC`, `read CD0/ETC/LUMAOS.CFG`, `run hello.sw`.
