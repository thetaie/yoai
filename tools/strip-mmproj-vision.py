"""Drop the vision tower out of an omni mmproj, keeping the audio half.

Why this exists: llama.cpp's clip_init loads EVERY tower the file declares.
There is no runtime switch for "audio only" -- the one skip in that function is
a hardcoded Gemma-3N special case -- so an omni mmproj costs its vision weights
in memory even in a process that will never be handed an image. On the
Qwen2.5-Omni mmproj that is 1.25 GiB of 2.44 for nothing.

What it does:

  - keeps  a.*     the audio encoder
           mm.a.*  the audio projector head
  - drops  v.*     the vision encoder
           mm.0.*  } the vision projector MLP
           mm.2.*  }
  - drops every clip.vision.* key and sets clip.has_vision_encoder = false,
    which is the flag clip.cpp actually reads to decide whether to build a
    vision context at all.

Usage:

    python tools/strip-mmproj-vision.py <in.gguf> <out.gguf>

This rewrites metadata, it does not requantize: every tensor kept is copied
byte for byte at its original type. That matters for the reason docs/voice.md
gives at length -- the audio encoder is the instrument the voice work measures
with, and this project has already been bitten once by quantizing one and
getting a confident wrong answer that the obvious check passed.

Verify the result by loading it, not by reading it. A file that parses is not a
file llama.cpp accepts.
"""
import sys
from pathlib import Path

# gguf-py travels with llama.cpp rather than pip, so find it the way the rest of
# this repo finds llama.cpp: through the root build.py's settings, loaded by
# path as the build scripts load it.
def _import_gguf():
    import importlib.util
    sys.dont_write_bytecode = True
    root = Path(__file__).resolve().parents[1]
    spec = importlib.util.spec_from_file_location("yoai_build", str(root / "build.py"))
    b = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(b)
    b.load_env()
    c = Path(b.env("LLAMA_SRC", b.third_dir("llama.cpp"))) / "gguf-py"
    if (c / "gguf" / "__init__.py").exists():
        sys.path.insert(0, str(c))
        return
    raise SystemExit(
        "gguf-py not found under %s. Set LLAMA_SRC in .env to a llama.cpp checkout, "
        "or fetch one: python third/sync.py clone llama.cpp" % c.parent
    )


_import_gguf()
from gguf import GGUFReader, GGUFWriter, GGUFValueType  # noqa: E402

DROP_TENSOR_PREFIXES = ("v.", "mm.0.", "mm.2.")
KEEP_TENSOR_PREFIXES = ("a.", "mm.a.")

DROP_KEY_PREFIXES = ("clip.vision.",)
# Written by GGUFWriter itself from its `arch` argument, so copying it would
# write it twice.
SKIP_KEYS = {"general.architecture"}


def copy_key(writer, field):
    """Re-add one KV field to the writer with its original type.

    An array has to keep BOTH of its types -- ARRAY plus the element type as
    `sub_type`. Passing the element type alone makes the writer treat the list
    as a scalar and fail inside the packer with a message about concatenating a
    list to a bytearray, which says nothing about which key was wrong.
    """
    types = list(field.types)
    if types and types[0] == GGUFValueType.ARRAY:
        element = types[1] if len(types) > 1 else GGUFValueType.STRING
        writer.add_key_value(field.name, field.contents(), GGUFValueType.ARRAY,
                             sub_type=element)
        return
    writer.add_key_value(field.name, field.contents(),
                         types[0] if types else GGUFValueType.STRING)


def main(src: str, dst: str) -> int:
    reader = GGUFReader(src)

    kept, dropped, kept_bytes, dropped_bytes = [], [], 0, 0
    for tensor in reader.tensors:
        if tensor.name.startswith(DROP_TENSOR_PREFIXES):
            dropped.append(tensor)
            dropped_bytes += tensor.n_bytes
        else:
            kept.append(tensor)
            kept_bytes += tensor.n_bytes
            if not tensor.name.startswith(KEEP_TENSOR_PREFIXES):
                # Not fatal, but say so: an unrecognised prefix in an omni mmproj
                # is more likely a tower this script has not been taught about
                # than something harmless.
                print("  note: keeping unrecognised tensor " + tensor.name)

    if not dropped:
        print("nothing to drop -- no v.* or vision projector tensors here")
        return 1

    writer = GGUFWriter(dst, "clip")

    for field in reader.fields.values():
        if field.name in SKIP_KEYS or field.name.startswith(DROP_KEY_PREFIXES):
            continue
        if field.name == "clip.has_vision_encoder":
            continue   # rewritten below
        copy_key(writer, field)

    # The flag clip.cpp reads. Without this the loader still tries to build a
    # vision context and fails on the tensors that are now absent.
    writer.add_key_value("clip.has_vision_encoder", False, GGUFValueType.BOOL)

    for tensor in kept:
        writer.add_tensor(tensor.name, tensor.data, raw_dtype=tensor.tensor_type)

    writer.write_header_to_file()
    writer.write_kv_data_to_file()
    writer.write_tensors_to_file()
    writer.close()

    gib = 1024 ** 3
    print("kept    %4d tensors  %6.2f GiB" % (len(kept), kept_bytes / gib))
    print("dropped %4d tensors  %6.2f GiB" % (len(dropped), dropped_bytes / gib))
    print("wrote   %s" % dst)
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    sys.exit(main(sys.argv[1], sys.argv[2]))
