#ifndef SATURN_GAME_OVERLAY_H
#define SATURN_GAME_OVERLAY_H

#include <stddef.h>
#include <stdint.h>
#include "runtime_settings.h"

typedef struct SDL_Window SDL_Window;
typedef union SDL_Event SDL_Event;
typedef struct game_overlay game_overlay;

enum game_overlay_setting {
    GAME_OVERLAY_WINDOW = 1u << 0,
    GAME_OVERLAY_FULLSCREEN = 1u << 1,
    GAME_OVERLAY_INTERNAL_SCALE = 1u << 2,
    GAME_OVERLAY_TEXTURE_FILTER = 1u << 3,
    GAME_OVERLAY_ANTIALIASING = 1u << 4,
    GAME_OVERLAY_MODEL_SMOOTHING = 1u << 5,
    GAME_OVERLAY_INTERPOLATION = 1u << 6,
    GAME_OVERLAY_TARGET_HZ = 1u << 7,
    GAME_OVERLAY_VOLUME = 1u << 8,
    GAME_OVERLAY_MUTED = 1u << 9,
    GAME_OVERLAY_ALL_SETTINGS = (1u << 10) - 1u
};

typedef struct game_overlay_resolution { int width, height; } game_overlay_resolution;
typedef struct game_overlay_view {
    saturn_runtime_settings settings;
    /* Bitmask above; unavailable controls remain visible with an explanation. */
    unsigned capabilities;
    const game_overlay_resolution *resolutions;
    unsigned resolution_count;
    int native_width, native_height;
    int internal_width, internal_height;
    float game_fps, present_fps;
    unsigned interpolation_matches, interpolation_span;
    const char *game_title;
    const char *renderer_name;
} game_overlay_view;

typedef struct game_overlay_actions {
    unsigned changed;
    saturn_runtime_settings settings;
    int resume, diagnostics, quit;
} game_overlay_actions;

typedef struct game_overlay_vertex { float position[2], uv[2]; uint8_t color[4]; } game_overlay_vertex;
typedef struct game_overlay_draw_command {
    uint32_t element_count, first_index;
    float clip_x, clip_y, clip_w, clip_h;
} game_overlay_draw_command;
typedef struct game_overlay_draw_data {
    const game_overlay_vertex *vertices;
    const uint16_t *indices;
    const game_overlay_draw_command *commands;
    size_t vertex_bytes, index_bytes;
    unsigned command_count;
    int width, height;
} game_overlay_draw_data;

/* UI state and draw data stay on the SDL/presentation thread. */
game_overlay *game_overlay_create(SDL_Window *window, char *error, size_t error_size);
void game_overlay_destroy(game_overlay *overlay);
void game_overlay_toggle(game_overlay *overlay);
void game_overlay_set_open(game_overlay *overlay, int open);
int game_overlay_is_open(const game_overlay *overlay);
void game_overlay_input_begin(game_overlay *overlay);
/* Returns 1 when the event belongs to the overlay. F2 always passes through. */
int game_overlay_event(game_overlay *overlay, const SDL_Event *event);
void game_overlay_input_end(game_overlay *overlay);
const game_overlay_draw_data *game_overlay_build(game_overlay *overlay,
    const game_overlay_view *view, game_overlay_actions *actions,
    int drawable_width, int drawable_height, float dpi_scale);
/* Baked RGBA atlas remains valid until destroy; it is not game media. */
const unsigned char *game_overlay_font_atlas(const game_overlay *overlay, int *width, int *height);

#endif
