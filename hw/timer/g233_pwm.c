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
 * 
 * TODO: define PWM output level transfer refer to duty and polarity config,
 * and connect PWM to GPIO controller
 */

#include "qemu/osdep.h"
#include "trace.h"
#include "hw/core/irq.h"
#include "hw/timer/g233_pwm.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/registerfields.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

/* PWM_GLB register fields */
REG32(PWM_GLB,             0x00)
    FIELD(PWM_GLB, CH0_EN,  0, 1)
    FIELD(PWM_GLB, CH1_EN,  1, 1)
    FIELD(PWM_GLB, CH2_EN,  2, 1)
    FIELD(PWM_GLB, CH3_EN,  3, 1)
    FIELD(PWM_GLB, CH0_DONE, 4, 1)
    FIELD(PWM_GLB, CH1_DONE, 5, 1)
    FIELD(PWM_GLB, CH2_DONE, 6, 1)
    FIELD(PWM_GLB, CH3_DONE, 7, 1)

/* PWM_CHn_CTRL register fields (per-channel) */
REG32(CH_CTRL,             0x00)
    FIELD(CH_CTRL, EN,      0, 1)
    FIELD(CH_CTRL, POL,     1, 1)
    FIELD(CH_CTRL, INTIE,   2, 1)

/* Channel register block: each channel occupies 0x10 bytes */
#define CH_CTRL_OFFSET(n)   (0x10 + (n) * 0x10 + 0x00)
#define CH_PERIOD_OFFSET(n) (0x10 + (n) * 0x10 + 0x04)
#define CH_DUTY_OFFSET(n)   (0x10 + (n) * 0x10 + 0x08)
#define CH_CNT_OFFSET(n)    (0x10 + (n) * 0x10 + 0x0C)

#define CH_CTRL_EN_MASK   0x1
#define CH_CTRL_POL_MASK  0x2
#define CH_CTRL_INTIE_MASK 0x4

static inline bool g233_pwm_ch_enabled(G233PwmState *s, int ch)
{
    return s->ch_ctrl[ch] & CH_CTRL_EN_MASK;
}

static inline bool g233_pwm_ch_intie(G233PwmState *s, int ch)
{
    return s->ch_ctrl[ch] & CH_CTRL_INTIE_MASK;
}

static inline uint64_t g233_pwm_ns_to_ticks(G233PwmState *s, uint64_t ns)
{
    return muldiv64(ns, s->freq_hz, NANOSECONDS_PER_SECOND);
}

static inline uint64_t g233_pwm_ticks_to_ns(G233PwmState *s, uint64_t ticks)
{
    return muldiv64(ticks, NANOSECONDS_PER_SECOND, s->freq_hz);
}

static uint32_t g233_pwm_get_cnt(G233PwmState *s, int ch)
{
    if (!g233_pwm_ch_enabled(s, ch)) {
        return 0;
    }

    uint64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t now_ticks = g233_pwm_ns_to_ticks(s, now_ns);
    uint64_t period = s->ch_period[ch];

    if (period == 0) {
        return 0;
    }

    uint64_t elapsed = now_ticks - s->tick_offset[ch];
    return (uint32_t)(elapsed % (period + 1));
}

static void g233_pwm_update_irq(G233PwmState *s)
{
    uint32_t done = s->pwm_glb & (R_PWM_GLB_CH0_DONE_MASK |
                                   R_PWM_GLB_CH1_DONE_MASK |
                                   R_PWM_GLB_CH2_DONE_MASK |
                                   R_PWM_GLB_CH3_DONE_MASK);

    if (done) {
        qemu_irq_raise(s->irq);
    } else {
        qemu_irq_lower(s->irq);
    }
}

static void g233_pwm_set_alarm(G233PwmState *s, int ch)
{
    if (!g233_pwm_ch_enabled(s, ch)) {
        timer_del(&s->timer[ch]);
        return;
    }

    uint32_t period = s->ch_period[ch];
    if (period == 0) {
        timer_del(&s->timer[ch]);
        return;
    }

    uint64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t now_ticks = g233_pwm_ns_to_ticks(s, now_ns);
    uint64_t elapsed = now_ticks - s->tick_offset[ch];
    uint64_t remaining = (period + 1) - (elapsed % (period + 1));
    uint64_t when_ns = now_ns + g233_pwm_ticks_to_ns(s, remaining);

    trace_g233_pwm_set_alarm(ch, when_ns, now_ns);
    timer_mod(&s->timer[ch], when_ns);
}

