#include "game_overlay.h"
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL2/SDL.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static game_overlay_view view;
static game_overlay_actions actions;
static game_overlay *overlay;
static Uint32 window_id;

static const game_overlay_draw_data *frame(void)
{
    return game_overlay_build(overlay, &view, &actions, 1280, 800, 1);
}

static int key(SDL_Keycode code, SDL_Keymod modifier)
{
    SDL_Event event;
    memset(&event, 0, sizeof event);
    event.type = SDL_KEYDOWN;
    event.key.windowID = window_id;
    event.key.keysym.sym = code;
    event.key.keysym.mod = modifier;
    game_overlay_input_begin(overlay);
    int consumed = game_overlay_event(overlay, &event);
    game_overlay_input_end(overlay);
    frame();
    return consumed;
}

static unsigned click(int x, int y)
{
    unsigned changed = 0;
    for (int down = 1; down >= 0; --down) {
        SDL_Event event;
        memset(&event, 0, sizeof event);
        event.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
        event.button.windowID = window_id;
        event.button.button = SDL_BUTTON_LEFT;
        event.button.x = x; event.button.y = y;
        game_overlay_input_begin(overlay);
        SDL_Event motion;
        memset(&motion, 0, sizeof motion);
        motion.type = SDL_MOUSEMOTION;
        motion.motion.windowID = window_id;
        motion.motion.x = x; motion.motion.y = y;
        assert(game_overlay_event(overlay, &motion));
        assert(game_overlay_event(overlay, &event));
        game_overlay_input_end(overlay);
        frame();
        changed |= actions.changed;
        view.settings = actions.settings;
        if (!game_overlay_is_open(overlay)) break;
    }
    return changed;
}

static void foreign_window_input(void)
{
    SDL_Window *other = SDL_CreateWindow("Diagnostics regression", 0, 0, 1280, 800, SDL_WINDOW_HIDDEN);
    assert(other);
    Uint32 foreign_id = SDL_GetWindowID(other);
    const Uint32 types[] = {SDL_KEYDOWN, SDL_KEYUP, SDL_TEXTINPUT, SDL_TEXTEDITING,
        SDL_MOUSEMOTION, SDL_MOUSEBUTTONDOWN, SDL_MOUSEBUTTONUP, SDL_MOUSEWHEEL};
    saturn_runtime_settings before = view.settings;
    for (unsigned i = 0; i < sizeof types / sizeof types[0]; ++i) {
        SDL_Event event = {0};
        event.type = types[i];
        switch (event.type) {
        case SDL_KEYDOWN: case SDL_KEYUP:
            event.key.windowID = foreign_id; event.key.keysym.sym = SDLK_F1; break;
        case SDL_TEXTINPUT:
            event.text.windowID = foreign_id; strcpy(event.text.text, "240"); break;
        case SDL_TEXTEDITING:
            event.edit.windowID = foreign_id; strcpy(event.edit.text, "240"); break;
        case SDL_MOUSEMOTION:
            event.motion.windowID = foreign_id; event.motion.x = 610; event.motion.y = 450; break;
        case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
            event.button.windowID = foreign_id; event.button.button = SDL_BUTTON_LEFT;
            event.button.x = 610; event.button.y = 450; break;
        case SDL_MOUSEWHEEL:
            event.wheel.windowID = foreign_id; event.wheel.y = 3; break;
        }
        game_overlay_input_begin(overlay);
        assert(!game_overlay_event(overlay, &event));
        game_overlay_input_end(overlay);
        frame();
        assert(game_overlay_is_open(overlay) && !actions.changed);
        assert(memcmp(&actions.settings, &before, sizeof before) == 0);
    }
    /* Global hot-plug events still reach the runtime; gameplay button events
     * remain captured while the panel is open. */
    SDL_Event event = {0};
    event.type = SDL_CONTROLLERDEVICEADDED;
    assert(!game_overlay_event(overlay, &event));
    event.type = SDL_CONTROLLERDEVICEREMOVED;
    assert(!game_overlay_event(overlay, &event));
    event.type = SDL_CONTROLLERBUTTONDOWN;
    assert(game_overlay_event(overlay, &event));
    SDL_DestroyWindow(other);
}

static void lost_mouse_release(void)
{
    /* Start a real slider drag, lose focus, and release in another window.
     * The next ordinary click must work on its first press. */
    SDL_Event event = {0};
    event.type = SDL_MOUSEBUTTONDOWN; event.button.windowID = window_id;
    event.button.button = SDL_BUTTON_LEFT; event.button.x = 720; event.button.y = 373;
    game_overlay_input_begin(overlay);
    assert(game_overlay_event(overlay, &event));
    game_overlay_input_end(overlay);
    frame(); view.settings = actions.settings;
    event.type = SDL_WINDOWEVENT; event.window.windowID = window_id;
    event.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
    game_overlay_input_begin(overlay);
    assert(game_overlay_event(overlay, &event));
    game_overlay_input_end(overlay);
    frame();
    int old_mute = view.settings.muted;
    assert(click(610, 450) == GAME_OVERLAY_MUTED);
    assert(view.settings.muted != old_mute);
}

