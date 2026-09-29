/*
 * gbemu - a Game Boy emulator in C
 *
 * cpu.c - the Sharp LR35902 CPU. One big switch statement implements
 * every documented opcode, plus the full 0xCB prefix table.
 */

#include "gb.h"

/* flag bits */
#define F_Z 0x80
#define F_N 0x40
#define F_H 0x20
#define F_C 0x10

/* base T-cycle cost of every (unprefixed) opcode */
static const u8 base_cycles[256] = {
/*       0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F */
/* 0x */ 4,12, 8, 8, 4, 4, 8, 4,20, 8, 8, 8, 4, 4, 8, 4,
/* 1x */ 4,12, 8, 8, 4, 4, 8, 4,12, 8, 8, 8, 4, 4, 8, 4,
/* 2x */ 8,12, 8, 8, 4, 4, 8, 4, 8, 8, 8, 8, 4, 4, 8, 4,
/* 3x */ 8,12, 8, 8,12,12,12, 4, 8, 8, 8, 8, 4, 4, 8, 4,
/* 4x */ 4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
/* 5x */ 4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
/* 6x */ 4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
/* 7x */ 8, 8, 8, 8, 8, 8, 4, 8, 4, 4, 4, 4, 4, 4, 8, 4,
/* 8x */ 4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
/* 9x */ 4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
/* Ax */ 4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
/* Bx */ 4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 4, 8, 4,
/* Cx */ 8,12,12,16,12,16, 8,16, 8,16,12, 4,12,24, 8,16,
/* Dx */ 8,12,12, 4,12,16, 8,16, 8,16,12, 4,12, 4, 8,16,
/* Ex */12,12, 8, 4, 4,16, 8,16,16, 4,16, 4, 4, 4, 8,16,
/* Fx */12,12, 8, 4, 4,16, 8,16,12, 8,16, 4, 4, 4, 8,16,
};

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static inline u8 fetch8(gb_t *gb)
{
    return gb_read(gb, gb->cpu.r.pc++);
}

static inline u16 fetch16(gb_t *gb)
{
    u16 lo = fetch8(gb);
    u16 hi = fetch8(gb);
    return (u16)(lo | (hi << 8));
}

static inline bool flag(gb_t *gb, u8 f)   { return (gb->cpu.r.f & f) != 0; }
static inline void setf(gb_t *gb, u8 f, bool v)
{
    if (v) gb->cpu.r.f |= f;
    else   gb->cpu.r.f &= (u8)~f;
}

static u8 r8_get(gb_t *gb, int i)
{
    switch (i) {
        case 0: return gb->cpu.r.b;
        case 1: return gb->cpu.r.c;
        case 2: return gb->cpu.r.d;
        case 3: return gb->cpu.r.e;
        case 4: return gb->cpu.r.h;
        case 5: return gb->cpu.r.l;
        case 6: return gb_read(gb, gb->cpu.r.hl);
        default: return gb->cpu.r.a;
    }
}

static void r8_set(gb_t *gb, int i, u8 v)
{
    switch (i) {
        case 0: gb->cpu.r.b = v; break;
        case 1: gb->cpu.r.c = v; break;
        case 2: gb->cpu.r.d = v; break;
        case 3: gb->cpu.r.e = v; break;
        case 4: gb->cpu.r.h = v; break;
        case 5: gb->cpu.r.l = v; break;
        case 6: gb_write(gb, gb->cpu.r.hl, v); break;
        default: gb->cpu.r.a = v; break;
    }
}

static u16 r16_get(gb_t *gb, int i)
{
    switch (i) {
        case 0: return gb->cpu.r.bc;
        case 1: return gb->cpu.r.de;
        case 2: return gb->cpu.r.hl;
        default: return gb->cpu.r.sp;
    }
}

static void push16(gb_t *gb, u16 v)
{
    gb->cpu.r.sp -= 2;
    gb_write(gb, gb->cpu.r.sp, (u8)(v & 0xFF));
    gb_write(gb, (u16)(gb->cpu.r.sp + 1), (u8)(v >> 8));
}

