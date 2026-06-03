/*
 * Copyright (c) 2018, Impinj, Inc.
 *
 * i.MX2 Watchdog IP block
 *
 * Author: Andrey Smirnov <andrew.smirnov@gmail.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/watchdog.h"
#include "migration/vmstate.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/registerfields.h"

#include "hw/watchdog/g233_wdt.h"
#include "trace.h"

#define FEED_KEY 0x5A5A5A5A
#define LOCK_KEY 0x1ACCE551

/* WDT_CTRL register fields */
REG32(WDT_CTRL, 0x00)
    FIELD(WDT_CTRL, EN, 0, 1)
    FIELD(WDT_CTRL, INTEN, 1, 1)
    FIELD(WDT_CTRL, RSTEN, 2, 1)
    FIELD(WDT_CTRL, LOCK, 3, 1)

/* WDT_LOAD register fields */
REG32(WDT_LOAD, 0x04)
    FIELD(WDT_LOAD, LOAD, 0, 32)

/* WDT_VAL register fields */
REG32(WDT_VAL, 0x08)
    FIELD(WDT_VAL, VAL, 0, 32)

/* WDT_SR register fields */
REG32(WDT_SR, 0x0C)
    FIELD(WDT_SR, TIMEOUT, 0, 1)

/* WDT_KEY register fields */
REG32(WDT_KEY, 0x10)
    FIELD(WDT_KEY, KEY, 0, 32)


static inline uint64_t g233_wdt_ns_to_ticks(G233WdtState *s, uint64_t ns)
{
    return muldiv64(ns, s->freq_hz, NANOSECONDS_PER_SECOND);
}

static inline uint64_t g233_wdt_ticks_to_ns(G233WdtState *s, uint64_t ticks)
{
    return muldiv64(ticks, NANOSECONDS_PER_SECOND, s->freq_hz);
}

static uint32_t g233_wdt_get_cntdown_val(G233WdtState *s)
{
    if (!(s->wdt_ctrl & R_WDT_CTRL_EN_MASK)) {
        return s->wdt_load;
    }

    uint64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t now_ticks = g233_wdt_ns_to_ticks(s, now_ns);
    uint64_t elapsed = now_ticks - s->tick_offset;

    if (elapsed >= s->wdt_load) {
        return 0;
    }

    return (uint32_t)(s->wdt_load - elapsed);
}

static void g233_wdt_expire(void *opaque)
{
    G233WdtState *s = G233_WDT(opaque);

    trace_g233_wdt_expire();

    s->wdt_sr |= R_WDT_SR_TIMEOUT_MASK;
    if (s->wdt_ctrl & R_WDT_CTRL_INTEN_MASK) {
        qemu_set_irq(s->irq, 1);
    }
    if (s->wdt_ctrl & R_WDT_CTRL_RSTEN_MASK) {
        watchdog_perform_action();
    }
}

static void g233_wdt_rearm_timer(G233WdtState *s)
{
    timer_del(&s->timer);

    if (!(s->wdt_ctrl & R_WDT_CTRL_EN_MASK)) {
        return;
    }

    uint64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t now_ticks = g233_wdt_ns_to_ticks(s, now_ns);
    uint64_t remaining = s->wdt_load - (now_ticks - s->tick_offset);

    if (remaining == 0) {
        remaining = s->wdt_load;
    }

    uint64_t when_ns = now_ns + g233_wdt_ticks_to_ns(s, remaining);
    timer_mod(&s->timer, when_ns);
}

static void g233_wdt_feed(G233WdtState *s)
{
    trace_g233_wdt_feed();

    s->wdt_sr &= ~R_WDT_SR_TIMEOUT_MASK;
    qemu_irq_lower(s->irq);

    uint64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->tick_offset = g233_wdt_ns_to_ticks(s, now_ns);

    g233_wdt_rearm_timer(s);
}

static void g233_wdt_reset(DeviceState *dev)
{
    G233WdtState *s = G233_WDT(dev);

    trace_g233_wdt_reset();

    s->wdt_ctrl = 0;
    s->wdt_load = 0x0000FFFF;
    s->wdt_sr = 0;
    s->tick_offset = 0;

    timer_del(&s->timer);
    qemu_irq_lower(s->irq);
}

