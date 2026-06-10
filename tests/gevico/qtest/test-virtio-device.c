/*
 * QTest: G233 virtio-mmio device attachment (virtio-blk + virtio-net)
 *
 * Verifies that virtio-blk and virtio-net backend devices can be
 * attached to the virtio-mmio transport slots, that their DeviceID
 * registers reflect the correct device type, and that their config
 * spaces are accessible through the MMIO window.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "standard-headers/linux/virtio_ids.h"

/* Address map constants (must match g233.c memmap) */
#define VIRTIO_MMIO_BASE        0x10100000ULL
#define VIRTIO_MMIO_SIZE        0x1000ULL
#define VIRTIO_SLOT_COUNT       8

/* virtio-mmio register offsets */
#define VIRTIO_MMIO_MAGIC_VALUE     0x000
#define VIRTIO_MMIO_VERSION         0x004
#define VIRTIO_MMIO_DEVICE_ID       0x008
#define VIRTIO_MMIO_VENDOR_ID       0x00c
#define VIRTIO_MMIO_DEVICE_FEATURES 0x010
#define VIRTIO_MMIO_QUEUE_SEL       0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX   0x034
#define VIRTIO_MMIO_STATUS          0x070
#define VIRTIO_MMIO_CONFIG          0x100

/* Expected identification values */
#define VIRT_MAGIC    0x74726976   /* "virt" */
#define VIRT_VENDOR   0x554D4551   /* "QEMU" */

/* Test disk image size: 16 MB */
#define TEST_IMAGE_SIZE  (16 * 1024 * 1024)

static inline uint64_t slot_addr(int slot)
{
    return VIRTIO_MMIO_BASE + slot * VIRTIO_MMIO_SIZE;
}

/* QEMU args for a machine with virtio-blk + virtio-net attached */
static char test_image_path[PATH_MAX];

static void create_test_image(void)
{
    int fd, ret;

    snprintf(test_image_path, sizeof(test_image_path),
             "%s/g233-virtio-blk-XXXXXX", g_get_tmp_dir());

    fd = g_mkstemp(test_image_path);
    g_assert_cmpint(fd, >=, 0);
    ret = ftruncate(fd, TEST_IMAGE_SIZE);
    g_assert_cmpint(ret, ==, 0);
    close(fd);
}

static void remove_test_image(void)
{
    unlink(test_image_path);
}

static QTestState *qts_with_devices(void)
{
    char *args;

    args = g_strdup_printf(
        "-machine g233 -m 256 "
        "-drive file=%s,format=raw,if=none,id=hd0 "
        "-device virtio-blk-device,drive=hd0 "
        "-netdev user,id=net0 "
        "-device virtio-net-device,netdev=net0",
        test_image_path);

    return qtest_init(args);
}

/*
 * Scan all virtio-mmio slots and return the slot index that has the
 * given device_id, or -1 if not found.
 */
static int find_device_slot(QTestState *qts, uint32_t device_id)
{
    int i;

    for (i = 0; i < VIRTIO_SLOT_COUNT; i++) {
        if (qtest_readl(qts, slot_addr(i) + VIRTIO_MMIO_DEVICE_ID)
            == device_id) {
            return i;
        }
    }
    return -1;
}

/* ---- Test cases ---- */

/* virtio-blk device should appear in one of the slots */
static void test_virtio_blk_present(void)
{
    QTestState *qts = qts_with_devices();
    int slot = find_device_slot(qts, VIRTIO_ID_BLOCK);

    g_assert_cmpint(slot, >=, 0);
    g_assert_cmphex(
        qtest_readl(qts, slot_addr(slot) + VIRTIO_MMIO_MAGIC_VALUE),
        ==, VIRT_MAGIC);
    g_assert_cmphex(
        qtest_readl(qts, slot_addr(slot) + VIRTIO_MMIO_VENDOR_ID),
        ==, VIRT_VENDOR);

    qtest_quit(qts);
}

/* virtio-blk config space: capacity (first 8 bytes at CONFIG offset) */
static void test_virtio_blk_config_capacity(void)
{
    QTestState *qts = qts_with_devices();
    int slot = find_device_slot(qts, VIRTIO_ID_BLOCK);
    uint64_t capacity;

    g_assert_cmpint(slot, >=, 0);

    /*
     * The first field of virtio_blk_config is capacity (64-bit,
     * in 512-byte sectors).  Read it as two 32-bit reads.
     * For a 16 MB image: 16 * 1024 * 1024 / 512 = 0x8000 sectors.
     */
    capacity = (uint64_t)qtest_readl(qts,
        slot_addr(slot) + VIRTIO_MMIO_CONFIG + 4) << 32;
    capacity |= qtest_readl(qts, slot_addr(slot) + VIRTIO_MMIO_CONFIG);

    g_assert_cmpuint(capacity, ==, TEST_IMAGE_SIZE / 512);

    qtest_quit(qts);
}