static void g233_pwm_ch_interrupt(G233PwmState *s, int ch)
{
    trace_g233_pwm_interrupt(ch);

    /* Set the DONE flag in PWM_GLB */
    uint32_t done_bit = R_PWM_GLB_CH0_DONE_MASK << ch;
    s->pwm_glb |= done_bit;

    g233_pwm_update_irq(s);

    /* Reset the channel counter: update tick_offset so cnt wraps to 0 */
    uint64_t now_ticks = g233_pwm_ns_to_ticks(s,
                                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    uint32_t period = s->ch_period[ch];

    if (period == 0) {
        s->tick_offset[ch] = now_ticks;
    } else {
        uint64_t elapsed = now_ticks - s->tick_offset[ch];
        uint64_t full_cycles = elapsed / (period + 1);
        s->tick_offset[ch] += full_cycles * (period + 1);
    }

    /* Re-arm the alarm for the next period */
    g233_pwm_set_alarm(s, ch);
}

static void g233_pwm_interrupt_0(void *opaque)
{
    g233_pwm_ch_interrupt(opaque, 0);
}

static void g233_pwm_interrupt_1(void *opaque)
{
    g233_pwm_ch_interrupt(opaque, 1);
}

static void g233_pwm_interrupt_2(void *opaque)
{
    g233_pwm_ch_interrupt(opaque, 2);
}

static void g233_pwm_interrupt_3(void *opaque)
{
    g233_pwm_ch_interrupt(opaque, 3);
}

static QEMUTimerCB * const g233_pwm_timer_cb[G233_PWM_CHANS] = {
    g233_pwm_interrupt_0,
    g233_pwm_interrupt_1,
    g233_pwm_interrupt_2,
    g233_pwm_interrupt_3,
};

static uint64_t g233_pwm_read(void *opaque, hwaddr addr, unsigned int size)
{
    G233PwmState *s = opaque;

    trace_g233_pwm_read(addr);

    /* Global register */
    if (addr == A_PWM_GLB) {
        uint32_t glb = s->pwm_glb;

        /* CHx_EN bits [3:0] are read-only mirrors of per-channel EN bits */
        glb &= ~(R_PWM_GLB_CH0_EN_MASK | R_PWM_GLB_CH1_EN_MASK |
                  R_PWM_GLB_CH2_EN_MASK | R_PWM_GLB_CH3_EN_MASK);
        for (int i = 0; i < G233_PWM_CHANS; i++) {
            if (g233_pwm_ch_enabled(s, i)) {
                glb |= R_PWM_GLB_CH0_EN_MASK << i;
            }
        }
        return glb;
    }

    /* Channel registers: addr = 0x10 + N*0x10 + offset */
    if (addr >= 0x10 && addr < 0x10 + G233_PWM_CHANS * 0x10) {
        hwaddr ch_base = addr - 0x10;
        int ch = ch_base / 0x10;
        hwaddr ch_off = ch_base % 0x10;

        switch (ch_off) {
        case 0x00: /* PWM_CHn_CTRL */
            return s->ch_ctrl[ch];
        case 0x04: /* PWM_CHn_PERIOD */
            return s->ch_period[ch];
        case 0x08: /* PWM_CHn_DUTY */
            return s->ch_duty[ch];
        case 0x0C: /* PWM_CHn_CNT (read-only) */
            return g233_pwm_get_cnt(s, ch);
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: Bad channel offset 0x%"HWADDR_PRIx"\n",
                          __func__, addr);
            return 0;
        }
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "%s: Bad read offset 0x%"HWADDR_PRIx"\n", __func__, addr);
    return 0;
}

