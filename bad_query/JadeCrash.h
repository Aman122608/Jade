//
//  JadeCrash.h
//  Jade
//
//  Persistent crash + breadcrumb logging for Jade.
//  Reports are written to <HOME>/Documents/JadeCrashLogs/ so they survive
//  the process dying and are reachable from the Files app (file sharing).
//

#ifndef JADE_CRASH_H
#define JADE_CRASH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Install signal handlers (SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT/SIGTRAP/SIGSYS),
/// open the rolling session log and write the session marker. Call once, early,
/// on the main thread. Safe to call more than once (no-op after the first).
void JadeCrashInstall(void);

/// Install the NSObject-level exception handler. Called by JadeCrashInstall();
/// exposed separately only so the ObjC translation unit can be the one to
/// register it.
void JadeCrashInstallExceptionHandler(void);

/// Extra context lines appended to every report (app version, bundle id, ...).
/// Pass NULL for any field you do not have. Call before JadeCrashInstall().
void JadeCrashSetContext(const char *appVersion,
                         const char *bundleID,
                         const char *osVersion,
                         const char *deviceModel);

/// Append one breadcrumb line to the session log. Bounded, thread-safe enough
/// for logging (single write(2) per line).
void JadeCrashBreadcrumbString(const char *line);

/// printf-style variant for C call sites.
void JadeCrashBreadcrumb(const char *fmt, ...);

/// Write a free-form report of the given kind ("exception", "note", ...).
/// Used by the ObjC exception handler and by anything that wants a snapshot.
void JadeCrashWriteReport(const char *kind, const char *text);

/// Absolute path of the crash log directory (never NULL after install).
const char *JadeCrashDirectory(void);

/// Absolute path of the current session log (never NULL after install).
const char *JadeCrashSessionLogPath(void);

/// Drop the "session active" marker: called when the app goes to background,
/// so the next launch does not report the previous run as crashed.
void JadeCrashMarkCleanExit(void);

/// Re-create the "session active" marker: called when the app becomes active.
void JadeCrashMarkSessionActive(void);

#ifdef __cplusplus
}
#endif

#endif /* JADE_CRASH_H */
