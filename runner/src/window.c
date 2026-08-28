/* window.c — SDL2 front end.
 *
 * Boots a title under the interpreter and presents what the video hardware
 * currently holds, one emulated frame at a time. The composite is deliberately
 * simple for now: VDP2 back screen, then the VDP1 display framebuffer over it.
 * That is enough to see the moment the game starts drawing, which is the point
 * -- this is a debugging window that happens to be the eventual game window.
 *
 * The title bar carries live state (PC, instruction count, fault reason) so no
 * font rendering is needed to know what the machine is doing.
 */
#include "saturn.h"
#include "disc.h"
#include "game_config.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

void bios_hle_install(saturn *s);
int  bios_rom_load(saturn *s, const char *path, char *err, size_t errsz);
void bios_reset_vector(saturn *s, uint32_t *pc, uint32_t *sp);

#define FB_W 512
#define FB_H 256

/* Panel size: framebuffer + palette/VRAM columns + a text readout. */
#define PANEL_W 784
#define PANEL_H 372

static saturn g_sys;
static uint32_t g_pixels[PANEL_W * PANEL_H];
/* The real output: the VDP2 composite at its native size, scaled to the
 * window. The debug panel is still available on F1, but it is a development
 * view -- what a Saturn actually puts on the screen is this. */
static uint32_t g_frame[704 * 512];
static int      g_debug;

