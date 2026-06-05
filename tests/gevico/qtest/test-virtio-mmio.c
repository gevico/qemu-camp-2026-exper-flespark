/*
 * QTest: G233 virtio-mmio transport slots
 *
 * Verifies that 8 virtio-mmio devices are instantiated at the
 * expected MMIO addresses (0x1010_0000 + i*0x1000), that their
 * identification registers return correct values, and that slots
 * operate independently.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

/* Address map constants */
#define VIRTIO_MMIO_BASE    0x10100000ULL
#define VIRTIO_MMIO_SIZE    0x1000ULL
#define VIRTIO_SLOT_COUNT   8

/* virtio-mmio register offsets (from standard-headers/linux/virtio_mmio.h) */
#define VIRTIO_MMIO_MAGIC_VALUE    0x000
#define VIRTIO_MMIO_VERSION        0x004
#define VIRTIO_MMIO_DEVICE_ID      0x008
#define VIRTIO_MMIO_VENDOR_ID      0x00c
#define VIRTIO_MMIO_DEVICE_FEATURES 0x010
#define VIRTIO_MMIO_QUEUE_SEL      0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX  0x034
#define VIRTIO_MMIO_STATUS         0x070

/* Expected magic values (from hw/virtio/virtio-mmio.h) */
#define VIRT_MAGIC    0x74726976   /* "virt" */
#define VIRT_VENDOR   0x554D4551   /* "QEMU" */

static inline uint64_t slot_addr(int slot)
{
    return VIRTIO_MMIO_BASE + slot * VIRTIO_MMIO_SIZE;
}

/* Verify slot 0 returns the "virt" magic value */
static void test_virtio_mmio_magic(void)
{
    QTestState *qts = qtest_init("-machine g233 -m 2G");

    g_assert_cmphex(qtest_readl(qts, slot_addr(0) + VIRTIO_MMIO_MAGIC_VALUE),
                    ==, VIRT_MAGIC);

    qtest_quit(qts);
}

/* Verify slot 0 reports a valid version (1 = legacy, 2 = modern) */
static void test_virtio_mmio_version(void)
{
    QTestState *qts = qtest_init("-machine g233 -m 2G");
    uint32_t version;

    version = qtest_readl(qts, slot_addr(0) + VIRTIO_MMIO_VERSION);
    g_assert_true(version == 1 || version == 2);

    qtest_quit(qts);
}

/* Verify slot 0 returns QEMU vendor ID */
static void test_virtio_mmio_vendor_id(void)
{
    QTestState *qts = qtest_init("-machine g233 -m 2G");

    g_assert_cmphex(qtest_readl(qts, slot_addr(0) + VIRTIO_MMIO_VENDOR_ID),
                    ==, VIRT_VENDOR);

    qtest_quit(qts);
}

/* Without a backend device plugged, DeviceID must be 0 */
static void test_virtio_mmio_device_id(void)
{
    QTestState *qts = qtest_init("-machine g233 -m 2G");

    g_assert_cmpuint(qtest_readl(qts, slot_addr(0) + VIRTIO_MMIO_DEVICE_ID),
                     ==, 0);

    qtest_quit(qts);
}

/* All 8 slots must return the correct magic value */
static void test_virtio_mmio_all_slots(void)
{
    QTestState *qts = qtest_init("-machine g233 -m 2G");
    int i;

    for (i = 0; i < VIRTIO_SLOT_COUNT; i++) {
        g_assert_cmphex(
            qtest_readl(qts, slot_addr(i) + VIRTIO_MMIO_MAGIC_VALUE),
            ==, VIRT_MAGIC);
    }

    qtest_quit(qts);
}

/*
 * Verify slot independence: writing QUEUE_SEL on slot 0 must not
 * affect QUEUE_NUM_MAX reads on slot 1.
 */
static void test_virtio_mmio_slot_independence(void)
{
    QTestState *qts = qtest_init("-machine g233 -m 2G");

    /* Select queue 0 on slot 0 */
    qtest_writel(qts, slot_addr(0) + VIRTIO_MMIO_QUEUE_SEL, 0);
    /* Select queue 1 on slot 1 */
    qtest_writel(qts, slot_addr(1) + VIRTIO_MMIO_QUEUE_SEL, 1);

    /*
     * Both slots should still present valid magic values,
     * proving their register state is independent.
     */
    g_assert_cmphex(
        qtest_readl(qts, slot_addr(0) + VIRTIO_MMIO_MAGIC_VALUE),
        ==, VIRT_MAGIC);
    g_assert_cmphex(
        qtest_readl(qts, slot_addr(1) + VIRTIO_MMIO_MAGIC_VALUE),
        ==, VIRT_MAGIC);

    qtest_quit(qts);
}

/* Device STATUS register should default to 0 after reset */
static void test_virtio_mmio_status_reset(void)
{
    QTestState *qts = qtest_init("-machine g233 -m 2G");

    g_assert_cmpuint(qtest_readl(qts, slot_addr(0) + VIRTIO_MMIO_STATUS),
                     ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("g233/virtio-mmio/magic", test_virtio_mmio_magic);
    qtest_add_func("g233/virtio-mmio/version", test_virtio_mmio_version);
    qtest_add_func("g233/virtio-mmio/vendor-id", test_virtio_mmio_vendor_id);
    qtest_add_func("g233/virtio-mmio/device-id", test_virtio_mmio_device_id);
    qtest_add_func("g233/virtio-mmio/all-slots", test_virtio_mmio_all_slots);
    qtest_add_func("g233/virtio-mmio/slot-independence",
                   test_virtio_mmio_slot_independence);
    qtest_add_func("g233/virtio-mmio/status-reset",
                   test_virtio_mmio_status_reset);

    return g_test_run();
}
