#include "utils/bundle.hpp"

#include "utils/log.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#define YOAI_MKDIR(p) _mkdir(p)
#define YOAI_UNLINK(p) _unlink(p)
#define YOAI_RMDIR(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define YOAI_MKDIR(p) mkdir(p, 0700)
#define YOAI_UNLINK(p) unlink(p)
#define YOAI_RMDIR(p) rmdir(p)
#endif

namespace yoai {
namespace utils {
namespace {

const char  kMagic[8]   = {'B', 'C', 'B', 'U', 'N', 'D', 'L', '1'};
const size_t kCopyChunk = 1u << 20;   // 1 MiB: large enough to be storage-bound

bool read_exact(std::FILE* f, void* into, size_t n) {
    return std::fread(into, 1, n, f) == n;
}

unsigned long long le64(const unsigned char* p) {
    unsigned long long v = 0;
    for (int i = 7; i >= 0; --i) {
        v = (v << 8) | p[i];
    }
    return v;
}

unsigned int le32(const unsigned char* p) {
    return static_cast<unsigned int>(p[0]) | (static_cast<unsigned int>(p[1]) << 8) |
           (static_cast<unsigned int>(p[2]) << 16) | (static_cast<unsigned int>(p[3]) << 24);
}

unsigned int le16(const unsigned char* p) {
    return static_cast<unsigned int>(p[0]) | (static_cast<unsigned int>(p[1]) << 8);
}

// A name is only allowed to describe a place inside the destination.
//
// Checked rather than sanitized: a bundle whose table contains "../../etc" is
// not a bundle with a fixable typo, it is a bundle to refuse. Rewriting such a
// name into something safe would hide that the file was hostile.
bool name_is_safe(const std::string& name) {
    if (name.empty() || name.size() > 512) {
        return false;
    }
    if (name[0] == '/' || name[0] == '\\') {
        return false;
    }
    if (name.find('\\') != std::string::npos) {
        return false;   // one separator, and it is '/'
    }
    if (name.size() >= 2 && name[1] == ':') {
        return false;   // a drive letter
    }
    // ".." as a whole path component, anywhere.
    size_t at = 0;
    while (at < name.size()) {
        const size_t slash = name.find('/', at);
        const std::string part =
            name.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
        if (part == ".." || part == ".") {
            return false;
        }
        if (slash == std::string::npos) {
            break;
        }
        at = slash + 1;
    }
    return true;
}

bool make_dir(const std::string& path) {
    if (YOAI_MKDIR(path.c_str()) == 0) {
        return true;
    }
    // Already there is success: extraction runs over a tree it may have made
    // itself on a previous attempt.
    std::FILE* probe = std::fopen((path + "/.").c_str(), "rb");
    if (probe) {
        std::fclose(probe);
        return true;
    }
#if defined(_WIN32)
    return _access(path.c_str(), 0) == 0;
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

// Create every parent directory an entry names, so a bundle can carry a tree.
bool make_parents(const std::string& dest_dir, const std::string& name) {
    size_t at = 0;
    while (true) {
        const size_t slash = name.find('/', at);
        if (slash == std::string::npos) {
            return true;
        }
        if (!make_dir(dest_dir + "/" + name.substr(0, slash))) {
            return false;
        }
        at = slash + 1;
    }
}

bool read_table(std::FILE* f, std::vector<BundleEntry>* out, unsigned long long* total) {
    unsigned char head[12];
    if (!read_exact(f, head, sizeof(head))) {
        return false;
    }
    if (std::memcmp(head, kMagic, sizeof(kMagic)) != 0) {
        return false;
    }
    const unsigned int count = le32(head + 8);
    if (count == 0 || count > 4096) {
        return false;
    }

    unsigned long long sum = 0;
    for (unsigned int i = 0; i < count; ++i) {
        unsigned char len_bytes[2];
        if (!read_exact(f, len_bytes, 2)) {
            return false;
        }
        const unsigned int len = le16(len_bytes);
        if (len == 0 || len > 512) {
            return false;
        }
        std::string name(len, '\0');
        if (!read_exact(f, &name[0], len)) {
            return false;
        }
        unsigned char size_bytes[8];
        if (!read_exact(f, size_bytes, 8)) {
            return false;
        }
        BundleEntry e;
        e.name = name;
        e.size = le64(size_bytes);
        sum += e.size;
        out->push_back(e);
    }
    if (total) {
        *total = sum;
    }
    return true;
}

// Remove what an aborted extraction wrote. Best effort: the point is to leave
// nothing MNN would try to load, not to guarantee a pristine filesystem.
void remove_written(const std::string& dest_dir, const std::vector<BundleEntry>& entries,
                    size_t upto) {
    for (size_t i = 0; i < upto && i < entries.size(); ++i) {
        YOAI_UNLINK((dest_dir + "/" + entries[i].name).c_str());
    }
    YOAI_RMDIR(dest_dir.c_str());
}

}  // namespace

bool bundle_list(const std::string& bundle_path, std::vector<BundleEntry>* out,
                 unsigned long long* total_bytes) {
    if (!out) {
        return false;
    }
    out->clear();
    std::FILE* f = std::fopen(bundle_path.c_str(), "rb");
    if (!f) {
        log_error("bundle: cannot open '%s'", bundle_path.c_str());
        return false;
    }
    const bool ok = read_table(f, out, total_bytes);
    std::fclose(f);
    if (!ok) {
        log_error("bundle: '%s' is not a bundle, or its table is truncated",
                  bundle_path.c_str());
        out->clear();
    }
    return ok;
}

bool bundle_extract(const std::string& bundle_path, const std::string& dest_dir,
                    BundleProgress on_progress, void* user_data) {
    std::FILE* f = std::fopen(bundle_path.c_str(), "rb");
    if (!f) {
        log_error("bundle: cannot open '%s'", bundle_path.c_str());
        return false;
    }

    std::vector<BundleEntry> entries;
    unsigned long long total = 0;
    if (!read_table(f, &entries, &total)) {
        log_error("bundle: '%s' is not a bundle", bundle_path.c_str());
        std::fclose(f);
        return false;
    }

    for (size_t i = 0; i < entries.size(); ++i) {
        if (!name_is_safe(entries[i].name)) {
            // Named in the log: a refusal nobody can see is a mystery.
            log_error("bundle: refusing '%s' -- an entry may not escape its destination",
                      entries[i].name.c_str());
            std::fclose(f);
            return false;
        }
    }

    if (!make_dir(dest_dir)) {
        log_error("bundle: cannot create '%s'", dest_dir.c_str());
        std::fclose(f);
        return false;
    }

    std::vector<char> chunk(kCopyChunk);
    unsigned long long done = 0;

    for (size_t i = 0; i < entries.size(); ++i) {
        const BundleEntry& e = entries[i];
        if (!make_parents(dest_dir, e.name)) {
            log_error("bundle: cannot create directories for '%s'", e.name.c_str());
            std::fclose(f);
            remove_written(dest_dir, entries, i);
            return false;
        }

        const std::string path = dest_dir + "/" + e.name;
        std::FILE* out = std::fopen(path.c_str(), "wb");
        if (!out) {
            log_error("bundle: cannot write '%s'", path.c_str());
            std::fclose(f);
            remove_written(dest_dir, entries, i);
            return false;
        }

        unsigned long long left = e.size;
        bool ok = true;
        while (left > 0) {
            const size_t want = left < kCopyChunk ? static_cast<size_t>(left) : kCopyChunk;
            const size_t got  = std::fread(&chunk[0], 1, want, f);
            if (got != want || std::fwrite(&chunk[0], 1, got, out) != got) {
                // A truncated bundle is the likely cause, and it matters: MNN
                // would load a short weight file and answer with numbers.
                log_error("bundle: '%s' ended early while writing '%s'",
                          bundle_path.c_str(), e.name.c_str());
                ok = false;
                break;
            }
            left -= got;
            done += got;
            if (on_progress && !on_progress(e.name.c_str(), done, total, user_data)) {
                log_warn("bundle: extraction cancelled at '%s'", e.name.c_str());
                ok = false;
                break;
            }
        }
        std::fclose(out);
        if (!ok) {
            std::fclose(f);
            remove_written(dest_dir, entries, i + 1);
            return false;
        }
    }

    std::fclose(f);
    log_info("bundle: extracted %d files, %llu bytes, into %s",
             static_cast<int>(entries.size()), done, dest_dir.c_str());
    return true;
}

}  // namespace utils
}  // namespace yoai
