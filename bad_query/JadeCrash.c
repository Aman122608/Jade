//
//  JadeCrash.c
//  Jade
//
//  Crash handling lives in C on purpose: the fatal-signal path must not touch
//  the Swift runtime, ARC, or anything that can take a lock. Everything below
//  the JadeCrashInstall() line runs on the crashing thread with only
//  open/write/close/backtrace.
//

#include "JadeCrash.h"

#include <dirent.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define JADE_PATH_MAX      1024
#define JADE_TAIL_BYTES    (24 * 1024)
#define JADE_MAX_REPORTS   30
#define JADE_MAX_FRAMES    128
#define JADE_CONTEXT_MAX   1024
#define JADE_SESSION_LOG_MAX (2UL * 1024UL * 1024UL)

static char g_dir[JADE_PATH_MAX];
static char g_session_log[JADE_PATH_MAX];
static char g_marker[JADE_PATH_MAX];
static char g_context[JADE_CONTEXT_MAX];
static volatile int g_log_fd = -1;
static volatile int g_installed = 0;
static volatile unsigned long g_log_bytes = 0;
static char g_alt_stack[64 * 1024];

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static void jade_copy(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void jade_delete(const char *path) {
    if (path && path[0]) unlink(path);
}

static void jade_stamp_compact(char *out, size_t cap) {
    time_t now = time(NULL);
    struct tm tmv;
    memset(&tmv, 0, sizeof tmv);
    localtime_r(&now, &tmv);
    strftime(out, cap, "%Y%m%d-%H%M%S", &tmv);
}

static void jade_stamp_human(char *out, size_t cap) {
    time_t now = time(NULL);
    struct tm tmv;
    memset(&tmv, 0, sizeof tmv);
    localtime_r(&now, &tmv);
    strftime(out, cap, "%Y-%m-%d %H:%M:%S %z", &tmv);
}

static void jade_mkdir_p(const char *path) {
    char tmp[JADE_PATH_MAX];
    jade_copy(tmp, sizeof tmp, path);
    size_t len = strlen(tmp);
    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            mkdir(tmp, 0755);
            tmp[i] = '/';
        }
    }
    mkdir(tmp, 0755);
}

static const char *jade_signame(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS:  return "SIGBUS";
        case SIGILL:  return "SIGILL";
        case SIGFPE:  return "SIGFPE";
        case SIGABRT: return "SIGABRT";
        case SIGTRAP: return "SIGTRAP";
        case SIGSYS:  return "SIGSYS";
        case SIGPIPE: return "SIGPIPE";
        default:      return "SIG?";
    }
}

static void jade_sysctl_string(const char *name, char *out, size_t cap) {
    size_t len = cap;
    if (sysctlbyname(name, out, &len, NULL, 0) != 0 || len == 0) {
        jade_copy(out, cap, "unknown");
    }
    out[cap - 1] = '\0';
}

// ---------------------------------------------------------------------------
// Paths / session log
// ---------------------------------------------------------------------------

static void jade_paths_init(void) {
    const char *home = getenv("HOME");
    if (!home || !home[0]) home = "/var/mobile";
    char docs[JADE_PATH_MAX];
    snprintf(docs, sizeof docs, "%s/Documents", home);
    jade_mkdir_p(docs);
    snprintf(g_dir, sizeof g_dir, "%s/JadeCrashLogs", docs);
    jade_mkdir_p(g_dir);
}

// Delete the oldest reports so the folder cannot grow without bound.
static void jade_prune_reports(void) {
    DIR *d = opendir(g_dir);
    if (!d) return;

    char names[JADE_MAX_REPORTS * 2][64];
    time_t stamps[JADE_MAX_REPORTS * 2];
    int count = 0;
    int cap = (int)(sizeof names / sizeof names[0]);

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL && count < cap) {
        const char *n = entry->d_name;
        int is_report = (strncmp(n, "crash-", 6) == 0) ||
                        (strncmp(n, "exception-", 10) == 0) ||
                        (strncmp(n, "note-", 5) == 0);
        int is_session = (strncmp(n, "session-", 8) == 0);
        if (!is_report && !is_session) continue;
        char full[JADE_PATH_MAX];
        snprintf(full, sizeof full, "%s/%s", g_dir, n);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        jade_copy(names[count], sizeof names[0], n);
        stamps[count] = st.st_mtime;
        count++;
    }
    closedir(d);

    if (count <= JADE_MAX_REPORTS) return;

    // Sort newest first (selection sort, tiny N).
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (stamps[j] > stamps[i]) {
                time_t ts = stamps[i]; stamps[i] = stamps[j]; stamps[j] = ts;
                char tmp[64];
                jade_copy(tmp, sizeof tmp, names[i]);
                jade_copy(names[i], sizeof names[0], names[j]);
                jade_copy(names[j], sizeof names[0], tmp);
            }
        }
    }
    for (int i = JADE_MAX_REPORTS; i < count; i++) {
        char full[JADE_PATH_MAX];
        snprintf(full, sizeof full, "%s/%s", g_dir, names[i]);
        unlink(full);
    }
}

