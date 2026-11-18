#!/bin/sh
# Cross-compile pnet for the PSP uClinux target (BFLT, no MMU).
#   TC=/path/to/toolchain-uclinux_on_psp-0.22  ./build.sh
# Output: ./pnet  -> copy to the memory stick as /net/pnet
# NOTE: the code must stay free of floating point (see docs/TROUBLESHOOTING.md).
set -e
: "${TC:?set TC to the directory that contains staging_dir}"
PATH="$TC/staging_dir/usr/bin:$PATH"
mipsel-linux-uclibc-gcc -Os -Wall -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -isystem "$TC/staging_dir/usr/include" \
    -Wl,-elf2flt="-s 65536" -B"$TC/staging_dir/usr/lib" -L"$TC/staging_dir/usr/lib" \
    -o pnet pnet.c wpa.c
ls -l pnet
# sanity check: FPU instructions may only appear inside libc's printf (never executed without %f)
if mipsel-linux-uclibc-objdump -d pnet.gdb | awk '/^[0-9a-f]+ <.*>:/{fn=$2} /\<(lwc1|swc1|ldc1|sdc1|mtc1|mfc1|cvt\.|add\.[sd]|mul\.[sd]|div\.[sd]|bc1)/{print fn}' | sort -u | grep -v -e _fpmaxtostr -e _ppfs_setargs -e vfprintf; then
    echo "WARNING: FPU instructions outside libc printf, this will hang the PSP" >&2
    exit 1
fi
