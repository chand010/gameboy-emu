/*
 * gbemu - a Game Boy emulator in C
 *
 * gb.h - the central "game boy" structure and public API.
 *
 * Everything that makes up a running Game Boy (CPU, memory, cartridge,
 * display, audio, timers, input) lives in one struct so that the code stays
 * easy to follow. There is exactly one dynamic allocation of note: the
 * cartridge ROM and its battery backed RAM. Everything else is fixed size.
 */

#ifndef GB_H
#define GB_H

#include "types.h"

#define GB_SCREEN_W 160
#define GB_SCREEN_H 144

/* CPU clock in Hz and the number of T-cycles that make up one frame. */
#define GB_CLOCK_HZ      4194304
#define GB_CYCLES_FRAME  70224          /* 456 * 154 */

/* ------------------------------------------------------------------ */
/* CPU registers                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    /* Anonymous structs/unions let us read registers on their own (a, f)
     * or as a 16 bit pair (af) without any shifting. This relies on the
     * host being little endian, which is true for every mainstream
     * platform this emulator targets. */
    union { struct { u8 f, a; }; u16 af; };
    union { struct { u8 c, b; }; u16 bc; };
    union { struct { u8 e, d; }; u16 de; };
    union { struct { u8 l, h; }; u16 hl; };
    u16 sp;
    u16 pc;
} regs_t;

typedef struct {
    regs_t r;
    bool ime;          /* interrupt master enable */
    int  ime_delay;    /* EI takes effect one instruction later */
    bool halted;
} cpu_t;

/* ------------------------------------------------------------------ */
/* Cartridge                                                           */
/* ------------------------------------------------------------------ */

#define MBC_NONE 0
#define MBC_MBC1 1
#define MBC_MBC2 2
#define MBC_MBC3 3
#define MBC_MBC5 5

typedef struct {
    u8  *rom;
    size_t rom_size;
    size_t rom_banks;      /* actual 16KB banks present in the file */

    u8   mbc;
    bool has_ram;
    bool has_battery;
    u8   *ram;
    size_t ram_size;
    size_t ram_banks;      /* 8KB banks */

    /* banking state */
    bool ram_enabled;
    u16  rom_bank;         /* current 16KB ROM bank at 0x4000 */
    u8   ram_bank;         /* current 8KB RAM bank at 0xA000 */
    bool mbc1_mode;        /* false = simple, true = advanced */

    /* MBC3 real time clock */
    u8   rtc_select;       /* which RTC register is mapped into RAM area */
    u8   rtc_latch_value;
    bool rtc_latched;
    u64  rtc_latch_time;   /* host time when latch happened (ms) */
    u8   rtc_regs[5];      /* latched sec, min, hour, day-lo, day-hi */

    char *ram_path;        /* where to save/load .sav, may be NULL */
} cart_t;

/* ------------------------------------------------------------------ */
/* MMU / bus                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    u8 vram[0x2000];       /* 0x8000 - 0x9FFF */
    u8 wram[0x2000];       /* 0xC000 - 0xDFFF */
    u8 oam[0xA0];          /* 0xFE00 - 0xFE9F */
    u8 io[0x80];           /* 0xFF00 - 0xFF7F */
    u8 hram[0x7F];         /* 0xFF80 - 0xFFFE */
    u8 ie;                 /* 0xFFFF */
} mmu_t;

/* ------------------------------------------------------------------ */
/* Timer                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    u16 div;               /* internal 16 bit divider */
    u8  tima;
    u8  tma;
    u8  tac;
    int div_accum;         /* T-cycles accumulated for DIV */
    int tima_accum;        /* T-cycles accumulated for TIMA */
    bool tima_overflow;    /* reload TIMA with TMA next step */
} timer_state_t;

/* ------------------------------------------------------------------ */
/* GPU                                                                 */
/* ------------------------------------------------------------------ */

enum {
    LCD_MODE_HBLANK = 0,
    LCD_MODE_VBLANK = 1,
    LCD_MODE_OAM    = 2,
    LCD_MODE_VRAM   = 3,
};

typedef struct {
    u8  mode;              /* current LCD mode */
    int cycles;            /* T-cycles into the current scanline */
    u8  window_line;       /* window internal line counter */
    bool stat_lyc;         /* LYC == LY coincidence flag (STAT bit 2) */
    bool lcd_on;           /* tracks the LCD enable bit transitions */

    u32 framebuffer[GB_SCREEN_W * GB_SCREEN_H];
} gpu_t;