/* Saturn RGB555 (bit 15 = opaque) -> ARGB8888. */
static uint32_t rgb555(uint16_t p)
{
    uint32_t r = (p      ) & 0x1F;
    uint32_t g = (p >>  5) & 0x1F;
    uint32_t b = (p >> 10) & 0x1F;
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* VDP2 back-screen colour. BKTAU/BKTAL (0x25F800AC/AE) point into VDP2 VRAM. */
static uint32_t back_colour(saturn *s)
{
    uint16_t bktau = s->vdp2_reg[0xAC >> 1];
    uint16_t bktal = s->vdp2_reg[0xAE >> 1];
    uint32_t addr  = (((uint32_t)(bktau & 0x7) << 16) | bktal) << 1;
    if (addr + 1 < VDP2_VRAM_SZ) {
        uint16_t c = (uint16_t)((s->vdp2_vram[addr] << 8) | s->vdp2_vram[addr + 1]);
        if (c) return rgb555(c);
    }
    return 0xFF000000u;
}

static void compose(saturn *s)
{
    /* The buffer being displayed is the one that is not the draw target. */
    const uint8_t *fb = s->vdp1_fb[s->fb_draw ^ 1];
    uint32_t back = back_colour(s);

    for (int y = 0; y < FB_H; y++) {
        for (int x = 0; x < FB_W; x++) {
            uint32_t o = (uint32_t)(y * FB_W + x) * 2u;
            uint16_t p = (uint16_t)((fb[o] << 8) | fb[o + 1]);
            g_pixels[y * FB_W + x] = p ? rgb555(p) : back;
        }
    }
}

/* Display size from VDP2 TVMD (0x25F80000). */

/* Keyboard -> Saturn digital pad.
 *
 * INTBACK peripheral data, the form the BIOS and games actually read
 * (smpc.c intback_peripheral inverts these, so here they are ACTIVE HIGH):
 *   byte 1 (pad1_lo): b7 Right b6 Left b5 Down b4 Up b3 Start b2 A b1 C b0 B
 *   byte 2 (pad1_hi): b7 R     b6 X    b5 Y    b4 Z  b3 L
 */
/* The gamepad, if one is plugged in. SDL's GameController layer sits on top of
 * XInput on Windows, so any 360/One/generic pad reports the same button names
 * and no per-device mapping is needed here.
 *
 * Face layout: a Saturn pad is two rows of three (X Y Z over A B C) plus two
 * shoulders, which does not fit a modern diamond exactly. The mapping below
 * keeps A/B/X/Y where a player expects them and puts Saturn C and Z on the
 * shoulder buttons, with the analogue triggers left for Saturn L/R:
 *
 *     Saturn  A B C   X Y Z   L  R    Start
 *     Pad     A B RB  X Y LB  LT RT   Start
 *
 * SATURN_PAD=0 ignores controllers entirely. */
static SDL_GameController *g_pad;

static void pad_open_first(void)
{
    int i;
    if (g_pad) return;
    for (i = 0; i < SDL_NumJoysticks(); i++) {
        if (!SDL_IsGameController(i)) continue;
        g_pad = SDL_GameControllerOpen(i);
        if (g_pad) {
            fprintf(stderr, "[pad] %s connected\n",
                    SDL_GameControllerName(g_pad));
            return;
        }
    }
}

static void pad_close(SDL_JoystickID which)
{
    if (!g_pad) return;
    if (SDL_JoystickInstanceID(
            SDL_GameControllerGetJoystick(g_pad)) != which) return;
    SDL_GameControllerClose(g_pad);
    g_pad = NULL;
    fprintf(stderr, "[pad] disconnected\n");
}

static void poll_pad(saturn *s)
{
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    uint8_t lo = 0, hi = 0;

    if (k[SDL_SCANCODE_RIGHT] || k[SDL_SCANCODE_D]) lo |= 0x80;
    if (k[SDL_SCANCODE_LEFT]  || k[SDL_SCANCODE_A]) lo |= 0x40;
    if (k[SDL_SCANCODE_DOWN]  || k[SDL_SCANCODE_S]) lo |= 0x20;
    if (k[SDL_SCANCODE_UP]    || k[SDL_SCANCODE_W]) lo |= 0x10;
    if (k[SDL_SCANCODE_RETURN])                     lo |= 0x08;  /* Start */
    if (k[SDL_SCANCODE_Z])                          lo |= 0x04;  /* A     */
    if (k[SDL_SCANCODE_C])                          lo |= 0x02;  /* C     */
    if (k[SDL_SCANCODE_X])                          lo |= 0x01;  /* B     */

    if (k[SDL_SCANCODE_E])                          hi |= 0x80;  /* R     */
    if (k[SDL_SCANCODE_R])                          hi |= 0x40;  /* X     */
    if (k[SDL_SCANCODE_T])                          hi |= 0x20;  /* Y     */
    if (k[SDL_SCANCODE_Y])                          hi |= 0x10;  /* Z     */
    if (k[SDL_SCANCODE_Q])                          hi |= 0x08;  /* L     */

    /* Controller input is OR-ed onto the keyboard, so both stay live and a
     * player can use either without a mode switch. */
    if (g_pad) {
        #define BTN(b) SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_##b)
        /* The stick reads as a direction too: a digital pad is all the SMPC
         * reports today, so an analogue title still needs a usable D-pad.
         * The dead zone is SDL's recommended ~8000 of 32767. */
        Sint16 ax = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX);
        Sint16 ay = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTY);
        const Sint16 dead = 8000;

        if (BTN(DPAD_RIGHT) || ax >  dead) lo |= 0x80;
        if (BTN(DPAD_LEFT)  || ax < -dead) lo |= 0x40;
        if (BTN(DPAD_DOWN)  || ay >  dead) lo |= 0x20;
        if (BTN(DPAD_UP)    || ay < -dead) lo |= 0x10;
        if (BTN(START))                    lo |= 0x08;
        if (BTN(A))                        lo |= 0x04;   /* Saturn A */
        if (BTN(RIGHTSHOULDER))            lo |= 0x02;   /* Saturn C */
        if (BTN(B))                        lo |= 0x01;   /* Saturn B */

        if (BTN(X))                        hi |= 0x40;   /* Saturn X */
        if (BTN(Y))                        hi |= 0x20;   /* Saturn Y */
        if (BTN(LEFTSHOULDER))             hi |= 0x10;   /* Saturn Z */
        /* Triggers are axes, not buttons: half-pressed counts as pressed. */
        if (SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT)
            > 16000) hi |= 0x08;                          /* Saturn L */
        if (SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
            > 16000) hi |= 0x80;                          /* Saturn R */
        #undef BTN
    }

    s->pad1_lo = lo;
    s->pad1_hi = hi;
}

static void display_size(saturn *s, int *w, int *h)
{
    uint16_t tvmd = s->vdp2_reg[0];
    static const int hw[4] = { 320, 352, 640, 704 };
    static const int vh[4] = { 224, 240, 256, 256 };
    *w = hw[tvmd & 1 ? 1 : 0];
    *h = vh[(tvmd >> 4) & 3];
    if (*w <= 0 || *w > FB_W) *w = 320;
    if (*h <= 0 || *h > FB_H) *h = 224;
}


