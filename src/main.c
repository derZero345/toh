/*
 * Pathfinder - hex grid tabletop helper. C / Vulkan port of the Java program.
 *
 * Usage: pathfinder [--exec <line>]... [--auto-file <png>] [--size WxH]
 *
 * --exec lines are run one per frame, after the first frame. A line is either a
 * console command (e.g. ":r 2d6") or a scripted input action, which is mainly
 * useful for automated testing:
 *   @move X Y        mouse motion        @click X Y / @rclick X Y   mouse button
 *   @drag X Y        motion with LMB     @wheel N                   wheel notches
 *   @key [mods+]KEY  key press, e.g. @key ctrl+return, @key alt+c, @key lshift+down
 *   @down KEY / @up KEY                  hold or release a key
 *   @type TEXT       typed text          @shot FILE  save a screenshot
 *   @wait N          idle N frames       @quit
 * --auto-file makes every file dialog (images, :save, :load) return the given file.
 */
#include "app.h"
#include "font.h"
#include "vk_renderer.h"

#include <stdio.h>
#include <strings.h>

App *gh;
static App app;

/* ---- scripted input ---------------------------------------------------------------- */

static void push_mouse_button(float x, float y, Uint8 button, bool down) {
    SDL_Event e;
    SDL_zero(e);
    e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.x = x;
    e.button.y = y;
    e.button.button = button;
    e.button.down = down;
    e.button.clicks = 1;
    io_handle_event(&e);
}

static void push_motion(float x, float y, SDL_MouseButtonFlags state) {
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.x = x;
    e.motion.y = y;
    e.motion.state = state;
    io_handle_event(&e);
}

