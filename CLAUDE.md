# ApricotFields

The general-purpose engine layer of Apricot: rendering (Aprend), math, archives,
audio, sync. See `../CLAUDE.md` (the `eqdev` workspace root) for how this repo fits
into the larger architecture; this file covers conventions specific to working
inside `ApricotFields` itself.

## The one rule everything else follows

**General purpose: a module knows nothing about what its caller is doing with it.**
ApArchive doesn't know what an archive holds, ApSync doesn't know what is being kept
in step, ApMath doesn't know what a mesh is or why a ray is cast. Every header says
so in its opening comment, along with what the module deliberately does *not* do.

- No application-domain concept, name or assumption in here - CAD, BIM or any
  other. If a change needs to know what the application is, it belongs above this
  layer.
- **ApricotFields doesn't know who calls it.** Nothing here - code, comments,
  headers, docs - names a caller or assumes which one it is. Write "the caller".
- What the bytes, ids, masks and keys *mean* is the caller's. Take them as opaque
  values and hand them back unchanged.
- A new capability that only one caller wants is still written for any caller, or
  it goes in that caller instead.
- `include/cad/` and `src/cad/` are empty leftovers of the deprecated ApCAD. Don't
  put anything in them.

## Module map

| Module | Header | Prefix | Notes |
|---|---|---|---|
| Core | `apricore.h` | `APRESULT`, `apri_` | The shared result enum and debug names |
| Aprend | `render/aprend*.h` | `aprend_` / `APREND_` | Renderer over SpudGPU. Older code, mid-overhaul |
| ApMath | `aprimath.h`, `math/apmathbvh.h` | `Apri*` types, `apmath_` / `APMATH_` | CPU ray casting, double precision, unrelated to Aprend |
| ApArchive | `archive/aparchive.h` | `aparchive_` / `APARCHIVE_` | ZIP through caller callbacks; no I/O of its own |
| ApAudio | `audio/apaudio.h`, `audio/apaudiocues.h` | `apaudio_` / `APAUDIO_` | Mixer, cues and sound banks |
| ApSync | `sync/apsyncnet.h` | `apsync_` / `APSYNC_` | Sessions, channels, snapshots over SpudNet |

`apricotfields.h` is not a full umbrella: it includes Aprend's seven headers and
nothing of the other modules. Callers include the module header they need.

Each module that has unfinished work keeps it in `AP<MODULE>_TODO.md` at the repo
root. Read the module's TODO before changing it, and update it in the same change
when you finish or add an item.

## Two generations of code - match the one you're in

Aprend is older and written differently from the four newer modules. Don't carry
one style into the other, and don't convert Aprend to the newer shape unless asked.

| | Aprend (`src/render/`) | ApArchive, ApAudio, ApSync, ApMath |
|---|---|---|
| Indentation, braces | tabs, attached braces | 4 spaces, Allman braces |
| Creating a handle | returns the handle, `NULL` on failure | returns `APRESULT`, handle through an `out_` parameter (set to null on failure) |
| Allocation | `malloc` + in-place construction, `goto` cleanup | `new (std::nothrow)` and `delete`, then a `try` block - not the convention any more, see "Code conventions" |
| Failure detail | `printf` of the `SPUDRESULT` | per-call `AP<MODULE>_ERROR` message struct |

Public headers use tabs in both generations.

`.clang-format` describes the Aprend style. Running it over a newer-module file
rewrites the whole file, so don't run clang-format on a file unless asked; format
what you touch by hand to match its neighbours.

## Writing a new module, or a new call in a newer module

Follow `aparchive.h` and `apsyncnet.h`, which state these rules in a "Conventions"
block near the top. A new header gets the same block.

- **Results.** Every call that can fail returns `APRESULT`. Use the general codes
  first; add an `APRESULT_<MODULE>_*` code to `apricore.h` only for a failure the
  caller would branch on, in the module's own numbered block (ApArchive 1001+,
  ApSync 2001+). Never renumber an existing code.
