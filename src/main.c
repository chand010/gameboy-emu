/*
 * gbemu - a Game Boy emulator in C
 *
 * main.c - machine lifecycle, the SDL2 front end and the command line
 * interface.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "gb.h"
#include "cart.h"
#include "apu.h"
#include "joypad.h"

/* ------------------------------------------------------------------ */
/* Machine lifecycle                                                   */
/* ------------------------------------------------------------------ */

int gb_init(gb_t *gb, const char *rom_path)
{
    memset(gb, 0, sizeof(*gb));

    /* Register state after the DMG boot ROM has run. */
    gb->cpu.r.a = 0x01; gb->cpu.r.f = 0xB0;
    gb->cpu.r.b = 0x00; gb->cpu.r.c = 0x13;
    gb->cpu.r.d = 0x00; gb->cpu.r.e = 0xD8;
    gb->cpu.r.h = 0x01; gb->cpu.r.l = 0x4D;
    gb->cpu.r.sp = 0xFFFE;
    gb->cpu.r.pc = 0x0100;

    gb->mmu.io[0x40] = 0x91;   /* LCDC: LCD on, BG on */
    gb->mmu.io[0x47] = 0xFC;   /* BGP  */
    gb->mmu.io[0x48] = 0xFF;   /* OBP0 */
    gb->mmu.io[0x49] = 0xFF;   /* OBP1 */
    gb->mmu.ie = 0x00;
    gb->iflag = 0x01;          /* VBlank pending, like real hardware */

    joypad_reset(gb);
    apu_reset(gb);

    return cart_load(gb, rom_path);
}

void gb_destroy(gb_t *gb)
{
    cart_destroy(gb);
}

int gb_load_rom(gb_t *gb, const char *rom_path)
{
    return cart_load(gb, rom_path);
}

void gb_request_interrupt(gb_t *gb, int bit)
{
    if (bit >= 0 && bit <= 4)
        gb->iflag |= (u8)(1 << bit);
}

int gb_run_frame(gb_t *gb)
{
    gb->frame_ready = false;

    while (!gb->frame_ready) {
        int c = 0;
        cpu_step(gb, &c);
        timer_step(gb, c);
        gpu_step(gb, c);
        apu_step(gb, c);
        gb->total_cycles += (u64)c;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* SDL2 front end                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    int scale;
    bool mute;
    const char *screenshot_path;
    int screenshot_frames;
} options_t;

static int sdl_button(SDL_Keycode key)
{
    switch (key) {
        case SDLK_z:         return JOYP_B;
        case SDLK_x:         return JOYP_A;
        case SDLK_RETURN:    return JOYP_START;
        case SDLK_BACKSPACE: return JOYP_SELECT;
        case SDLK_RIGHT:     return JOYP_RIGHT;
        case SDLK_LEFT:      return JOYP_LEFT;
        case SDLK_UP:        return JOYP_UP;
        case SDLK_DOWN:      return JOYP_DOWN;
        default:             return -1;
    }
}

static void print_usage(const char *prog)
{
    printf("usage: %s <rom.gb> [options]\n\n", prog);
    printf("options:\n");
    printf("  --scale N          window scale factor (default 4)\n");
    printf("  --mute             disable audio\n");
    printf("  --frames N         exit after N frames\n");
    printf("  --screenshot FILE  save a screenshot after --frames and exit\n");
    printf("\nkeys:\n");
    printf("  arrows = d-pad   Z = B   X = A\n");
    printf("  Enter = start    Backspace = select\n");
    printf("  F12 = save screenshot   Esc = quit\n");
}

static void save_screenshot(gb_t *gb, const char *path)
{
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, GB_SCREEN_W, GB_SCREEN_H,
                                                    32, SDL_PIXELFORMAT_ARGB8888);
    if (!s) {
        fprintf(stderr, "could not create screenshot surface: %s\n", SDL_GetError());
        return;
    }
    memcpy(s->pixels, gb->gpu.framebuffer, GB_SCREEN_W * GB_SCREEN_H * 4);
    SDL_SaveBMP(s, path);
    SDL_FreeSurface(s);
    printf("screenshot written to %s\n", path);
}

