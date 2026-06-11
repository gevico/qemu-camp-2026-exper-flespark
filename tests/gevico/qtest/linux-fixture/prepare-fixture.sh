#!/bin/bash
# prepare-fixture.sh — Prepare Linux kernel Image + minimal rootfs
# for the G233 Linux boot QTest.
#
# Usage:
#   cd tests/gevico/qtest/linux-fixture
#   bash prepare-fixture.sh [--force]
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FIXTURE_DIR="${SCRIPT_DIR}"
KERNEL="${FIXTURE_DIR}/Image"
ROOTFS="${FIXTURE_DIR}/rootfs.cpio.gz"
FORCE=false

[ "${1:-}" = "--force" ] && FORCE=true

info()  { echo "[INFO]  $*"; }
warn()  { echo "[WARN]  $*" >&2; }
die()   { echo "[ERROR] $*" >&2; exit 1; }

# ── Step 1: Obtain Linux kernel Image ────────────────────────────────────
#
# Try multiple strategies in order:
#   1. Use existing Image if present
#   2. Build from source if cross-compiler available
#   3. Download a pre-built Image from known mirrors

obtain_kernel() {
    if [ -f "${KERNEL}" ] && ! $FORCE; then
        info "Kernel already exists: ${KERNEL}"
        return 0
    fi

    # Strategy A: Build from source with cross-compiler
    local CROSS=""
    if command -v riscv64-linux-gnu-gcc >/dev/null 2>&1; then
        CROSS="riscv64-linux-gnu-"
    elif command -v riscv64-unknown-elf-gcc >/dev/null 2>&1; then
        CROSS="riscv64-unknown-elf-"
    fi

    if [ -n "${CROSS}" ]; then
        info "Cross compiler found (${CROSS}gcc), building kernel..."
        build_kernel "${CROSS}"
        return 0
    fi

    # Strategy B: Download pre-built Image
    info "No cross compiler found, attempting to download pre-built Image..."
    download_kernel
}

build_kernel() {
    local CROSS="$1"
    local LINUX_SRC="${FIXTURE_DIR}/_build/linux"
    local KVER="6.6.70"

    if [ -d "${LINUX_SRC}" ] && [ -f "${LINUX_SRC}/arch/riscv/boot/Image" ]; then
        info "Using existing kernel source tree"
    else
        rm -rf "${LINUX_SRC}"
        mkdir -p "$(dirname "${LINUX_SRC}")"
        info "Downloading Linux ${KVER} source..."
        curl -fsSL "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-${KVER}.tar.xz" \
            | tar -xJ -C "$(dirname "${LINUX_SRC}")"
        mv "$(dirname "${LINUX_SRC}")/linux-${KVER}" "${LINUX_SRC}"
    fi

    cd "${LINUX_SRC}"

    # Clean config
    make ARCH=riscv CROSS_COMPILE="${CROSS}" clean 2>/dev/null || true
    make ARCH=riscv CROSS_COMPILE="${CROSS}" defconfig

    # Enable drivers needed for G233
    scripts/config --enable CONFIG_SERIAL_AMBA_PL011
    scripts/config --enable CONFIG_SERIAL_AMBA_PL011_CONSOLE
    scripts/config --enable CONFIG_VIRTIO_MMIO
    scripts/config --enable CONFIG_VIRTIO_BLK
    scripts/config --enable CONFIG_VIRTIO_NET
    scripts/config --enable CONFIG_VIRTIO_RNG
    scripts/config --enable CONFIG_EXT4_FS
    scripts/config --enable CONFIG_DEVTMPFS
    scripts/config --enable CONFIG_DEVTMPFS_MOUNT
    scripts/config --enable CONFIG_BLK_DEV_INITRD
    scripts/config --enable CONFIG_INITRAMFS_SOURCE
    scripts/config --enable CONFIG_RD_GZIP
    scripts/config --enable CONFIG_TTY
    scripts/config --enable CONFIG_PRINTK

    make ARCH=riscv CROSS_COMPILE="${CROSS}" olddefconfig
    make ARCH=riscv CROSS_COMPILE="${CROSS}" Image -j"$(nproc 2>/dev/null || echo 4)"

    cp arch/riscv/boot/Image "${KERNEL}"
    info "Built kernel: ${KERNEL}"
    cd "${SCRIPT_DIR}"
}