- **Error messages.** A call that can fail for a reason worth a sentence takes an
  `AP<MODULE>_ERROR *` (NULL to skip). It is per call, not stored on the object, so
  concurrent readers stay thread-safe.
- **Struct versions.** Every struct that may grow starts with `struct_size`. Fields
  are only ever added at the end, and the implementation reads it through
  `read_versioned(desc, <STRUCT>_V1_SIZE, d)`. Adding a field in the middle, or
  reordering, breaks binaries built against the old header.
- **No defaults in a desc.** A limit or a time left at 0 is refused with
  `APRESULT_INVALID_ARGUMENT` unless the field's comment says what 0 means.
- **Validate every argument before allocating anything**, then allocate (by hand,
  per "Code conventions"), then fill in inside a `try`. `apmath_mesh_bvh_create`
  shows the order; its `new (std::nothrow)` is the part not to copy.
- **No C++ exception leaves a C entry point.** Internal helpers may throw
  `std::bad_alloc` (they say so in a comment); every `extern "C"` function that can
  reach one catches it and returns `APRESULT_OUT_OF_MEMORY`, freeing what it built.
- **Destroy accepts null.** Value getters on a null handle return 0.
- **Threads are part of the contract.** The header says which calls are safe from
  which threads. Don't add a callback that runs on a module's own thread; ApSync
  delivers events by polling for that reason.
- **Platform gaps come from SpudLib's `SPUD*_EXT_*` macros**, never from a
  platform's name, and surface as `APRESULT_UNSUPPORTED`.

## Code conventions

- **Argument checks are written line by line: one `if`, one condition, one
  return.** They sit at the top of the function, in parameter order, before any
  allocation or call into SpudLib, and each reports exactly what was wrong: its own
  `APRESULT`, and its own message where the call takes an `AP<MODULE>_ERROR`:

  ```cpp
  if (d.max_backlog_size == 0)
      return fail(error, APRESULT_INVALID_ARGUMENT, "max_backlog_size is 0");
  if (d.handshake_timeout_ms == 0)
      return fail(error, APRESULT_INVALID_ARGUMENT, "handshake_timeout_ms is 0");
  ```

  Don't join checks with `||` or `&&`. A joined check can only give one code and
  one message, so the caller can't tell which argument failed. If two arguments
  would return the same code today, they still get a line each, so one can be given
  a more specific code or message later without restructuring.
- Where the `return` goes (same line or the next) and the brace style follow the
  file's generation, per the table above.
- **No allocating `new` and no `delete`: allocate and free by hand.** Which
  allocator depends on whether the object's type has a constructor.
- **A type with a constructor:** `malloc`, check the pointer, then construct the
  object in place in that memory. `malloc`, not `calloc`, because the constructor
  is what initialises it. The in-place form needs `<new>`.

  ```cpp
  aprend_uniform_buffer_t *object = (aprend_uniform_buffer_t *)malloc(sizeof(aprend_uniform_buffer_t));
  if (!object)
      return nullptr;
  object = new (object) aprend_uniform_buffer_t();
  ```

  It is destroyed the same two steps in reverse: the destructor called explicitly,
  then `free`.

  ```cpp
  object->~aprend_uniform_buffer_t();
  free(object);
  ```
- **A type with no constructor:** `calloc`, check the pointer, and that is all.
  The zeroed memory is its initial state, and nothing is constructed. It is
  destroyed with `free` alone.

  ```cpp
  aprend_uniform *uniforms = (aprend_uniform *)calloc(count, sizeof(aprend_uniform));
  if (!uniforms)
      return nullptr;
  ```
- "Has a constructor" includes a type that only has members which do: a struct
  holding a `std::vector`, `std::string` or `std::thread` must be constructed in
  place and have its destructor called, or those members are never set up or
  released. That is every handle struct in the newer modules, and every Aprend
  handle struct (they declare constructors). The public C structs - descs,
  `aprend_uniform`, the `Apri*` math types - are plain.