static void push_key(SDL_Keycode key, bool down) {
    SDL_Event e;
    SDL_zero(e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.key = key;
    e.key.down = down;
    io_handle_event(&e);
}

static SDL_Keycode key_from_name(const char *name) {
    if (!strcasecmp(name, "lshift")) return SDLK_LSHIFT;
    if (!strcasecmp(name, "rshift")) return SDLK_RSHIFT;
    if (!strcasecmp(name, "shift"))  return SDLK_LSHIFT;
    if (!strcasecmp(name, "ctrl"))   return SDLK_LCTRL;
    if (!strcasecmp(name, "alt"))    return SDLK_LALT;
    if (!strcasecmp(name, "enter"))  return SDLK_RETURN;
    return SDL_GetKeyFromName(name);
}

/* Returns the number of extra frames to wait. */
static int run_script_line(const char *line, const char **screenshot) {
    if (line[0] != '@') {
        console_command(line);
        return 0;
    }
    char cmd[32] = { 0 }, arg[256] = { 0 };
    float x = 0, y = 0;
    sscanf(line, "@%31s %255[^\n]", cmd, arg);
    sscanf(arg, "%f %f", &x, &y);
    if (!strcmp(cmd, "move")) {
        push_motion(x, y, 0);
    } else if (!strcmp(cmd, "drag")) {
        push_motion(x, y, SDL_BUTTON_LMASK);
    } else if (!strcmp(cmd, "click") || !strcmp(cmd, "rclick")) {
        Uint8 b = cmd[0] == 'r' ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
        push_motion(x, y, 0);
        push_mouse_button(x, y, b, true);
        push_mouse_button(x, y, b, false);
    } else if (!strcmp(cmd, "wheel")) {
        SDL_Event e;
        SDL_zero(e);
        e.type = SDL_EVENT_MOUSE_WHEEL;
        e.wheel.integer_y = -(int)x; /* notches, Java sign convention */
        e.wheel.mouse_x = (float)gh->mousePos.x;
        e.wheel.mouse_y = (float)gh->mousePos.y;
        io_handle_event(&e);
    } else if (!strcmp(cmd, "key")) {
        SDL_Keycode mods[4];
        int nmods = 0;
        char *tok = arg, *plus;
        while ((plus = strchr(tok, '+')) && plus[1] && nmods < 4) {
            *plus = '\0';
            mods[nmods++] = key_from_name(tok);
            tok = plus + 1;
        }
        SDL_Keycode k = key_from_name(tok);
        for (int i = 0; i < nmods; i++) push_key(mods[i], true);
        push_key(k, true);
        push_key(k, false);
        for (int i = nmods - 1; i >= 0; i--) push_key(mods[i], false);
    } else if (!strcmp(cmd, "down") || !strcmp(cmd, "up")) {
        push_key(key_from_name(arg), cmd[0] == 'd');
    } else if (!strcmp(cmd, "type")) {
        SDL_Event e;
        SDL_zero(e);
        e.type = SDL_EVENT_TEXT_INPUT;
        e.text.text = arg;
        io_handle_event(&e);
    } else if (!strcmp(cmd, "shot")) {
        static char path[256];
        snprintf(path, sizeof(path), "%s", arg);
        *screenshot = path;
    } else if (!strcmp(cmd, "wait")) {
        return (int)x;
    } else if (!strcmp(cmd, "quit")) {
        gh->running = false;
    } else {
        fprintf(stderr, "Unknown script action: %s\n", line);
    }
    return 0;
}

/* ---- main ---------------------------------------------------------------------------- */

static void find_resource_dir(char *out, size_t n) {
    const char *base = SDL_GetBasePath();
    if (base) {
        snprintf(out, n, "%sresources", base);
        SDL_PathInfo info;
        if (SDL_GetPathInfo(out, &info) && info.type == SDL_PATHTYPE_DIRECTORY) return;
    }
    snprintf(out, n, "resources");
}

static void update_window_size(void) {
    SDL_GetWindowSize(gh->window, &gh->winW, &gh->winH);
    float density = SDL_GetWindowPixelDensity(gh->window);
    gh->pixelScale = density > 0 ? density : 1.0f;
}

int main(int argc, char **argv) {
    const char **script = calloc((size_t)argc + 1, sizeof(char *));
    int nscript = 0;
    int width = 600, height = 400;
    gh = &app;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--exec") && i + 1 < argc) script[nscript++] = argv[++i];
        else if (!strcmp(argv[i], "--auto-file") && i + 1 < argc) app.autoFile = argv[++i];
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &width, &height);
        else {
            fprintf(stderr, "usage: %s [--exec <line>]... [--auto-file <png>] [--size WxH]\n", argv[0]);
            return 2;
        }
    }

    srand((unsigned)SDL_GetPerformanceCounter());
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    app.window = SDL_CreateWindow("Bitti bitti 15 Punkte", width, height,
                                  SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!app.window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }
    update_window_size();

    if (!vkr_init(app.window)) return 1;
    if (!font_init()) return 1;
    char resdir[512];
    find_resource_dir(resdir, sizeof(resdir));
    gfx_init(&app, resdir);
    save_init();
    SDL_StartTextInput(app.window);

    Uint64 last = SDL_GetTicks();
    Uint64 tick_acc = 0;
    int script_pos = 0, script_wait = 1;

    /* Frames are only drawn when something may have changed: after input, while the
     * camera scrolls, while a script runs and when the console caret blinks.
     * Otherwise the loop sleeps until the next event. */
    bool redraw = true;
    while (app.running) {
        bool scrolling = app.ADown || app.DDown || app.SDown || app.WDown;
        bool scripted = script_pos < nscript;
        SDL_Event e;
        bool have_event = false;
        if (!redraw && !scrolling && !scripted) {
            /* idle: wake for the next event, or for the next caret blink phase */
            Sint32 timeout = app.console.active ? (Sint32)(500 - SDL_GetTicks() % 500) + 1 : 1000;
            have_event = SDL_WaitEventTimeout(&e, timeout);
            if (!have_event && app.console.active) redraw = true;
            /* don't replay the idle time as camera ticks */
            last = SDL_GetTicks();
            tick_acc = 0;
        }
        while (have_event || SDL_PollEvent(&e)) {
            have_event = false;
            redraw = true;
            if (e.type == SDL_EVENT_QUIT) {
                app.running = false;
            } else if (e.type == SDL_EVENT_WINDOW_RESIZED || e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
                       e.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) {
                update_window_size();
                vkr_resize();
            } else if (script_pos < nscript &&
                       (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                        e.type == SDL_EVENT_MOUSE_BUTTON_UP || e.type == SDL_EVENT_MOUSE_WHEEL)) {
                /* while a script runs, ignore the real mouse so it cannot interfere */
            } else if (e.type == app.dialogEvent) {
                gfx_handle_file_result((const char *)e.user.data1);
                SDL_free(e.user.data1);
            } else {
                io_handle_event(&e);
            }
        }

        /* fixed 16 ms game tick, like the Java producer thread */
        Uint64 now = SDL_GetTicks();
        tick_acc += now - last;
        last = now;
        if (tick_acc > 250) tick_acc = 250;
        while (tick_acc >= 16) {
            int ox = app.gridOffsetX, oy = app.gridOffsetY;
            game_tick();
            if (ox != app.gridOffsetX || oy != app.gridOffsetY) redraw = true;
            tick_acc -= 16;
        }

        const char *screenshot = NULL;
        if (script_pos < nscript) {
            if (--script_wait <= 0) script_wait = 1 + run_script_line(script[script_pos++], &screenshot);
            redraw = true;
        }

        if (SDL_GetWindowFlags(app.window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(16);
            continue;
        }
        if (!redraw) {
            /* keys held but nothing moved yet: wait for the next tick instead of spinning */
            if (scrolling) SDL_Delay(1);
            continue;
        }
        gfx_update_tools();
        gfx_draw_frame(screenshot);
        redraw = false;
    }

    SDL_StopTextInput(app.window);
    gfx_shutdown();
    font_shutdown();
    vkr_shutdown();
    SDL_DestroyWindow(app.window);
    SDL_Quit();
    free(script);
    return 0;
}
