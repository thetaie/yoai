"""Pack an MNN model directory into one file the user can copy to a phone.

An MNN omni model is about twenty files -- config.json plus a graph and a weight
blob for the thinker, audio encoder, talker, diffusion transformer, vocoder and
speaker dictionary -- and MNN resolves every one of them relative to config.json.
So the whole directory has to arrive intact. Asking somebody to copy twenty
files onto a phone and preserve the layout is asking them to fail; one file they
can manage.

The app extracts it with src/utils/bundle.cpp, which holds the format.

No compression, deliberately. The payload is quantized weights -- int4 and int8
blobs with nothing a general-purpose compressor can find -- so the ceiling is a
few percent, against a compression library on three platforms and a
decompression pass over several gigabytes on a phone. This writes a header and
then concatenates.

Usage:

    python tools/pack-mnn-model.py <model-dir> <out.bundle> [--with-vision]
    python tools/pack-mnn-model.py --list <out.bundle>
"""
import os
import struct
import sys

MAGIC = b"BCBUNDL1"

# Skipped rather than packed: droppings from a previous run on the desktop, and
# the caches MNN writes beside the model. They are regenerated on the device and
# would otherwise be gigabytes of dead weight in the copy.
SKIP_NAMES = {"output.wav", ".DS_Store", "Thumbs.db"}
SKIP_PREFIXES = ("tmp_", ".cache", ".git")
SKIP_SUFFIXES = (".bundle",)

# The vision tower, excluded by default, and it is not a small saving: 353 MB of
# the 3.74 GB for a capability this app does not use. MNN reads is_visual out of
# llm_config.json and never loads it, so the files are pure freight.
#
# --with-vision keeps them, for a caller who does want images.
VISION_PREFIXES = ("visual.mnn",)


def collect(model_dir, with_vision=False):
    """Every file to pack, as (relative name with '/', absolute path, size)."""
    out = []
    for root, dirs, files in os.walk(model_dir):
        dirs[:] = [d for d in dirs if not d.startswith(SKIP_PREFIXES)]
        for name in sorted(files):
            if name in SKIP_NAMES or name.startswith(SKIP_PREFIXES) \
                    or name.endswith(SKIP_SUFFIXES):
                continue
            if not with_vision and name.startswith(VISION_PREFIXES):
                continue
            path = os.path.join(root, name)
            rel = os.path.relpath(path, model_dir).replace(os.sep, "/")
            out.append((rel, path, os.path.getsize(path)))
    out.sort(key=lambda e: e[0])
    return out


def pack(model_dir, out_path, with_vision=False):
    entries = collect(model_dir, with_vision)
    if not entries:
        raise SystemExit("nothing to pack under " + model_dir)
    if not any(e[0] == "config.json" for e in entries):
        # Refused rather than packed: MNN resolves everything relative to
        # config.json, so a bundle without one cannot be loaded and the failure
        # would surface on the phone instead of here.
        raise SystemExit("no config.json in " + model_dir + " -- is that an MNN model?")

    table = bytearray()
    table += MAGIC
    table += struct.pack("<I", len(entries))
    for rel, _, size in entries:
        raw = rel.encode("utf-8")
        table += struct.pack("<H", len(raw))
        table += raw
        table += struct.pack("<Q", size)

    total = sum(e[2] for e in entries)
    written = 0
    with open(out_path, "wb") as out:
        out.write(table)
        for rel, path, size in entries:
            with open(path, "rb") as src:
                left = size
                while left:
                    chunk = src.read(min(left, 1 << 20))
                    if not chunk:
                        # The table already promised this many bytes, so a file
                        # that shrank mid-pack would produce a bundle the
                        # extractor rejects. Better to stop here.
                        raise SystemExit("%s ended early" % path)
                    out.write(chunk)
                    left -= len(chunk)
                    written += len(chunk)
            print("  %-34s %9.1f MB" % (rel, size / 1048576.0))

    print("\n%d files, %.2f GB -> %s" % (len(entries), written / 1073741824.0, out_path))
    print("header %d bytes" % len(table))
    if not with_vision:
        print("vision tower excluded (--with-vision keeps it)")


def show(bundle_path):
    with open(bundle_path, "rb") as f:
        if f.read(8) != MAGIC:
            raise SystemExit(bundle_path + " is not a bundle")
        (count,) = struct.unpack("<I", f.read(4))
        total = 0
        for _ in range(count):
            (n,) = struct.unpack("<H", f.read(2))
            name = f.read(n).decode("utf-8")
            (size,) = struct.unpack("<Q", f.read(8))
            total += size
            print("  %-34s %9.1f MB" % (name, size / 1048576.0))
        print("\n%d files, %.2f GB of payload" % (count, total / 1073741824.0))
        header = f.tell()
        actual = os.path.getsize(bundle_path)
        if actual != header + total:
            print("MISMATCH: file is %d bytes, table says %d" % (actual, header + total))


if __name__ == "__main__":
    args = sys.argv[1:]
    with_vision = "--with-vision" in args
    args = [a for a in args if a != "--with-vision"]
    if len(args) == 2 and args[0] == "--list":
        show(args[1])
    elif len(args) == 2:
        pack(args[0], args[1], with_vision)
    else:
        raise SystemExit(__doc__)
