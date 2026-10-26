/* fshim_main: the SDL Android entry point (getMainFunction() == "fshim_main").
 *
 * SDLActivity dlopens libfshim.so and calls this on SDL's own thread through
 * SDL_RunApp(), with argv[0] = "app_process".
 *
 *   fshim_main --selftest ...   pure SDL3 + GLES 3.2 clear-colour window
 *   fshim_main <game args>      load libfactorio_game.so next to this .so and
 *                               run the real game main() through fshim_run()
 *
 * The selftest deliberately resolves every SDL/GL entry point dynamically so
 * this file stays independent of SDL headers and of libGLESv2/libEGL link deps.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <android/log.h>

#include "fshim.h"

/* Defined at the bottom; declared here so own_directory() can take its
 * address with dladdr(). */
int fshim_main(int argc, char **argv);

#define TAG "fshim"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* SDL3 constants we need (SDL3/SDL_video.h, SDL3/SDL_init.h). */
#define SDL_INIT_VIDEO 0x00000020u

#define SDL_WINDOW_FULLSCREEN 0x0000000000000001ULL
#define SDL_WINDOW_OPENGL 0x0000000000000002ULL
#define SDL_WINDOW_SHOWN 0x0000000000000004ULL

#define SDL_GL_ALPHA_SIZE 3
#define SDL_GL_DOUBLEBUFFER 5
#define SDL_GL_DEPTH_SIZE 6
#define SDL_GL_STENCIL_SIZE 7
#define SDL_GL_CONTEXT_MAJOR_VERSION 17
#define SDL_GL_CONTEXT_MINOR_VERSION 18
#define SDL_GL_CONTEXT_PROFILE_MASK 20
#define SDL_GL_CONTEXT_PROFILE_CORE 0x0001
#define SDL_GL_CONTEXT_PROFILE_ES 0x0004

#define SDL_EVENT_QUIT 0x100u
#define SDL_EVENT_FINGER_DOWN 0x700u
#define SDL_EVENT_FINGER_UP 0x701u

/* GL / EGL constants. */
#define GL_VENDOR 0x1F00u
#define GL_RENDERER 0x1F01u
#define GL_VERSION 0x1F02u
#define GL_SHADING_LANGUAGE_VERSION 0x8B8Cu
#define GL_COLOR_BUFFER_BIT 0x00004000u

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void *load_sym(void *handle, const char *name)
{
    void *p = dlsym(handle, name);
    if (!p) {
        LOGE("missing symbol %s: %s", name, dlerror());
    }
    return p;
}

#define LOAD_SYM(handle, field, name) \
    do { *(void **)(&(api->field)) = load_sym((handle), (name)); } while (0)

typedef struct {
    bool (*Init)(uint32_t flags);
    void (*Quit)(void);
    const char *(*GetError)(void);
    const char *(*GetCurrentVideoDriver)(void);
    bool (*GL_SetAttribute)(int attr, int value);
    void *(*CreateWindow)(const char *title, int w, int h, uint64_t flags);
    void (*DestroyWindow)(void *window);
    void *(*GL_CreateContext)(void *window);
    void (*GL_DestroyContext)(void *context);
    bool (*GL_SwapWindow)(void *window);
    bool (*PollEvent)(void *event);
    bool (*GetWindowSizeInPixels)(void *window, int *w, int *h);
    void *(*GL_GetProcAddress)(const char *proc);
} SdlApi;

/* libEGL is only needed for error codes in diagnostics. */
typedef struct {
    void *handle;
    int (*GetError)(void);
    void *(*GetCurrentDisplay)(void);
    const char *(*QueryString)(void *display, int name);
    void (*GetConfigAttrib)(void *display, void *config, int attribute, int *value);
    bool (*GetConfigs)(void *display, void **configs, int config_size, int *num_config);
    void *(*GetCurrentSurface)(int readdraw);
} EglApi;

static int egl_error(const EglApi *egl)
{
    return (egl->GetError && egl->handle) ? egl->GetError() : -1;
}

static void egl_log(const EglApi *egl, const char *where)
{
    int err = egl_error(egl);
    if (err == 0x3000 /* EGL_SUCCESS */) {
        LOGI("eglGetError(%s) = 0x%04x (EGL_SUCCESS)", where, err);
    } else {
        LOGE("eglGetError(%s) = 0x%04x", where, err);
    }
}

