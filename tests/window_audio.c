/* Exercise the real SDL callback and ring drain without opening an audio
 * device. Build with -flto -fwhole-program so the unused frontend is removed. */
#define SDL_MAIN_HANDLED
#define main unused_frontend_main
#include "../runner/src/window.c"
#undef main

static int failures, checks;
static void check(int passed, const char *name)
{
    ++checks;
    if (!passed) { ++failures; fprintf(stderr, "FAIL %s\n", name); }
}

static void prepare_audio(unsigned available)
{
    g_sys.snd_rp = 0;
    g_sys.snd_wp = available;
    for (unsigned i = 0; i < SATURN_AUDIO_RING_FRAMES; ++i) {
        g_sys.snd_buf[i * 2] = 16000;
        g_sys.snd_buf[i * 2 + 1] = -24000;
    }
}

int main(void)
{
    int16_t output[128], original[128];
    prepare_audio(9000);
    memcpy(original, g_sys.snd_buf, sizeof original);
    SDL_AtomicSet(&audio_gain, 100);
    audio_cb(&g_sys, (Uint8 *)output, sizeof output);
    check(!memcmp(output, original, sizeof output), "100 percent preserves every playback sample");
    check(g_sys.snd_rp == 64 && g_sys.snd_wp == 9000, "callback consumes exactly the requested frames");

    prepare_audio(9000);
    SDL_AtomicSet(&audio_gain, 50);
    audio_cb(&g_sys, (Uint8 *)output, sizeof output);
    int half = 1;
    for (unsigned i = 0; i < 64; ++i)
        if (output[i * 2] != 8000 || output[i * 2 + 1] != -12000) half = 0;
    check(half, "half volume preserves both channel signs and scales amplitude");
    check(!memcmp(g_sys.snd_buf, original, sizeof original), "playback volume leaves guest ring samples unchanged");

    prepare_audio(9000);
    SDL_AtomicSet(&audio_gain, 0);
    audio_cb(&g_sys, (Uint8 *)output, sizeof output);
    int silent = 1;
    for (unsigned i = 0; i < 128; ++i) if (output[i]) silent = 0;
    check(silent, "mute silences the complete callback buffer");
    check(g_sys.snd_rp == 64 && g_sys.snd_wp == 9000, "mute drains normally instead of accumulating stale audio");
    check(!memcmp(g_sys.snd_buf, original, sizeof original), "mute leaves guest and diagnostic source samples unchanged");

    prepare_audio(9000);
    SDL_AtomicSet(&audio_gain, 100);
    audio_cb(&g_sys, (Uint8 *)output, sizeof output);
    check(!memcmp(output, original, sizeof output), "unmute resumes unmodified samples without resetting the source clock");

    /* Start from a primed callback, then exhaust the ring mid-buffer. The
     * underrun tail must fade toward silence at the selected host volume. */
    prepare_audio(4);
    SDL_AtomicSet(&audio_gain, 50);
    audio_cb(&g_sys, (Uint8 *)output, sizeof output);
    check(output[0] == 8000 && output[1] == -12000 && output[6] == 8000 && output[7] == -12000,
          "available samples retain their gain before an underrun");
    int fading = 1;
    for (unsigned i = 4; i < 64; ++i)
        if (output[i * 2] < 0 || output[i * 2] > output[(i - 1) * 2] ||
            output[i * 2 + 1] > 0 || output[i * 2 + 1] < output[(i - 1) * 2 + 1]) fading = 0;
    check(fading && output[126] == 0 && output[127] == 0, "underrun fade stays bounded and reaches silence in both channels");
    check(g_sys.snd_rp == 4 && !memcmp(g_sys.snd_buf, original, sizeof original),
          "underrun neither consumes missing samples nor modifies source audio");
    memset(output, 0x55, sizeof output);
    audio_cb(&g_sys, (Uint8 *)output, sizeof output);
    silent = 1;
    for (unsigned i = 0; i < 128; ++i) if (output[i]) silent = 0;
    check(silent && g_sys.snd_rp == 4, "unprimed callback emits silence while waiting for fresh audio");

    printf("window audio: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
