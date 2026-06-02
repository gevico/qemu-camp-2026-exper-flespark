/*
 * G233 PWM
 *
 * Copyright (c) 2026 Chao Liu <chao.liu@yeah.net>
 *
 * Author:  Chao Liu <chao.liu@yeah.net>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef HW_G233_PWM_H
#define HW_G233_PWM_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_G233_PWM "g233-pwm"

#define G233_PWM(obj) \
    OBJECT_CHECK(G233PwmState, (obj), TYPE_G233_PWM)

#define G233_PWM_CHANS          4
#define G233_PWM_SIZE           0x1000

typedef struct G233PwmState {
    /* <private> */
    SysBusDevice parent_obj;

    /* <public> */
    MemoryRegion mmio;
    QEMUTimer timer[G233_PWM_CHANS];
    uint64_t freq_hz;

    /* Per-channel tick_offset: when enabled, the tick time when cnt was 0 */
    uint64_t tick_offset[G233_PWM_CHANS];

    uint32_t pwm_glb;
    uint32_t ch_ctrl[G233_PWM_CHANS];
    uint32_t ch_period[G233_PWM_CHANS];
    uint32_t ch_duty[G233_PWM_CHANS];

    qemu_irq irq;
} G233PwmState;

#endif /* HW_G233_PWM_H */