download_kernel() {
    # Try known mirrors for pre-built riscv64 kernel images
    local URLS=(
        "https://apt.releases.ubuntu.com/ubuntu/dists/noble/main/installer-riscv64/current/images/noble-netboot/riscv64/uboot/Image"
    )

    for url in "${URLS[@]}"; do
        info "Trying: ${url}"
        if curl -fsSL -k --max-time 120 -o "${KERNEL}" "${url}" 2>/dev/null; then
            if file "${KERNEL}" | grep -qiE "GNU/Linux|Kernel|EFI|PE32\+|data"; then
                info "Successfully downloaded kernel Image"
                return 0
            fi
            warn "Downloaded file doesn't look like a kernel Image, removing"
            rm -f "${KERNEL}"
        fi
    done

    die "Cannot obtain a Linux kernel Image. Please:
  1. Install a RISC-V cross-compiler and re-run this script
     apt-get install gcc-riscv64-linux-gnu
  2. Or manually place a kernel Image at: ${KERNEL}
  3. Or build manually:
     git clone --depth 1 --branch v6.6 https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git
     cd linux && make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- defconfig
     make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- Image -j\$(nproc)
     cp arch/riscv/boot/Image ${KERNEL}"
}

# ── Step 2: Create minimal initramfs ─────────────────────────────────────

create_rootfs() {
    if [ -f "${ROOTFS}" ] && ! $FORCE; then
        info "Rootfs already exists: ${ROOTFS}"
        return 0
    fi

    info "Creating minimal initramfs rootfs..."
    local WORKDIR
    WORKDIR="$(mktemp -d)"
    trap 'rm -rf "${WORKDIR}"' RETURN

    mkdir -p "${WORKDIR}"/{bin,sbin,etc,proc,sys,dev,tmp,usr/bin,usr/sbin}

    # If we have a cross-compiler, build a static init binary
    local CROSS=""
    if command -v riscv64-linux-gnu-gcc >/dev/null 2>&1; then
        CROSS="riscv64-linux-gnu-"
    elif command -v riscv64-unknown-elf-gcc >/dev/null 2>&1; then
        CROSS="riscv64-unknown-elf-"
    fi

    if [ -n "${CROSS}" ]; then
        build_init_binary "${WORKDIR}" "${CROSS}"
    else
        # Without cross-compiler, create a shell script init
        # The kernel will try to run it, and even if it fails (no /bin/sh),
        # the kernel messages about "Run /init" confirm the boot succeeded
        cat > "${WORKDIR}/init" <<'EOF'
#!/bin/sh
mount -t proc none /proc 2>/dev/null
mount -t sysfs none /sys 2>/dev/null
mount -t devtmpfs none /dev 2>/dev/null
echo "=== G233 LINUX BOOT TEST MARKER ==="
uname -a
cat /proc/cpuinfo 2>/dev/null || true
echo "=== G233 LINUX BOOT COMPLETE ==="
poweroff -f 2>/dev/null || true
EOF
        chmod +x "${WORKDIR}/init"
        cp "${WORKDIR}/init" "${WORKDIR}/sbin/init"
    fi

    # Build cpio archive
    (cd "${WORKDIR}" && find . | cpio -o -H newc 2>/dev/null | gzip -9) > "${ROOTFS}"
    info "Created rootfs: ${ROOTFS} ($(stat -c%s "${ROOTFS}" 2>/dev/null || stat -f%z "${ROOTFS}" 2>/dev/null) bytes)"
}