/* ------------------------------------------------------------------ */
/* APU                                                                 */
/* ------------------------------------------------------------------ */

#define APU_SAMPLE_RATE 44100
#define APU_FRAME_MAX_SAMPLES 2048

typedef struct {
    /* channel 1 - square with sweep */
    struct {
        bool on;
        bool dac;
        u8   nr10, nr11, nr12, nr13, nr14;
        u8   volume;
        int  envelope_period;
        int  envelope_timer;
        bool length_on;
        int  length;
        u16  frequency;
        int  timer;         /* counts down to zero -> clock */
        int  duty_index;
        int  sweep_period;
        int  sweep_timer;
        int  sweep_shift;
        bool sweep_negate;
        bool sweep_enabled;
        int  shadow_freq;
    } ch1;

    /* channel 2 - square */
    struct {
        bool on;
        bool dac;
        u8   nr21, nr22, nr23, nr24;
        u8   volume;
        int  envelope_period;
        int  envelope_timer;
        bool length_on;
        int  length;
        u16  frequency;
        int  timer;
        int  duty_index;
    } ch2;

    /* channel 3 - wave */
    struct {
        bool on;
        bool dac;
        u8   nr30, nr31, nr32, nr33, nr34;
        int  volume_shift;
        bool length_on;
        int  length;
        u16  frequency;
        int  timer;
        int  wave_pos;
        u8   wave_ram[16];
    } ch3;

    /* channel 4 - noise */
    struct {
        bool on;
        bool dac;
        u8   nr41, nr42, nr43, nr44;
        u8   volume;
        int  envelope_period;
        int  envelope_timer;
        bool length_on;
        int  length;
        u16  lfsr;
        u16  clock_shift;
        u8   divisor_code;
        bool width_mode;
        u16  frequency;      /* LFSR clock rate in Hz */
        int  timer;          /* sample phase accumulator */
    } ch4;

    u8 nr50, nr51, nr52;
    bool enabled;

    int  frame_seq_cycles;   /* T-cycle accumulator for the 512Hz sequencer */
    int  frame_seq_step;     /* 0..7 */

    int  sample_cycles;      /* T-cycle accumulator between samples */
    s16  frame_buf[APU_FRAME_MAX_SAMPLES];
    int  frame_len;
} apu_t;

/* ------------------------------------------------------------------ */
/* Joypad                                                              */
/* ------------------------------------------------------------------ */

enum {
    JOYP_A      = 0,
    JOYP_B      = 1,
    JOYP_SELECT = 2,
    JOYP_START  = 3,
    JOYP_RIGHT  = 4,
    JOYP_LEFT   = 5,
    JOYP_UP     = 6,
    JOYP_DOWN   = 7,
};

typedef struct {
    u8  p1;                 /* raw P1 register bits (select lines) */
    u8  buttons;            /* bit N set when button N is pressed */
    u8  prev_lines;         /* previous state of the input lines */
} joypad_t;

/* ------------------------------------------------------------------ */
/* The whole machine                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    cpu_t   cpu;
    mmu_t   mmu;
    cart_t  cart;
    timer_state_t timer;
    gpu_t   gpu;
    apu_t   apu;
    joypad_t joypad;

    u8 iflag;               /* interrupt flag register 0xFF0F */

    bool vblank_requested;  /* set when the GPU enters VBlank */
    bool frame_ready;       /* set once a full frame has been drawn */
    u64  total_cycles;
} gb_t;

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

int  gb_init(gb_t *gb, const char *rom_path);
void gb_destroy(gb_t *gb);

int  gb_load_rom(gb_t *gb, const char *rom_path);

u8   gb_read(gb_t *gb, u16 addr);
void gb_write(gb_t *gb, u16 addr, u8 value);

int  gb_run_frame(gb_t *gb);           /* run exactly one frame of cycles */

void cpu_step(gb_t *gb, int *cycles);  /* execute one instruction */

void timer_step(gb_t *gb, int cycles);
void gpu_step(gb_t *gb, int cycles);
void apu_step(gb_t *gb, int cycles);

void gb_request_interrupt(gb_t *gb, int bit);
void gb_press_button(gb_t *gb, int button);
void gb_release_button(gb_t *gb, int button);

#endif /* GB_H */