- A function that returns `APRESULT` returns `APRESULT_OUT_OF_MEMORY` for the null
  pointer. Constructing in place doesn't remove the `try`: filling the object in
  can still throw `std::bad_alloc`, and the `catch` then calls the destructor and
  `free`, in that order.
- Every allocation has exactly one matching release, on every failure path as well
  as in the destroy call.
- Aprend already does this, by hand or through the `APREND_MALLOC__T` /
  `APREND_CONSTRUCT__T` / `APREND_DESTRUCT__T` macros in
  `src/render/aprend_internal.hpp`; a few of its plain arrays use `malloc` where
  this says `calloc`. ApArchive, ApAudio, ApSync and ApMath don't follow it yet:
  they use `new (std::nothrow)` and `delete`. Write new code this way; convert an
  old allocation only when you are already changing that function, and convert
  both ends together - memory from an allocating `new` must never reach `free`,
  nor `malloc` memory reach `delete`.
- About half of the existing checks are joined (`if (!instance || !desc)`), mostly
  in Aprend. Write new checks line by line; split an old joined one only when you
  are already changing that function.

## Public headers

- Plain C: `#if __cplusplus` / `extern "C"`, `<stdint.h>` and `<stdbool.h>`, no C++
  types. glm, glaze, libdeflate and the standard library never appear in `include/`.
- Opaque handles are `typedef struct <name>_t *<name>;`. The struct is defined only
  in the module's `src/<module>/*_internal.hpp`, which nothing outside that
  directory includes.
- Lower-case for handles and functions (`apsync_session`), upper-case for value
  structs, enums and constants (`APSYNC_SESSION_DESC`, `APSYNC_CHANNEL_ORDERED`).
- Header comments are the contract: what the call does, its preconditions, its
  failures. They are `/* */` and name parameters in `[brackets]`. Implementation
  notes are `//` comments in the `.cpp`.
- Math types crossing the API are the `Apri*` structs from `aprimath.h`. They are
  laid out identically to their GLM counterparts so `to_glm()` can reinterpret them
  in place; changing a member's order or type breaks that silently.
- In Debug, an Aprend handle struct's first member is its `debug_name` pointer
  (`apricore.h` explains why). Keep it first.

## Aprend

- Aprend calls SpudGPU's backend-agnostic C API only. It never includes a Vulkan,
  D3D12 or Metal header and never branches on the backend at runtime.
- Aprend is handed its `spudgpu_device` in `aprend_instance_desc`. It doesn't
  create the SpudGPU instance, enumerate devices or choose one.
- A choice SpudGPU leaves open is either a rendering matter, made in Aprend
  explicitly, or one Aprend takes from its own caller through a desc. It is never
  pushed back down into SpudLib as a default and never left implicit.
- SpudGPU's plan for hardware without dynamic rendering is a caller-owned
  `spudgpu_framebuffer` (`spudlib/CLAUDE.md`; designed, not built). When it is
  built, Aprend is that owner: `aprend_framebuffer` would hold and cache it, so the
  rest of Aprend makes one begin/end call either way. Nothing in Aprend does this
  yet.
- Picking is not Aprend's job. GPU pick-ID buffers were removed on purpose.
- `aprendbuffers.h` carries its author's note that it is due a rewrite. Extend it
  only as far as the task needs.

## Reference: Hazel

Hazel (2025 renderer; Yan Chernikov / Studio Cherno) is a major reference point
for building this repo, and the engine this repo's competency is compared to:
what Hazel can do is the bar for what ApricotFields should be able to do. It is a
C++ engine whose reference copy is at `../.refs/HazelRenderer2025/Hazel/Hazel`
(source under `src/Hazel/`), read-only and not part of this repo; see
`../CLAUDE.md`, "External references".

**It is a relative reference: Hazel answers "how has this been solved", this file
answers "how is it written here".** Read Hazel for the technique, then write it
under this repo's rules. Where the two disagree, this file wins, every time.
"Hazel does it this way" is never a reason to break a rule here, and nothing is
copied across in Hazel's shape to be converted later.

