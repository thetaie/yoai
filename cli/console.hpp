// UTF-8 in and out of a terminal, on Windows too. Header-only, for the tools
// in cli/ and for examples/server/.
//
// Everything in the core is UTF-8, and on macOS and Linux so is the terminal.
// A Windows console is not: it hands main() its arguments in the ANSI code
// page (936 on a Chinese install), renders the bytes it is given through that
// code page, and returns typed input in it. So a Chinese question passed as an
// argument reaches the model as GBK, and a Chinese reply prints as mojibake --
// the same cause utils/log.cpp works around for log lines.
//
// The remedy is the wide API, used only when the stream really is a console.
// Redirected input and output stay plain UTF-8 bytes, which is what a file or
// a pipe wants.

#pragma once

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>   // CommandLineToArgvW; link shell32
#endif

namespace cli {

#ifdef _WIN32

inline std::string to_utf8(const wchar_t* w, int n) {
    if (n <= 0) {
        return std::string();
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, n, &out[0], bytes, nullptr, nullptr);
    return out;
}

inline bool is_console(HANDLE h) {
    DWORD mode = 0;
    return h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode) != 0;
}

#endif

// The arguments as UTF-8, argv[0] included. Windows re-reads them from the
// wide command line, since the argv main() got has already been through the
// code page and cannot be converted back.
inline std::vector<std::string> utf8_args(int argc, char** argv) {
    std::vector<std::string> out;
#ifdef _WIN32
    int      n    = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &n);
    if (wide != nullptr) {
        for (int i = 0; i < n; ++i) {
            out.push_back(to_utf8(wide[i], static_cast<int>(wcslen(wide[i]))));
        }
        LocalFree(wide);
        return out;
    }
#endif
    for (int i = 0; i < argc; ++i) {
        out.push_back(argv[i]);
    }
    return out;
}

// Write UTF-8 text to stdout or stderr. The text must be whole characters,
// which is what every event the core sends is.
inline void write(std::FILE* f, const std::string& s) {
    std::fflush(f);
#ifdef _WIN32
    HANDLE h = GetStdHandle(f == stderr ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    if (is_console(h)) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                          nullptr, 0);
        std::wstring w(static_cast<std::size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
        DWORD written = 0;
        WriteConsoleW(h, w.data(), static_cast<DWORD>(w.size()), &written, nullptr);
        return;
    }
#endif
    std::fwrite(s.data(), 1, s.size(), f);
    std::fflush(f);
}

// One line of UTF-8 from stdin, without its line ending. False at the end of
// input: Ctrl-D on a POSIX terminal, Ctrl-Z then Enter on a Windows console.
inline bool read_line(std::string* out) {
    out->clear();
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if (is_console(h)) {
        std::wstring line;
        wchar_t      buf[512];
        for (;;) {
            DWORD got = 0;
            if (!ReadConsoleW(h, buf, 512, &got, nullptr) || got == 0) {
                return false;
            }
            line.append(buf, got);
            if (line.find(L'\n') != std::wstring::npos) {
                break;
            }
        }
        if (!line.empty() && line[0] == 0x1A) {   // Ctrl-Z
            return false;
        }
        while (!line.empty() && (line.back() == L'\n' || line.back() == L'\r')) {
            line.pop_back();
        }
        *out = to_utf8(line.data(), static_cast<int>(line.size()));
        return true;
    }
#endif
    if (!std::getline(std::cin, *out)) {
        return false;
    }
    if (!out->empty() && out->back() == '\r') {
        out->pop_back();
    }
    return true;
}

}  // namespace cli
