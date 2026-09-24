// One file in, a directory out.
//
// Why this exists: an MNN model is not a file, it is about twenty of them --
// config.json plus a graph and a weight blob for the thinker, the audio encoder,
// the talker, the diffusion transformer, the vocoder and the speaker dictionary,
// 3.8 GB together. MNN resolves every one of them relative to config.json, so
// the whole directory has to reach the device intact. Asking somebody to copy
// twenty files onto a phone and keep the layout is asking them to fail.
//
// So: pack it into one file on a workstation (design/pack-mnn-model.py), copy
// that one file across, and extract it into the app's own storage here.
//
// **Not compressed, on purpose.** The payload is quantized weights -- int4 and
// int8 blobs with no structure a general-purpose compressor can find. Measured
// or not, the ceiling is a few percent, and the cost is a compression library on
// three platforms plus a decompression pass over 3.8 GB on a phone. Packing is
// concatenation and extraction is a copy; both run at whatever the storage does.
//
// The format is deliberately the simplest thing that can be checked:
//
//     "BCBUNDL1"                        8 bytes
//     count                             uint32 little-endian
//     count x {                         the table
//         name_len                      uint16 little-endian
//         name                          name_len bytes, UTF-8, '/' separated
//         size                          uint64 little-endian
//     }
//     payload                           each entry's bytes, in table order
//
// Offsets are implicit: entry i starts where entry i-1 ended. That is one fewer
// field to disagree with itself, and it lets the packer stream without seeking
// back to fix a header.

#pragma once

#include <string>
#include <vector>

namespace yoai {
namespace platform {

// Progress as extraction runs, so a host can show something during several
// gigabytes of copying. `name` is the entry being written.
//
// Return false to abort. An aborted extraction leaves a partial directory --
// bundle_extract removes it, because a half-written model that MNN would happily
// try to load is worse than no model.
typedef bool (*BundleProgress)(const char* name,
                               unsigned long long done_bytes,
                               unsigned long long total_bytes,
                               void* user_data);

struct BundleEntry {
    std::string        name;
    unsigned long long size = 0;
};

// Read just the table. Cheap -- it does not touch the payload -- so a host can
// show what a bundle contains, and how much room it will need, before
// committing to the copy.
//
// Returns false when the file is missing, truncated, or not a bundle.
bool bundle_list(const std::string& bundle_path,
                 std::vector<BundleEntry>* out,
                 unsigned long long* total_bytes);

// Extract into `dest_dir`, creating it and any subdirectories an entry names.
//
// Returns false on any failure, having removed what it wrote. Partial success is
// not a state this hands back: MNN would load a truncated weight file and
// produce numbers rather than an error, which is the failure shape this project
// keeps trying to design out.
//
// **Entry names are validated, not trusted.** A name that is absolute, names a
// drive, contains a backslash, or contains ".." anywhere is rejected and the
// whole extraction fails. A bundle is a file the user copied from somewhere, and
// an archive that can write outside its destination is a well-known way to own a
// device.
bool bundle_extract(const std::string& bundle_path,
                    const std::string& dest_dir,
                    BundleProgress on_progress,
                    void* user_data);

}  // namespace platform
}  // namespace yoai
