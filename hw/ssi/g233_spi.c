/*
 * G233 SPI Controller
 *
 * Copyright (c) 2026 Chao Liu <chao.liu@yeah.net>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include "qemu/osdep.h"
#include "hw/ssi/g233_spi.h"
#include "hw/ssi/ssi.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "qemu/log.h"
#include "qemu/module.h"

static void g233_spi_update_irq(G233SpiState *s)
{
    int level = 0;

    if ((s->sr & SR_TXE) && (s->cr1 & CR1_TXEIE)) {
        level = 1;
    }
    if ((s->sr & SR_RXNE) && (s->cr1 & CR1_RXNEIE)) {
        level = 1;
    }
    if ((s->sr & SR_OVERRUN) && (s->cr1 & CR1_ERRIE)) {
        level = 1;
    }

    qemu_set_irq(s->irq, level);
}

static void g233_spi_update_cs(G233SpiState *s)
{
    int i;
    int active_cs = s->cr2 & 0x3;

    for (i = 0; i < G233_SPI_NUM_CS; i++) {
        qemu_set_irq(s->cs_lines[i], i != active_cs);
    }
}

static void g233_spi_reset(DeviceState *d)
{
    G233SpiState *s = G233_SPI(d);

    s->cr1 = 0;
    s->cr2 = 0;
    s->sr = SR_TXE;
    s->rx_byte = 0;

    g233_spi_update_cs(s);
    g233_spi_update_irq(s);
}

static uint64_t g233_spi_read(void *opaque, hwaddr addr, unsigned int size)
{
    G233SpiState *s = G233_SPI(opaque);
    uint32_t r = 0;

    switch (addr) {
    case R_SPI_CR1:
        r = s->cr1;
        break;

    case R_SPI_CR2:
        r = s->cr2;
        break;

    case R_SPI_SR:
        r = s->sr;
        break;

    case R_SPI_DR:
        r = s->rx_byte;
        s->sr &= ~SR_RXNE;
        g233_spi_update_irq(s);
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad read at offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }

    return r;
}

static void g233_spi_write(void *opaque, hwaddr addr,
                            uint64_t val64, unsigned int size)
{
    G233SpiState *s = G233_SPI(opaque);
    uint32_t value = val64;

    switch (addr) {
    case R_SPI_CR1:
        s->cr1 = value & 0xE5;  /* SPE|MSTR|ERRIE|RXNEIE|TXEIE */
        break;

    case R_SPI_CR2: {
        uint32_t old_cs = s->cr2 & 0x3;
        s->cr2 = value & 0x3;
        if ((s->cr2 & 0x3) != old_cs) {
            g233_spi_update_cs(s);
        }
        break;
    }

    case R_SPI_SR:
        /* OVERRUN is W1C */
        if (value & SR_OVERRUN) {
            s->sr &= ~SR_OVERRUN;
        }
        g233_spi_update_irq(s);
        break;

    case R_SPI_DR: {
        uint8_t tx = (uint8_t)value;
        uint8_t rx;

        if (!(s->cr1 & CR1_SPE)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: write to DR while SPI disabled\n", __func__);
            break;
        }

        /* OVERRUN: previous RX data not yet read */
        if (s->sr & SR_RXNE) {
            s->sr |= SR_OVERRUN;
        }

        rx = ssi_transfer(s->spi, tx);
        s->rx_byte = rx;
        s->sr |= SR_RXNE;
        s->sr |= SR_TXE;

        g233_spi_update_irq(s);
        break;
    }

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad write at offset 0x%" HWADDR_PRIx
                      " value=0x%x\n", __func__, addr, value);
        break;
    }
}

static const MemoryRegionOps g233_spi_ops = {
    .read = g233_spi_read,
    .write = g233_spi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void g233_spi_realize(DeviceState *dev, Error **errp)
{
    G233SpiState *s = G233_SPI(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    int i;

    s->spi = ssi_create_bus(dev, "spi");

    sysbus_init_irq(sbd, &s->irq);

    for (i = 0; i < G233_SPI_NUM_CS; i++) {
        sysbus_init_irq(sbd, &s->cs_lines[i]);
    }

    memory_region_init_io(&s->mmio, OBJECT(s), &g233_spi_ops, s,
                          TYPE_G233_SPI, G233_SPI_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);
}

static void g233_spi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = g233_spi_realize;
    device_class_set_legacy_reset(dc, g233_spi_reset);
}

static const TypeInfo g233_spi_info = {
    .name           = TYPE_G233_SPI,
    .parent         = TYPE_SYS_BUS_DEVICE,
    .instance_size  = sizeof(G233SpiState),
    .class_init     = g233_spi_class_init,
};

static void g233_spi_register_types(void)
{
    type_register_static(&g233_spi_info);
}

type_init(g233_spi_register_types)
