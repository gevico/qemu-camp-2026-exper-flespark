/*
 * Copyright (c) 2026 Chao Liu <chao.liu@yeah.net>
 *
 * G233 Watchdog IP block
 *
 * Author: Chao Liu <chao.liu@yeah.net>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef WDT_G233_H
#define WDT_G233_H

#include "qemu/bitops.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_G233_WDT "g233-wdt"
OBJECT_DECLARE_SIMPLE_TYPE(G233WdtState, G233_WDT)

#define G233_WDT_SIZE 0x1000

struct G233WdtState {
    /* <private> */
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    qemu_irq irq;

    QEMUTimer timer;
    uint64_t freq_hz;
    
    /* tick offset when wdt enable */
    uint64_t tick_offset;

    uint32_t wdt_ctrl;
    uint32_t wdt_load;
    uint32_t wdt_sr;
};

#endif /* WDT_G233_H */