Where it maps: `Hazel/Renderer` onto Aprend most directly (pipelines and their
specifications, render and compute passes, framebuffers, command buffers, uniform
and storage buffer sets, shaders, materials, the scene renderer).

**SpudGPU, not NVRHI.** Hazel renders through NVRHI; this repo renders through
SpudGPU's C API and nothing else. Read Hazel's `nvrhi::` calls and its
`Hazel/Platform` code for what they achieve, then do it with what SpudGPU
exposes. NVRHI is never added as a dependency, and a capability NVRHI has that
SpudGPU lacks is not a reason to change SpudGPU to resemble it: the comparison
with Hazel is made at this layer only.

What a Hazel idiom becomes here:

| In Hazel | Here |
|---|---|
| `class Pipeline : public RefCounted`, held as `Ref<Pipeline>` | An opaque handle, `typedef struct <name>_t *<name>;`, with a create and a destroy call and one owner. No reference count crosses the API |
| `static Ref<X> Create(const XSpecification &)` | `<prefix>_x_create(...)` taking a desc pointer; how the handle comes back follows the generation of code you're in |
| `XSpecification` holding `std::string`, `Ref<>` members and default member values | A plain C desc: `<stdint.h>` types, handles, `const char *`. `struct_size` first in the newer modules, and no defaults |
| `static` state and getters on `Renderer` | State lives on the instance handle the caller passes in. No globals |
| `Renderer::Submit` lambdas and `RT_` functions | No closure crosses the API. What is safe from which thread is written in the header |
| `HZ_CORE_ASSERT(false, ...)` on a bad input | Argument checks line by line, each returning its own result |
| `Ref<X>::Create`, `new` | `malloc` and in-place construction, or `calloc`, per "Code conventions" |
| `namespace Hazel`, `PascalCase` methods, `m_` members | The module prefix; lower-case handles and functions, upper-case value structs and enums |
| `glm::` and `std::` types in a header | `Apri*` structs and C types; no C++ type in `include/` |
| A renderer that knows its scene, assets and editor | No application-domain concept. The caller hands over what to draw |

The sections that hold these rules are "Writing a new module...", "Code
conventions" and "Public headers" above. A Hazel idiom not in the table is
handled the same way: find the rule it meets and follow the rule.

## Tests

Headless, no GPU, no window: `tests/ap<module>_tests.*`, one executable per module,
registered with `add_test` behind `APRICOTFIELDS_BUILD_TESTS`.

- Tests for a C header are written in C on purpose, to prove the header compiles
  and links as C. Keep them `.c`.
- No framework: a `CHECK(condition)` macro, a `failures` counter, `test_*`
  functions called from `main`, exit code 1 on any failure.
- A new module ships with its tests file; a new call ships with a test in the
  existing one. Brute-force comparison against a slow obvious implementation is the
  preferred check for anything algorithmic (`test_mesh_against_brute_force`).

When asked to verify, on macOS: configure and build with the
`apricotfields-macos-metal` preset, then run `ctest` in `build-macos-metal`.

## Build

- A new source or header file is added to the `add_library` list in
  `CMakeLists.txt` by hand; there is no glob.
- Third-party code is vendored under `../.deps/` (glm, libdeflate, glaze) and
  linked `PRIVATE` unless a public header needs it. libdeflate is pinned on
  purpose: its DEFLATE output is part of what makes an archive byte-identical.
- Don't set `CMAKE_MSVC_RUNTIME_LIBRARY` here; the workspace root sets it once.
- Shaders are compiled with glslc on Windows and Linux only. That block must not
  run on Apple.

## Saying what has and hasn't run

Much of this repo was written on one platform. The TODO files record exactly which
platforms have compiled and run each module. When you write or change code, update
that record truthfully: "written, not compiled" and "builds on macOS, Windows and
Linux never built" are useful; an unqualified "done" is not.