int main(void)
{
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_VIDEODRIVER, "dummy");
    assert(SDL_Init(SDL_INIT_VIDEO) == 0);
    SDL_Window *window = SDL_CreateWindow("Overlay regression", 0, 0, 1280, 800, SDL_WINDOW_HIDDEN);
    assert(window);
    window_id = SDL_GetWindowID(window);
    char error[256] = {0};
    overlay = game_overlay_create(window, error, sizeof error);
    if (!overlay) fprintf(stderr, "%s\n", error);
    assert(overlay);
    view.settings = (saturn_runtime_settings){1280,800,0,1,0,0,0,0,120,100,0};
    view.capabilities = GAME_OVERLAY_ALL_SETTINGS;
    view.native_width = 320; view.native_height = 224;
    int width = 0, height = 0;
    assert(game_overlay_font_atlas(overlay, &width, &height));
    assert(width > 0 && height > 0);
    assert(!game_overlay_is_open(overlay));
    assert(!key(SDLK_SPACE, KMOD_NONE));
    assert(key(SDLK_F1, KMOD_NONE));
    assert(game_overlay_is_open(overlay));
    const game_overlay_draw_data *draw = frame();
    assert(draw->command_count > 0 && draw->vertex_bytes > 0 && draw->index_bytes > 0);
    for (unsigned i = 0; i < draw->command_count; ++i)
        assert((uint64_t)draw->commands[i].first_index + draw->commands[i].element_count <= draw->index_bytes / 2);
    assert(!key(SDLK_F2, KMOD_NONE));
    assert(game_overlay_is_open(overlay));
    for (int i = 0; i < 4; ++i) key(SDLK_TAB, KMOD_NONE);
    assert(key(SDLK_RIGHT, KMOD_NONE));
    assert(actions.changed == GAME_OVERLAY_FULLSCREEN && actions.settings.fullscreen == 1);
    view.settings = actions.settings;

    key(SDLK_PAGEDOWN, KMOD_NONE); /* Graphics. Four tabs precede page controls. */
    for (int i = 0; i < 4; ++i) key(SDLK_TAB, KMOD_NONE);
    key(SDLK_RIGHT, KMOD_NONE);
    assert(actions.changed == GAME_OVERLAY_INTERNAL_SCALE && actions.settings.internal_scale == 2);
    view.settings = actions.settings;
    view.capabilities &= ~GAME_OVERLAY_INTERNAL_SCALE;
    key(SDLK_RIGHT, KMOD_NONE);
    assert(!actions.changed && actions.settings.internal_scale == 2);
    view.capabilities = GAME_OVERLAY_ALL_SETTINGS;

    key(SDLK_PAGEDOWN, KMOD_NONE); /* Motion. */
    for (int i = 0; i < 4; ++i) key(SDLK_TAB, KMOD_NONE);
    key(SDLK_SPACE, KMOD_NONE);
    assert(actions.changed == GAME_OVERLAY_INTERPOLATION && actions.settings.interpolation == 1);
    view.settings = actions.settings;
    key(SDLK_TAB, KMOD_NONE);
    key(SDLK_RIGHT, KMOD_NONE);
    assert(actions.changed == GAME_OVERLAY_TARGET_HZ && actions.settings.target_hz == 121);
    view.settings = actions.settings;

    key(SDLK_PAGEDOWN, KMOD_NONE); /* Audio. */
    for (int i = 0; i < 4; ++i) key(SDLK_TAB, KMOD_NONE);
    key(SDLK_LEFT, KMOD_NONE);
    assert(actions.changed == GAME_OVERLAY_VOLUME && actions.settings.volume == 95);
    view.settings = actions.settings;
    key(SDLK_TAB, KMOD_NONE);
    key(SDLK_SPACE, KMOD_NONE);
    assert(actions.changed == GAME_OVERLAY_MUTED && actions.settings.muted == 1);
    view.settings = actions.settings;
    key(SDLK_TAB, KMOD_NONE); /* Resume uses the same path as the mouse button. */
    key(SDLK_RETURN, KMOD_NONE);
    assert(actions.resume && !game_overlay_is_open(overlay));
    assert(!frame()->command_count);
    key(SDLK_F1, KMOD_NONE);
    assert(game_overlay_is_open(overlay));
    key(SDLK_ESCAPE, KMOD_NONE);
    assert(actions.resume && !game_overlay_is_open(overlay));
    assert(!key(SDLK_SPACE, KMOD_NONE));
    key(SDLK_F1, KMOD_NONE); /* Audio page is retained when reopened. */
    assert(click(610, 450) == GAME_OVERLAY_MUTED);
    assert(!view.settings.muted);
    int old_volume = view.settings.volume;
    assert(click(720, 373) == GAME_OVERLAY_VOLUME);
    assert(view.settings.volume != old_volume);
    /* An unsupported field must survive resetting a page with mixed capabilities. */
    view.settings.volume = 73; view.settings.muted = 1;
    view.capabilities &= ~GAME_OVERLAY_VOLUME;
    assert(click(886, 630) == GAME_OVERLAY_MUTED);
    assert(view.settings.volume == 73 && view.settings.muted == 0);
    assert(click(389, 630) == 0);
    assert(!game_overlay_is_open(overlay));
    /* Resume closes on the mouse press, so its release never reached the
     * closed panel. Reopening must clear that button and accept one click. */
    key(SDLK_F1, KMOD_NONE);
    int previous_mute = view.settings.muted;
    assert(click(610, 450) == GAME_OVERLAY_MUTED);
    assert(view.settings.muted != previous_mute);
    foreign_window_input();
    view.capabilities = GAME_OVERLAY_ALL_SETTINGS;
    lost_mouse_release();
    game_overlay_destroy(overlay);
    SDL_DestroyWindow(window);
    SDL_Quit();
    puts("PASS: native overlay input isolation, focus/reopen release, capability masks, settings actions, and resume");
    return 0;
}