static void g233_pwm_write(void *opaque, hwaddr addr,
                            uint64_t val64, unsigned int size)
{
    G233PwmState *s = opaque;
    uint32_t value = val64;

    trace_g233_pwm_write(value, addr);

    /* Global register */
    if (addr == A_PWM_GLB) {
        /* CHx_DONE bits are W1C: writing 1 clears them */
        uint32_t done_bits = value & (R_PWM_GLB_CH0_DONE_MASK |
                                      R_PWM_GLB_CH1_DONE_MASK |
                                      R_PWM_GLB_CH2_DONE_MASK |
                                      R_PWM_GLB_CH3_DONE_MASK);
        s->pwm_glb &= ~done_bits;

        /* CHx_EN bits [3:0] are read-only, ignore writes */
        g233_pwm_update_irq(s);
        return;
    }

    /* Channel registers */
    if (addr >= 0x10 && addr < 0x10 + G233_PWM_CHANS * 0x10) {
        hwaddr ch_base = addr - 0x10;
        int ch = ch_base / 0x10;
        hwaddr ch_off = ch_base % 0x10;
        bool was_enabled = g233_pwm_ch_enabled(s, ch);

        switch (ch_off) {
        case 0x00: { /* PWM_CHn_CTRL */
            uint32_t new_ctrl = value & (CH_CTRL_EN_MASK | CH_CTRL_POL_MASK |
                                         CH_CTRL_INTIE_MASK);
            bool now_enabled = new_ctrl & CH_CTRL_EN_MASK;
            s->ch_ctrl[ch] = new_ctrl;

            if (!was_enabled && now_enabled) {
                /* Channel just enabled: set tick_offset to now */
                uint64_t now_ticks = g233_pwm_ns_to_ticks(s,
                                    qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
                s->tick_offset[ch] = now_ticks;
                g233_pwm_set_alarm(s, ch);
            } else if (was_enabled && !now_enabled) {
                /* Channel just disabled: stop timer, cnt reads 0 */
                timer_del(&s->timer[ch]);
            }
            break;
        }
        case 0x04: /* PWM_CHn_PERIOD */
            s->ch_period[ch] = value;
            if (g233_pwm_ch_enabled(s, ch)) {
                g233_pwm_set_alarm(s, ch);
            }
            break;
        case 0x08: /* PWM_CHn_DUTY */
            s->ch_duty[ch] = value;
            break;
        case 0x0C: /* PWM_CHn_CNT: read-only, ignore writes */
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: PWM_CH%d_CNT is read-only\n", __func__, ch);
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: Bad channel offset 0x%"HWADDR_PRIx"\n",
                          __func__, addr);
        }
        return;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "%s: Bad write offset 0x%"HWADDR_PRIx"\n", __func__, addr);
}

static void g233_pwm_reset(DeviceState *dev)
{
    G233PwmState *s = G233_PWM(dev);

    s->pwm_glb = 0x00000000;

    for (int i = 0; i < G233_PWM_CHANS; i++) {
        s->ch_ctrl[i] = 0x00000000;
        s->ch_period[i] = 0x00000000;
        s->ch_duty[i] = 0x00000000;
        s->tick_offset[i] = 0;
        timer_del(&s->timer[i]);
    }

    qemu_irq_lower(s->irq);
}

static const MemoryRegionOps g233_pwm_ops = {
    .read = g233_pwm_read,
    .write = g233_pwm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static const VMStateDescription vmstate_g233_pwm = {
    .name = TYPE_G233_PWM,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_TIMER_ARRAY(timer, G233PwmState, G233_PWM_CHANS),
        VMSTATE_UINT64_ARRAY(tick_offset, G233PwmState, G233_PWM_CHANS),
        VMSTATE_UINT32(pwm_glb, G233PwmState),
        VMSTATE_UINT32_ARRAY(ch_ctrl, G233PwmState, G233_PWM_CHANS),
        VMSTATE_UINT32_ARRAY(ch_period, G233PwmState, G233_PWM_CHANS),
        VMSTATE_UINT32_ARRAY(ch_duty, G233PwmState, G233_PWM_CHANS),
        VMSTATE_END_OF_LIST()
    }
};

static const Property g233_pwm_properties[] = {
    DEFINE_PROP_UINT64("clock-frequency", G233PwmState, freq_hz, 50000000ULL),
};

static void g233_pwm_init(Object *obj)
{
    G233PwmState *s = G233_PWM(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);

    memory_region_init_io(&s->mmio, obj, &g233_pwm_ops, s,
                          TYPE_G233_PWM, G233_PWM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static void g233_pwm_realize(DeviceState *dev, Error **errp)
{
    G233PwmState *s = G233_PWM(dev);

    for (int i = 0; i < G233_PWM_CHANS; i++) {
        timer_init_ns(&s->timer[i], QEMU_CLOCK_VIRTUAL,
                      g233_pwm_timer_cb[i], s);
    }
}

static void g233_pwm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, g233_pwm_reset);
    device_class_set_props(dc, g233_pwm_properties);
    dc->vmsd = &vmstate_g233_pwm;
    dc->realize = g233_pwm_realize;
}

static const TypeInfo g233_pwm_info = {
    .name          = TYPE_G233_PWM,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(G233PwmState),
    .instance_init = g233_pwm_init,
    .class_init    = g233_pwm_class_init,
};

static void g233_pwm_register_types(void)
{
    type_register_static(&g233_pwm_info);
}

type_init(g233_pwm_register_types)