static uint64_t g233_wdt_read(void *opaque, hwaddr addr, unsigned int size)
{
    G233WdtState *s = G233_WDT(opaque);

    uint32_t value = 0;

    switch (addr) {
    case A_WDT_CTRL:
        value = s->wdt_ctrl;
        break;
    case A_WDT_LOAD:
        value = s->wdt_load;
        break;
    case A_WDT_VAL:
        value = g233_wdt_get_cntdown_val(s);
        break;
    case A_WDT_SR:
        value = s->wdt_sr;
        break;
    case A_WDT_KEY:
        value = 0;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Bad read offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }

    trace_g233_wdt_read(addr, (uint64_t)value);

    return value;
}

static void g233_wdt_write(void *opaque, hwaddr addr,
                           uint64_t value, unsigned int size)
{
    G233WdtState *s = G233_WDT(opaque);

    trace_g233_wdt_write(addr, (uint64_t)value);

    switch (addr) {
    case A_WDT_KEY:
        if (value == FEED_KEY) {
            g233_wdt_feed(s);
        } else if (value == LOCK_KEY) {
            s->wdt_ctrl |= R_WDT_CTRL_LOCK_MASK;
        }
        break;
    case A_WDT_CTRL:
        if (s->wdt_ctrl & R_WDT_CTRL_LOCK_MASK) {
            qemu_log_mask(LOG_GUEST_ERROR, "WDT is locked\n");
            return;
        }
        s->wdt_ctrl = value & ~(R_WDT_CTRL_LOCK_MASK);
        if (value & R_WDT_CTRL_EN_MASK) {
            uint64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            s->tick_offset = g233_wdt_ns_to_ticks(s, now_ns);
            g233_wdt_rearm_timer(s);
        } else {
            timer_del(&s->timer);
        }
        break;
    case A_WDT_LOAD:
        if (s->wdt_ctrl & R_WDT_CTRL_LOCK_MASK) {
            qemu_log_mask(LOG_GUEST_ERROR, "WDT is locked\n");
            return;
        }
        s->wdt_load = value;
        if (s->wdt_ctrl & R_WDT_CTRL_EN_MASK) {
            g233_wdt_rearm_timer(s);
        }
        break;
    case A_WDT_SR:
        /* W1C: writing 1 clears the TIMEOUT bit */
        if (value & R_WDT_SR_TIMEOUT_MASK) {
            s->wdt_sr &= ~R_WDT_SR_TIMEOUT_MASK;
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Bad write offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
}

static const MemoryRegionOps g233_wdt_ops = {
    .read  = g233_wdt_read,
    .write = g233_wdt_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static const VMStateDescription vmstate_g233_wdt = {
    .name = "g233.wdt",
    .fields = (const VMStateField[]) {
        VMSTATE_TIMER(timer, G233WdtState),
        VMSTATE_UINT32(wdt_ctrl, G233WdtState),
        VMSTATE_UINT32(wdt_load, G233WdtState),
        VMSTATE_UINT32(wdt_sr, G233WdtState),
        VMSTATE_UINT64(tick_offset, G233WdtState),
        VMSTATE_END_OF_LIST()
    }
};

static void g233_wdt_realize(DeviceState *dev, Error **errp)
{
    G233WdtState *s = G233_WDT(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev),
                          &g233_wdt_ops, s,
                          TYPE_G233_WDT,
                          G233_WDT_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);
    sysbus_init_irq(sbd, &s->irq);

    timer_init_ns(&s->timer, QEMU_CLOCK_VIRTUAL, g233_wdt_expire, s);
}

static const Property g233_wdt_properties[] = {
    DEFINE_PROP_UINT64("clock-frequency", G233WdtState, freq_hz, 50000000ULL),
};

static void g233_wdt_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, g233_wdt_properties);
    dc->realize = g233_wdt_realize;
    device_class_set_legacy_reset(dc, g233_wdt_reset);
    dc->vmsd = &vmstate_g233_wdt;
    dc->desc = "G233 watchdog timer";
    set_bit(DEVICE_CATEGORY_WATCHDOG, dc->categories);
}

static const TypeInfo g233_wdt_info = {
    .name          = TYPE_G233_WDT,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(G233WdtState),
    .class_init    = g233_wdt_class_init,
};

static void g233_wdt_register_type(void)
{
    type_register_static(&g233_wdt_info);
}
type_init(g233_wdt_register_type)