// ---------------------------------------------------------------------------
// Report writing
// ---------------------------------------------------------------------------

static void jade_write_tail(int out_fd, const char *path, size_t want_bytes) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    off_t end = lseek(fd, 0, SEEK_END);
    if (end <= 0) { close(fd); return; }
    size_t take = (size_t)end;
    if (take > want_bytes) take = want_bytes;
    off_t start = end - (off_t)take;
    if (take > 0 && lseek(fd, start, SEEK_SET) == start) {
        char buf[4096];
        size_t remaining = take;
        while (remaining > 0) {
            size_t chunk = remaining < sizeof buf ? remaining : sizeof buf;
            ssize_t got = read(fd, buf, chunk);
            if (got <= 0) break;
            write(out_fd, buf, (size_t)got);
            remaining -= (size_t)got;
        }
    }
    close(fd);
}

static void jade_write_images(int fd, uint32_t limit) {
    uint32_t total = _dyld_image_count();
    char line[1024];
    for (uint32_t i = 0; i < total && i < limit; i++) {
        const struct mach_header *hdr = _dyld_get_image_header(i);
        const char *name = _dyld_get_image_name(i);
        if (!hdr || !name) continue;
        intptr_t slide = _dyld_get_image_vmaddr_slide(i);
        uintptr_t base = (uintptr_t)hdr + (uintptr_t)slide;
        int n = snprintf(line, sizeof line, "  0x%016lx  %s\n",
                         (unsigned long)base, name);
        if (n > 0) write(fd, line, (size_t)n);
    }
}

static void jade_write_crash_report(const char *kind, int sig,
                                    const siginfo_t *info) {
    char stamp[32];
    jade_stamp_compact(stamp, sizeof stamp);   // YYYYmmdd-HHMMSS
    char human[64];
    jade_stamp_human(human, sizeof human);

    char path[JADE_PATH_MAX];
    snprintf(path, sizeof path, "%s/%s-%s-%d.txt", g_dir, kind, stamp, (int)getpid());

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;

    char head[1024];
    int n = snprintf(head, sizeof head,
                     "===== JADE CRASH REPORT =====\n"
                     "kind: %s\n"
                     "generated: %s\n"
                     "signal: %d (%s)\n"
                     "context:\n%s\n",
                     kind, human, sig, jade_signame(sig), g_context);
    if (n > 0) write(fd, head, (size_t)n);

    if (info) {
        char fault[256];
        int fn = snprintf(fault, sizeof fault,
                          "fault address: %p\nfault code: %d\n",
                          info->si_addr, info->si_code);
        if (fn > 0) write(fd, fault, (size_t)fn);
    }

    void *frames[JADE_MAX_FRAMES];
    int count = backtrace(frames, JADE_MAX_FRAMES);
    const char *bt_header =
        "\n--- backtrace (raw addresses; symbolize against the dSYM) ---\n";
    write(fd, bt_header, strlen(bt_header));
    if (count > 0) backtrace_symbols_fd(frames, count, fd);

    const char *img_header = "\n--- loaded images (base address  path) ---\n";
    write(fd, img_header, strlen(img_header));
    jade_write_images(fd, 64);

    const char *tail_header = "\n--- session log tail ---\n";
    write(fd, tail_header, strlen(tail_header));
    jade_write_tail(fd, g_session_log, JADE_TAIL_BYTES);

    const char *footer = "\n===== END OF REPORT =====\n";
    write(fd, footer, strlen(footer));
    close(fd);
}

void JadeCrashWriteReport(const char *kind, const char *text) {
    if (!kind) kind = "note";
    if (!g_dir[0]) jade_paths_init();

    char stamp[32];
    jade_stamp_compact(stamp, sizeof stamp);
    char human[64];
    jade_stamp_human(human, sizeof human);

    char path[JADE_PATH_MAX];
    snprintf(path, sizeof path, "%s/%s-%s.txt", g_dir, kind, stamp);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;

    char head[1024];
    int n = snprintf(head, sizeof head,
                     "===== JADE REPORT =====\n"
                     "kind: %s\n"
                     "generated: %s\n"
                     "context:\n%s\n\n",
                     kind, human, g_context[0] ? g_context : "(no context)\n");
    if (n > 0) write(fd, head, (size_t)n);
    if (text) write(fd, text, strlen(text));

    const char *tail_header = "\n--- session log tail ---\n";
    write(fd, tail_header, strlen(tail_header));
    jade_write_tail(fd, g_session_log, JADE_TAIL_BYTES);

    const char *footer = "\n===== END OF REPORT =====\n";
    write(fd, footer, strlen(footer));
    close(fd);
    JadeCrashBreadcrumbString("report written");
}