/* virtio-blk should have at least one virtqueue */
static void test_virtio_blk_queue(void)
{
    QTestState *qts = qts_with_devices();
    int slot = find_device_slot(qts, VIRTIO_ID_BLOCK);
    uint32_t max_size;

    g_assert_cmpint(slot, >=, 0);

    /* Select queue 0 and read its max size */
    qtest_writel(qts, slot_addr(slot) + VIRTIO_MMIO_QUEUE_SEL, 0);
    max_size = qtest_readl(qts, slot_addr(slot) + VIRTIO_MMIO_QUEUE_NUM_MAX);

    g_assert_cmpuint(max_size, >, 0);

    qtest_quit(qts);
}

/* virtio-net device should appear in a different slot than virtio-blk */
static void test_virtio_net_present(void)
{
    QTestState *qts = qts_with_devices();
    int blk_slot = find_device_slot(qts, VIRTIO_ID_BLOCK);
    int net_slot = find_device_slot(qts, VIRTIO_ID_NET);

    g_assert_cmpint(net_slot, >=, 0);
    g_assert_cmpint(blk_slot, !=, net_slot);
    g_assert_cmphex(
        qtest_readl(qts, slot_addr(net_slot) + VIRTIO_MMIO_MAGIC_VALUE),
        ==, VIRT_MAGIC);

    qtest_quit(qts);
}

/* virtio-net config space: MAC address should be 6 readable bytes */
static void test_virtio_net_config_mac(void)
{
    QTestState *qts = qts_with_devices();
    int net_slot = find_device_slot(qts, VIRTIO_ID_NET);
    uint32_t mac_lo, mac_hi;
    uint8_t mac[6];

    g_assert_cmpint(net_slot, >=, 0);

    /*
     * struct virtio_net_config starts with mac[6].
     * Read bytes 0-3 and 4-5 as two 32-bit reads from config space.
     * The MAC is assigned by QEMU (user-mode networking default).
     */
    mac_lo = qtest_readl(qts, slot_addr(net_slot) + VIRTIO_MMIO_CONFIG);
    mac_hi = qtest_readl(qts, slot_addr(net_slot) + VIRTIO_MMIO_CONFIG + 4);

    mac[0] = mac_lo & 0xFF;
    mac[1] = (mac_lo >> 8) & 0xFF;
    mac[2] = (mac_lo >> 16) & 0xFF;
    mac[3] = (mac_lo >> 24) & 0xFF;
    mac[4] = mac_hi & 0xFF;
    mac[5] = (mac_hi >> 8) & 0xFF;

    /*
     * QEMU assigns a MAC from the 52:54:00:xx:xx:xx range for
     * user-mode networking.  Check the OUI prefix.
     */
    g_assert_cmpuint(mac[0], ==, 0x52);
    g_assert_cmpuint(mac[1], ==, 0x54);
    g_assert_cmpuint(mac[2], ==, 0x00);

    qtest_quit(qts);
}

/* Empty slots should still report DeviceID = 0 */
static void test_empty_slots(void)
{
    QTestState *qts = qts_with_devices();
    int i;

    for (i = 0; i < VIRTIO_SLOT_COUNT; i++) {
        uint32_t devid = qtest_readl(qts,
            slot_addr(i) + VIRTIO_MMIO_DEVICE_ID);

        if (devid == 0) {
            /* Empty slot: magic still valid */
            g_assert_cmphex(qtest_readl(qts,
                slot_addr(i) + VIRTIO_MMIO_MAGIC_VALUE), ==, VIRT_MAGIC);
        } else {
            /* Occupied slot: must be blk or net */
            g_assert_true(devid == VIRTIO_ID_BLOCK ||
                          devid == VIRTIO_ID_NET);
        }
    }

    qtest_quit(qts);
}

/* All occupied slots should have non-zero device features */
static void test_device_features(void)
{
    QTestState *qts = qts_with_devices();
    int blk_slot = find_device_slot(qts, VIRTIO_ID_BLOCK);
    int net_slot = find_device_slot(qts, VIRTIO_ID_NET);

    g_assert_cmpint(blk_slot, >=, 0);
    g_assert_cmpint(net_slot, >=, 0);

    g_assert_cmpuint(qtest_readl(qts,
        slot_addr(blk_slot) + VIRTIO_MMIO_DEVICE_FEATURES), !=, 0);
    g_assert_cmpuint(qtest_readl(qts,
        slot_addr(net_slot) + VIRTIO_MMIO_DEVICE_FEATURES), !=, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);

    create_test_image();

    qtest_add_func("g233/virtio-blk/present", test_virtio_blk_present);
    qtest_add_func("g233/virtio-blk/config-capacity",
                   test_virtio_blk_config_capacity);
    qtest_add_func("g233/virtio-blk/queue", test_virtio_blk_queue);
    qtest_add_func("g233/virtio-net/present", test_virtio_net_present);
    qtest_add_func("g233/virtio-net/config-mac", test_virtio_net_config_mac);
    qtest_add_func("g233/virtio-device/empty-slots", test_empty_slots);
    qtest_add_func("g233/virtio-device/features", test_device_features);

    ret = g_test_run();

    remove_test_image();

    return ret;
}
