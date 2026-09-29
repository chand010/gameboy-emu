/*
 * gbemu - a Game Boy emulator in C
 *
 * gpu.c - the LCD: mode/STAT state machine, scanline rendering of the
 * background, window and sprites, and the (green) colour palette.
 */

#include <string.h>

#include "gb.h"

/* Classic Game Boy green palette, from lightest to darkest. Stored in
 * 0xAARRGGBB order so the framebuffer can be copied straight into an SDL
 * ARGB8888 texture. */
static const u32 shades[4] = {
    0xFF9BBC0F,   /* colour 0 - lightest */
    0xFF8BAC0F,   /* colour 1 */
    0xFF306230,   /* colour 2 */
    0xFF0F380F,   /* colour 3 - darkest */
};

static void set_lcd_mode(gb_t *gb, u8 mode)
{
    gpu_t *g = &gb->gpu;
    if (mode == g->mode)
        return;
    g->mode = mode;

    u8 stat = gb->mmu.io[0x41];
    gb->mmu.io[0x41] = (stat & 0xFC) | mode;

    /* fire the matching STAT interrupt if enabled */
    if (mode == LCD_MODE_HBLANK && (stat & 0x08)) gb_request_interrupt(gb, 1);
    if (mode == LCD_MODE_VBLANK && (stat & 0x10)) gb_request_interrupt(gb, 1);
    if (mode == LCD_MODE_OAM    && (stat & 0x20)) gb_request_interrupt(gb, 1);
}

static void update_lyc(gb_t *gb)
{
    gpu_t *g = &gb->gpu;
    u8 ly  = gb->mmu.io[0x44];
    u8 lyc = gb->mmu.io[0x45];
    u8 stat = gb->mmu.io[0x41];

    bool match = (ly == lyc);
    if (match && !g->stat_lyc) {
        if (stat & 0x40)             /* LYC == LY STAT interrupt */
            gb_request_interrupt(gb, 1);
    }
    g->stat_lyc = match;

    if (match) stat |= 0x04;
    else       stat &= ~0x04;
    gb->mmu.io[0x41] = stat;
}

/* Render one scanline into the framebuffer. */
static void render_scanline(gb_t *gb, u8 ly)
{
    gpu_t *g = &gb->gpu;
    u8 lcdc = gb->mmu.io[0x40];
    u32 *line = &g->framebuffer[(size_t)ly * GB_SCREEN_W];

    u8 scx = gb->mmu.io[0x43];
    u8 scy = gb->mmu.io[0x42];
    u8 wy  = gb->mmu.io[0x4A];
    u8 wx  = gb->mmu.io[0x4B];

    u8  bgp = gb->mmu.io[0x47];
    bool unsig = (lcdc & 0x10) != 0;             /* tile data at 0x8000 */
    u16 bg_map = (lcdc & 0x08) ? 0x9C00 : 0x9800;
    u16 win_map = (lcdc & 0x40) ? 0x9C00 : 0x9800;
    bool bg_enabled  = (lcdc & 0x01) != 0;
    bool win_enabled = (lcdc & 0x20) != 0;
    bool window_active = win_enabled && wy <= ly;

    u8 bgc[GB_SCREEN_W];
    memset(bgc, 0, sizeof(bgc));

    for (int x = 0; x < GB_SCREEN_W; x++) {
        u8 color = 0;

        if (bg_enabled || window_active) {
            bool use_window = window_active && (x >= (wx - 7));

            int px, py;
            u16 map;
            if (use_window) {
                px = x - (wx - 7);
                py = g->window_line;
                map = win_map;
            } else {
                px = (x + scx) & 0xFF;
                py = (ly + scy) & 0xFF;
                map = bg_map;
            }

            u8 tx = (px >> 3) & 31;
            u8 ty = (py >> 3) & 31;
            u8 tile = gb->mmu.vram[map - 0x8000 + ty * 32 + tx];

            u16 addr;
            if (unsig) addr = (u16)(0x8000 + tile * 16);
            else       addr = (u16)(0x9000 + (s8)tile * 16);

            u8 b0 = gb->mmu.vram[addr - 0x8000 + (py & 7) * 2];
            u8 b1 = gb->mmu.vram[addr - 0x8000 + (py & 7) * 2 + 1];
            int bit = 7 - (px & 7);
            color = (u8)(((b1 >> bit) & 1) << 1 | ((b0 >> bit) & 1));
        }

        bgc[x] = color;
        line[x] = shades[(bgp >> (color * 2)) & 3];
    }

    /* Sprites */
    if (lcdc & 0x02) {
        int height = (lcdc & 0x04) ? 16 : 8;
        int indices[10];
        int count = 0;

        for (int i = 0; i < 40 && count < 10; i++) {
            int sy = gb->mmu.oam[i * 4] - 16;
            int sx = gb->mmu.oam[i * 4 + 1] - 8;
            if (sx == 0 || sx >= 168) continue;
            if (sy == 0 || sy >= 160) continue;
            if (ly < sy || ly >= sy + height) continue;
            indices[count++] = i;
        }

        /* draw back to front so earlier OAM entries stay on top */
        for (int k = count - 1; k >= 0; k--) {
            int i = indices[k];
            int sy = gb->mmu.oam[i * 4] - 16;
            int sx = gb->mmu.oam[i * 4 + 1] - 8;
            u8 tile = gb->mmu.oam[i * 4 + 2];
            u8 flags = gb->mmu.oam[i * 4 + 3];

            int row = ly - sy;
            if (flags & 0x40)
                row = height - 1 - row;

            u16 addr;
            if (height == 16) {
                u8 base = tile & 0xFE;
                if (row >= 8)
                    addr = (u16)(0x8000 + (base + 1) * 16 + (row - 8) * 2);
                else
                    addr = (u16)(0x8000 + base * 16 + row * 2);
            } else {
                addr = (u16)(0x8000 + tile * 16 + row * 2);
            }

            u8 b0 = gb->mmu.vram[addr - 0x8000];
            u8 b1 = gb->mmu.vram[addr - 0x8000 + 1];
            u8 pal = (flags & 0x10) ? gb->mmu.io[0x49] : gb->mmu.io[0x48];
            bool behind_bg = (flags & 0x80) != 0;

            for (int x = 0; x < 8; x++) {
                int px = x;
                if (flags & 0x20) px = 7 - x;

                int bit = 7 - px;
                u8 color = (u8)(((b1 >> bit) & 1) << 1 | ((b0 >> bit) & 1));
                if (color == 0)
                    continue;

                int screenx = sx + x;
                if (screenx < 0 || screenx >= GB_SCREEN_W)
                    continue;

                if (behind_bg && bgc[screenx] != 0)
                    continue;

                line[screenx] = shades[(pal >> (color * 2)) & 3];
            }
        }
    }
}