// ---------------------------------------------------------------------------
// Fatal signal path
// ---------------------------------------------------------------------------

static void jade_signal_handler(int sig, siginfo_t *info, void *uap) {
    (void)uap;
    // Write the report first, then restore the default disposition and let the
    // kernel re-deliver the signal so the system still produces its own .ips.
    jade_write_crash_report("crash", sig, info);
    if (g_marker[0]) unlink(g_marker);

    struct sigaction dfl;
    memset(&dfl, 0, sizeof dfl);
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(sig, &dfl, NULL);
    kill(getpid(), sig);
    _exit(1);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void JadeCrashBreadcrumbString(const char *line) {
    if (!line || g_log_fd < 0) return;
    char ts[32];
    jade_stamp_human(ts, sizeof ts);
    char buf[1024];
    int n = snprintf(buf, sizeof buf, "%s  %s\n", ts, line);
    if (n > 0) {
        size_t len = (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1;
        ssize_t wrote = write(g_log_fd, buf, len);
        if (wrote > 0) {
            g_log_bytes += (unsigned long)wrote;
            // Keep the session log bounded: drop the head once it gets large,
            // so a long session cannot fill the container.
            if (g_log_bytes > JADE_SESSION_LOG_MAX) {
                ftruncate(g_log_fd, 0);
                g_log_bytes = 0;
            }
        }
    }
}

void JadeCrashBreadcrumb(const char *fmt, ...) {
    if (!fmt || g_log_fd < 0) return;
    char tmp[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    JadeCrashBreadcrumbString(tmp);
}

void JadeCrashSetContext(const char *appVersion,
                         const char *bundleID,
                         const char *osVersion,
                         const char *deviceModel) {
    char device[128];
    char osv[128];
    jade_sysctl_string("hw.machine", device, sizeof device);
    jade_sysctl_string("kern.osproductversion", osv, sizeof osv);

    snprintf(g_context, sizeof g_context,
             "  app: %s\n"
             "  bundle: %s\n"
             "  os: %s\n"
             "  device: %s\n"
             "  pid: %d\n",
             appVersion ? appVersion : "unknown",
             bundleID ? bundleID : "unknown",
             osVersion ? osVersion : osv,
             deviceModel ? deviceModel : device,
             (int)getpid());
}

void JadeCrashMarkSessionActive(void) {
    if (!g_marker[0]) return;
    int fd = open(g_marker, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    char human[64];
    jade_stamp_human(human, sizeof human);
    write(fd, human, strlen(human));
    write(fd, "\n", 1);
    close(fd);
}

void JadeCrashMarkCleanExit(void) {
    jade_delete(g_marker);
    JadeCrashBreadcrumbString("session: marked clean exit");
}

const char *JadeCrashDirectory(void) { return g_dir[0] ? g_dir : NULL; }
const char *JadeCrashSessionLogPath(void) { return g_session_log[0] ? g_session_log : NULL; }

void JadeCrashInstall(void) {
    if (g_installed) return;
    g_installed = 1;

    jade_paths_init();
    if (!g_dir[0]) return;

    if (!g_context[0]) JadeCrashSetContext(NULL, NULL, NULL, NULL);
    jade_prune_reports();

    // Detect whether the previous run died without reaching the background.
    snprintf(g_marker, sizeof g_marker, "%s/.session-active", g_dir);
    int unclean_previous = (access(g_marker, F_OK) == 0);

    char stamp[32];
    jade_stamp_compact(stamp, sizeof stamp);
    snprintf(g_session_log, sizeof g_session_log, "%s/session-%s.log", g_dir, stamp);
    g_log_fd = open(g_session_log, O_WRONLY | O_CREAT | O_APPEND, 0644);

    JadeCrashMarkSessionActive();

    char line[256];
    snprintf(line, sizeof line, "session: started%s",
             unclean_previous ? " (previous session ended without a clean exit)" : "");
    JadeCrashBreadcrumbString(line);
    JadeCrashBreadcrumbString(g_context);

    // Warm up backtrace() so the fatal path does not hit lazy allocation.
    void *warm[JADE_MAX_FRAMES];
    (void)backtrace(warm, JADE_MAX_FRAMES);

    stack_t ss;
    memset(&ss, 0, sizeof ss);
    ss.ss_sp = g_alt_stack;
    ss.ss_size = sizeof g_alt_stack;
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = jade_signal_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);

    int signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP, SIGSYS };
    for (size_t i = 0; i < sizeof signals / sizeof signals[0]; i++) {
        sigaction(signals[i], &sa, NULL);
    }

    JadeCrashInstallExceptionHandler();
}
