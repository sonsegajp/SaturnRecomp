#ifndef SATURN_RUNTIME_SETTINGS_H
#define SATURN_RUNTIME_SETTINGS_H

enum { SATURN_MIN_WINDOW_WIDTH = 640, SATURN_MIN_WINDOW_HEIGHT = 480 };

/* Shared presentation settings. These do not change Saturn hardware state. */
typedef struct saturn_runtime_settings {
    int window_width,window_height;
    int fullscreen;
    int internal_scale;       /* 1..4 times native rendering resolution */
    int texture_filter;       /* 0 nearest, 1 bilinear */
    int antialiasing;         /* 0 off, 1 FXAA */
    int model_smoothing;      /* 0 native, 1 supersampled model edges */
    int interpolation;
    int target_hz;            /* preferred 60..240 Hz, retained when disabled */
    int volume;               /* 0..100, applied only to host playback */
    int muted;
} saturn_runtime_settings;

void saturn_settings_defaults(saturn_runtime_settings *settings);
void saturn_settings_normalize(saturn_runtime_settings *settings);
int saturn_settings_load(saturn_runtime_settings *settings,const char *path);
int saturn_settings_save(const saturn_runtime_settings *settings,const char *path);

#endif