static u16 pop16(gb_t *gb)
{
    u16 lo = gb_read(gb, gb->cpu.r.sp);
    u16 hi = gb_read(gb, (u16)(gb->cpu.r.sp + 1));
    gb->cpu.r.sp += 2;
    return (u16)(lo | (hi << 8));
}

static bool condition(gb_t *gb, int cc)
{
    switch (cc) {
        case 0: return !flag(gb, F_Z);
        case 1: return  flag(gb, F_Z);
        case 2: return !flag(gb, F_C);
        default: return flag(gb, F_C);
    }
}

static void service_interrupt(gb_t *gb, u8 pending)
{
    cpu_t *c = &gb->cpu;
    c->ime = false;
    c->ime_delay = 0;

    push16(gb, c->r.pc);

    if (pending & 0x01)      { c->r.pc = 0x40; gb->iflag &= ~0x01; }
    else if (pending & 0x02) { c->r.pc = 0x48; gb->iflag &= ~0x02; }
    else if (pending & 0x04) { c->r.pc = 0x50; gb->iflag &= ~0x04; }
    else if (pending & 0x08) { c->r.pc = 0x58; gb->iflag &= ~0x08; }
    else                     { c->r.pc = 0x60; gb->iflag &= ~0x10; }
}

/* ------------------------------------------------------------------ */
/* 0xCB prefix                                                         */
/* ------------------------------------------------------------------ */

static void exec_cb(gb_t *gb, int *cycles)
{
    u8 op = fetch8(gb);
    int reg = op & 7;
    int group = op >> 6;          /* 0 rotate, 1 BIT, 2 RES, 3 SET */
    int bit = (op >> 3) & 7;

    *cycles = 8;
    if (reg == 6)
        *cycles = (group == 1) ? 12 : 16;

    u8 v = r8_get(gb, reg);
    u8 res = v;

    if (group == 0) {
        u8 f = 0;
        switch (bit) {
            case 0: { u8 c = v >> 7; res = (u8)(v << 1) | c; if (c) f |= F_C; break; }      /* RLC */
            case 1: { u8 c = v & 1; res = (u8)(v >> 1) | (u8)(c << 7); if (c) f |= F_C; break; } /* RRC */
            case 2: { u8 c = v >> 7; res = (u8)(v << 1) | (flag(gb, F_C) ? 1 : 0); if (c) f |= F_C; break; } /* RL */
            case 3: { u8 c = v & 1; res = (u8)(v >> 1) | (u8)((flag(gb, F_C) ? 1 : 0) << 7); if (c) f |= F_C; break; } /* RR */
            case 4: { u8 c = v >> 7; res = (u8)(v << 1); if (c) f |= F_C; break; }          /* SLA */
            case 5: { u8 c = v & 1; res = (u8)((v >> 1) | (v & 0x80)); if (c) f |= F_C; break; } /* SRA */
            case 6: res = (u8)((v << 4) | (v >> 4)); break;                                   /* SWAP */
            case 7: { u8 c = v & 1; res = (u8)(v >> 1); if (c) f |= F_C; break; }           /* SRL */
        }
        if (res == 0) f |= F_Z;
        r8_set(gb, reg, res);
        gb->cpu.r.f = f;              /* Z, N=0, H=0, C */
    } else if (group == 1) {
        /* BIT b, r */
        gb->cpu.r.f = (u8)((gb->cpu.r.f & F_C) | F_H | ((v & (1 << bit)) ? 0 : F_Z));
    } else if (group == 2) {
        /* RES b, r */
        res = (u8)(v & ~(1 << bit));
        r8_set(gb, reg, res);
    } else {
        /* SET b, r */
        res = (u8)(v | (1 << bit));
        r8_set(gb, reg, res);
    }
}

/* ------------------------------------------------------------------ */
/* The instruction interpreter                                          */
/* ------------------------------------------------------------------ */

