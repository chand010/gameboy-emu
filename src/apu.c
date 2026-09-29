/*
 * gbemu - a Game Boy emulator in C
 *
 * apu.c - a functional audio processing unit.
 *
 * All four channels are implemented (two square waves, the wave channel
 * and noise). Samples are produced inside apu_step() and collected in a
 * per-frame buffer; main.c forwards them to SDL once a frame is done.
 */

#include <string.h>

#include "gb.h"
#include "apu.h"

#define T_CYCLES_PER_SAMPLE (GB_CLOCK_HZ / APU_SAMPLE_RATE)

static const u8 duty_pattern[4][8] = {
    { 0, 0, 0, 0, 0, 0, 0, 1 },   /* 12.5% */
    { 1, 0, 0, 0, 0, 0, 0, 1 },   /* 25%   */
    { 1, 0, 0, 0, 0, 1, 1, 1 },   /* 50%   */
    { 0, 1, 1, 1, 1, 1, 1, 0 },   /* 75%   */
};

void apu_reset(gb_t *gb)
{
    apu_t *a = &gb->apu;
    memset(a, 0, sizeof(*a));
    a->nr51 = 0xF3;      /* all channels routed to both speakers */
    a->ch1.nr10 = 0x80;  /* no sweep */
}

/* ------------------------------------------------------------------ */
/* Triggers                                                            */
/* ------------------------------------------------------------------ */

static u16 ch_frequency(u8 low, u8 high)
{
    return (u16)(2048 - (((high & 0x07) << 8) | low));
}

static void ch1_trigger(gb_t *gb)
{
    apu_t *a = &gb->apu;
    a->ch1.on = a->ch1.dac;
    if (a->ch1.length == 0)
        a->ch1.length = 64;
    a->ch1.envelope_timer = a->ch1.envelope_period;
    a->ch1.volume = a->ch1.nr12 >> 4;
    a->ch1.frequency = ch_frequency(a->ch1.nr13, a->ch1.nr14);
    a->ch1.timer = a->ch1.frequency;

    a->ch1.shadow_freq = a->ch1.frequency;
    a->ch1.sweep_period = (a->ch1.nr10 >> 4) & 0x07;
    a->ch1.sweep_shift  = a->ch1.nr10 & 0x07;
    a->ch1.sweep_negate = (a->ch1.nr10 & 0x08) != 0;
    a->ch1.sweep_timer  = a->ch1.sweep_period;
    a->ch1.sweep_enabled = (a->ch1.sweep_period != 0 || a->ch1.sweep_shift != 0);
}

static void ch2_trigger(gb_t *gb)
{
    apu_t *a = &gb->apu;
    a->ch2.on = a->ch2.dac;
    if (a->ch2.length == 0)
        a->ch2.length = 64;
    a->ch2.envelope_timer = a->ch2.envelope_period;
    a->ch2.volume = a->ch2.nr22 >> 4;
    a->ch2.frequency = ch_frequency(a->ch2.nr23, a->ch2.nr24);
    a->ch2.timer = a->ch2.frequency;
}

static void ch3_trigger(gb_t *gb)
{
    apu_t *a = &gb->apu;
    a->ch3.on = a->ch3.dac;
    if (a->ch3.length == 0)
        a->ch3.length = 256;
    a->ch3.frequency = ch_frequency(a->ch3.nr33, a->ch3.nr34);
    a->ch3.timer = a->ch3.frequency;
    a->ch3.wave_pos = 0;
}

static u16 noise_divisor(u8 code)
{
    return (code == 0) ? 8 : (u16)(code << 4);
}

static void ch4_trigger(gb_t *gb)
{
    apu_t *a = &gb->apu;
    a->ch4.on = a->ch4.dac;
    if (a->ch4.length == 0)
        a->ch4.length = 64;
    a->ch4.envelope_timer = a->ch4.envelope_period;
    a->ch4.volume = a->ch4.nr42 >> 4;
    a->ch4.clock_shift = a->ch4.nr43 >> 4;
    a->ch4.width_mode = (a->ch4.nr43 & 0x08) != 0;
    a->ch4.divisor_code = a->ch4.nr43 & 0x07;
    a->ch4.timer = 0;
    a->ch4.lfsr = 0x7FFF;

    u32 freq = 524288u / noise_divisor(a->ch4.divisor_code);
    freq >>= (a->ch4.clock_shift + 1);
    a->ch4.frequency = (u16)freq;
}

