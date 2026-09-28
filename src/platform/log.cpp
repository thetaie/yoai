#include "platform/log.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

#if defined(__ANDROID__)
#include <android/log.h>
#elif defined(__OHOS__)
#include <hilog/log.h>
#define YOAI_USE_HILOG 1
#elif defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include <os/log.h>
#define YOAI_USE_OSLOG 1
#endif
#elif defined(_WIN32)
#include <vector>

#include <windows.h>
#endif

namespace yoai { namespace platform {

namespace {

// Process-wide minimum level; a log call below it is dropped before formatting.
// Defaults to Info so log_debug() stays silent until set_log_level lowers it.
std::atomic<int> g_min_level{static_cast<int>(LogLevel::Info)};

bool enabled(LogLevel level) {
    return static_cast<int>(level) >= g_min_level.load(std::memory_order_relaxed);
}

#if defined(__ANDROID__)
constexpr const char* kTag = "bodyclock";

void emit(int prio, const char* fmt, va_list ap) {
    __android_log_vprint(prio, kTag, fmt, ap);
}
#elif defined(YOAI_USE_HILOG)
// HarmonyOS. Without this branch the core's log lines went to stderr, which
// nothing on a device reads -- so the library was effectively silent exactly
// where it is hardest to attach a debugger.
//
// The level parameter is an int rather than hilog's own ::LogLevel because that
// type name collides with ours inside this namespace; the cast is at the call
// site below, not spread through four functions.
void emit(int level, const char* fmt, va_list ap) {
    char buf[4096];
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    // %{public}s so the line is not redacted to <private> in DevEco's Log window.
    OH_LOG_Print(LOG_APP, static_cast<::LogLevel>(level), 0x3200, "bodyclock", "%{public}s", buf);
}
#elif defined(YOAI_USE_OSLOG)
void emit(os_log_type_t type, const char* fmt, va_list ap) {
    char buf[1024];
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    // %{public}s so the log isn't redacted in Console.app / `log stream`.
    os_log_with_type(OS_LOG_DEFAULT, type, "%{public}s", buf);
}
#else
// Serializes the single-write emit below so concurrent threads -- one per
// in-flight turn -- cannot interleave one record's bytes with another's.
std::mutex g_emit_mutex;

// One assembled record, out.
//
// **A Windows console is not a byte sink.** It renders whatever arrives through
// the active code page, which on a Chinese install is CP936, so UTF-8 comes out
// as the mojibake this project spent an evening reading -- and only from here,
// because Node writes its own lines through the wide API and is never affected.
// WriteConsoleW takes UTF-16 and bypasses the code page, and `stdio: 'inherit'`
// in electron-vite means this process holds the user's console directly, so
// fixing it here fixes what they actually read.
//
// A REDIRECTED stderr -- a pipe, a file -- is a byte sink and keeps the plain
// UTF-8 write, which is what anything reading a log file wants and what every
// other platform gets. GetConsoleMode is what tells the two apart.
void write_record(const char* line) {
#if defined(_WIN32)
    const HANDLE h    = GetStdHandle(STD_ERROR_HANDLE);
    DWORD        mode = 0;
    if (h != nullptr && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode) != 0) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, line, -1, nullptr, 0);
        if (n > 1) {
            std::vector<wchar_t> wide(static_cast<std::size_t>(n));
            if (MultiByteToWideChar(CP_UTF8, 0, line, -1, wide.data(), n) == n) {
                DWORD written = 0;
                // n - 1: the count is of wchar_t INCLUDING the terminator, and
                // the console would draw that as a glyph.
                WriteConsoleW(h, wide.data(), static_cast<DWORD>(n - 1), &written, nullptr);
                return;
            }
        }
    }
#endif
    std::fputs(line, stderr);
}

