#pragma once

// Hard compile-time floor, checked here because every translation unit in the
// core includes this header. Catches build-system misconfiguration that
// silently downgrades the standard -- a native module that forgot its
// `cppFlags` -- as a one-line error here rather than as a pile of missing
// std:: names deeper in.
//
// MSVC stops reporting __cplusplus correctly unless /Zc:__cplusplus is set
// (it does in this build), but consult _MSVC_LANG anyway so a stray include
// of this header from a project that forgot the flag still works.
#if defined(_MSVC_LANG)
    static_assert(_MSVC_LANG  >= 201103L, "bodyclock requires C++11 or newer");
#else
    static_assert(__cplusplus >= 201103L, "bodyclock requires C++11 or newer");
#endif

namespace bodyclock { namespace platform {

// Minimum severity that will be emitted. Anything below the current level is
// dropped. Mirrors the LOG_LEVEL config values; the threshold is process-wide.
enum class LogLevel { Debug, Info, Warn, Error };

// Set the process-wide minimum log level. `set_log_level(const char*)` accepts
// the LOG_LEVEL spellings (case-insensitive): "debug", "info", "warning"/"warn",
// "error", "critical"/"fatal"; an unrecognized value leaves the level unchanged.
// Call once at startup, before threads spin up. Default level is Info, so
// log_debug() is silent unless the level is lowered to Debug.
void set_log_level(LogLevel level);
void set_log_level(const char* level);

void log_info(const char* fmt, ...);
void log_warn(const char* fmt, ...);
void log_error(const char* fmt, ...);
void log_debug(const char* fmt, ...);

}
}
