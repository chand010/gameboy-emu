/*
 * gbemu - a Game Boy emulator in C
 *
 * mmu.c - the memory bus. Every byte the CPU reads or writes funnels
 * through gb_read() / gb_write(), which route the access to the right
 * device (cartridge, VRAM, IO registers, timer, audio, ...).
 */

#include <stdio.h>
#include <string.h>

#include "gb.h"
#include "cart.h"
#include "apu.h"
#include "joypad.h"

/* ------------------------------------------------------------------ */
/* IO register helpers                                                 */
/* ------------------------------------------------------------------ */

static u8 io_read(gb_t *gb, u8 off)
{
    switch (off) {
        case 0x00: return joypad_read(gb);          /* P1 */
        case 0x01: return gb->mmu.io[0x01];         /* SB */
        case 0x02: return gb->mmu.io[0x02] | 0x7E;  /* SC */

        case 0x04: return (u8)(gb->timer.div >> 8); /* DIV */
        case 0x05: return gb->timer.tima;           /* TIMA */
        case 0x06: return gb->timer.tma;            /* TMA */
        case 0x07: return (u8)(gb->timer.tac | 0xF8); /* TAC */

        case 0x0F: return (u8)(gb->iflag | 0xE0);   /* IF */

        case 0x40: return gb->mmu.io[0x40];         /* LCDC */
        case 0x41: return (u8)(gb->mmu.io[0x41] | 0x80); /* STAT */
        case 0x42: return gb->mmu.io[0x42];         /* SCY */
        case 0x43: return gb->mmu.io[0x43];         /* SCX */
        case 0x44: return gb->mmu.io[0x44];         /* LY */
        case 0x45: return gb->mmu.io[0x45];         /* LYC */
        case 0x46: return gb->mmu.io[0x46];         /* DMA */
        case 0x47: return gb->mmu.io[0x47];         /* BGP */
        case 0x48: return gb->mmu.io[0x48];         /* OBP0 */
        case 0x49: return gb->mmu.io[0x49];         /* OBP1 */
        case 0x4A: return gb->mmu.io[0x4A];         /* WY */
        case 0x4B: return gb->mmu.io[0x4B];         /* WX */
        case 0x50: return 0x01;                     /* boot ROM disabled */

        default:
            if (off >= 0x10 && off <= 0x3F)
                return apu_read_register(gb, off);
            return gb->mmu.io[off];
    }
}

static void io_write(gb_t *gb, u8 off, u8 value)
{
    switch (off) {
        case 0x00: joypad_write(gb, value); break;

        case 0x01: gb->mmu.io[0x01] = value; break;   /* SB */
        case 0x02: /* SC */
            gb->mmu.io[0x02] = value;
            /* Serial debugging: print SB when a transfer starts. */
            if (value == 0x81)
                putchar(gb->mmu.io[0x01]);
            break;

        case 0x04: gb->timer.div = 0; break;          /* DIV */
        case 0x05: gb->timer.tima = value; break;     /* TIMA */
        case 0x06: gb->timer.tma = value; break;      /* TMA */
        case 0x07: gb->timer.tac = (u8)(value & 0x07); break; /* TAC */

        case 0x0F: gb->iflag = (u8)(value & 0x1F); break; /* IF */

        case 0x40: gb->mmu.io[0x40] = value; break;   /* LCDC */
        case 0x41: /* STAT - bits 3-6 writable only */
            gb->mmu.io[0x41] = (u8)((value & 0x78) | (gb->mmu.io[0x41] & 0x07));
            break;
        case 0x42: gb->mmu.io[0x42] = value; break;   /* SCY */
        case 0x43: gb->mmu.io[0x43] = value; break;   /* SCX */
        case 0x44: break;                             /* LY is read only */
        case 0x45: gb->mmu.io[0x45] = value; break;   /* LYC */
        case 0x46: {                                  /* OAM DMA */
            u16 src = (u16)(value << 8);
            for (u16 i = 0; i < 0xA0; i++)
                gb->mmu.oam[i] = gb_read(gb, (u16)(src + i));
            break;
        }
        case 0x47: gb->mmu.io[0x47] = value; break;   /* BGP */
        case 0x48: gb->mmu.io[0x48] = value; break;   /* OBP0 */
        case 0x49: gb->mmu.io[0x49] = value; break;   /* OBP1 */
        case 0x4A: gb->mmu.io[0x4A] = value; break;   /* WY */
        case 0x4B: gb->mmu.io[0x4B] = value; break;   /* WX */
        case 0x50: break;                             /* boot ROM select */

        default:
            if (off >= 0x10 && off <= 0x3F)
                apu_write_register(gb, off, value);
            else
                gb->mmu.io[off] = value;
            break;
    }
}

/* ------------------------------------------------------------------ */
/* The bus                                                             */
/* ------------------------------------------------------------------ */

u8 gb_read(gb_t *gb, u16 addr)
{
    if (addr < 0x8000) {
        return cart_read(gb, addr);
    } else if (addr < 0xA000) {
        return gb->mmu.vram[addr - 0x8000];
    } else if (addr < 0xC000) {
        return cart_read(gb, addr);
    } else if (addr < 0xE000) {
        return gb->mmu.wram[addr - 0xC000];
    } else if (addr < 0xFE00) {
        return gb->mmu.wram[addr - 0xE000];       /* WRAM echo */
    } else if (addr < 0xFEA0) {
        return gb->mmu.oam[addr - 0xFE00];
    } else if (addr < 0xFF00) {
        return 0xFF;                              /* unusable area */
    } else if (addr < 0xFF80) {
        return io_read(gb, (u8)(addr - 0xFF00));
    } else if (addr < 0xFFFF) {
        return gb->mmu.hram[addr - 0xFF80];
    } else {
        return gb->mmu.ie;                        /* 0xFFFF */
    }
}

void gb_write(gb_t *gb, u16 addr, u8 value)
{
    if (addr < 0x8000) {
        cart_write(gb, addr, value);
    } else if (addr < 0xA000) {
        gb->mmu.vram[addr - 0x8000] = value;
    } else if (addr < 0xC000) {
        cart_write(gb, addr, value);
    } else if (addr < 0xE000) {
        gb->mmu.wram[addr - 0xC000] = value;
    } else if (addr < 0xFE00) {
        gb->mmu.wram[addr - 0xE000] = value;      /* WRAM echo */
    } else if (addr < 0xFEA0) {
        gb->mmu.oam[addr - 0xFE00] = value;
    } else if (addr < 0xFF00) {
        /* unusable area - writes are ignored */
    } else if (addr < 0xFF80) {
        io_write(gb, (u8)(addr - 0xFF00), value);
    } else if (addr < 0xFFFF) {
        gb->mmu.hram[addr - 0xFF80] = value;
    } else {
        gb->mmu.ie = value;                       /* 0xFFFF */
    }
}
