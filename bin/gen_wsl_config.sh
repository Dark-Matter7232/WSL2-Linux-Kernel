#!/usr/bin/env bash

BASE=$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")"/.. && pwd)
cd "${BASE}" || exit ${?}

CONFIG=arch/x86/configs/wsl2_defconfig

curl -LSso "${CONFIG}" https://github.com/microsoft/WSL2-Linux-Kernel/raw/linux-msft-wsl-4.19.y/Microsoft/config-wsl

# Initial tuning
#   * FTRACE: Limit attack surface and avoids a warning at boot.
#   * MODULES: Limit attack surface and we don't support them anyways.
#   * LTO_CLANG: Optimization.
#   * CFI_CLANG: Hardening.
#   * LOCALVERSION_AUTO: Helpful when running development builds.
#   * LOCALVERSION: Replace 'standard' with 'cbl' since this is a Clang built kernel.
#   * FRAME_WARN: The 64-bit default is 2048. Clang uses more stack space so this avoids build-time warnings.
./scripts/config \
    --file "${CONFIG}" \
    -d FTRACE \
    -d MODULES \
    -e LTO_CLANG \
    -e CFI_CLANG \
    -e LOCALVERSION_AUTO \
    --set-str LOCALVERSION "-microsoft-cbl" \
    -u FRAME_WARN

# Enable/disable a bunch of checks based on kconfig-hardened-check
# https://github.com/a13xp0p0v/kconfig-hardened-check
./scripts/config \
    --file "${CONFIG}" \
    -d AIO \
    -d DEBUG_FS \
    -d DEVMEM \
    -d HARDENED_USERCOPY_FALLBACK \
    -d KSM \
    -d LEGACY_PTYS \
    -d PROC_KCORE \
    -d VT \
    -d X86_IOPL_IOPERM \
    -e BUG_ON_DATA_CORRUPTION \
    -e DEBUG_CREDENTIALS \
    -e DEBUG_LIST \
    -e DEBUG_NOTIFIERS \
    -e DEBUG_SG \
    -e DEBUG_VIRTUAL \
    -e FORTIFY_SOURCE \
    -e HARDENED_USERCOPY \
    -e INIT_STACK_ALL \
    -e INTEGRITY \
    -e LOCK_DOWN_KERNEL_FORCE_CONFIDENTIALITY \
    -e SECURITY_LOADPIN \
    -e SECURITY_LOADPIN_ENFORCE \
    -e SECURITY_LOCKDOWN_LSM \
    -e SECURITY_LOCKDOWN_LSM_EARLY \
    -e SECURITY_SAFESETID \
    -e SECURITY_YAMA \
    -e SLAB_FREELIST_HARDENED \
    -e SLAB_FREELIST_RANDOM \
    -e SLUB_DEBUG \
    -e SHUFFLE_PAGE_ALLOCATOR \
    --set-val ARCH_MMAP_RND_BITS 32

./bin/build.sh -u