static bool load_egl(EglApi *egl)
{
    memset(egl, 0, sizeof *egl);
    egl->handle = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
    if (!egl->handle) {
        LOGE("dlopen(libEGL.so) failed: %s", dlerror());
        return false;
    }
    *(void **)(&egl->GetError) = dlsym(egl->handle, "eglGetError");
    *(void **)(&egl->GetCurrentDisplay) = dlsym(egl->handle, "eglGetCurrentDisplay");
    *(void **)(&egl->QueryString) = dlsym(egl->handle, "eglQueryString");
    *(void **)(&egl->GetConfigAttrib) = dlsym(egl->handle, "eglGetConfigAttrib");
    *(void **)(&egl->GetConfigs) = dlsym(egl->handle, "eglGetConfigs");
    *(void **)(&egl->GetCurrentSurface) = dlsym(egl->handle, "eglGetCurrentSurface");
    return egl->GetError != NULL;
}

/* eglGetCurrentDisplay() + eglQueryString() are the only version report that
 * does not depend on the GL context being current. */
static void log_egl_version(const EglApi *egl, const char *when)
{
    if (!egl->GetCurrentDisplay || !egl->QueryString) {
        return;
    }
    void *dpy = egl->GetCurrentDisplay();
    if (!dpy) {
        LOGW("eglGetCurrentDisplay(%s) = NULL", when);
        return;
    }
    const char *ver = egl->QueryString(dpy, 0x3054 /* EGL_VERSION */);
    const char *vendor = egl->QueryString(dpy, 0x3053 /* EGL_VENDOR */);
    const char *apis = egl->QueryString(dpy, 0x308D /* EGL_CLIENT_APIS */);
    LOGI("EGL(%s): version=%s vendor=%s apis=%s", when,
         ver ? ver : "(null)", vendor ? vendor : "(null)", apis ? apis : "(null)");
}

static bool load_sdl(SdlApi *api, void **out_handle)
{
    memset(api, 0, sizeof *api);
    void *h = dlopen("libSDL3.so", RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        LOGE("dlopen(libSDL3.so) failed: %s", dlerror());
        return false;
    }
    LOAD_SYM(h, Init, "SDL_Init");
    LOAD_SYM(h, Quit, "SDL_Quit");
    LOAD_SYM(h, GetError, "SDL_GetError");
    LOAD_SYM(h, GetCurrentVideoDriver, "SDL_GetCurrentVideoDriver");
    LOAD_SYM(h, GL_SetAttribute, "SDL_GL_SetAttribute");
    LOAD_SYM(h, CreateWindow, "SDL_CreateWindow");
    LOAD_SYM(h, DestroyWindow, "SDL_DestroyWindow");
    LOAD_SYM(h, GL_CreateContext, "SDL_GL_CreateContext");
    LOAD_SYM(h, GL_DestroyContext, "SDL_GL_DestroyContext");
    LOAD_SYM(h, GL_SwapWindow, "SDL_GL_SwapWindow");
    LOAD_SYM(h, PollEvent, "SDL_PollEvent");
    LOAD_SYM(h, GetWindowSizeInPixels, "SDL_GetWindowSizeInPixels");
    LOAD_SYM(h, GL_GetProcAddress, "SDL_GL_GetProcAddress");

    if (!api->Init || !api->GL_SetAttribute || !api->CreateWindow ||
        !api->GL_CreateContext || !api->GL_GetProcAddress) {
        LOGE("libSDL3.so is missing required entry points");
        return false;
    }
    *out_handle = h;
    return true;
}

/* SDL3's SDL_Event is a union of at most ~56 bytes; the first member is the
 * Uint32 type.  Keep an over-sized, over-aligned buffer so we never depend on
 * the exact layout. */
typedef union {
    uint32_t type;
    unsigned char raw[256];
    uint64_t align;
} SdlEventBuf;

