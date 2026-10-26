/* glibc -> bionic compatibility symbols.
 *
 * tools/audit.py computes the exact set of the game's dynamic imports that
 * bionic does not provide (50 on API 33).  Semantics that matter are forwarded
 * to bionic; semantics that cannot exist on Android (glibc locale internals,
 * ucontext, the resolver) degrade to a well-defined safe result instead of a
 * link failure.
 */
#define _GNU_SOURCE

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <locale.h>
#include <langinfo.h>
#include <malloc.h>
#include <netdb.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/auxv.h>
#include <sys/sysinfo.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#include <android/log.h>

#define TAG "fshim"
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

/* ------------------------------------------------------------------ start-up */

extern char **environ;

int __libc_start_main(int (*main_fn)(int, char **, char **), int argc,
                      char **argv, void (*init)(void), void (*fini)(void),
                      void (*rtld_fini)(void), void *stack_end)
{
    (void)fini;
    (void)rtld_fini;
    (void)stack_end;
    if (init) {
        init();
    }
    _exit(main_fn(argc, argv, environ));
}

/* Returned only as an empty answer; nothing in the port needs unwinding info. */
int _dl_find_object(void *address, void *result)
{
    (void)address;
    (void)result;
    return -1;
}

/* ------------------------------------------------------------- libc extras */

int *__errno_location(void)
{
    return __errno();
}

char *secure_getenv(const char *name)
{
    return getenv(name);
}

unsigned long __getauxval(unsigned long type)
{
    return getauxval(type);
}

char __libc_single_threaded = 0;

int *__h_errno_location(void)
{
    static int value;
    return &value;
}

int bcmp(const void *left, const void *right, size_t size)
{
    return memcmp(left, right, size);
}

int malloc_trim(size_t pad)
{
    (void)pad;
    return 0;
}

int pthread_setcanceltype(int type, int *previous)
{
    (void)type;
    if (previous) {
        *previous = 0;
    }
    return 0;
}

int posix_spawn_file_actions_addchdir_np(void *actions, const char *path)
{
    (void)actions;
    (void)path;
    return ENOSYS;
}

/* ------------------------------------------------------------- ucontext (absent) */

int getcontext(void *context)
{
    (void)context;
    errno = ENOSYS;
    return -1;
}

int setcontext(const void *context)
{
    (void)context;
    errno = ENOSYS;
    return -1;
}

void makecontext(void *context, void (*function)(void), int argc, ...)
{
    (void)context;
    (void)function;
    (void)argc;
    LOGW("makecontext() is unavailable on bionic");
}

/* ------------------------------------------------------------- ctype tables */

/* glibc's bit values (ctype.h: _ISbit(bit)). */
#define IS_UPPER 0x0100
#define IS_LOWER 0x0200
#define IS_ALPHA 0x0400
#define IS_DIGIT 0x0800
#define IS_XDIGIT 0x1000
#define IS_SPACE 0x2000
#define IS_PRINT 0x4000
#define IS_GRAPH 0x8000
#define IS_BLANK 0x0001
#define IS_CNTRL 0x0002
#define IS_PUNCT 0x0004
#define IS_ALNUM 0x0008

static unsigned short ctype_flags[384];
static int ctype_lower[384];
static int ctype_upper[384];
static const unsigned short *ctype_flags_mid = ctype_flags + 128;
static const int *ctype_lower_mid = ctype_lower + 128;
static const int *ctype_upper_mid = ctype_upper + 128;
static int ctype_ready;