// Desktop stderr path. The mobile branches need no timestamp: logcat, hilog and
// os_log stamp their own entries. Here we prepend a local-time stamp so
// tail-able log files and CLI output are sortable / correlate-able.
void emit(const char* level, const char* fmt, va_list ap) {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto t   = system_clock::to_time_t(now);
    const auto ms  = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;

    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif

    // Normal output ("2026-06-03 14:30:00.123") is 23 chars + NUL = 24, but the
    // compiler can't prove the std::tm fields are in range and warns that a wild
    // value could need up to 77 bytes (-Wformat-truncation). Size for that worst
    // case so the bound is provable; snprintf truncates safely either way.
    char ts[80];
    std::snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d.%03lld",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec,
        static_cast<long long>(ms));

    // Render the message body, then write the whole record ("[ts] [level] msg\n")
    // in one call under the lock. Separate prefix/body/newline writes would let
    // another thread's record splice into the middle of this one; one write of
    // the assembled line keeps each record intact and correctly ordered.
    char msg[4096];
    std::vsnprintf(msg, sizeof(msg), fmt, ap);

    char line[4352];
    std::snprintf(line, sizeof(line), "[%s] [%s] %s\n", ts, level, msg);

    std::lock_guard<std::mutex> lock(g_emit_mutex);
    write_record(line);
}
#endif

}

//------------------------------------------------------------------------------
// Level control
//------------------------------------------------------------------------------

void set_log_level(LogLevel level) {
    g_min_level.store(static_cast<int>(level), std::memory_order_relaxed);
}

void set_log_level(const char* level) {
    if (!level) return;
    std::string s;
    for (const char* p = level; *p; ++p) {
        s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(*p))));
    }
    if      (s == "debug")                       set_log_level(LogLevel::Debug);
    else if (s == "info")                        set_log_level(LogLevel::Info);
    else if (s == "warning" || s == "warn")      set_log_level(LogLevel::Warn);
    else if (s == "error" || s == "critical" ||
             s == "fatal")                       set_log_level(LogLevel::Error);
    // Unrecognized spelling: leave the current level untouched.
}

//------------------------------------------------------------------------------
// Public logging API
//------------------------------------------------------------------------------

void log_info(const char* fmt, ...) {
    if (!enabled(LogLevel::Info)) return;
    va_list ap; va_start(ap, fmt);
#if defined(__ANDROID__)
    emit(ANDROID_LOG_INFO, fmt, ap);
#elif defined(YOAI_USE_HILOG)
    emit(LOG_INFO, fmt, ap);
#elif defined(YOAI_USE_OSLOG)
    emit(OS_LOG_TYPE_INFO, fmt, ap);
#else
    emit("INFO", fmt, ap);
#endif
    va_end(ap);
}

//------------------------------------------------------------------------------

void log_warn(const char* fmt, ...) {
    if (!enabled(LogLevel::Warn)) return;
    va_list ap; va_start(ap, fmt);
#if defined(__ANDROID__)
    emit(ANDROID_LOG_WARN, fmt, ap);
#elif defined(YOAI_USE_HILOG)
    emit(LOG_WARN, fmt, ap);
#elif defined(YOAI_USE_OSLOG)
    emit(OS_LOG_TYPE_DEFAULT, fmt, ap);
#else
    emit("WARN", fmt, ap);
#endif
    va_end(ap);
}

//------------------------------------------------------------------------------

void log_error(const char* fmt, ...) {
    if (!enabled(LogLevel::Error)) return;
    va_list ap; va_start(ap, fmt);
#if defined(__ANDROID__)
    emit(ANDROID_LOG_ERROR, fmt, ap);
#elif defined(YOAI_USE_HILOG)
    emit(LOG_ERROR, fmt, ap);
#elif defined(YOAI_USE_OSLOG)
    emit(OS_LOG_TYPE_ERROR, fmt, ap);
#else
    emit("ERROR", fmt, ap);
#endif
    va_end(ap);
}

//------------------------------------------------------------------------------

void log_debug(const char* fmt, ...) {
    if (!enabled(LogLevel::Debug)) return;
    va_list ap; va_start(ap, fmt);
#if defined(__ANDROID__)
    emit(ANDROID_LOG_DEBUG, fmt, ap);
#elif defined(YOAI_USE_HILOG)
    emit(LOG_DEBUG, fmt, ap);
#elif defined(YOAI_USE_OSLOG)
    emit(OS_LOG_TYPE_DEBUG, fmt, ap);
#else
    emit("DEBUG", fmt, ap);
#endif
    va_end(ap);
}

}
}
