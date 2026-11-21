# Galaxy S20+ LTE (SM-G985F, y2s): measured board facts

Read from one SM-G985F on 2026-11-21, with read-only commands from the phone's Linux and Android
(scripts in the S20plusLinux project: `ios/tools/board-facts.sh`, `ios/tools/gpt-read.sh`). No serial
numbers, IMEI, MAC addresses, calibration or other partitions' GUIDs are recorded here.

This phone is not stock: it dual-boots Android 16 (KernelSU) and mainline Linux (Ubuntu), and keeps TWRP
in `recovery`. The port must keep both. That is why iOS gets its own partition (`darwin`) and boots as a
third image written into `boot`, and why nothing of iOS may ever write `super`, `userdata` or `y2s-root`.

## Identity

| | |
|---|---|
| Model (`ro.boot.em.model`) | SM-G985F. **The ROM spoofs `ro.product.model`**: never identify the phone by it |
| Bootloader (`ro.boot.bootloader`) | G985FXXSNHYB1 |
| RAM | 8 GB (Linux sees 7,589,896 kB after carve-outs) |
| Storage | UFS, 4096-byte logical blocks; LUN 0 127,934,660,608 B; LUNs 1-4 4/4/8/16 MiB |

## Boot image (`boot`, sda13)

Android boot image header **v2**, page size **2048**, board **`SRPSG12A018KU`** (from the phone's own Android
boot image). `boot` is 61,865,984 B; `recovery` (TWRP) 69,009,408 B. HakiOS's legacy y2s flow packed a
65.8 MiB payload into `recovery`; it does not fit `boot`, so here root.apfs lives in `darwin`, not the image.

## Memory

Banks (identical to x1slte): `0x80000000-0xBAAFFFFF`, `0xC1200000-0xDFFFFFFF`, `0xE1900000-0xFFFFFFFF`,
`0x880000000-0x9FEBFFFFF`. Reserved (all no-map): sec_debug_magic `0x80000000`+4K, kaslr `0x80001000`+4K,
secure_camera `0x96000000`+50M, el2_code `0xC1400000`+2M, s2mpu-table `0xC1800000`+3M, seclog `0xC3000000`+512K,
cp_rmem_2 `0xD0000000`+64M, ramoops `0xEB310000`+1M, camera_rmem `0xED210000`+1M, teegris `0xED310000`+8K,
cp_rmem `0xF0000000`+8M, framebuffer `0xF1000000`, abox_pci `0xF4200000`+8M, cp_msi `0xF6200000`+4K,
wifi_msi `0xF7200000`+4K, sec_rdx_bootdev `0x880000000`+173M. The ABOX DSP memsets `0xF8000000-0xFA9FFFFF`
at every DSP boot. `xnuloader/s20_memory.h`'s CP regions match.

## Partitions on LUN 0 (4 KiB LBAs)

| Name | Node | First LBA | Bytes | Notes |
|---|---|---|---|---|
| up_param | sda4 | 13312 | 8,388,608 | |
| dtbo | sda11 | 29056 | 8,388,608 | |
| boot | sda13 | 31120 | 61,865,984 | the selected OS's image |
| recovery | sda14 | 46224 | 69,009,408 | TWRP, keep |
| misc | sda19 | 82656 | 1,048,576 | |
| vbmeta | sda23 | 95200 | 65,536 | |
| super | sda26 | 103424 | 9,437,184,000 | Android 16 — never written |
| cache | sda29 | 2671104 | 629,145,600 | |
| y2s-root | sda32 | 2850304 | 51,539,607,552 | Linux — never written |
| **darwin** | **sda34** | **15433216** | **34,359,738,368** | entry 34, type APFS `7C3457EF-0000-11AA-AA11-00306543ECAC`, unique `6E1A3C5B-2F44-4B8E-9D27-53D2A0F1C934` |
| userdata | sda33 | 23821824 | 30,360,449,024 | Android — never written |

The GPT keeps Samsung's 512-byte header and 128 entries. It must be edited with a tool that keeps the
header size (sfdisk/sgdisk rewrite it as 92 and sboot then restores the stock PIT layout). The `darwin`
entry survived an Android reboot (2026-11-21).

`darwin`'s blocks: 0..2042367 system volume + cryptexes (from the computer, `tools/darwin-write.py`),
2042368..8388607 the data volume (kernel `ufs_write=1 ufs_darwin_data=1`, `bsd/dev/redwood_ufs.c`).

## Differences from x1slte that matter

- Not a single-OS install: no `--erase`, no `super`/`userdata` use, TWRP kept. iOS boots from `boot`.
- The panel is the S20+'s S6E3HAB "Hubble2" (x1slte: Hubble1); touch is the same Y79A (`s6sy761`).
- On this phone, **probing SPI4/USI4 (fingerprint) from the normal world hard-hung the SoC** under Linux,
  and the sensor is TEEGRIS-owned. Touch ID stays off here until reviewed separately.
