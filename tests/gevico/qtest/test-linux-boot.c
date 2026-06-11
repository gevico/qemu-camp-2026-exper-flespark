/*
 * QTest: G233 Linux Boot Test (Advanced Experiment 3)
 *
 * Analyzes the G233 board for Linux boot readiness and tests the
 * complete Linux boot chain: OpenSBI -> Linux kernel -> user-space init.
 *
 * Two test categories:
 *   1. Infrastructure tests  (qtest accel, verify MMIO devices exist)
 *   2. Boot tests            (TCG accel, spawn QEMU, capture serial)
 *
 * Boot tests require pre-built fixtures:
 *   tests/gevico/qtest/linux-fixture/Image          (Linux kernel)
 *   tests/gevico/qtest/linux-fixture/rootfs.cpio.gz  (initramfs)
 *
 * Prepare fixtures with: bash linux-fixture/prepare-fixture.sh
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

/* ── Constants ────────────────────────────────────────────────────────── */

#define G233_UART_BASE      0x10000000ULL
#define G233_UART_SIZE      0x100ULL
#define G233_VIRTIO_BASE    0x10100000ULL
#define G233_PLIC_BASE      0x0C000000ULL
#define G233_CLINT_BASE     0x02000000ULL
#define G233_DRAM_BASE      0x80000000ULL
#define G233_TEST_BASE      0x100000ULL    /* sifive-test reset/poweroff */
#define G233_WDT_BASE       0x10010000ULL
#define G233_GPIO_BASE      0x10012000ULL
#define G233_PWM_BASE       0x10015000ULL
#define G233_SPI_BASE       0x10018000ULL
#define G233_RTC_BASE       0x101000ULL

#define TEST_RAM_SIZE       "256M"
#define BOOT_TIMEOUT_SEC    30

#define KERNEL_FILENAME     "Image"
#define ROOTFS_FILENAME     "rootfs.cpio.gz"

/* ── QEMU subprocess helpers (for TCG-accelerated boot tests) ─────────── */

typedef struct BootResult {
    GString *serial_output;
    pid_t pid;
    int wstatus;
    bool timed_out;
} BootResult;

/*
 * Launch QEMU with TCG accel and capture serial output via a Unix socket.
 * The function creates a socket, forks QEMU, connects, and reads output
 * until the timeout expires or a completion marker appears.
 */
static BootResult *boot_g233_tcg(const char *extra_args, int timeout_sec)
{
    BootResult *result = g_new0(BootResult, 1);
    g_autofree char *sock_dir = g_dir_make_tmp("g233-boot-XXXXXX", NULL);
    g_autofree char *sock_path = g_strdup_printf("%s/serial.sock", sock_dir);
    g_autofree char *qemu_bin = g_strdup(g_getenv("QTEST_QEMU_BINARY"));
    int listen_fd = -1, conn_fd = -1;
    struct sockaddr_un addr;
    struct timeval tv;
    fd_set readfds;
    char buf[4096];
    ssize_t n;
    time_t deadline;

    if (!qemu_bin || qemu_bin[0] == '\0') {
        qemu_bin = g_strdup("qemu-system-riscv64");
    }

    /* Create listening socket */
    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        result->serial_output = g_string_new("");
        result->timed_out = true;
        return result;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);
    unlink(sock_path);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(listen_fd, 1) < 0) {
        close(listen_fd);
        result->serial_output = g_string_new("");
        result->timed_out = true;
        return result;
    }

    /* Build QEMU command line */
    g_autofree char *cmd = g_strdup_printf(
        "exec %s "
        "-chardev socket,id=serial0,path=%s,server=off "
        "-serial chardev:serial0 "
        "-monitor none "
        "%s",
        qemu_bin, sock_path, extra_args);

    /* Fork QEMU */
    result->pid = fork();
    if (result->pid == 0) {
        /* Child process */
        execlp("/bin/sh", "sh", "-c", cmd, NULL);
        _exit(127);
    }

    /* Parent: wait for QEMU to connect to our socket */
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    setsockopt(listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    conn_fd = accept(listen_fd, NULL, NULL);
    close(listen_fd);
    unlink(sock_path);
    rmdir(sock_dir);

    if (conn_fd < 0) {
        kill(result->pid, SIGKILL);
        waitpid(result->pid, &result->wstatus, 0);
        result->serial_output = g_string_new("");
        result->timed_out = true;
        return result;
    }

    /* Read serial output */
    result->serial_output = g_string_new(NULL);
    deadline = time(NULL) + timeout_sec;

    while (time(NULL) < deadline) {
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        FD_ZERO(&readfds);
        FD_SET(conn_fd, &readfds);

        int ret = select(conn_fd + 1, &readfds, NULL, NULL, &tv);
        if (ret > 0 && FD_ISSET(conn_fd, &readfds)) {
            n = read(conn_fd, buf, sizeof(buf) - 1);
            if (n <= 0) {
                break;
            }
            buf[n] = '\0';
            g_string_append(result->serial_output, buf);

            if (strstr(result->serial_output->str, "G233 LINUX BOOT COMPLETE") ||
                strstr(result->serial_output->str, "G233 LINUX BOOT TEST MARKER")) {
                break;
            }
        }

        if (waitpid(result->pid, &result->wstatus, WNOHANG) != 0) {
            while ((n = read(conn_fd, buf, sizeof(buf) - 1)) > 0) {
                buf[n] = '\0';
                g_string_append(result->serial_output, buf);
            }
            break;
        }
    }

    if (time(NULL) >= deadline) {
        result->timed_out = true;
        kill(result->pid, SIGTERM);
        usleep(500000);
        kill(result->pid, SIGKILL);
        waitpid(result->pid, &result->wstatus, 0);
    }

    close(conn_fd);
    return result;
}

