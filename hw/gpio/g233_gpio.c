/*
 * G233 GPIO Controller
 *
 * Copyright (c) 2025 Chao Liu <chao.liu@yeah.net>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Reference: G233 SoC Hardware Manual, Chapter 7 — GPIO Controller
 *
 * 32-bit register map (base 0x1001_2000):
 *   0x00  GPIO_DIR   — direction (0=input, 1=output)
 *   0x04  GPIO_OUT   — output data
 *   0x08  GPIO_IN    — input data (read-only; reflects OUT when DIR=output)
 *   0x0C  GPIO_IE    — interrupt enable
 *   0x10  GPIO_IS    — interrupt status (write-1-to-clear)
 *   0x14  GPIO_TRIG  — trigger type (0=edge, 1=level)
 *   0x18  GPIO_POL   — polarity (0=low/falling, 1=high/rising)
 *
 * All pin interrupts are OR-ed into a single PLIC IRQ line (IRQ 2).
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/gpio/g233_gpio.h"
#include "migration/vmstate.h"
#include "trace.h"

static void g233_gpio_update_irq(G233GPIOState *s)
{
    bool pending = (s->gpio_is & s->gpio_ie) != 0;
    qemu_set_irq(s->irq, pending);
}

static void g233_gpio_update_state(G233GPIOState *s)
{
    uint32_t actual_in = 0;
    uint32_t changed;
    uint32_t pin_val, prev_val;
    bool trig_level, pol_high;
    uint32_t new_is = s->gpio_is;

    for (int i = 0; i < G233_GPIO_PINS; i++) {
        uint32_t mask = 1u << i;
        bool is_output = (s->gpio_dir & mask) != 0;
        bool ext_driven = (s->in_mask & mask) != 0;

        if (is_output) {
            actual_in = deposit32(actual_in, i, 1, extract32(s->gpio_out, i, 1));
        } else if (ext_driven) {
            actual_in = deposit32(actual_in, i, 1, extract32(s->ext_in, i, 1));
        } else {
            actual_in = deposit32(actual_in, i, 1, 0);
        }
    }

    changed = actual_in ^ s->prev_in;
    s->gpio_in = actual_in;

    for (int i = 0; i < G233_GPIO_PINS; i++) {
        uint32_t mask = 1u << i;
        pin_val = extract32(actual_in, i, 1);
        prev_val = extract32(s->prev_in, i, 1);
        trig_level = extract32(s->gpio_trig, i, 1);
        pol_high = extract32(s->gpio_pol, i, 1);

        if (trig_level) {
            if ((pol_high && pin_val) || (!pol_high && !pin_val)) {
                if (s->gpio_ie & mask) {
                    new_is |= mask;
                }
            } else {
                new_is &= ~mask;
            }
        } else {
            if (changed & mask) {
                bool rising = pin_val && !prev_val;
                bool falling = !pin_val && prev_val;
                if ((pol_high && rising) || (!pol_high && falling)) {
                    if (s->gpio_ie & mask) {
                        new_is |= mask;
                    }
                }
            }
        }

        if ((s->gpio_dir & mask) != 0) {
            qemu_set_irq(s->output[i], pin_val);
        }
    }

    s->gpio_is = new_is;
    s->prev_in = actual_in;
    g233_gpio_update_irq(s);
}

static uint64_t g233_gpio_read(void *opaque, hwaddr offset, unsigned int size)
{
    G233GPIOState *s = G233_GPIO(opaque);
    uint64_t r = 0;

    switch (offset) {
    case G233_GPIO_REG_DIR:
        r = s->gpio_dir;
        break;
    case G233_GPIO_REG_OUT:
        r = s->gpio_out;
        break;
    case G233_GPIO_REG_IN:
        r = s->gpio_in;
        break;
    case G233_GPIO_REG_IE:
        r = s->gpio_ie;
        break;
    case G233_GPIO_REG_IS:
        r = s->gpio_is;
        break;
    case G233_GPIO_REG_TRIG:
        r = s->gpio_trig;
        break;
    case G233_GPIO_REG_POL:
        r = s->gpio_pol;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad read offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
    }

    return r;
}

static void g233_gpio_write(void *opaque, hwaddr offset,
                             uint64_t value, unsigned int size)
{
    G233GPIOState *s = G233_GPIO(opaque);

    switch (offset) {
    case G233_GPIO_REG_DIR:
        s->gpio_dir = value;
        break;
    case G233_GPIO_REG_OUT:
        s->gpio_out = value;
        break;
    case G233_GPIO_REG_IN:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: GPIO_IN is read-only\n", __func__);
        break;
    case G233_GPIO_REG_IE:
        s->gpio_ie = value;
        break;
    case G233_GPIO_REG_IS:
        s->gpio_is &= ~value;
        break;
    case G233_GPIO_REG_TRIG:
        s->gpio_trig = value;
        break;
    case G233_GPIO_REG_POL:
        s->gpio_pol = value;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad write offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return;
    }

    g233_gpio_update_state(s);
}

static const MemoryRegionOps g233_gpio_ops = {
    .read =  g233_gpio_read,
    .write = g233_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void g233_gpio_set(void *opaque, int line, int value)
{
    G233GPIOState *s = G233_GPIO(opaque);

    assert(line >= 0 && line < G233_GPIO_PINS);

    s->in_mask = deposit32(s->in_mask, line, 1, value >= 0);
    if (value >= 0) {
        s->ext_in = deposit32(s->ext_in, line, 1, value != 0);
    }

    g233_gpio_update_state(s);
}

static void g233_gpio_reset(DeviceState *dev)
{
    G233GPIOState *s = G233_GPIO(dev);

    s->gpio_dir  = 0;
    s->gpio_out  = 0;
    s->gpio_in   = 0;
    s->gpio_ie   = 0;
    s->gpio_is   = 0;
    s->gpio_trig = 0;
    s->gpio_pol  = 0;
    s->prev_in   = 0;
    s->in_mask   = 0;
    s->ext_in    = 0;
}

static const VMStateDescription vmstate_g233_gpio = {
    .name = TYPE_G233_GPIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(gpio_dir,  G233GPIOState),
        VMSTATE_UINT32(gpio_out,  G233GPIOState),
        VMSTATE_UINT32(gpio_in,   G233GPIOState),
        VMSTATE_UINT32(gpio_ie,   G233GPIOState),
        VMSTATE_UINT32(gpio_is,   G233GPIOState),
        VMSTATE_UINT32(gpio_trig, G233GPIOState),
        VMSTATE_UINT32(gpio_pol,  G233GPIOState),
        VMSTATE_UINT32(prev_in,   G233GPIOState),
        VMSTATE_UINT32(in_mask,   G233GPIOState),
        VMSTATE_UINT32(ext_in,    G233GPIOState),
        VMSTATE_END_OF_LIST()
    }
};

static void g233_gpio_realize(DeviceState *dev, Error **errp)
{
    G233GPIOState *s = G233_GPIO(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &g233_gpio_ops, s,
                          TYPE_G233_GPIO, G233_GPIO_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);

    sysbus_init_irq(sbd, &s->irq);

    qdev_init_gpio_in(DEVICE(s), g233_gpio_set, G233_GPIO_PINS);
    qdev_init_gpio_out(DEVICE(s), s->output, G233_GPIO_PINS);
}

static void g233_gpio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_g233_gpio;
    dc->realize = g233_gpio_realize;
    device_class_set_legacy_reset(dc, g233_gpio_reset);
    dc->desc = "G233 GPIO Controller";
}

static const TypeInfo g233_gpio_info = {
    .name          = TYPE_G233_GPIO,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(G233GPIOState),
    .class_init    = g233_gpio_class_init,
};

static void g233_gpio_register_types(void)
{
    type_register_static(&g233_gpio_info);
}

type_init(g233_gpio_register_types)
