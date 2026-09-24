# RapidJSON

Commit **`24b5e7a8b27f42fa16b96fc70aade9106cf7102f`** of the Tencent/rapidjson
repository on GitHub, dated 2025-02-26: its `include/rapidjson/` directory,
unchanged, and its `license.txt`. There has been no tagged release since 1.1.0
in 2016, so a commit is the only way to name a version.

It is the exact tree the build used to get from vcpkg: the vcpkg port at this
repo's baseline pins the same commit and applies no patches, and these headers
were copied from that install and compared byte for byte.

Nothing in this directory is edited. To change RapidJSON, replace
`include/rapidjson/` wholesale and record the new commit at the top of this file.

## Why it is vendored rather than resolved

It is header-only, so a package manager has nothing to build for it — only
headers to copy, and a CMake config file that vcpkg ships and most distro
packages do not. Carrying it here takes a package off every mobile cross-build
and `rapidjson-dev` off the desktop list, and pins its version in the repo where
it can be seen, instead of in a port.

## The one thing to know before touching it

**Two RapidJSON versions in one `.so` crash.** It is header-only templates, so
its member functions are emitted as weak symbols into every object that
includes it, and the linker keeps one definition of each. libMNN is compiled
against the 1.1.0 release MNN carries, whose `MemoryPoolAllocator` has a
different layout from this one's — measured as a SIGSEGV in
`MemoryPoolAllocator::Malloc` on MNN's first JSON parse.

So when the omni lane is on, the root `CMakeLists.txt` puts MNN's include
directory `BEFORE` this one, and the core's two JSON files (`src/llm/openai_chat.cpp`,
`src/platform/c_api.cpp`) compile against MNN's copy instead. A new file that
includes RapidJSON has to be in the same target, or the crash comes back.

## License

MIT, in `license.txt`. The same file covers the `msinttypes` headers under
`include/rapidjson/`, which are 3-clause BSD; both want the notice kept with the
source, which this directory does. The file also names a JSON-licensed part --
upstream's `bin/jsonchecker/` -- and that is not here: only `include/` was
taken.