int main(int argc, char **argv)
{
    options_t opt = { .scale = 4, .mute = false,
                      .screenshot_path = NULL, .screenshot_frames = 0 };
    const char *rom_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            opt.scale = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--mute") == 0) {
            opt.mute = true;
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            opt.screenshot_frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            opt.screenshot_path = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (!rom_path) {
            rom_path = argv[i];
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!rom_path) {
        print_usage(argv[0]);
        return 1;
    }

    if (opt.scale < 1) opt.scale = 1;
    if (opt.scale > 8) opt.scale = 8;

    /* screenshots can run without a display */
    if (opt.screenshot_path)
        setenv("SDL_VIDEODRIVER", "dummy", 1);
    if (opt.screenshot_path || opt.mute)
        setenv("SDL_AUDIODRIVER", "dummy", 1);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    atexit(SDL_Quit);

    gb_t gb;
    if (gb_init(&gb, rom_path) < 0) {
        fprintf(stderr, "failed to load ROM\n");
        return 1;
    }

    /* video */
    SDL_Window *window = SDL_CreateWindow("gbemu",
                                          SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          GB_SCREEN_W * opt.scale, GB_SCREEN_H * opt.scale,
                                          SDL_WINDOW_SHOWN);
    if (!window) {
        fprintf(stderr, "could not create window: %s\n", SDL_GetError());
        gb_destroy(&gb);
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        fprintf(stderr, "could not create renderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        gb_destroy(&gb);
        return 1;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");   /* crisp pixels */
    SDL_RenderSetLogicalSize(renderer, GB_SCREEN_W, GB_SCREEN_H);

    SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                             SDL_TEXTUREACCESS_STREAMING,
                                             GB_SCREEN_W, GB_SCREEN_H);
    if (!texture) {
        fprintf(stderr, "could not create texture: %s\n", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        gb_destroy(&gb);
        return 1;
    }

    /* audio */
    SDL_AudioDeviceID audio_dev = 0;
    if (!opt.mute) {
        SDL_AudioSpec want, have;
        SDL_zero(want);
        want.freq = APU_SAMPLE_RATE;
        want.format = AUDIO_S16SYS;
        want.channels = 1;
        want.samples = 1024;
        audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (audio_dev == 0)
            fprintf(stderr, "audio unavailable: %s\n", SDL_GetError());
        else
            SDL_PauseAudioDevice(audio_dev, 0);
    }

    printf("gbemu running: %s  (scale %d, audio %s)\n",
           rom_path, opt.scale, audio_dev ? "on" : "off");

    bool running = true;
    u64 frame_count = 0;
    u64 freq = SDL_GetPerformanceFrequency();
    u64 frame_start = SDL_GetPerformanceCounter();

    while (running) {
        /* input */
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
                case SDL_QUIT:
                    running = false;
                    break;
                case SDL_KEYDOWN:
                    if (ev.key.keysym.sym == SDLK_ESCAPE) {
                        running = false;
                    } else if (ev.key.keysym.sym == SDLK_F12 && !opt.screenshot_path) {
                        save_screenshot(&gb, "gbemu.bmp");
                    } else {
                        int btn = sdl_button(ev.key.keysym.sym);
                        if (btn >= 0 && !ev.key.repeat)
                            gb_press_button(&gb, btn);
                    }
                    break;
                case SDL_KEYUP:
                    if (ev.key.keysym.sym == SDLK_ESCAPE)
                        break;
                    {
                        int btn = sdl_button(ev.key.keysym.sym);
                        if (btn >= 0)
                            gb_release_button(&gb, btn);
                    }
                    break;
                default:
                    break;
            }
        }

        gb_run_frame(&gb);
        frame_count++;

        /* present */
        SDL_UpdateTexture(texture, NULL, gb.gpu.framebuffer, GB_SCREEN_W * 4);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);

        /* audio */
        if (audio_dev && gb.apu.frame_len > 0) {
            SDL_QueueAudio(audio_dev, gb.apu.frame_buf,
                           (u32)gb.apu.frame_len * sizeof(s16));
            gb.apu.frame_len = 0;
        }

        /* screenshot mode */
        if (opt.screenshot_path && (int)frame_count >= opt.screenshot_frames) {
            save_screenshot(&gb, opt.screenshot_path);
            running = false;
        } else if (opt.screenshot_frames > 0 &&
                   !opt.screenshot_path && frame_count >= (u64)opt.screenshot_frames) {
            running = false;
        }

        /* keep roughly 60 fps */
        u64 now = SDL_GetPerformanceCounter();
        double elapsed = (double)(now - frame_start) / (double)freq;
        frame_start = now;
        double target = 1.0 / 60.0;
        if (elapsed < target) {
            SDL_Delay((u32)((target - elapsed) * 1000.0));
        }
    }

    if (audio_dev)
        SDL_CloseAudioDevice(audio_dev);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);

    gb_destroy(&gb);
    printf("bye\n");
    return 0;
}