/* SDL audio callback: runs on SDL's thread, so it only moves the ring's READ
 * pointer while the runtime thread only moves the write pointer. Never alter
 * the source rate here: adaptive resampling hid underruns by changing pitch.
 * Buffer a short lead, drain at the SCSP's exact 44.1 kHz, and ramp only the
 * edge of a genuine underrun so it cannot become a full-scale click. */
/* SATURN_SNDBUF=<frames>: audio lead in frames. Lower = less latency, more
 * risk of an underrun ramp; higher = smoother under load but laggier. */
static uint32_t snd_target(void)
{
    static uint32_t v;
    if (!v) {
        const char *e = getenv("SATURN_SNDBUF");
        v = e ? (uint32_t)strtoul(e, NULL, 0) : SND_TARGET;
        if (v < 256u)  v = 256u;
        if (v > 8192u) v = 8192u;
    }
    return v;
}

static void audio_cb(void *user, Uint8 *stream, int len)
{
    saturn *s = (saturn *)user;
    uint32_t frames = (uint32_t)len / 4u;         /* stereo, 16-bit */
    int16_t *out = (int16_t *)stream;
    static int primed;
    uint32_t fill = (s->snd_wp + SND_RING - s->snd_rp) % SND_RING;
    uint32_t got, i;
    uint32_t target = snd_target();

    if (!primed) {
        if (fill < target) {
            SDL_memset(stream, 0, (size_t)len);
            return;
        }
        primed = 1;
    }

    got = sound_drain(s, out, frames);
    if (got < frames) {
        uint32_t fade = frames - got;
        int16_t last_l = got ? out[(got - 1u) * 2u + 0u] : 0;
        int16_t last_r = got ? out[(got - 1u) * 2u + 1u] : 0;
        if (fade > 64u) fade = 64u;
        for (i = 0; i < fade; i++) {
            uint32_t gain = fade - i - 1u;
            out[(got + i) * 2u + 0u] = (int16_t)((int32_t)last_l * (int32_t)gain / (int32_t)fade);
            out[(got + i) * 2u + 1u] = (int16_t)((int32_t)last_r * (int32_t)gain / (int32_t)fade);
        }
        SDL_memset(out + (got + fade) * 2u, 0,
                   (size_t)(frames - got - fade) * 4u);
        primed = 0;
    }
}