/* ------------------------------------------------------------------ */
/* Clocking helpers                                                    */
/* ------------------------------------------------------------------ */

static void square_clock(apu_t *a, int ch)
{
    if (ch == 1) {
        if (a->ch1.frequency == 0) return;
        a->ch1.timer--;
        if (a->ch1.timer <= 0) {
            a->ch1.timer += a->ch1.frequency;
            a->ch1.duty_index = (a->ch1.duty_index + 1) & 7;
        }
    } else {
        if (a->ch2.frequency == 0) return;
        a->ch2.timer--;
        if (a->ch2.timer <= 0) {
            a->ch2.timer += a->ch2.frequency;
            a->ch2.duty_index = (a->ch2.duty_index + 1) & 7;
        }
    }
}

static void wave_clock(apu_t *a)
{
    if (a->ch3.frequency == 0) return;
    a->ch3.timer--;
    if (a->ch3.timer <= 0) {
        a->ch3.timer += a->ch3.frequency;
        a->ch3.wave_pos = (a->ch3.wave_pos + 1) & 31;
    }
}

static void noise_clock(apu_t *a)
{
    u16 bit = (a->ch4.lfsr ^ (a->ch4.lfsr >> 1)) & 1;
    a->ch4.lfsr = (u16)((a->ch4.lfsr >> 1) | (bit << 14));
    if (a->ch4.width_mode)
        a->ch4.lfsr = (u16)((a->ch4.lfsr & ~(1 << 6)) | (bit << 6));
}

/* ------------------------------------------------------------------ */
/* Frame sequencer (512 Hz base clock)                                 */
/* ------------------------------------------------------------------ */

static void length_clock(gb_t *gb)
{
    apu_t *a = &gb->apu;

    if (a->ch1.length_on && a->ch1.length > 0 && --a->ch1.length == 0)
        a->ch1.on = false;
    if (a->ch2.length_on && a->ch2.length > 0 && --a->ch2.length == 0)
        a->ch2.on = false;
    if (a->ch3.length_on && a->ch3.length > 0 && --a->ch3.length == 0)
        a->ch3.on = false;
    if (a->ch4.length_on && a->ch4.length > 0 && --a->ch4.length == 0)
        a->ch4.on = false;
}

static void sweep_clock(gb_t *gb)
{
    apu_t *a = &gb->apu;

    if (!a->ch1.sweep_enabled || a->ch1.sweep_period == 0)
        return;

    a->ch1.sweep_timer--;
    if (a->ch1.sweep_timer > 0)
        return;
    a->ch1.sweep_timer = a->ch1.sweep_period;

    int new_freq = a->ch1.shadow_freq;
    int delta = new_freq >> a->ch1.sweep_shift;
    if (a->ch1.sweep_negate) new_freq -= delta;
    else                     new_freq += delta;

    if (new_freq > 2047) {
        a->ch1.on = false;
    } else if (new_freq >= 0 && a->ch1.sweep_shift > 0) {
        a->ch1.shadow_freq = (u16)new_freq;
        a->ch1.frequency = (u16)new_freq;
        a->ch1.nr13 = (u8)(new_freq & 0xFF);
        a->ch1.nr14 = (u8)((a->ch1.nr14 & 0xF8) | ((new_freq >> 8) & 0x07));
    }
}

static void envelope_clock(gb_t *gb)
{
    apu_t *a = &gb->apu;

    /* channel 1 */
    if (a->ch1.envelope_period > 0) {
        a->ch1.envelope_timer--;
        if (a->ch1.envelope_timer <= 0) {
            a->ch1.envelope_timer = a->ch1.envelope_period;
            if ((a->ch1.nr12 & 0x08) && a->ch1.volume < 15) a->ch1.volume++;
            else if (!(a->ch1.nr12 & 0x08) && a->ch1.volume > 0) a->ch1.volume--;
        }
    }

    /* channel 2 */
    if (a->ch2.envelope_period > 0) {
        a->ch2.envelope_timer--;
        if (a->ch2.envelope_timer <= 0) {
            a->ch2.envelope_timer = a->ch2.envelope_period;
            if ((a->ch2.nr22 & 0x08) && a->ch2.volume < 15) a->ch2.volume++;
            else if (!(a->ch2.nr22 & 0x08) && a->ch2.volume > 0) a->ch2.volume--;
        }
    }

    /* channel 4 */
    if (a->ch4.envelope_period > 0) {
        a->ch4.envelope_timer--;
        if (a->ch4.envelope_timer <= 0) {
            a->ch4.envelope_timer = a->ch4.envelope_period;
            if ((a->ch4.nr42 & 0x08) && a->ch4.volume < 15) a->ch4.volume++;
            else if (!(a->ch4.nr42 & 0x08) && a->ch4.volume > 0) a->ch4.volume--;
        }
    }
}