void gpu_step(gb_t *gb, int cycles)
{
    gpu_t *g = &gb->gpu;
    u8 lcdc = gb->mmu.io[0x40];
    bool lcd_enabled = (lcdc & 0x80) != 0;

    if (!lcd_enabled) {
        /* LCD disabled: LY freezes at 0 and the mode becomes HBlank. */
        if (g->lcd_on) {
            gb->mmu.io[0x44] = 0;
            g->window_line = 0;
            g->cycles = 0;
            set_lcd_mode(gb, LCD_MODE_HBLANK);
            update_lyc(gb);
        }
        g->lcd_on = false;

        /* Keep the main loop ticking even with the LCD off. */
        g->cycles += cycles;
        while (g->cycles >= GB_CYCLES_FRAME) {
            g->cycles -= GB_CYCLES_FRAME;
            gb->frame_ready = true;
        }
        return;
    }

    if (!g->lcd_on) {
        /* LCD just turned back on: start a fresh frame. */
        g->lcd_on = true;
        g->cycles = 0;
        gb->mmu.io[0x44] = 0;
        g->window_line = 0;
        set_lcd_mode(gb, LCD_MODE_OAM);
        update_lyc(gb);
    }

    g->cycles += cycles;

    while (g->cycles >= 456) {
        g->cycles -= 456;
        u8 ly = gb->mmu.io[0x44];

        if (ly < 144)
            render_scanline(gb, ly);

        u8 next = (u8)(ly + 1);
        if (next == 144) {
            set_lcd_mode(gb, LCD_MODE_VBLANK);
            gb_request_interrupt(gb, 0);       /* VBlank interrupt */
            gb->vblank_requested = true;
        } else if (next > 153) {
            next = 0;
            g->window_line = 0;
            set_lcd_mode(gb, LCD_MODE_OAM);
            gb->frame_ready = true;
        }

        gb->mmu.io[0x44] = next;

        /* the window line counter advances once per finished line */
        if (ly < 144 && (lcdc & 0x20) && gb->mmu.io[0x4A] <= ly)
            g->window_line++;

        update_lyc(gb);
    }

    /* pick the mode for the current position inside the scanline */
    u8 ly = gb->mmu.io[0x44];
    if (ly < 144) {
        u8 mode = LCD_MODE_HBLANK;
        if (g->cycles < 80)      mode = LCD_MODE_OAM;
        else if (g->cycles < 252) mode = LCD_MODE_VRAM;
        set_lcd_mode(gb, mode);
    }
}