int main(int argc, char **argv)
{
    game_config g;
    disc d;
    iso_fs fs;
    saturn_ip ip;
    saturn *s = &g_sys;
    const iso_entry *first_read = NULL;
    void *image = NULL;
    size_t image_size = 0;
    uint32_t load_addr = 0, entry = 0, sp;

    SDL_Window   *win;
    SDL_Renderer *ren;
    SDL_Texture  *tex;
    SDL_Texture  *frametex = NULL;
    int           g_texw = 0, g_texh = 0;
    int running = 1, paused = 0;
    int no_bios = 0;
    int bios_boot = 0;
    uint64_t frame = 0, prof_next = 180, prof_frame_mark = 0;
    SDL_AudioDeviceID audio_dev = 0;

    /* Field pacing state. See the comment on the pacer in the main loop. */
    uint64_t perf_freq = 1, prev_tick = 0, fps_mark = 0, fps_count = 0, prof_mark = 0;
    double   accum = 0.0, field_secs = 1.0 / 59.94, cur_fps = 0.0;
    int      have_frame = 0, uncapped = 0, headless = 0, profile = 0;
    uint64_t max_frames = 0;

    if (argc < 2) {
        fprintf(stderr,
            "usage: saturnwin <games/<name>/game.toml> [nobios]\n"
            "  space = pause/resume, f = single frame, esc = quit\n");
        return 2;
    }
    /* A field is LINES_TOTAL scanlines of machine time now, so there is no
     * instructions-per-frame knob: SATURN_SLICE tunes the interleave. */
    for (int k = 1; k < argc; k++)
        if (!strcmp(argv[k], "nobios")) no_bios = 1;

    if (gc_load(&g, argv[1]) != 0) { fprintf(stderr, "error: %s\n", g.err); return 1; }
    if (disc_open(&d, g.disc) != 0) { fprintf(stderr, "error: %s\n", d.err); return 1; }
    if (ip_read(&d, &ip) != 0) { fprintf(stderr, "error: not a Saturn disc\n"); return 1; }
    s->area_code = getenv("SATURN_AREA")
                 ? (uint8_t)strtoul(getenv("SATURN_AREA"), NULL, 0)
                 : saturn_area_from_ip(ip.area);
    if (iso_read(&d, &fs) != 0) { fprintf(stderr, "error: %s\n", d.err); return 1; }

    for (int i = 0; i < g.nmodules && !first_read; i++) {
        if (!g.modules[i].is_first_read) continue;
        for (int j = 0; j < fs.nentries; j++)
            if (!fs.entries[j].is_dir &&
                strcmp(fs.entries[j].path, g.modules[i].file) == 0) {
                first_read = &fs.entries[j];
                load_addr  = g.modules[i].load_addr ? g.modules[i].load_addr
                                                    : ip.first_read_addr;
                entry      = g.modules[i].entry ? g.modules[i].entry : load_addr;
                break;
            }
    }
    if (!first_read) { fprintf(stderr, "error: no first_read module on disc\n"); return 1; }

    image = iso_extract(&d, first_read, &image_size);
    if (!image) { fprintf(stderr, "error: cannot read %s\n", first_read->path); return 1; }

    saturn_init(s);
    cdb_init(s, &d, &fs);
    /* Keep the windowed runner's battery-backed SMPC state in the same
     * per-game location as saturnboot.  The two binaries have separate mains,
     * so boot.c's persistence setup does not apply here. */
    {
        const char *toml = argv[1];
        const char *slash = strrchr(toml, '/');
        const char *bslash = strrchr(toml, 92);   /* backslash */
        const char *cut = slash > bslash ? slash : bslash;
        size_t dirlen = cut ? (size_t)(cut - toml) + 1 : 0;
        if (dirlen && dirlen < sizeof(s->smpc_state_path) - 16) {
            memcpy(s->smpc_state_path, toml, dirlen);
            strcpy(s->smpc_state_path + dirlen, "smpc.bin");
        } else {
            strcpy(s->smpc_state_path, "smpc.bin");
        }
        smpc_persist_load(s);
    }
    if (g.bios[0] && !no_bios) {
        char err[256];
        if (bios_rom_load(s, g.bios, err, sizeof(err)) != 0) {
            fprintf(stderr, "error: %s\n", err);
            return 1;
        }
        bios_reset_vector(s, &entry, &sp);
        bios_boot = 1;
        printf("BIOS %s -> reset PC 0x%08X SP 0x%08X\n", g.bios, entry, sp);
    } else {
        bios_hle_install(s);
        sp = ip.stack_m ? ip.stack_m : 0x06100000u;
        printf("no BIOS configured - using HLE stubs\n");
    }
    /* Only stand in for the IPL when there is no real BIOS. On the BIOS
     * path the IPL fetches the 1st-read itself over the CD block, and
     * pre-loading 447KB at 0x06004000 lands straight on the work area the
     * IPL is using for its own code and literals -- the window booted the
     * game over the BIOS and showed black. boot.c has always got this
     * right; this loop never had the test. */
    if (!bios_boot)
        for (size_t i = 0; i < image_size; i++)
            bus_w8(s, load_addr + (uint32_t)i, ((const uint8_t *)image)[i]);

    sh2_reset(&s->master, s, 0, entry, sp);

    /* Hybrid boot, same as boot.c: run the real BIOS until its work area is
     * built, then stand in for the IPL -- load the first-read image, stub the
     * SCU user vectors (they point into the CD player we just overwrote),
     * and jump to the game with V-Blank enabled. */
    if (bios_boot && getenv("SATURN_HANDOFF")) {
        uint32_t stop = (uint32_t)strtoul(getenv("SATURN_HANDOFF"), NULL, 0);
        uint64_t guard = 0;
        sh2 *c = &s->master;
        while (guard < (uint64_t)stop && !c->halted)
            guard += saturn_run_field(s);
        printf("handoff at BIOS PC 0x%08X after %llu instructions\n",
               c->pc, (unsigned long long)guard);
        for (size_t i = 0; i < image_size; i++)
            bus_w8(s, load_addr + (uint32_t)i, ((const uint8_t *)image)[i]);
        if (!getenv("SATURN_NOPROTECT")) {
            s->prot_lo = load_addr & 0x0FFFFFFFu;
            s->prot_hi = s->prot_lo + (uint32_t)image_size;
        }
        entry = load_addr;
        sp    = ip.stack_m ? ip.stack_m : 0x06100000u;
        if (!getenv("SATURN_NOSTUBVEC")) {
            const uint32_t stub = 0x06000F00u;
            bus_w16(s, stub + 0, 0x000B);   /* rts */
            bus_w16(s, stub + 2, 0x0009);   /* nop */
            for (uint32_t v = 0x40; v <= 0x5F; v++)
                bus_w32(s, 0x06000900u + v * 4u, stub);
        }
        s->scu_ipend = 0;
        s->scu_reg[0xA0 >> 2] = 0xFFFFFFFFu & ~0x3u;
        c->sr &= ~SR_I;
        c->pc = entry;
        c->r[15] = sp;
        bios_boot = 0;   /* the window now runs the GAME, not the BIOS UI */
    }
    free(image);

    printf("%s\n", g.name);
    if (bios_boot)
        printf("1st-read left to the IPL (CD block serves it)\n");
    else
        printf("loaded %s (%zu bytes) at 0x%08X, entry 0x%08X\n",
               first_read->path, image_size, load_addr, entry);
    printf("space = pause, f = step one frame, esc = quit\n");

    /* Windows otherwise permits a 15.6 ms scheduler tick, large enough to
     * miss an entire Saturn field when the pacer asks for a 1 ms yield. */
    SDL_SetHint(SDL_HINT_TIMER_RESOLUTION, "1");
    /* GAMECONTROLLER pulls in the joystick subsystem too. A missing or broken
     * controller driver must not stop the emulator from running, so this is a
     * separate, non-fatal init rather than a flag on the one above. */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (!getenv("SATURN_PAD") || strcmp(getenv("SATURN_PAD"), "0") != 0) {
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0)
            fprintf(stderr, "[pad] controllers unavailable: %s\n", SDL_GetError());
        else
            pad_open_first();
    }
    /* Open at 4:3. PANEL_W/PANEL_H size the DEBUG panel (784x372, about 2.1:1);
     * using them for the window made every game open stretched wide. */
    win = SDL_CreateWindow("SaturnRecomp", SDL_WINDOWPOS_CENTERED,
                           SDL_WINDOWPOS_CENTERED, 960, 720,
                           SDL_WINDOW_RESIZABLE);
    /* The field pacer below already targets 59.94 Hz. A second host-display
     * vsync wait turns a slightly-late field into a full missed refresh and
     * can quantize 50-55 fps of work down to 30 fps. */
    if (!win) {
        fprintf(stderr, "SDL window setup failed: %s\n", SDL_GetError());
        return 1;
    }