static void build_ctype(void)
{
    if (ctype_ready) {
        return;
    }
    for (int c = 0; c < 256; c++) {
        unsigned short flags = 0;
        if (c >= 'A' && c <= 'Z') flags |= IS_UPPER | IS_ALPHA;
        if (c >= 'a' && c <= 'z') flags |= IS_LOWER | IS_ALPHA;
        if (c >= '0' && c <= '9') flags |= IS_DIGIT;
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
            flags |= IS_XDIGIT;
        if (c == ' ' || (c >= '\t' && c <= '\r')) flags |= IS_SPACE;
        if (c == ' ' || c == '\t') flags |= IS_BLANK;
        if (c < 0x20 || c == 0x7f) flags |= IS_CNTRL;
        if (c >= 0x21 && c <= 0x7e) flags |= IS_PRINT | IS_GRAPH;
        if (c > 0x20 && c < 0x7f && !(flags & (IS_ALNUM | IS_GRAPH)))
            flags |= IS_PUNCT;
        if (flags & (IS_ALPHA | IS_DIGIT)) flags |= IS_ALNUM;
        if (flags & IS_GRAPH && !(flags & IS_ALNUM)) flags |= IS_PUNCT;
        ctype_flags[c + 128] = flags;
        ctype_lower[c + 128] = (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
        ctype_upper[c + 128] = (c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c;
    }
    /* glibc fills the -128..-1 range with zeros; the arrays are zeroed. */
    ctype_ready = 1;
}

const unsigned short **__ctype_b_loc(void)
{
    build_ctype();
    if (!ctype_ready) {
        LOGW("ctype init failed");
    }
    __android_log_print(ANDROID_LOG_INFO, TAG, "__ctype_b_loc -> %p (table %p)",
                        (void *)&ctype_flags_mid, (void *)ctype_flags_mid);
    return &ctype_flags_mid;
}

const int **__ctype_tolower_loc(void)
{
    build_ctype();
    return &ctype_lower_mid;
}

const int **__ctype_toupper_loc(void)
{
    build_ctype();
    return &ctype_upper_mid;
}

/* --------------------------------------------------------- locale internals */

/* glibc's `struct __locale_struct`.  libstdc++'s GNU locale model reads this
 * layout directly and never calls __ctype_b_loc() for it:
 *   ctype<char>::classic_table()  -> ldr  x0, [locale, #0x68]   (__ctype_b)
 *   ctype<char>::ctype(...)       -> ldr  q31,[locale, #0x70]   (tolower/toupper)
 * bionic's locale_t is a different object with none of these slots, so we
 * hand libstdc++ objects that have the glibc shape.  Their named-locale slots
 * are never interpreted (all glibc functions that would are shimmed here).
 *
 * Note that libstdc++ calls __newlocale(1 << LC_ALL, i.e. mask 0x40, "C", 0):
 * LC_ALL is not a bit in either libc's category mask, bionic rejects the call
 * with EINVAL, glibc silently treats it as "all defaults".  Returning a
 * C-populated object preserves glibc's behaviour.
 */
struct glibc_locale {
    void *__locales[13];                 /* 0x00 struct __locale_data *   */
    const unsigned short *__ctype_b;     /* 0x68                        */
    const int *__ctype_tolower;          /* 0x70                        */
    const int *__ctype_toupper;          /* 0x78                        */
    const char *__names[13];             /* 0x80                        */
};

struct glibc_locale_storage {
    struct glibc_locale locale;
    char name[64];
};

#define GLOBAL_LOCALE ((locale_t)-1)

/* NULL means LC_GLOBAL_LOCALE, exactly like glibc's thread locale. */
static __thread locale_t thread_locale;

static struct glibc_locale *locale_alloc(const char *name)
{
    struct glibc_locale_storage *storage =
        calloc(1, sizeof(struct glibc_locale_storage));
    if (!storage) {
        return NULL;
    }
    build_ctype();
    storage->locale.__ctype_b = ctype_flags_mid;
    storage->locale.__ctype_tolower = ctype_lower_mid;
    storage->locale.__ctype_toupper = ctype_upper_mid;
    if (!name || !*name) {
        name = "C";
    }
    snprintf(storage->name, sizeof(storage->name), "%s", name);
    for (int i = 0; i < 13; i++) {
        storage->locale.__names[i] = storage->name;
    }
    return &storage->locale;
}

locale_t __newlocale(int mask, const char *name, locale_t base)
{
    if (mask == 0) {
        return base;
    }
    struct glibc_locale *result = locale_alloc(name);
    LOGW("__newlocale(mask=0x%x, \"%s\") -> %p", mask,
         name ? name : "(null)", (void *)result);
    return (locale_t)result;
}

locale_t __duplocale(locale_t dataset)
{
    if (!dataset || dataset == GLOBAL_LOCALE) {
        return (locale_t)locale_alloc("C");
    }
    const struct glibc_locale *source = (const struct glibc_locale *)dataset;
    struct glibc_locale *copy =
        (struct glibc_locale *)locale_alloc(source->__names[0]);
    return (locale_t)copy;
}

void __freelocale(locale_t dataset)
{
    if (dataset && dataset != GLOBAL_LOCALE) {
        free(dataset);
    }
}

locale_t __uselocale(locale_t dataset)
{
    locale_t previous = thread_locale;
    if (!dataset) {
        /* Query only: report what is installed right now. */
        return previous ? previous : GLOBAL_LOCALE;
    }
    if (dataset == GLOBAL_LOCALE) {
        thread_locale = NULL;
    } else {
        thread_locale = dataset;
    }
    return previous ? previous : GLOBAL_LOCALE;
}

/* glibc nl_item values are ((category << 16) | index); bionic's are a flat
 * enum, so translate what libstdc++'s facets ask for and keep the C answers
 * for everything else. */
char *__nl_langinfo_l(int item, locale_t locale)
{
    (void)locale;
    int category = item >> 16;
    int index = item & 0xffff;

    if (category == 0) {                  /* LC_CTYPE */
        /* libstdc++ only asks for the codeset; class tables come from the
         * glibc-shaped locale object. */
        return index == 14 ? nl_langinfo(CODESET) : (char *)"";
    }
    switch (category) {
    case 2:                               /* LC_TIME */
        if (index <= 6) return nl_langinfo(ABDAY_1 + index);
        if (index <= 13) return nl_langinfo(DAY_1 + (index - 7));
        if (index <= 25) return nl_langinfo(ABMON_1 + (index - 14));
        if (index <= 37) return nl_langinfo(MON_1 + (index - 26));
        switch (index) {
        case 38: return nl_langinfo(AM_STR);
        case 39: return nl_langinfo(PM_STR);
        case 40: return nl_langinfo(D_T_FMT);
        case 41: return nl_langinfo(D_FMT);
        case 42: return nl_langinfo(T_FMT);
        case 43: return nl_langinfo(T_FMT_AMPM);
        case 44: return nl_langinfo(ERA);
        case 45: return nl_langinfo(ERA_D_FMT);
        case 46: return nl_langinfo(ERA_D_T_FMT);
        case 47: return nl_langinfo(ERA_T_FMT);
        case 48: return nl_langinfo(ALT_DIGITS);
        }
        break;
    case 1:                               /* LC_NUMERIC */
        switch (index) {
        case 0: return nl_langinfo(RADIXCHAR);
        case 1: return nl_langinfo(THOUSEP);
        case 2: return (char *)"";        /* no grouping in the C locale */
        }
        break;
    case 4:                               /* LC_MONETARY, C locale values */
        switch (index) {
        case 0: case 1: return nl_langinfo(CRNCYSTR);
        case 2: case 3: case 4: case 5: case 6: return (char *)"";
        case 7: case 8: case 9: case 10:
        case 11: case 12: case 13: case 14: return (char *)"\x7f"; /* CHAR_MAX */
        }
        break;
    case 5:                               /* LC_MESSAGES */
        switch (index) {
        case 0: return nl_langinfo(YESEXPR);
        case 1: return nl_langinfo(NOEXPR);
        case 2: return nl_langinfo(CODESET);
        }
        break;
    }
    return (char *)"";
}

size_t __strftime_l(char *buffer, size_t max, const char *format,
                    const struct tm *tm, locale_t locale)
{
    (void)locale;
    return strftime(buffer, max, format, tm);
}

size_t __wcsftime_l(wchar_t *buffer, size_t max, const wchar_t *format,
                    const struct tm *tm, locale_t locale)
{
    (void)locale;
    return wcsftime(buffer, max, format, tm);
}

double __strtod_l(const char *text, char **end, locale_t locale)
{
    (void)locale;   /* our locale_t is a glibc-shaped object, not a bionic one */
    return strtod(text, end);
}

float __strtof_l(const char *text, char **end, locale_t locale)
{
    (void)locale;
    return strtof(text, end);
}

int __strcoll_l(const char *left, const char *right, locale_t locale)
{
    (void)locale;
    return strcoll(left, right);
}

size_t __strxfrm_l(char *dest, const char *src, size_t size, locale_t locale)
{
    (void)locale;
    return strxfrm(dest, src, size);
}

int __wcscoll_l(const wchar_t *left, const wchar_t *right, locale_t locale)
{
    (void)locale;
    return wcscoll(left, right);
}

size_t __wcsxfrm_l(wchar_t *dest, const wchar_t *src, size_t size, locale_t locale)
{
    (void)locale;
    return wcsxfrm(dest, src, size);
}

int __iswctype_l(wint_t wc, wctype_t type, locale_t locale)
{
    (void)locale;
    return iswctype(wc, type);
}

wint_t __towlower_l(wint_t wc, locale_t locale)
{
    (void)locale;
    return towlower(wc);
}

wint_t __towupper_l(wint_t wc, locale_t locale)
{
    (void)locale;
    return towupper(wc);
}

wctype_t __wctype_l(const char *name, locale_t locale)
{
    (void)locale;
    return wctype(name);
}

/* --------------------------------------------------------------- gettext-ish */

char *bindtextdomain(const char *domain, const char *directory)
{
    (void)domain;
    return (char *)directory;
}

char *bind_textdomain_codeset(const char *domain, const char *codeset)
{
    (void)domain;
    return (char *)codeset;
}

char *textdomain(const char *domain)
{
    (void)domain;
    return (char *)"";
}

char *gettext(const char *message)
{
    return (char *)message;
}

char *dgettext(const char *domain, const char *message)
{
    (void)domain;
    return (char *)message;
}

/* -------------------------------------------------------------- C23 libc names */

long __isoc23_strtol(const char *text, char **end, int base)
{
    return strtol(text, end, base);
}

long long __isoc23_strtoll(const char *text, char **end, int base)
{
    return strtoll(text, end, base);
}

unsigned long __isoc23_strtoul(const char *text, char **end, int base)
{
    return strtoul(text, end, base);
}

unsigned long long __isoc23_strtoull(const char *text, char **end, int base)
{
    return strtoull(text, end, base);
}

long __isoc23_wcstol(const wchar_t *text, wchar_t **end, int base)
{
    return wcstol(text, end, base);
}

int __isoc23_sscanf(const char *text, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vsscanf(text, format, args);
    va_end(args);
    return result;
}

int __isoc23_vsscanf(const char *text, const char *format, va_list args)
{
    return vsscanf(text, format, args);
}

int __isoc23_fscanf(FILE *stream, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vfscanf(stream, format, args);
    va_end(args);
    return result;
}

int __isoc99_sscanf(const char *text, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vsscanf(text, format, args);
    va_end(args);
    return result;
}

/* --------------------------------------------------------------- misc strings */

char *__xpg_basename(char *path)
{
    if (!path || !*path) {
        return (char *)".";
    }
    char *end = path + strlen(path) - 1;
    while (end > path && *end == '/') {
        *end-- = '\0';
    }
    if (end == path && *end == '/') {
        return path;
    }
    char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

int __xpg_strerror_r(int error, char *buffer, size_t size)
{
    /* bionic's strerror_r returns char*, not int */
    (void)strerror_r(error, buffer, size);
    return 0;
}

int __sigsetjmp(void *environment, int save_mask)
{
    return sigsetjmp(*(sigjmp_buf *)environment, save_mask);
}

/* --------------------------------------------------------------- resolver */

/* The game (glibc) and bionic disagree about struct addrinfo: both start with
 * the four ints and the socklen_t, but then
 *
 *   glibc:  ai_addr@0x18      ai_canonname@0x20
 *   bionic: ai_canonname@0x18 ai_addr@0x20
 *
 * and about the AI_* flag values.  Passing bionic nodes straight through makes
 * the game read ai_canonname (NULL unless AI_CANONNAME was asked for) as its
 * ai_addr: the first RCON address lookup crashed in memcpy(NULL, ...,
 * sizeof(sockaddr_in)) inside OtherSettings::validateLoadedRconSocketAddress.
 *
 * We therefore ask bionic for its list, translate every node into the glibc
 * layout (separately allocated address and canonical name) and remember the
 * translated lists so our freeaddrinfo can release them.  Anything we did not
 * hand out is forwarded to bionic untouched; conversely, a translated list we
 * failed to register still degrades safely with bionic's freeaddrinfo (it
 * frees the pointer in the first slot, which is our ai_addr) at the cost of a
 * leaked canonical name.
 *
 * bionic inlines ai_addr into the node allocation and frees only the pointer
 * at 0x18, so translated nodes must not simply be swapped in place.
 */
struct glibc_addrinfo {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    socklen_t ai_addrlen;
    struct sockaddr *ai_addr;        /* 0x18 */
    char *ai_canonname;              /* 0x20 */
    struct glibc_addrinfo *ai_next;  /* 0x28 */
};

_Static_assert(offsetof(struct glibc_addrinfo, ai_addr) == 0x18,
               "glibc ai_addr slot");
_Static_assert(offsetof(struct glibc_addrinfo, ai_canonname) == 0x20,
               "glibc ai_canonname slot");
_Static_assert(sizeof(struct glibc_addrinfo) == 0x30, "glibc addrinfo size");

static int (*bionic_getaddrinfo)(const char *, const char *,
                                 const struct addrinfo *,
                                 struct addrinfo **);
static void (*bionic_freeaddrinfo)(struct addrinfo *);
static pthread_once_t resolver_once = PTHREAD_ONCE_INIT;

static void resolver_init(void)
{
    bionic_getaddrinfo = (int (*)(const char *, const char *,
                                  const struct addrinfo *,
                                  struct addrinfo **))
        dlsym(RTLD_NEXT, "getaddrinfo");
    bionic_freeaddrinfo = (void (*)(struct addrinfo *))
        dlsym(RTLD_NEXT, "freeaddrinfo");
    if (!bionic_getaddrinfo || !bionic_freeaddrinfo) {
        LOGW("resolver: dlsym(RTLD_NEXT) failed for getaddrinfo/freeaddrinfo");
    }
}

/* Addrinfo lists we allocated, so freeaddrinfo can tell them from bionic's. */
#define TRANSLATED_MAX 32
static struct addrinfo *translated[TRANSLATED_MAX];
static pthread_mutex_t translated_lock = PTHREAD_MUTEX_INITIALIZER;

static int translated_add(struct addrinfo *head)
{
    int stored = 0;
    pthread_mutex_lock(&translated_lock);
    for (int i = 0; i < TRANSLATED_MAX; i++) {
        if (!translated[i]) {
            translated[i] = head;
            stored = 1;
            break;
        }
    }
    pthread_mutex_unlock(&translated_lock);
    if (!stored) {
        LOGW("resolver: translated-list table full, list will leak");
    }
    return stored;
}

static int translated_take(struct addrinfo *head)
{
    int found = 0;
    pthread_mutex_lock(&translated_lock);
    for (int i = 0; i < TRANSLATED_MAX; i++) {
        if (translated[i] == head) {
            translated[i] = NULL;
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&translated_lock);
    return found;
}

static void translated_free(struct glibc_addrinfo *head)
{
    while (head) {
        struct glibc_addrinfo *next = head->ai_next;
        free(head->ai_addr);
        free(head->ai_canonname);
        free(head);
        head = next;
    }
}

/* glibc AI_* -> bionic AI_* (same low bits for the first three). */
static int translate_flags(int flags)
{
    int result = 0;
    if (flags & 0x0001) result |= AI_PASSIVE;
    if (flags & 0x0002) result |= AI_CANONNAME;
    if (flags & 0x0004) result |= AI_NUMERICHOST;
    if (flags & 0x0400) result |= AI_NUMERICSERV;  /* 0x400 is ADDRCONFIG in bionic */
    if (flags & 0x0020) result |= AI_ADDRCONFIG;
    if (flags & 0x0010) result |= AI_ALL;
    if (flags & 0x0008) result |= AI_V4MAPPED;
    return result;
}

static struct glibc_addrinfo *translate_list(const struct addrinfo *source)
{
    struct glibc_addrinfo *head = NULL;
    struct glibc_addrinfo **tail = &head;
    for (const struct addrinfo *src = source; src; src = src->ai_next) {
        struct glibc_addrinfo *dst = calloc(1, sizeof(*dst));
        if (!dst) {
            translated_free(head);
            return NULL;
        }
        dst->ai_family = src->ai_family;
        dst->ai_socktype = src->ai_socktype;
        dst->ai_protocol = src->ai_protocol;
        dst->ai_addrlen = src->ai_addrlen;
        if (src->ai_addr && src->ai_addrlen) {
            dst->ai_addr = malloc(src->ai_addrlen);
            if (!dst->ai_addr) {
                free(dst);
                translated_free(head);
                return NULL;
            }
            memcpy(dst->ai_addr, src->ai_addr, src->ai_addrlen);
        }
        if (src->ai_canonname) {
            dst->ai_canonname = strdup(src->ai_canonname);
            if (!dst->ai_canonname) {
                free(dst->ai_addr);
                free(dst);
                translated_free(head);
                return NULL;
            }
        }
        *tail = dst;
        tail = &dst->ai_next;
    }
    return head;
}

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **result)
{
    pthread_once(&resolver_once, resolver_init);
    if (!bionic_getaddrinfo || !bionic_freeaddrinfo) {
        return EAI_SYSTEM;
    }

    /* Only the leading ints matter to bionic; give it a clean bionic struct so
     * it never reads glibc's ai_addr as ai_canonname. */
    struct addrinfo bionic_hints;
    if (hints) {
        memset(&bionic_hints, 0, sizeof(bionic_hints));
        bionic_hints.ai_flags = translate_flags(hints->ai_flags);
        bionic_hints.ai_family = hints->ai_family;
        bionic_hints.ai_socktype = hints->ai_socktype;
        bionic_hints.ai_protocol = hints->ai_protocol;
        hints = &bionic_hints;
    }

    struct addrinfo *native = NULL;
    int error = bionic_getaddrinfo(node, service, hints, &native);
    if (error != 0) {
        return error;
    }
    struct glibc_addrinfo *translated_list = translate_list(native);
    bionic_freeaddrinfo(native);
    if (!translated_list) {
        return EAI_MEMORY;
    }
    translated_add((struct addrinfo *)translated_list);
    LOGI("getaddrinfo(\"%s\", \"%s\") -> %s addrlen %u",
         node ? node : "(null)", service ? service : "(null)",
         translated_list->ai_addr ? "address" : "no address",
         (unsigned)translated_list->ai_addrlen);
    *result = (struct addrinfo *)translated_list;
    return 0;
}

void freeaddrinfo(struct addrinfo *list)
{
    if (!list) {
        return;
    }
    pthread_once(&resolver_once, resolver_init);
    if (translated_take(list)) {
        translated_free((struct glibc_addrinfo *)list);
        return;
    }
    if (bionic_freeaddrinfo) {
        bionic_freeaddrinfo(list);
    }
}

int res_nquery(void *state, const char *name, int class_, int type,
               unsigned char *answer, int answer_length)
{
    (void)state;
    (void)name;
    (void)class_;
    (void)type;
    (void)answer;
    (void)answer_length;
    return -1;
}

/* ------------------------------------------------------------- sysconf */

/* The game was built against glibc, whose _SC_* constant values differ
 * from bionic's: glibc _SC_PAGESIZE=30 is bionic _SC_XOPEN_ENH_I18N, and
 * glibc _SC_PHYS_PAGES=85 is bionic _SC_THREAD_SAFE_FUNCTIONS.  Before
 * this wrapper the game computed its "RAM: 0 MB" line from those
 * mismatches, which forced the lowest video-memory quality profile.
 * Translate the desktop constants the game actually queries and forward
 * everything else to bionic's sysconf (found via RTLD_NEXT, since the
 * game binds this file's sysconf first). */
static long (*sysconf_bionic)(int);

static long sysconf_real(int name)
{
    if (!sysconf_bionic) {
        sysconf_bionic = (long (*)(int))dlsym(RTLD_NEXT, "sysconf");
        if (!sysconf_bionic) {
            /* Not reachable in practice: libc.so is always in the link map. */
            return -1;
        }
    }
    return sysconf_bionic(name);
}

long sysconf(int name)
{
    static int logged;
    switch (name) {
    case 30: /* glibc _SC_PAGESIZE */
        return sysconf_real(_SC_PAGESIZE);
    case 83: /* glibc _SC_NPROCESSORS_CONF */
        return sysconf_real(_SC_NPROCESSORS_CONF);
    case 84: /* glibc _SC_NPROCESSORS_ONLN */
        return sysconf_real(_SC_NPROCESSORS_ONLN);
    case 85: { /* glibc _SC_PHYS_PAGES */
        struct sysinfo si;
        if (sysinfo(&si) != 0 || si.mem_unit == 0) {
            return -1;
        }
        long page = sysconf_real(_SC_PAGESIZE);
        long pages = (long)((unsigned long long)si.totalram * si.mem_unit / 4096ULL);
        if (!logged) {
            logged = 1;
            LOGI("sysconf: glibc _SC_PHYS_PAGES -> %ld pages (%ld MB); "
                 "bionic constant translation active",
                 pages, pages * 4096 / (1024 * 1024));
        }
        return page > 0 ? pages : -1;
    }
    case 86: { /* glibc _SC_AVPHYS_PAGES */
        FILE *f = fopen("/proc/meminfo", "r");
        if (!f) {
            return -1;
        }
        char line[128];
        long avail_kb = -1;
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "MemAvailable:", 13) == 0) {
                avail_kb = strtol(line + 13, NULL, 10);
                break;
            }
        }
        fclose(f);
        return avail_kb > 0 ? avail_kb / 4 : -1;
    }
    default:
        return sysconf_real(name);
    }
}