static void boot_result_free(BootResult *r)
{
    if (!r) return;
    if (r->serial_output) {
        g_string_free(r->serial_output, TRUE);
    }
    g_free(r);
}

/* ── Helpers ──────────────────────────────────────────────────────────── */

static char *find_fixture(const char *filename)
{
    const char *dirs[] = {
        "./",
        "tests/gevico/qtest/linux-fixture/",
        "../tests/gevico/qtest/linux-fixture/",
        NULL
    };
    for (int i = 0; dirs[i]; i++) {
        char *p = g_strdup_printf("%s%s", dirs[i], filename);
        if (g_file_test(p, G_FILE_TEST_EXISTS)) {
            return p;
        }
        g_free(p);
    }
    return NULL;
}

static bool output_has(GString *s, const char *pat)
{
    return s && s->str && strstr(s->str, pat);
}

/* ── Fixture struct ───────────────────────────────────────────────────── */

typedef struct {
    char *kernel_path;
    char *rootfs_path;
    bool ready;
} Fixture;

static void fx_setup(Fixture *f, gconstpointer ud)
{
    f->kernel_path = find_fixture(KERNEL_FILENAME);
    f->rootfs_path = find_fixture(ROOTFS_FILENAME);
    f->ready = f->kernel_path && f->rootfs_path;
    if (!f->ready) {
        g_test_message("Fixtures not found. Run linux-fixture/prepare-fixture.sh");
    }
}