static void frame_seq_tick(gb_t *gb)
{
    apu_t *a = &gb->apu;

    switch (a->frame_seq_step) {
        case 0: length_clock(gb); break;
        case 2: length_clock(gb); sweep_clock(gb); break;
        case 4: length_clock(gb); break;
        case 6: length_clock(gb); sweep_clock(gb); break;
        case 7: envelope_clock(gb); break;
        default: break;
    }
}

/* ------------------------------------------------------------------ */
/* Sample generation                                                   */
/* ------------------------------------------------------------------ */

static s16 generate_sample(gb_t *gb)
{
    apu_t *a = &gb->apu;

    square_clock(a, 1);
    square_clock(a, 2);
    wave_clock(a);

    if (a->ch4.frequency > 0) {
        a->ch4.timer += a->ch4.frequency;
        while (a->ch4.timer >= APU_SAMPLE_RATE) {
            a->ch4.timer -= APU_SAMPLE_RATE;
            noise_clock(a);
        }
    }

    int total = 0;

    if (a->ch1.on && (a->nr51 & 0x11)) {
        int amp = duty_pattern[(a->ch1.nr11 >> 6) & 3][a->ch1.duty_index] ?
                  a->ch1.volume : -a->ch1.volume;
        total += amp;
    }
    if (a->ch2.on && (a->nr51 & 0x22)) {
        int amp = duty_pattern[(a->ch2.nr21 >> 6) & 3][a->ch2.duty_index] ?
                  a->ch2.volume : -a->ch2.volume;
        total += amp;
    }
    if (a->ch3.on && (a->nr51 & 0x44)) {
        int sample = a->ch3.wave_ram[a->ch3.wave_pos >> 1];
        if (a->ch3.wave_pos & 1) sample &= 0x0F;
        else                     sample >>= 4;
        int shift = (a->ch3.nr32 >> 5) & 3;
        if (shift == 0) sample = 0;
        else            sample >>= (shift - 1);
        total += (sample - 8);
    }
    if (a->ch4.on && (a->nr51 & 0x88)) {
        int amp = (a->ch4.lfsr & 1) ? -a->ch4.volume : a->ch4.volume;
        total += amp;
    }

    int vol = a->nr50 & 0x07;
    int out = total * vol * 220;
    if (out > 32767) out = 32767;
    if (out < -32768) out = -32768;

    return (s16)out;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void apu_step(gb_t *gb, int cycles)
{
    apu_t *a = &gb->apu;
    if (!a->enabled)
        return;

    /* frame sequencer runs at 512 Hz */
    a->frame_seq_cycles += cycles;
    while (a->frame_seq_cycles >= (GB_CLOCK_HZ / 512)) {
        a->frame_seq_cycles -= (GB_CLOCK_HZ / 512);
        frame_seq_tick(gb);
        a->frame_seq_step = (a->frame_seq_step + 1) & 7;
    }

    /* generate audio samples */
    a->sample_cycles += cycles;
    while (a->sample_cycles >= T_CYCLES_PER_SAMPLE) {
        a->sample_cycles -= T_CYCLES_PER_SAMPLE;
        if (a->frame_len < APU_FRAME_MAX_SAMPLES)
            a->frame_buf[a->frame_len++] = generate_sample(gb);
    }
}

u8 apu_read_register(gb_t *gb, u8 off)
{
    apu_t *a = &gb->apu;

    switch (off) {
        case 0x10: return a->ch1.nr10;
        case 0x11: return a->ch1.nr11;
        case 0x12: return a->ch1.nr12;
        case 0x13: return a->ch1.nr13;
        case 0x14: return a->ch1.nr14;

        case 0x16: return a->ch2.nr21;
        case 0x17: return a->ch2.nr22;
        case 0x18: return a->ch2.nr23;
        case 0x19: return a->ch2.nr24;

        case 0x1A: return a->ch3.nr30;
        case 0x1B: return a->ch3.nr31;
        case 0x1C: return a->ch3.nr32;
        case 0x1D: return a->ch3.nr33;
        case 0x1E: return a->ch3.nr34;

        case 0x20: return a->ch4.nr41;
        case 0x21: return a->ch4.nr42;
        case 0x22: return a->ch4.nr43;
        case 0x23: return a->ch4.nr44;

        case 0x24: return a->nr50;
        case 0x25: return a->nr51;
        case 0x26: {
            u8 status = (u8)((a->enabled ? 0x80 : 0x00) |
                             (a->ch1.on ? 0x01 : 0) |
                             (a->ch2.on ? 0x02 : 0) |
                             (a->ch3.on ? 0x04 : 0) |
                             (a->ch4.on ? 0x08 : 0));
            return (u8)(status | 0x70);
        }

        case 0x30: case 0x31: case 0x32: case 0x33:
        case 0x34: case 0x35: case 0x36: case 0x37:
        case 0x38: case 0x39: case 0x3A: case 0x3B:
        case 0x3C: case 0x3D: case 0x3E: case 0x3F:
            return a->ch3.wave_ram[off - 0x30];

        default:
            return 0xFF;
    }
}

void apu_write_register(gb_t *gb, u8 off, u8 value)
{
    apu_t *a = &gb->apu;

    switch (off) {
        case 0x10: a->ch1.nr10 = value; break;

        case 0x11:
            a->ch1.nr11 = value;
            a->ch1.length = 64 - (value & 0x3F);
            break;

        case 0x12:
            a->ch1.nr12 = value;
            a->ch1.dac = (value & 0xF8) != 0;
            if (!a->ch1.dac) a->ch1.on = false;
            a->ch1.envelope_period = value & 0x07;
            break;

        case 0x13: a->ch1.nr13 = value; break;

        case 0x14:
            a->ch1.nr14 = value;
            a->ch1.length_on = (value & 0x40) != 0;
            if (value & 0x80) ch1_trigger(gb);
            break;

        case 0x16:
            a->ch2.nr21 = value;
            a->ch2.length = 64 - (value & 0x3F);
            break;

        case 0x17:
            a->ch2.nr22 = value;
            a->ch2.dac = (value & 0xF8) != 0;
            if (!a->ch2.dac) a->ch2.on = false;
            a->ch2.envelope_period = value & 0x07;
            break;

        case 0x18: a->ch2.nr23 = value; break;

        case 0x19:
            a->ch2.nr24 = value;
            a->ch2.length_on = (value & 0x40) != 0;
            if (value & 0x80) ch2_trigger(gb);
            break;

        case 0x1A:
            a->ch3.nr30 = value;
            a->ch3.dac = (value & 0x80) != 0;
            if (!a->ch3.dac) a->ch3.on = false;
            break;

        case 0x1B:
            a->ch3.nr31 = value;
            a->ch3.length = 256 - value;
            break;

        case 0x1C: a->ch3.nr32 = value; break;

        case 0x1D: a->ch3.nr33 = value; break;

        case 0x1E:
            a->ch3.nr34 = value;
            a->ch3.length_on = (value & 0x40) != 0;
            if (value & 0x80) ch3_trigger(gb);
            break;

        case 0x20:
            a->ch4.nr41 = value;
            a->ch4.length = 64 - (value & 0x3F);
            break;

        case 0x21:
            a->ch4.nr42 = value;
            a->ch4.dac = (value & 0xF8) != 0;
            if (!a->ch4.dac) a->ch4.on = false;
            a->ch4.envelope_period = value & 0x07;
            break;

        case 0x22: a->ch4.nr43 = value; break;

        case 0x23:
            a->ch4.nr44 = value;
            a->ch4.length_on = (value & 0x40) != 0;
            if (value & 0x80) ch4_trigger(gb);
            break;

        case 0x24: a->nr50 = value; break;
        case 0x25: a->nr51 = value; break;

        case 0x26:
            if ((value & 0x80) && !a->enabled) {
                a->enabled = true;
                a->frame_seq_cycles = 0;
                a->frame_seq_step = 0;
            } else if (!(value & 0x80) && a->enabled) {
                a->enabled = false;
                a->ch1.on = a->ch2.on = a->ch3.on = a->ch4.on = false;
                a->ch1.length = a->ch2.length = a->ch3.length = a->ch4.length = 0;
            }
            break;

        case 0x30: case 0x31: case 0x32: case 0x33:
        case 0x34: case 0x35: case 0x36: case 0x37:
        case 0x38: case 0x39: case 0x3A: case 0x3B:
        case 0x3C: case 0x3D: case 0x3E: case 0x3F:
            a->ch3.wave_ram[off - 0x30] = value;
            break;

        default:
            break;
    }
}