#ifdef _WIN32
    /* Real-time presentation competes poorly with compilers, indexers and
     * sync clients at the default process class. Above Normal is deliberately
     * below HIGH/REALTIME, but gives the field/audio deadlines precedence over
     * background work. */
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
#endif
    /* SATURN_RENDERER=opengl|software|direct3d picks the SDL backend.
     *
     * This exists for SCREEN CAPTURE. SDL defaults to Direct3D on Windows, and
     * OBS's plain "Window Capture (BitBlt)" cannot read a D3D-composed window
     * -- it records a black rectangle. An OpenGL or software window is drawn
     * through GDI and captures fine. (The alternative, for anyone who wants to
     * keep D3D, is to set OBS's capture method to "Windows 10 (1903+)".) */
    {
        const char *rd = getenv("SATURN_RENDERER");
        if (rd && *rd) SDL_SetHint(SDL_HINT_RENDER_DRIVER, rd);
    }
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    /* Headless profiling and remote desktops may expose no accelerated
     * renderer.  Software presentation is slower, but it is a functional
     * fallback and lets the same production loop be measured unattended. */
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    {   /* name it, so a capture problem is diagnosable from the log */
        SDL_RendererInfo ri;
        if (ren && SDL_GetRendererInfo(ren, &ri) == 0)
            fprintf(stderr, "[video] renderer: %s\n", ri.name);
    }
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                            SDL_TEXTUREACCESS_STREAMING, PANEL_W, PANEL_H);
    if (!ren || !tex) {
        fprintf(stderr, "SDL setup failed: %s\n", SDL_GetError());
        return 1;
    }

    /* Audio. The core fills a ring from the scheduler and the callback drains
     * it at the SCSP's exact 44.1 kHz.  Pitch is never altered to hide a slow
     * field; the field path is optimized to remain ahead of the device. */
    {
        SDL_AudioSpec want, got;
        SDL_memset(&want, 0, sizeof want);
        want.freq     = 44100;
        want.format   = AUDIO_S16SYS;
        want.channels = 2;
        /* Device buffer: 512 frames is ~12 ms, half the previous 1024.
         * Combined with the smaller SND_TARGET this roughly halves the
         * input-to-sound delay. */
        want.samples  = 512;
        want.callback = audio_cb;
        want.userdata = s;
        /* allowed_changes = 0 makes SDL hand back EXACTLY this format and
         * build a converter if the device disagrees. The old call passed a
         * non-NULL `obtained` to SDL_OpenAudio, which does the opposite: it
         * lets the device dictate terms and returns its native spec with no
         * conversion layer. On WASAPI that is 48 kHz float32, so our signed
         * 16-bit frames were being read as floats -- bit patterns that land
         * in the denormal range, i.e. inaudible. That is exactly why
         * SATURN_WAV had audio in it while the window was silent. */
        audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
        if (!audio_dev)
            fprintf(stderr, "audio unavailable: %s (running silent)\n", SDL_GetError());
        else {
            fprintf(stderr, "[audio] device %u: %d Hz %d ch fmt 0x%04X buf %u\n",
                    (unsigned)audio_dev, got.freq, got.channels,
                    (unsigned)got.format, (unsigned)got.samples);
            SDL_PauseAudioDevice(audio_dev, 0);
        }
    }

    perf_freq = SDL_GetPerformanceFrequency();
    if (!perf_freq) perf_freq = 1;
    prev_tick = SDL_GetPerformanceCounter();
    fps_mark  = prev_tick;
    prof_mark = prev_tick;
    uncapped  = getenv("SATURN_UNCAP") != NULL;
    headless  = getenv("SATURN_HEADLESS") != NULL;
    profile   = getenv("SATURN_PROF") != NULL;
    if (getenv("SATURN_MAX_FRAMES"))
        max_frames = strtoull(getenv("SATURN_MAX_FRAMES"), NULL, 0);
    { const char *fpsenv = getenv("SATURN_FPS");
      if (fpsenv) { double v = atof(fpsenv); if (v > 1.0) field_secs = 1.0 / v; } }

    frt_irq_init();
    while (running) {
        SDL_Event e;
        int step_one = 0, advanced;   /* fields run this iteration */
        double pace_period = field_secs;

        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = 0;
            /* Hot-plug: a pad plugged in mid-session should just start
             * working, and unplugging one must not leave a dangling handle. */
            if (e.type == SDL_CONTROLLERDEVICEADDED)   pad_open_first();
            if (e.type == SDL_CONTROLLERDEVICEREMOVED) pad_close(e.cdevice.which);
            if (e.type == SDL_KEYDOWN) {
                switch (e.key.keysym.sym) {
                case SDLK_ESCAPE: running = 0; break;
                case SDLK_SPACE:  paused = !paused; break;
                case SDLK_f:      step_one = 1; break;
                case SDLK_F1:     g_debug = !g_debug; break;
                default: break;
                }
            }
        }

        poll_pad(s);

        /* Pace the machine off a wall clock, not off the host's vertical
         * blank. The renderer is created with PRESENTVSYNC, so with no pacer
         * at all this loop advanced exactly one Saturn field per monitor
         * refresh -- on a 144 Hz display that is 144 fields/sec, 2.4x too
         * fast, and it fed the 44.1 kHz ring at the same 2.4x, so the writer
         * permanently lapped the reader. */
        {
            uint64_t now = SDL_GetPerformanceCounter();
            double   dt  = (double)(now - prev_tick) / (double)perf_freq;
            prev_tick = now;
            if (dt > 0.25) dt = 0.25;   /* after a stall, resync, do not sprint */
            accum += dt;
        }

        advanced = 0;
        if (paused && !step_one) {
            accum = 0.0;
        } else if (step_one) {
            saturn_run_field(s); frame++; advanced++; accum = 0.0;
        } else {
            /* Track the audio device's real clock instead of drifting against
             * it: nudge the field period by at most 1% to steer the ring
             * toward a target fill. Without this the host's 44.1 kHz and our
             * 59.94 Hz are two free-running crystals, and the ring slowly
             * walks into either a permanent underrun or a permanent drop. */
            double period = field_secs;
            int    budget = 4;
            if (audio_dev) {
                uint32_t fill = (s->snd_wp + SND_RING - s->snd_rp) % SND_RING;
                double   tgt  = (double)snd_target();
                double   err  = ((double)fill - tgt) / tgt;
                if (err >  1.0) err =  1.0;
                if (err < -1.0) err = -1.0;
                period = field_secs * (1.0 + 0.01 * err);
            }
            pace_period = period;
            if (uncapped) { saturn_run_field(s); frame++; advanced++; }
            else while (accum >= period && budget-- > 0) {
                saturn_run_field(s);
                frame++; advanced++;
                accum -= period;
            }
            /* Too far behind to ever catch up: drop the debt rather than
             * spiral, so a slow patch plays late but never compounds. */
            if (accum > period * 4.0) accum = 0.0;
        }

        {   /* achieved field rate, so "is it keeping up?" is visible */
            uint64_t now = SDL_GetPerformanceCounter();
            double   el  = (double)(now - fps_mark) / (double)perf_freq;
            fps_count += (uint64_t)advanced;
            if (el >= 0.5) {
                cur_fps = (double)fps_count / el;
                fps_count = 0; fps_mark = now;
            }
        }

        /* SATURN_PROF in the window: the headless runner can only ever profile
         * a BIOS menu, because getting a game to gameplay needs input. This
         * reports the same master/slave/video split against a real playing
         * frame, which is the only place the answer is interesting. */
        if (profile && frame >= prof_next && advanced) {
            uint64_t pnow = SDL_GetPerformanceCounter();
            double pel = (double)(pnow - prof_mark) / (double)perf_freq;
            double interval_fps = pel > 0.0
                ? (double)(frame - prof_frame_mark) / pel : 0.0;
            double m = (double)s->prof_master, sl = (double)s->prof_slave;
            double v = (double)s->prof_video, oth = (double)s->prof_other;
            double tot = m + sl + v + oth;
            if (tot < 1) tot = 1;
            fprintf(stderr, "[prof] %.1f fps | master %.1f%% slave %.1f%% video %.1f%% sound/cd %.1f%% "
                    "| fast %.1f%% | %dx%d\n", interval_fps,
                    100.0 * m / tot, 100.0 * sl / tot, 100.0 * v / tot,
                    100.0 * oth / tot,
                    100.0 * (double)s->fastpath_hits /
                        (double)(s->fastpath_hits + s->slowpath_hits + 1),
                    g_texw, g_texh);
            s->prof_master = s->prof_slave = s->prof_video = s->prof_other = 0;
            s->fastpath_hits = s->slowpath_hits = 0;
            if (getenv("SATURN_OPHIST")) sh2_report_ophist(stderr);
            prof_mark = pnow;
            prof_frame_mark = frame;
            do { prof_next += 180; } while (prof_next <= frame);
        }

        if (g_debug) {
            debugview_render(s, g_pixels, PANEL_W, PANEL_H);
            SDL_UpdateTexture(tex, NULL, g_pixels, PANEL_W * (int)sizeof(uint32_t));
        } else if (advanced || !have_frame) {
            /* Compositing is the most expensive thing this loop does, so only
             * do it when a field actually advanced. On a refresh faster than
             * 59.94 Hz the extra presents below just re-show this texture. */
            int dw, dh;
            vdp2_display_size(s, &dw, &dh);
            if (dw < 1) dw = 320;
            if (dh < 1) dh = 224;
            if (dw > 704) dw = 704;
            if (dh > 512) dh = 512;
            if (profile) {
                uint64_t tv = __rdtsc();
                vdp2_render(s, g_frame, dw, dh, 1);
                s->prof_video += __rdtsc() - tv;
            } else {
                vdp2_render(s, g_frame, dw, dh, 1);
            }
            if (dw != g_texw || dh != g_texh) {
                if (frametex) SDL_DestroyTexture(frametex);
                frametex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                             SDL_TEXTUREACCESS_STREAMING, dw, dh);
                g_texw = dw; g_texh = dh;
            }
            SDL_UpdateTexture(frametex, NULL, g_frame, dw * (int)sizeof(uint32_t));
            have_frame = 1;
        }
        if (!headless) {
            SDL_Texture *show = g_debug ? tex : frametex;
            SDL_RenderClear(ren);
            if (show) {
                /* Always present the game at the Saturn's 4:3 DISPLAY aspect,
                 * pillar/letterboxed inside whatever the window happens to be.
                 * The framebuffer is 320x224, 352x240 or 704x480 depending on
                 * mode -- none of those are 4:3 in square pixels -- so copying
                 * to the full window stretched every one of them differently.
                 * The debug panel keeps its own layout and is drawn as-is. */
                SDL_Rect dst;
                int ww = 0, wh = 0;
                SDL_GetRendererOutputSize(ren, &ww, &wh);
                if (g_debug) {
                    dst.x = dst.y = 0; dst.w = ww; dst.h = wh;
                } else if (ww * 3 <= wh * 4) {      /* window taller than 4:3 */
                    dst.w = ww; dst.h = ww * 3 / 4;
                } else {                            /* window wider than 4:3 */
                    dst.h = wh; dst.w = wh * 4 / 3;
                }
                if (!g_debug) { dst.x = (ww - dst.w) / 2; dst.y = (wh - dst.h) / 2; }
                SDL_RenderCopy(ren, show, NULL, &dst);
            }
        }
        if (!headless) SDL_RenderPresent(ren);
        /* One coarse sleep plus a short deadline spin. Repeated Delay(1)
         * calls accumulated rounding/dispatch overhead and paced a 59.94 Hz
         * target at 54-57 Hz even with >70 Hz of measured core capacity. */
        if (!advanced && !uncapped && !paused) {
            uint64_t now = SDL_GetPerformanceCounter();
            double effective = accum + (double)(now - prev_tick) / (double)perf_freq;
            double remain = pace_period - effective;
            if (remain > 0.0) {
                uint64_t deadline = now + (uint64_t)(remain * (double)perf_freq);
                /* Leave 3 ms for the precise phase: SDL's nominal 1 ms sleep
                 * still overshoots by about 0.7-1.0 ms on this Windows host. */
                if (remain > 0.004) {
                    Uint32 ms = (Uint32)((remain - 0.003) * 1000.0);
                    if (ms) SDL_Delay(ms);
                }
                while (SDL_GetPerformanceCounter() < deadline) {
#ifdef _WIN32
                    YieldProcessor();
#endif
                }
            }
        }

        if (!headless) {
            sh2 *c = &s->master;
            char title[320];
            snprintf(title, sizeof(title),
                     "SaturnRecomp - %s | %.1f/%.2f fps | %dx%d | frame %llu | PC %08X | %llu instr | %s%s",
                     bios_boot ? "BIOS boot (disc inserted)" : g.name,
                     cur_fps, 1.0 / field_secs, g_texw, g_texh,
                     (unsigned long long)frame, c->pc,
                     (unsigned long long)c->cycles,
                     c->halted ? "HALTED: " : (paused ? "paused" : "running"),
                     c->halted ? c->fault : "");
            SDL_SetWindowTitle(win, title);
        }
        /* Clean, deterministic exit for PGO training and automated visual
         * runs.  Exiting through the normal tail matters: GCC writes its
         * profile counters at process shutdown, while force-killing a helper
         * loses the workload data we just collected. */
        if (max_frames && frame >= max_frames) running = 0;
    }

    printf("\nstopped at PC=0x%08X after %llu instructions%s%s\n",
           s->master.pc, (unsigned long long)s->master.cycles,
           s->master.halted ? " - HALTED: " : "",
           s->master.halted ? s->master.fault : "");
    saturn_report_trace(s, stdout);

    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    iso_free(&fs);
    disc_close(&d);
    return 0;
}
