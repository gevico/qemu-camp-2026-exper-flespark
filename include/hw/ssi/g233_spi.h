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

#ifndef HW_G233_SPI_H
#define HW_G233_SPI_H

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/ssi/ssi.h"
#include "qom/object.h"

#define TYPE_G233_SPI "g233-spi"
OBJECT_DECLARE_SIMPLE_TYPE(G233SpiState, G233_SPI)

#define G233_SPI_NUM_CS  4
#define G233_SPI_SIZE    0x1000

/* Register offsets */
#define R_SPI_CR1   0x00
#define R_SPI_CR2   0x04
#define R_SPI_SR    0x08
#define R_SPI_DR    0x0C

/* SPI_CR1 bits */
#define CR1_SPE     (1u << 0)
#define CR1_MSTR    (1u << 2)
#define CR1_ERRIE   (1u << 5)
#define CR1_RXNEIE  (1u << 6)
#define CR1_TXEIE   (1u << 7)

/* SPI_SR bits */
#define SR_RXNE     (1u << 0)
#define SR_TXE      (1u << 1)
#define SR_OVERRUN  (1u << 4)

struct G233SpiState {
    /* <private> */
    SysBusDevice parent_obj;

    /* <public> */
    MemoryRegion mmio;
    qemu_irq irq;

    SSIBus *spi;
    qemu_irq cs_lines[G233_SPI_NUM_CS];

    uint32_t cr1;
    uint32_t cr2;
    uint32_t sr;
    uint8_t rx_byte;
};

#endif /* HW_G233_SPI_H */
