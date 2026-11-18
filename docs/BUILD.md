# Building

Everything below was done for real while preparing this repository: the kernel in step 4 was built from a pristine
copy of the upstream sources with exactly these patches, and its WLAN driver object is byte-identical to the one that
runs on our PSP.

## 0. What you need

* A Linux x86-64 machine (the PSP uClinux toolchain is a set of 32-bit x86 binaries: install the 32-bit libc, or use an
  `i386` container) and GNU make.
* The uClinux-on-PSP toolchain `toolchain-uclinux_on_psp-0.22.tar.bz2` and the loader package `uclinux_on_psp-0.22.zip`
  (both published by the *pspux* project on SourceForge). Unpack the toolchain and put `staging_dir/usr/bin` and
  `staging_dir/bin` in `PATH`.
* The kernel port: https://github.com/Krush206/PSPLinux (a 2.6.22-uc0 uClinux port for the PSP).
* A root filesystem image `rootfs.cpio`: the stock one from the 0.22 release works (its `rc.sysinit` runs
  `/ms0/auto.sh` if present, which is how our scripts start). In a built `vmlinux` the root filesystem is embedded as a
  gzip-compressed cpio archive between the symbols `__initramfs_start` and `__initramfs_end` (see `nm`); cut out the
  gzip stream (it begins with `1f 8b 08`) and `gunzip` it to get a plain `rootfs.cpio`. (We verified this on our own build.)
* A case-sensitive file system for the kernel tree (macOS users: create a case-sensitive APFS volume).

## 1. Kernel

```sh
git clone https://github.com/Krush206/PSPLinux kernel-src && cd kernel-src
patch -p1 < ../kernel/patches/0001-build-fixes-for-modern-make-and-toolchain.patch
patch -p1 < ../kernel/patches/0002-psp-wlan-driver-kconfig-makefile.patch
cp ../kernel/drivers/misc/psp_wlan_probe.c drivers/misc/

mkdir -p ../build/arch/mips/psp/ipl_sdk                 # needed once
sed "s|@ROOTFS_CPIO@|/absolute/path/to/rootfs.cpio|" ../kernel/config/psp-wlan.config > ../build/.config
export PATH=/path/to/toolchain/staging_dir/usr/bin:$PATH
yes "" | make ARCH=mips CROSS_COMPILE=mipsel-linux-uclibc- O=$PWD/../build oldconfig
make ARCH=mips CROSS_COMPILE=mipsel-linux-uclibc- O=$PWD/../build -j8 vmlinux
mipsel-linux-uclibc-objcopy -O binary -R .note -R .comment -S ../build/vmlinux x.bin
gzip -9 -c x.bin > vmlinux-wlan.bin
```

`O=` must be an absolute path. Warnings about `sysimgblt.c`, `pspfb.c` and a section mismatch in `head.o` are normal.

What the patches do: `0001` makes the tree build with a current GNU make and without the PSP SDK headers (two rules in the top
Makefile that new make versions reject, one stale `source` line in `net/Kconfig`, two linker-script files that the upstream
repository ignores through `.gitignore`, and `linux/types.h` instead of the pspsdk headers in `arch/mips/psp/ipl_sdk`). `0002`
adds the `CONFIG_PSP_WLAN_PROBE` option (the name is historical, it is the whole driver now).

## 2. pnet (userland helper)

```sh
cd tools/pnet
TC=/path/to/toolchain ./build.sh          # produces ./pnet (a BFLT executable)
```

Host-side test of the WPA2 primitives (no PSP needed; needs `openssl` for part of it):

```sh
cc -O1 -o wpa_test wpa.c wpa_test.c && python3 selftest.py ./wpa_test
```

**No floating point.** Do not add `float`/`double`, `%f`, `strtod`, `scanf("%lf")` or `math.h` to anything that runs on the PSP.
`build.sh` refuses to finish if FPU instructions show up outside libc's `printf`.

## 3. WLAN chip firmware (not included)

The chip needs two firmware images that belong to Sony. Extract them from your own PSP's firmware, see
`tools/fwextract/extract_wlanfw.py` for the steps.

## 4. wlanlog (optional, only for protocol research)

See `tools/wlanlog/README.md`.