static int run_selftest(bool es)
{
    const char *variant = es ? "es32" : "core33";
    SdlApi api;
    void *sdl = NULL;
    EglApi egl;
    bool have_egl = load_egl(&egl);

    if (!load_sdl(&api, &sdl)) {
        return 1;
    }

    LOGI("selftest[%s]: SDL3 library opened, libEGL diagnostics %s",
         variant, have_egl ? "available" : "unavailable");

    if (!api.Init(SDL_INIT_VIDEO)) {
        LOGE("SDL_Init(SDL_INIT_VIDEO) failed: %s", api.GetError ? api.GetError() : "?");
        return 1;
    }
    const char *driver = api.GetCurrentVideoDriver ? api.GetCurrentVideoDriver() : NULL;
    LOGI("selftest: SDL video driver = %s", driver ? driver : "(null)");

    /* The game asks SDL for desktop GL 3.3 core; --selftest-core33 replays
     * exactly that request so the EGL failure can be reported verbatim.
     * --selftest keeps GLES 3.2, which is what the compat layer must provide. */
    LOGI("selftest[%s]: requesting profile=%s %d.%d", variant,
         es ? "ES" : "CORE", 3, es ? 2 : 3);
    api.GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        es ? SDL_GL_CONTEXT_PROFILE_ES : SDL_GL_CONTEXT_PROFILE_CORE);
    api.GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    api.GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, es ? 2 : 3);
    api.GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    api.GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    api.GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    api.GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);

    void *win = api.CreateWindow("Factorio selftest", 1280, 720,
                                 SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_SHOWN);
    if (!win) {
        LOGE("SDL_CreateWindow failed: %s", api.GetError ? api.GetError() : "?");
        if (have_egl) egl_log(&egl, "after SDL_CreateWindow");
        api.Quit();
        return 1;
    }
    int ww = 0, wh = 0;
    if (api.GetWindowSizeInPixels) {
        api.GetWindowSizeInPixels(win, &ww, &wh);
    }
    LOGI("selftest: window created, %dx%d pixels", ww, wh);
    if (have_egl) {
        log_egl_version(&egl, "after SDL_CreateWindow");
        egl_log(&egl, "after SDL_CreateWindow");
    }

    void *ctx = api.GL_CreateContext(win);
    if (!ctx) {
        LOGE("SDL_GL_CreateContext failed: %s", api.GetError ? api.GetError() : "?");
        if (have_egl) {
            egl_log(&egl, "after SDL_GL_CreateContext");
            log_egl_version(&egl, "after SDL_GL_CreateContext");
        }
        api.DestroyWindow(win);
        api.Quit();
        return 1;
    }
    LOGI("selftest: GLES context created");
    if (have_egl) {
        log_egl_version(&egl, "after SDL_GL_CreateContext");
        egl_log(&egl, "after SDL_GL_CreateContext");
    }

    typedef const unsigned char *(*PFN_GETSTRING)(unsigned int name);
    typedef void (*PFN_CLEARCOLOR)(float, float, float, float);
    typedef void (*PFN_CLEAR)(unsigned int);
    typedef unsigned int (*PFN_GETERROR)(void);

    PFN_GETSTRING glGetString = NULL;
    PFN_CLEARCOLOR glClearColor = NULL;
    PFN_CLEAR glClear = NULL;
    PFN_GETERROR glGetError = NULL;
    *(void **)(&glGetString) = api.GL_GetProcAddress("glGetString");
    *(void **)(&glClearColor) = api.GL_GetProcAddress("glClearColor");
    *(void **)(&glClear) = api.GL_GetProcAddress("glClear");
    *(void **)(&glGetError) = api.GL_GetProcAddress("glGetError");

    if (glGetString) {
        const unsigned char *v = glGetString(GL_VERSION);
        const unsigned char *r = glGetString(GL_RENDERER);
        const unsigned char *vd = glGetString(GL_VENDOR);
        const unsigned char *sl = glGetString(GL_SHADING_LANGUAGE_VERSION);
        LOGI("GL_VERSION  = %s", v ? (const char *)v : "(null)");
        LOGI("GL_RENDERER = %s", r ? (const char *)r : "(null)");
        LOGI("GL_VENDOR   = %s", vd ? (const char *)vd : "(null)");
        LOGI("GL_SHADING_LANGUAGE_VERSION = %s", sl ? (const char *)sl : "(null)");
    } else {
        LOGE("SDL_GL_GetProcAddress(glGetString) returned NULL");
    }

    int frames = 0;
    bool quit = false;
    uint64_t start = now_ms();
    while (!quit && now_ms() - start < 10000u) {
        if (glClearColor) {
            glClearColor(0.1f, 0.6f, 0.6f, 1.0f); /* teal, unmistakable on screen */
        }
        if (glClear) {
            glClear(GL_COLOR_BUFFER_BIT);
        }
        api.GL_SwapWindow(win);
        frames++;

        SdlEventBuf ev;
        memset(&ev, 0, sizeof ev);
        while (api.PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT ||
                ev.type == SDL_EVENT_FINGER_DOWN ||
                ev.type == SDL_EVENT_FINGER_UP) {
                LOGI("selftest: event 0x%x, ending", ev.type);
                quit = true;
                break;
            }
            memset(&ev, 0, sizeof ev);
        }
        usleep(16000);
    }

    int glerr = glGetError ? (int)glGetError() : -1;
    LOGI("selftest: %d frames in %llu ms, glGetError=0x%x, window %dx%d",
         frames, (unsigned long long)(now_ms() - start), glerr, ww, wh);
    if (have_egl) {
        egl_log(&egl, "after render loop");
    }

    if (api.GL_DestroyContext) {
        api.GL_DestroyContext(ctx);
    }
    api.DestroyWindow(win);
    api.Quit();
    dlclose(sdl);
    if (have_egl) {
        dlclose(egl.handle);
    }
    LOGI("selftest: done");
    return 0;
}