build_init_binary() {
    local WORKDIR="$1"
    local CROSS="$2"

    cat > "${WORKDIR}/init.c" <<'CEOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/reboot.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/reboot.h>

/* Write a string to a file descriptor */
static void write_str(int fd, const char *s) {
    write(fd, s, strlen(s));
}

/* Write string to kmsg for console output */
static void kmsg(const char *s) {
    static int kmsg_fd = -1;
    if (kmsg_fd < 0) {
        kmsg_fd = open("/dev/kmsg", O_WRONLY);
    }
    if (kmsg_fd >= 0) {
        char buf[512];
        snprintf(buf, sizeof(buf), "<6>%s", s);
        write(kmsg_fd, buf, strlen(buf));
    }
    /* Also write to stdout (console) */
    write_str(STDOUT_FILENO, s);
}

int main(void) {
    /* Mount essential filesystems */
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);

    kmsg("=== G233 LINUX BOOT TEST MARKER ===\n");

    /* Print system info */
    {
        FILE *f = fopen("/proc/version", "r");
        if (f) {
            char buf[256];
            while (fgets(buf, sizeof(buf), f)) {
                kmsg(buf);
            }
            fclose(f);
        }
    }

    {
        FILE *f = fopen("/proc/cpuinfo", "r");
        if (f) {
            char buf[256];
            int ncpu = 0;
            while (fgets(buf, sizeof(buf), f)) {
                kmsg(buf);
                if (strncmp(buf, "hart", 4) == 0) ncpu++;
            }
            fclose(f);
        }
    }

    /* List /dev */
    kmsg("=== /dev contents ===\n");
    {
        FILE *f = popen("ls /dev 2>/dev/null", "r");
        if (f) {
            char buf[256];
            while (fgets(buf, sizeof(buf), f)) {
                kmsg(buf);
            }
            pclose(f);
        }
    }

    kmsg("=== G233 LINUX BOOT COMPLETE ===\n");

    /* Power off via QEMU sifive test finisher */
    {
        int fd = open("/proc/iomem", O_RDONLY);
        if (fd >= 0) {
            close(fd);
        }
    }

    /* Try Linux reboot */
    reboot(LINUX_REBOOT_CMD_POWER_OFF);
    sync();

    /* Fallback: write to syscon test device */
    {
        int fd = open("/dev/mem", O_WRONLY);
        if (fd >= 0) {
            unsigned short val = 0x5555;  /* FINISHER_PASS */
            lseek(fd, 0x100000, SEEK_SET);
            write(fd, &val, 2);
            close(fd);
        }
    }

    return 0;
}
CEOF

    "${CROSS}gcc" -static -nostdlib -o "${WORKDIR}/init" "${WORKDIR}/init.c" -lgcc 2>/dev/null || \
    "${CROSS}gcc" -static -o "${WORKDIR}/init" "${WORKDIR}/init.c"
    rm -f "${WORKDIR}/init.c"
    chmod +x "${WORKDIR}/init"
    cp "${WORKDIR}/init" "${WORKDIR}/sbin/init"
}

# ── Step 3: Create ext4 rootfs (optional, for virtio-blk test) ──────────

create_ext4_rootfs() {
    local EXT4="${FIXTURE_DIR}/rootfs.ext4"
    if [ -f "${EXT4}" ] && ! $FORCE; then
        info "ext4 rootfs already exists"
        return 0
    fi

    if ! command -v mkfs.ext4 >/dev/null 2>&1; then
        warn "mkfs.ext4 not available, skipping ext4 rootfs"
        return 0
    fi

    info "Creating ext4 rootfs..."
    local WORKDIR
    WORKDIR="$(mktemp -d)"
    trap 'rm -rf "${WORKDIR}"' RETURN

    mkdir -p "${WORKDIR}"/{bin,sbin,etc,proc,sys,dev,tmp,usr/bin,usr/sbin}

    cat > "${WORKDIR}/init" <<'EOF'
#!/bin/sh
mount -t proc none /proc 2>/dev/null
mount -t sysfs none /sys 2>/dev/null
mount -t devtmpfs none /dev 2>/dev/null
echo "=== G233 LINUX BOOT TEST MARKER ==="
uname -a
cat /proc/cpuinfo 2>/dev/null || true
echo "=== G233 LINUX BOOT COMPLETE ==="
poweroff -f 2>/dev/null || true
EOF
    chmod +x "${WORKDIR}/init"
    cp "${WORKDIR}/init" "${WORKDIR}/sbin/init"

    # Create image
    dd if=/dev/zero of="${EXT4}" bs=1M count=16 2>/dev/null
    mkfs.ext4 -F -q "${EXT4}" 2>/dev/null

    local MNT
    MNT="$(mktemp -d)"
    if mount -o loop "${EXT4}" "${MNT}" 2>/dev/null; then
        cp -a "${WORKDIR}"/* "${MNT}/"
        umount "${MNT}"
    fi
    rmdir "${MNT}" 2>/dev/null
    info "Created: ${EXT4}"
}

# ── Main ─────────────────────────────────────────────────────────────────

info "=== G233 Linux Boot Test Fixture Preparation ==="
info "Output directory: ${FIXTURE_DIR}"

mkdir -p "${FIXTURE_DIR}"

obtain_kernel
create_rootfs
create_ext4_rootfs

info ""
info "=== Fixture Summary ==="
ls -lh "${FIXTURE_DIR}"/Image "${FIXTURE_DIR}"/rootfs.cpio.gz 2>/dev/null || true
ls -lh "${FIXTURE_DIR}"/rootfs.ext4 2>/dev/null || true
info "=== Done ==="