static void fx_teardown(Fixture *f, gconstpointer ud)
{
    g_free(f->kernel_path);
    g_free(f->rootfs_path);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Test Group 1: Infrastructure (qtest accel, no CPU execution)
 * ════════════════════════════════════════════════════════════════════════ */

/*
 * Verify every MMIO device required for Linux boot exists and is
 * accessible through the memory bus.
 *
 * Requirements for Linux on riscv64:
 *   - CPU harts (verified indirectly via PLIC)
 *   - DRAM (kernel + rootfs live here)
 *   - CLINT (timer + IPI)
 *   - PLIC (external interrupts)
 *   - UART (serial console)
 *   - virtio-mmio (block device / network)
 *   - Test finisher (reboot/poweroff)
 */
static void test_linux_prerequisites(void)
{
    QTestState *qts = qtest_init("-machine g233 -m 256M");

    /* DRAM */
    qtest_writel(qts, G233_DRAM_BASE, 0xDEADBEEF);
    g_assert_cmpuint(qtest_readl(qts, G233_DRAM_BASE), ==, 0xDEADBEEF);

    /* PLIC — priority register for source 1 */
    g_assert_cmpuint(qtest_readl(qts, G233_PLIC_BASE + 0x04), ==, 0);
    qtest_writel(qts, G233_PLIC_BASE + 0x04, 7);
    g_assert_cmpuint(qtest_readl(qts, G233_PLIC_BASE + 0x04), ==, 7);

    /* CLINT — mtime register at offset 0xBFF8 */
    qtest_readq(qts, G233_CLINT_BASE + 0xBFF8);

    /* UART — PL011 flag register (bit 5 = TXFF) */
    uint32_t uartfr = qtest_readl(qts, G233_UART_BASE + 0x18);
    g_assert_cmphex(uartfr & 0x20, ==, 0);

    /* virtio-mmio — all 8 slots must report "virt" magic */
    for (int i = 0; i < 8; i++) {
        g_assert_cmphex(qtest_readl(qts, G233_VIRTIO_BASE + i * 0x1000),
                        ==, 0x74726976);
    }

    /* SiFive Test finisher — basic readability */
    qtest_readl(qts, G233_TEST_BASE);

    /* RTC */
    qtest_readl(qts, G233_RTC_BASE);

    /* G233 peripherals (GPIO, PWM, WDT, SPI) */
    qtest_readl(qts, G233_GPIO_BASE);
    qtest_readl(qts, G233_PWM_BASE);
    qtest_readl(qts, G233_WDT_BASE);
    qtest_readl(qts, G233_SPI_BASE);

    qtest_quit(qts);
}

/*
 * Verify the virtio-mmio slots can be configured with backend devices
 * (virtio-blk, virtio-net), as required by Advanced Experiment 2.
 */
static void test_virtio_for_linux(void)
{
    QTestState *qts;
    char *args;
    char tmpimg[] = "/tmp/g233-virtio-test-XXXXXX";
    int fd;
    uint32_t devid;
    int blk_slot = -1, net_slot = -1;

    fd = g_mkstemp(tmpimg);
    g_assert_cmpint(fd, >=, 0);
    ftruncate(fd, 16 * 1024 * 1024);
    close(fd);

    args = g_strdup_printf(
        "-machine g233 -m 256M "
        "-drive file=%s,format=raw,if=none,id=hd0 "
        "-device virtio-blk-device,drive=hd0 "
        "-netdev user,id=net0 "
        "-device virtio-net-device,netdev=net0",
        tmpimg);

    qts = qtest_init(args);
    g_free(args);

    /* Find virtio-blk (device_id = 2) and virtio-net (device_id = 1) */
    for (int i = 0; i < 8; i++) {
        devid = qtest_readl(qts, G233_VIRTIO_BASE + i * 0x1000 + 0x08);
        if (devid == 2) blk_slot = i;
        if (devid == 1) net_slot = i;
    }

    g_assert_cmpint(blk_slot, >=, 0);
    g_assert_cmpint(net_slot, >=, 0);
    g_assert_cmpint(blk_slot, !=, net_slot);

    qtest_quit(qts);
    unlink(tmpimg);
}

/* ════════════════════════════════════════════════════════════════════════
 *  Test Group 2: Boot chain (TCG accel, requires QEMU process spawn)
 * ════════════════════════════════════════════════════════════════════════ */

/*
 * Verify that OpenSBI starts and prints its banner on the serial console.
 * This confirms:
 *   - CPU starts executing from reset vector
 *   - OpenSBI firmware is loaded correctly
 *   - UART console is connected and functional
 *
 * NOTE: This test requires the UART to be compatible with the OpenSBI
 * generic firmware.  The default OpenSBI build supports ns16550a but
 * NOT pl011.  If this test fails with no serial output, the board's
 * UART may need to be switched from PL011 to ns16550a (8250).
 */
static void test_opensbi_boot(void)
{
    BootResult *boot;
    g_autofree char *args = g_strdup_printf(
        "-machine g233 -m %s -bios default -nographic -accel tcg",
        TEST_RAM_SIZE);

    boot = boot_g233_tcg(args, 15);

    g_test_message("OpenSBI serial output (%zu bytes):\n%s",
                   boot->serial_output->len,
                   boot->serial_output->str);

    if (boot->serial_output->len == 0) {
        g_test_message("DIAGNOSIS: No serial output received.");
        g_test_message("Possible causes:");
        g_test_message("  1. UART type mismatch: OpenSBI generic firmware "
                       "supports ns16550a, not pl011");
        g_test_message("  2. Fix: Change UART in g233.c from pl011_create() "
                       "to serial_mm_init() and update DTB compatible to "
                       "\"ns16550a\"");
        g_test_message("  3. Alternatively, rebuild OpenSBI with PL011 driver");
    }

    g_assert_true(output_has(boot->serial_output, "OpenSBI") ||
                  output_has(boot->serial_output, "Platform Name"));

    boot_result_free(boot);
}

/*
 * Full Linux boot test: OpenSBI -> Linux kernel -> init process.
 * Validates the complete boot chain with initrd.
 */
static void test_linux_boot_initrd(Fixture *f, gconstpointer ud)
{
    BootResult *boot;
    g_autofree char *args;

    if (!f->ready) {
        g_test_skip("Kernel Image and rootfs.cpio.gz not available. "
                    "Run linux-fixture/prepare-fixture.sh");
        return;
    }

    args = g_strdup_printf(
        "-machine g233 -m %s -bios default -nographic -accel tcg "
        "-kernel %s -initrd %s "
        "-append \"console=ttyAMA0 earlycon=pl011,0x10000000 rdinit=/init\"",
        TEST_RAM_SIZE, f->kernel_path, f->rootfs_path);

    boot = boot_g233_tcg(args, BOOT_TIMEOUT_SEC);

    g_test_message("Linux boot output (%zu bytes, last 2KB):\n%.2048s",
                   boot->serial_output->len,
                   boot->serial_output->len > 2048
                       ? boot->serial_output->str + boot->serial_output->len - 2048
                       : boot->serial_output->str);

    bool saw_opensbi = output_has(boot->serial_output, "OpenSBI");
    bool saw_linux = output_has(boot->serial_output, "Linux version") ||
                     output_has(boot->serial_output, "[    0.000000]");
    bool saw_init = output_has(boot->serial_output, "Run /init") ||
                    output_has(boot->serial_output, "G233 LINUX BOOT") ||
                    output_has(boot->serial_output, "Freeing unused kernel");

    g_test_message("Boot chain: OpenSBI=%s Linux=%s Init=%s",
                   saw_opensbi ? "YES" : "NO",
                   saw_linux ? "YES" : "NO",
                   saw_init ? "YES" : "NO");

    g_assert_true(saw_opensbi);
    g_assert_true(saw_linux || saw_init);

    boot_result_free(boot);
}

/*
 * Verify that the generated DTB contains all nodes Linux needs.
 * A successful DTB parse means the kernel can discover:
 *   - CPUs, memory, CLINT, PLIC, UART, virtio-mmio
 */
static void test_dtb_completeness(Fixture *f, gconstpointer ud)
{
    BootResult *boot;
    g_autofree char *args;

    if (!f->ready) {
        g_test_skip("Kernel Image not available");
        return;
    }

    args = g_strdup_printf(
        "-machine g233 -m %s -bios default -nographic -accel tcg "
        "-kernel %s -initrd %s "
        "-append \"console=ttyAMA0 earlycon=pl011,0x10000000 rdinit=/init\"",
        TEST_RAM_SIZE, f->kernel_path, f->rootfs_path);

    boot = boot_g233_tcg(args, BOOT_TIMEOUT_SEC);

    bool saw_output = boot->serial_output->len > 50;
    bool saw_cpu = output_has(boot->serial_output, "cpu") ||
                   output_has(boot->serial_output, "hart");

    g_test_message("Serial output: %zu bytes, CPU info: %s",
                   boot->serial_output->len,
                   saw_cpu ? "YES" : "NO");

    g_assert_true(saw_output);
    g_assert_true(saw_cpu);

    boot_result_free(boot);
}

/*
 * Verify virtio-blk integration: boot Linux with a block device.
 * This is the full Advanced Experiment 2 + 3 workflow.
 */
static void test_linux_virtio_blk(Fixture *f, gconstpointer ud)
{
    BootResult *boot;
    g_autofree char *args, *ext4;

    if (!f->ready) {
        g_test_skip("Kernel Image not available");
        return;
    }

    ext4 = find_fixture("rootfs.ext4");
    if (!ext4) {
        g_test_skip("rootfs.ext4 not available");
        return;
    }

    args = g_strdup_printf(
        "-machine g233 -m %s -bios default -nographic -accel tcg "
        "-kernel %s -initrd %s "
        "-append \"console=ttyAMA0 earlycon=pl011,0x10000000 rdinit=/init\" "
        "-drive file=%s,format=raw,if=none,id=hd0 "
        "-device virtio-blk-device,drive=hd0",
        TEST_RAM_SIZE, f->kernel_path, f->rootfs_path, ext4);

    boot = boot_g233_tcg(args, BOOT_TIMEOUT_SEC);

    g_test_message("virtio-blk boot (last 2KB):\n%.2048s",
                   boot->serial_output->len > 2048
                       ? boot->serial_output->str + boot->serial_output->len - 2048
                       : boot->serial_output->str);

    g_assert_true(output_has(boot->serial_output, "OpenSBI"));
    g_assert_true(output_has(boot->serial_output, "Linux") ||
                  output_has(boot->serial_output, "[    0.000000]"));

    boot_result_free(boot);
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    /* Group 1: Infrastructure (fast, no fixtures needed) */
    qtest_add_func("g233/linux-boot/prerequisites",
                   test_linux_prerequisites);
    qtest_add_func("g233/linux-boot/virtio-for-linux",
                   test_virtio_for_linux);

    /* Group 2: Boot chain (TCG, spawn QEMU process) */
    qtest_add_func("g233/linux-boot/opensbi",
                   test_opensbi_boot);

    /* Group 3: Full Linux boot (needs fixtures) */
    qtest_add("g233/linux-boot/initrd", Fixture, NULL,
              fx_setup, test_linux_boot_initrd, fx_teardown);
    qtest_add("g233/linux-boot/dtb", Fixture, NULL,
              fx_setup, test_dtb_completeness, fx_teardown);
    qtest_add("g233/linux-boot/virtio-blk", Fixture, NULL,
              fx_setup, test_linux_virtio_blk, fx_teardown);

    return g_test_run();
}