/* Directory containing this shared object, so the game image can sit next to
 * it inside the APK's nativeLibraryDir. */
static bool own_directory(char *out, size_t cap)
{
    Dl_info info;
    memset(&info, 0, sizeof info);
    if (dladdr((void *)&fshim_main, &info) && info.dli_fname && info.dli_fname[0] == '/') {
        char *slash = strrchr(info.dli_fname, '/');
        if (slash) {
            size_t len = (size_t)(slash - info.dli_fname);
            if (len > 0 && len < cap) {
                memcpy(out, info.dli_fname, len);
                out[len] = '\0';
                return true;
            }
        }
    }

    /* Fallback: /proc/self/maps (dladdr can lose the name in some linkers). */
    FILE *f = fopen("/proc/self/maps", "re");
    if (!f) {
        return false;
    }
    char line[1024];
    bool found = false;
    while (fgets(line, sizeof line, f)) {
        char *path = strchr(line, '/');
        if (path && strstr(path, "libfshim.so")) {
            char *slash = strrchr(path, '/');
            if (slash) {
                size_t len = (size_t)(slash - path);
                if (len > 0 && len < cap) {
                    memcpy(out, path, len);
                    out[len] = '\0';
                    found = true;
                }
            }
            break;
        }
    }
    fclose(f);
    return found;
}

/* The game's startup and crash handler write to stdout/stderr, which Android
 * sends to /dev/null for an app process.  Mirror both into a file next to
 * --config so the game log and any crash stacktrace can be pulled with adb. */
static void redirect_stdio(int argc, char **argv)
{
    const char *config = NULL;
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--config") == 0) {
            config = argv[i + 1];
            break;
        }
    }
    if (!config) {
        return;
    }

    char path[PATH_MAX];
    int n = snprintf(path, sizeof path, "%s", config);
    if (n <= 0 || (size_t)n >= sizeof path) {
        return;
    }
    char *slash = strrchr(path, '/');
    if (!slash) {
        return;
    }
    size_t dirlen = (size_t)(slash - path) + 1;
    static const char name[] = "fshim-native.log";
    if (dirlen + sizeof name > sizeof path) {
        return;
    }
    memcpy(path + dirlen, name, sizeof name);

    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0) {
        LOGW("cannot open %s for stdio redirection", path);
        return;
    }
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    close(fd);
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    LOGI("stdio redirected to %s", path);
}

int fshim_main(int argc, char **argv)
{
    const char *mode = (argc > 1) ? argv[1] : "(none)";
    LOGI("fshim_main: argc=%d, mode=%s", argc, mode);
    for (int i = 0; i < argc; i++) {
        LOGI("fshim_main: argv[%d] = %s", i, argv[i]);
    }

    if (strcmp(mode, "--selftest") == 0) {
        return run_selftest(true);
    }
    if (strcmp(mode, "--selftest-core33") == 0) {
        return run_selftest(false);
    }

    /* Optional GL compatibility layer.  Java loads it first when it is
     * packaged (FactorioActivity.getLibraries), but an explicit early load
     * makes the fsgl_lookup hook visible even if the .so is injected some
     * other way.  Absence is normal for the baseline APK. */
    void *glcompat = dlopen("libGLCompat.so", RTLD_NOW | RTLD_GLOBAL);
    if (glcompat) {
        LOGI("GLCompat loaded early");
    } else {
        LOGI("GLCompat not present: %s", dlerror());
    }

    char dir[PATH_MAX];
    if (!own_directory(dir, sizeof dir)) {
        LOGE("could not determine libfshim.so directory");
        return 1;
    }
    char game[PATH_MAX];
    int n = snprintf(game, sizeof game, "%s/libfactorio_game.so", dir);
    if (n <= 0 || (size_t)n >= sizeof game) {
        LOGE("game path too long for %s", dir);
        return 1;
    }
    LOGI("fshim_main: game image %s", game);
    if (access(game, R_OK) != 0) {
        LOGE("game image is not readable: %s", game);
        return 1;
    }
    redirect_stdio(argc, argv);
    return fshim_run(game, argc, argv);
}