void cpu_step(gb_t *gb, int *cycles)
{
    cpu_t *c = &gb->cpu;

    /* handle a pending interrupt first */
    if (c->ime) {
        u8 pending = (u8)(gb->iflag & gb->mmu.ie);
        if (pending) {
            service_interrupt(gb, pending);
            *cycles = 20;
            return;
        }
    }

    if (c->halted) {
        if (gb->iflag & gb->mmu.ie)
            c->halted = false;
        *cycles = 4;
        return;
    }

    u8 op = fetch8(gb);
    int cyc = base_cycles[op];

    switch (op) {
        /* ---- NOP ---- */
        case 0x00: break;

        /* ---- 16 bit loads ---- */
        case 0x01: gb->cpu.r.bc = fetch16(gb); break;
        case 0x11: gb->cpu.r.de = fetch16(gb); break;
        case 0x21: gb->cpu.r.hl = fetch16(gb); break;
        case 0x31: gb->cpu.r.sp = fetch16(gb); break;

        case 0x02: gb_write(gb, gb->cpu.r.bc, gb->cpu.r.a); break;
        case 0x12: gb_write(gb, gb->cpu.r.de, gb->cpu.r.a); break;
        case 0x0A: gb->cpu.r.a = gb_read(gb, gb->cpu.r.bc); break;
        case 0x1A: gb->cpu.r.a = gb_read(gb, gb->cpu.r.de); break;

        case 0x08: {   /* LD (nn), SP */
            u16 addr = fetch16(gb);
            gb_write(gb, addr, (u8)(gb->cpu.r.sp & 0xFF));
            gb_write(gb, (u16)(addr + 1), (u8)(gb->cpu.r.sp >> 8));
            break;
        }

        case 0xEA: {   /* LD (nn), A */
            u16 addr = fetch16(gb);
            gb_write(gb, addr, gb->cpu.r.a);
            break;
        }
        case 0xFA:     /* LD A, (nn) */
            gb->cpu.r.a = gb_read(gb, fetch16(gb));
            break;

        case 0xE0:     /* LDH (n), A */
            gb_write(gb, (u16)(0xFF00 + fetch8(gb)), gb->cpu.r.a);
            break;
        case 0xF0:     /* LDH A, (n) */
            gb->cpu.r.a = gb_read(gb, (u16)(0xFF00 + fetch8(gb)));
            break;

        case 0xE2:     /* LD (C), A */
            gb_write(gb, (u16)(0xFF00 + gb->cpu.r.c), gb->cpu.r.a);
            break;
        case 0xF2:     /* LD A, (C) */
            gb->cpu.r.a = gb_read(gb, (u16)(0xFF00 + gb->cpu.r.c));
            break;

        case 0x22:     /* LD (HL+), A */
            gb_write(gb, gb->cpu.r.hl++, gb->cpu.r.a);
            break;
        case 0x32:     /* LD (HL-), A */
            gb_write(gb, gb->cpu.r.hl--, gb->cpu.r.a);
            break;
        case 0x2A:     /* LD A, (HL+) */
            gb->cpu.r.a = gb_read(gb, gb->cpu.r.hl++);
            break;
        case 0x3A:     /* LD A, (HL-) */
            gb->cpu.r.a = gb_read(gb, gb->cpu.r.hl--);
            break;

        case 0xF9:     /* LD SP, HL */
            gb->cpu.r.sp = gb->cpu.r.hl;
            break;
        case 0xE9:     /* JP HL */
            gb->cpu.r.pc = gb->cpu.r.hl;
            break;

        /* ---- 16 bit increments ---- */
        case 0x03: gb->cpu.r.bc++; break;
        case 0x13: gb->cpu.r.de++; break;
        case 0x23: gb->cpu.r.hl++; break;
        case 0x33: gb->cpu.r.sp++; break;
        case 0x0B: gb->cpu.r.bc--; break;
        case 0x1B: gb->cpu.r.de--; break;
        case 0x2B: gb->cpu.r.hl--; break;
        case 0x3B: gb->cpu.r.sp--; break;

        /* ---- 8 bit increments ---- */
        case 0x04: case 0x0C: case 0x14: case 0x1C:
        case 0x24: case 0x2C: case 0x3C: {
            int i = (op >> 3) & 7;
            u8 v = r8_get(gb, i);
            u8 res = (u8)(v + 1);
            gb->cpu.r.f = (u8)((gb->cpu.r.f & F_C) | (res == 0 ? F_Z : 0) |
                               ((v & 0x0F) == 0x0F ? F_H : 0));
            r8_set(gb, i, res);
            break;
        }
        case 0x05: case 0x0D: case 0x15: case 0x1D:
        case 0x25: case 0x2D: case 0x3D: {
            int i = (op >> 3) & 7;
            u8 v = r8_get(gb, i);
            u8 res = (u8)(v - 1);
            gb->cpu.r.f = (u8)((gb->cpu.r.f & F_C) | F_N | (res == 0 ? F_Z : 0) |
                               ((v & 0x0F) == 0x00 ? F_H : 0));
            r8_set(gb, i, res);
            break;
        }

        case 0x34: {   /* INC (HL) */
            u8 v = gb_read(gb, gb->cpu.r.hl);
            u8 res = (u8)(v + 1);
            gb->cpu.r.f = (u8)((gb->cpu.r.f & F_C) | (res == 0 ? F_Z : 0) |
                               ((v & 0x0F) == 0x0F ? F_H : 0));
            gb_write(gb, gb->cpu.r.hl, res);
            break;
        }
        case 0x35: {   /* DEC (HL) */
            u8 v = gb_read(gb, gb->cpu.r.hl);
            u8 res = (u8)(v - 1);
            gb->cpu.r.f = (u8)((gb->cpu.r.f & F_C) | F_N | (res == 0 ? F_Z : 0) |
                               ((v & 0x0F) == 0x00 ? F_H : 0));
            gb_write(gb, gb->cpu.r.hl, res);
            break;
        }

        /* ---- 8 bit loads with immediate ---- */
        case 0x06: case 0x0E: case 0x16: case 0x1E:
        case 0x26: case 0x2E: case 0x3E: {
            int i = (op >> 3) & 7;
            r8_set(gb, i, fetch8(gb));
            break;
        }

        case 0x36:     /* LD (HL), n */
            gb_write(gb, gb->cpu.r.hl, fetch8(gb));
            break;

        /* ---- register to register loads (0x40-0x7F) ---- */
        case 0x40 ... 0x7F: {
            if (op == 0x76) {          /* HALT */
                c->halted = true;
            } else {
                int dst = (op >> 3) & 7;
                int src = op & 7;
                r8_set(gb, dst, r8_get(gb, src));
            }
            break;
        }

        /* ---- rotates (A only, Z flag untouched) ---- */
        case 0x07: {   /* RLCA */
            u8 c = gb->cpu.r.a >> 7;
            gb->cpu.r.a = (u8)((gb->cpu.r.a << 1) | c);
            gb->cpu.r.f = (u8)(c ? F_C : 0);
            break;
        }
        case 0x0F: {   /* RRCA */
            u8 c = gb->cpu.r.a & 1;
            gb->cpu.r.a = (u8)((gb->cpu.r.a >> 1) | (c << 7));
            gb->cpu.r.f = (u8)(c ? F_C : 0);
            break;
        }
        case 0x17: {   /* RLA */
            u8 c = gb->cpu.r.a >> 7;
            gb->cpu.r.a = (u8)((gb->cpu.r.a << 1) | (flag(gb, F_C) ? 1 : 0));
            gb->cpu.r.f = (u8)(c ? F_C : 0);
            break;
        }
        case 0x1F: {   /* RRA */
            u8 c = gb->cpu.r.a & 1;
            gb->cpu.r.a = (u8)((gb->cpu.r.a >> 1) | ((flag(gb, F_C) ? 1 : 0) << 7));
            gb->cpu.r.f = (u8)(c ? F_C : 0);
            break;
        }

        case 0x27: {   /* DAA */
            u8 a = gb->cpu.r.a;
            u8 adjust = 0;
            bool carry = flag(gb, F_C);
            bool neg = flag(gb, F_N);

            if (neg) {
                if (flag(gb, F_C)) adjust |= 0x60;
                if (flag(gb, F_H)) adjust |= 0x06;
                a = (u8)(a - adjust);
            } else {
                if (flag(gb, F_C) || a > 0x99) { adjust |= 0x60; carry = true; }
                if (flag(gb, F_H) || (a & 0x0F) > 0x09) adjust |= 0x06;
                a = (u8)(a + adjust);
            }

            gb->cpu.r.a = a;
            gb->cpu.r.f = (u8)((neg ? F_N : 0) |
                               (a == 0 ? F_Z : 0) |
                               (carry ? F_C : 0));
            break;
        }

        case 0x2F:     /* CPL */
            gb->cpu.r.a = (u8)~gb->cpu.r.a;
            gb->cpu.r.f |= F_N | F_H;
            break;

        case 0x37:     /* SCF */
            gb->cpu.r.f = (u8)((gb->cpu.r.f & F_Z) | F_C);
            break;
        case 0x3F:     /* CCF: C = ~C, H = 0, N = 0, Z unchanged */
            gb->cpu.r.f = (u8)((gb->cpu.r.f & F_Z) | (flag(gb, F_C) ? 0 : F_C));
            break;

        /* ---- ADD HL, rr ---- */
        case 0x09: case 0x19: case 0x29: case 0x39: {
            int i = (op >> 4) & 3;
            u16 rr = r16_get(gb, i);
            u16 hl = gb->cpu.r.hl;
            u32 res = (u32)hl + rr;
            gb->cpu.r.f = (u8)((gb->cpu.r.f & F_Z) |
                               (res > 0xFFFF ? F_C : 0) |
                               (((hl & 0xFFF) + (rr & 0xFFF)) > 0xFFF ? F_H : 0));
            gb->cpu.r.hl = (u16)res;
            break;
        }

        case 0xE8: {   /* ADD SP, e */
            s8 e = (s8)fetch8(gb);
            u16 sp = gb->cpu.r.sp;
            u16 res = (u16)(sp + e);
            u16 eu = (u16)(s16)e;
            gb->cpu.r.f = (u8)((((sp ^ eu ^ res) & 0x10) ? F_H : 0) |
                               (((sp ^ eu ^ res) & 0x100) ? F_C : 0));
            gb->cpu.r.sp = res;
            break;
        }
        case 0xF8: {   /* LD HL, SP+e */
            s8 e = (s8)fetch8(gb);
            u16 sp = gb->cpu.r.sp;
            u16 res = (u16)(sp + e);
            u16 eu = (u16)(s16)e;
            gb->cpu.r.f = (u8)((((sp ^ eu ^ res) & 0x10) ? F_H : 0) |
                               (((sp ^ eu ^ res) & 0x100) ? F_C : 0));
            gb->cpu.r.hl = res;
            break;
        }

        /* ---- ALU on A ---- */
        case 0x80 ... 0xBF: {
            int opc = (op >> 3) & 7;
            int src = op & 7;
            u8 v = r8_get(gb, src);
            u8 a = gb->cpu.r.a;
            u8 f = 0;

            switch (opc) {
                case 0: {   /* ADD A, v */
                    u16 r = (u16)(a + v);
                    if ((r & 0xFF) == 0) f |= F_Z;
                    if (((a & 0xF) + (v & 0xF)) & 0x10) f |= F_H;
                    if (r > 0xFF) f |= F_C;
                    a = (u8)r;
                    break;
                }
                case 1: {   /* ADC A, v */
                    u8 carry = flag(gb, F_C) ? 1 : 0;
                    u16 r = (u16)(a + v + carry);
                    if ((r & 0xFF) == 0) f |= F_Z;
                    if (((a & 0xF) + (v & 0xF) + carry) & 0x10) f |= F_H;
                    if (r > 0xFF) f |= F_C;
                    a = (u8)r;
                    break;
                }
                case 2: {   /* SUB v */
                    f |= F_N;
                    if (a == v) f |= F_Z;
                    if ((a & 0xF) < (v & 0xF)) f |= F_H;
                    if (a < v) f |= F_C;
                    a = (u8)(a - v);
                    break;
                }
                case 3: {   /* SBC A, v */
                    u8 carry = flag(gb, F_C) ? 1 : 0;
                    f |= F_N;
                    if ((u8)(a - v - carry) == 0) f |= F_Z;
                    if ((a & 0xF) < ((v & 0xF) + carry)) f |= F_H;
                    if (a < v + carry) f |= F_C;
                    a = (u8)(a - v - carry);
                    break;
                }
                case 4:     /* AND v */
                    a &= v;
                    f = (u8)(F_H | (a == 0 ? F_Z : 0));
                    break;
                case 5:     /* XOR v */
                    a ^= v;
                    f = (u8)(a == 0 ? F_Z : 0);
                    break;
                case 6:     /* OR v */
                    a |= v;
                    f = (u8)(a == 0 ? F_Z : 0);
                    break;
                case 7:     /* CP v */
                    f |= F_N;
                    if (a == v) f |= F_Z;
                    if ((a & 0xF) < (v & 0xF)) f |= F_H;
                    if (a < v) f |= F_C;
                    break;
            }

            gb->cpu.r.f = f;
            if (opc != 7)
                gb->cpu.r.a = a;
            break;
        }

        /* ---- jumps / calls / returns ---- */
        case 0x18: {   /* JR e */
            s8 e = (s8)fetch8(gb);
            gb->cpu.r.pc = (u16)(gb->cpu.r.pc + e);
            break;
        }
        case 0x20: case 0x28: case 0x30: case 0x38: {   /* JR cc, e */
            s8 e = (s8)fetch8(gb);
            if (condition(gb, (op >> 3) & 3)) {
                gb->cpu.r.pc = (u16)(gb->cpu.r.pc + e);
                cyc += 4;
            }
            break;
        }

        case 0xC3:     /* JP nn */
            gb->cpu.r.pc = fetch16(gb);
            break;
        case 0xC2: case 0xCA: case 0xD2: case 0xDA: {   /* JP cc, nn */
            u16 addr = fetch16(gb);
            if (condition(gb, (op >> 3) & 3)) {
                gb->cpu.r.pc = addr;
                cyc += 4;
            }
            break;
        }

        case 0xC9:     /* RET */
            gb->cpu.r.pc = pop16(gb);
            break;
        case 0xD9:     /* RETI */
            gb->cpu.r.pc = pop16(gb);
            gb->cpu.ime = true;
            gb->cpu.ime_delay = 0;
            break;
        case 0xC0: case 0xC8: case 0xD0: case 0xD8: {   /* RET cc */
            if (condition(gb, (op >> 3) & 3)) {
                gb->cpu.r.pc = pop16(gb);
                cyc += 12;
            }
            break;
        }

        case 0xCD: {   /* CALL nn */
            u16 addr = fetch16(gb);
            push16(gb, gb->cpu.r.pc);
            gb->cpu.r.pc = addr;
            break;
        }
        case 0xC4: case 0xCC: case 0xD4: case 0xDC: {   /* CALL cc, nn */
            u16 addr = fetch16(gb);
            if (condition(gb, (op >> 3) & 3)) {
                push16(gb, gb->cpu.r.pc);
                gb->cpu.r.pc = addr;
                cyc += 12;
            }
            break;
        }

        case 0xC7: case 0xCF: case 0xD7: case 0xDF:
        case 0xE7: case 0xEF: case 0xF7: case 0xFF: {   /* RST n */
            push16(gb, gb->cpu.r.pc);
            gb->cpu.r.pc = (u16)(op & 0x38);
            break;
        }

        /* ---- stack ---- */
        case 0xC5: push16(gb, gb->cpu.r.bc); break;
        case 0xD5: push16(gb, gb->cpu.r.de); break;
        case 0xE5: push16(gb, gb->cpu.r.hl); break;
        case 0xF5: push16(gb, gb->cpu.r.af); break;

        case 0xC1: gb->cpu.r.bc = pop16(gb); break;
        case 0xD1: gb->cpu.r.de = pop16(gb); break;
        case 0xE1: gb->cpu.r.hl = pop16(gb); break;
        case 0xF1:     /* POP AF */
            gb->cpu.r.af = pop16(gb);
            gb->cpu.r.f &= 0xF0;
            break;

        /* ---- misc ---- */
        case 0xF3:     /* DI */
            gb->cpu.ime = false;
            gb->cpu.ime_delay = 0;
            break;
        case 0xFB:     /* EI */
            gb->cpu.ime_delay = 2;
            break;

        case 0x10:     /* STOP - treated as NOP */
            break;

        case 0xCB:
            exec_cb(gb, &cyc);
            break;

        case 0xC6: {   /* ADD A, n */
            u8 v = fetch8(gb);
            u16 r = (u16)(gb->cpu.r.a + v);
            gb->cpu.r.f = (u8)(((r & 0xFF) == 0 ? F_Z : 0) |
                               (((gb->cpu.r.a & 0xF) + (v & 0xF)) & 0x10 ? F_H : 0) |
                               (r > 0xFF ? F_C : 0));
            gb->cpu.r.a = (u8)r;
            break;
        }
        case 0xCE: {   /* ADC A, n */
            u8 v = fetch8(gb);
            u8 carry = flag(gb, F_C) ? 1 : 0;
            u16 r = (u16)(gb->cpu.r.a + v + carry);
            gb->cpu.r.f = (u8)(((r & 0xFF) == 0 ? F_Z : 0) |
                               (((gb->cpu.r.a & 0xF) + (v & 0xF) + carry) & 0x10 ? F_H : 0) |
                               (r > 0xFF ? F_C : 0));
            gb->cpu.r.a = (u8)r;
            break;
        }
        case 0xD6: {   /* SUB n */
            u8 v = fetch8(gb);
            u8 a = gb->cpu.r.a;
            gb->cpu.r.f = (u8)(F_N | (a == v ? F_Z : 0) |
                               ((a & 0xF) < (v & 0xF) ? F_H : 0) |
                               (a < v ? F_C : 0));
            gb->cpu.r.a = (u8)(a - v);
            break;
        }
        case 0xDE: {   /* SBC A, n */
            u8 v = fetch8(gb);
            u8 carry = flag(gb, F_C) ? 1 : 0;
            u8 a = gb->cpu.r.a;
            gb->cpu.r.f = (u8)(F_N | ((u8)(a - v - carry) == 0 ? F_Z : 0) |
                               ((a & 0xF) < ((v & 0xF) + carry) ? F_H : 0) |
                               (a < v + carry ? F_C : 0));
            gb->cpu.r.a = (u8)(a - v - carry);
            break;
        }
        case 0xE6: {   /* AND n */
            gb->cpu.r.a &= fetch8(gb);
            gb->cpu.r.f = (u8)(F_H | (gb->cpu.r.a == 0 ? F_Z : 0));
            break;
        }
        case 0xEE: {   /* XOR n */
            gb->cpu.r.a ^= fetch8(gb);
            gb->cpu.r.f = (u8)(gb->cpu.r.a == 0 ? F_Z : 0);
            break;
        }
        case 0xF6: {   /* OR n */
            gb->cpu.r.a |= fetch8(gb);
            gb->cpu.r.f = (u8)(gb->cpu.r.a == 0 ? F_Z : 0);
            break;
        }
        case 0xFE: {   /* CP n */
            u8 v = fetch8(gb);
            u8 a = gb->cpu.r.a;
            gb->cpu.r.f = (u8)(F_N | (a == v ? F_Z : 0) |
                               ((a & 0xF) < (v & 0xF) ? F_H : 0) |
                               (a < v ? F_C : 0));
            break;
        }

        default:
            /* undocumented opcodes act as NOPs */
            break;
    }

    /* EI takes effect after the instruction that follows it */
    if (c->ime_delay > 0) {
        c->ime_delay--;
        if (c->ime_delay == 0)
            c->ime = true;
    }

    *cycles = cyc;
}
