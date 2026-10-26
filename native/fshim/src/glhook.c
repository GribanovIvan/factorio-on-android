/* Load libGLCompat.so early and globally so SDL_GL_GetProcAddress's lazy
 * dlopen/dlsym path (or its weak fast path) can find fsgl_lookup.
 *
 * This is a fallback for the APK glue: System.loadLibrary("GLCompat") is the
 * primary load.  The constructor runs when libfshim.so is loaded and is
 * idempotent; failure only logs and leaves SDL on the pure EGL path.
 */
#include <android/log.h>
#include <dlfcn.h>

#define TAG "glcompat"

__attribute__((visibility("default")))
int fsgl_preload(void)
{
    static int done;
    static int result;
    if (done) {
        return result;
    }
    done = 1;
    void *handle = dlopen("libGLCompat.so", RTLD_NOW | RTLD_GLOBAL);
    if (handle) {
        void *lookup = dlsym(handle, "fsgl_lookup");
        if (lookup) {
            __android_log_print(ANDROID_LOG_INFO, TAG,
                                "fshim preload: libGLCompat.so loaded, fsgl_lookup=%p", lookup);
            result = 1;
        } else {
            __android_log_print(ANDROID_LOG_ERROR, TAG,
                                "fshim preload: libGLCompat.so has no fsgl_lookup: %s", dlerror());
        }
    } else {
        __android_log_print(ANDROID_LOG_WARN, TAG,
                            "fshim preload: libGLCompat.so not loadable: %s (SDL will use EGL only)",
                            dlerror());
    }
    return result;
}

__attribute__((constructor))
static void fsgl_preload_ctor(void)
{
    fsgl_preload();
}
