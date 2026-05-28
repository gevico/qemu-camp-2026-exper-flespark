/*
 * G233 GPIO Controller
 *
 * Copyright (c) 2025 Chao Liu <chao.liu@yeah.net>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Reference: G233 SoC Hardware Manual, Chapter 7 — GPIO Controller
 *
 * QEMU interface:
 *  + sysbus MMIO region 0: GPIO registers (0x100 bytes)
 *  + sysbus IRQ: single interrupt line to PLIC (IRQ 2)
 *  + unnamed GPIO inputs 0..31: external pin level inputs
 *  + unnamed GPIO outputs 0..31: pin output level
 */

#ifndef G233_GPIO_H
#define G233_GPIO_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_G233_GPIO "g233-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(G233GPIOState, G233_GPIO)

#define G233_GPIO_PINS 32
#define G233_GPIO_SIZE 0x100

/* Register offsets */
#define G233_GPIO_REG_DIR   0x00
#define G233_GPIO_REG_OUT   0x04
#define G233_GPIO_REG_IN    0x08
#define G233_GPIO_REG_IE    0x0C
#define G233_GPIO_REG_IS    0x10
#define G233_GPIO_REG_TRIG  0x14
#define G233_GPIO_REG_POL   0x18

struct G233GPIOState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;

    /* IRQ to PLIC — single aggregated interrupt line */
    qemu_irq irq;

    /* Per-pin output lines */
    qemu_irq output[G233_GPIO_PINS];

    /* Register state */
    uint32_t gpio_dir;   /* Direction: 0=input, 1=output */
    uint32_t gpio_out;   /* Output data */
    uint32_t gpio_in;    /* Input data (read-only; reflects OUT when DIR=output) */
    uint32_t gpio_ie;    /* Interrupt enable */
    uint32_t gpio_is;    /* Interrupt status (write-1-clear) */
    uint32_t gpio_trig;  /* Trigger type: 0=edge, 1=level */
    uint32_t gpio_pol;   /* Polarity: 0=low/falling, 1=high/rising */

    /* Internal state for edge detection */
    uint32_t prev_in;    /* Previous pin level, for edge-trigger logic */
    uint32_t in_mask;    /* Which pins are driven externally */
    uint32_t ext_in;     /* External input values */
};

#endif /* G233_GPIO_H */
