# AGENTS.md

## Project overview

crivate is a Windows CLI application for storing and viewing private image
collections. A directory initialized by crivate acts as an encrypted album:

- `.crivate` stores the folder marker and password verifier.
- Each image is decoded to packed RGB in memory, encrypted, and stored as a `.cri`
  file in that directory.
- Viewing decrypts pixels into process memory and renders them directly with
  Direct3D 11. Decoded pixels must never intentionally be exported or persisted as
  PNG, JPEG, BMP, or another image file.

The command set covers album initialization, adding images, inspecting album
contents, viewing selected images, deleting images with confirmation, and changing
the folder password. Use the current source and usage text for the exact supported
commands and arguments; do not infer functionality from a project version number.

The product goal is a small, self-contained Windows executable that makes the
encrypt/view loop practical without writing decrypted image files. It is not a
general encrypted filesystem, image editor, export tool, or hardened enclave.
Image basenames, file counts, sizes, and directory metadata are not concealed.

## Sources of truth

Use these in descending order when behavior is unclear:

1. The current source under `src/` is authoritative for existing behavior.
2. `planning/todo.txt` records known defects and improvements. A plan requirement
   contradicted by an open TODO is not currently satisfied.
3. Versioned plans and idea documents under `planning/` explain intended behavior,
   design constraints, file formats, and the rationale behind existing features.
   Treat older plans as historical context rather than a complete description of
   the latest implementation.
4. Commit history is useful for the rationale behind phased features and fixes.

When completing an item in `planning/todo.txt`, change its marker from `TODO` to
`DONE` in the same commit as the fix. Do not silently remove or rewrite open items.

## Platform and toolchain

- Target: 64-bit Windows 10/11 only.
- Language: C++17, compiled as conforming C++ (`/permissive-`).
- Compiler/toolchain: MSVC with Visual Studio or Build Tools.
- Build system: CMake 3.20+.
- Runtime: one console-subsystem `crivate.exe`, statically linked MSVC runtime.
- Native APIs: Win32, WIC, Windows CNG (`bcrypt`), COM, and Direct3D 11.
- Dependencies: Windows system libraries only; there is no package manager or
  third-party library.

The CMake target defines `UNICODE`, `_UNICODE`, `WIN32_LEAN_AND_MEAN`, and
`NOMINMAX`, enables `/W4 /utf-8`, and requires an x64 build.

## Build and development commands

Run commands from the repository root.

```bat
gen.bat
cmake --build build --config Debug
cmake --build build --config Release
```

`gen.bat` finds the newest installed Visual Studio with the x64 C++ toolset,
configures `build/`, and also tries to generate root `compile_commands.json`
through Visual Studio's Ninja. Use `gen.bat --clean` when a clean configure is
actually needed; it deletes the generated `build/` directory.

Direct configuration is also valid:

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
```

The executable is emitted directly as `build/crivate.exe` for both Debug and
Release. Build artifacts, Visual Studio state, CMake output, and
`compile_commands.json` are generated files and must not be committed.

There is currently no automated test suite. Build the affected configuration and,
for behavior changes, manually exercise the relevant command in disposable
directories outside this repository. `init` requires a completely empty working
directory. Do not put `crivate.exe` in an initialized album; invoke it by absolute
path or through `PATH`.

## CLI model

crivate always operates on the process current working directory; there is no album
path argument.

```text
crivate init
crivate add <image-path> [image-path ...]
crivate count
crivate list
crivate view
crivate view <index>
crivate view <first> <last>
crivate del
crivate del <index>
crivate del <first> <last>
crivate password
```

Important semantics:

- Image indices are 1-based.
- `.cri` basenames are sorted with case-insensitive `CompareStringOrdinal`.
- Adding or deleting files can change all later indices; there is no manifest or
  stable image ID.
- Commands unlock independently. There is no cross-command session.
- `add` unlocks once and processes arguments in order. Directory arguments are
  non-recursive, extension-filtered, and sorted before processing.
- Existing destination `.cri` files are skipped, never overwritten.
- `view` snapshots its selected names once. A/Left and D/Right wrap within that
  set; W/S changes rendering coarseness; Esc closes the viewer.
- Every `del` form unlocks first and requires the exact echoed input `YES`.
  `.crivate` is never deleted by `del`.
- `password` permits the new password to equal the old one and re-salts/re-encrypts
  anyway.
- Unknown commands, invalid arity, and non-numeric indices print usage and return
  `2`. Operational failures return `1`; success returns `0`.
- User-facing data goes to stdout (`count`, `list`). Prompts, warnings, errors, and
  informational mutation messages go to stderr.

Preserve exact prompts and exit-code categories unless a task explicitly changes
the CLI contract.

## Repository structure

### Root

- `CMakeLists.txt`: single executable definition, platform checks, compiler flags,
  and Windows link libraries.
- `gen.bat`: Visual Studio discovery and CMake generation helper.
- `planning/`: historical plans, current intended behavior, and tracked TODOs.
- `LICENSE`: GNU GPL version 3.

### `src/main.cpp`

Owns the application layer:

- `wmain`, argument validation, command dispatch, and exit codes.
- Usage and user-facing diagnostics.
- Console/password/confirmation input, UTF-16-to-UTF-8 password conversion, and
  echo restoration.
- Unlock lifetime and top-level secret/RGB guards.
- Multi-file/directory add orchestration.
- View-set selection and the crypto-owning callback used by the viewer.
- Delete confirmation and selection.
- Two-phase password-change orchestration.

Keep low-level crypto, format, image decoding, and D3D implementation out of this
file when they fit an existing module.

### `src/folder.cpp/.h`

Owns album-level storage:

- Structural validation, creation, and unlock of `.crivate`.
- Password verifier construction and rekey header staging.
- Reading the stored PBKDF2 iteration count.
- Listing and ordinal case-insensitive sorting of cwd `.cri` basenames.
- Restricted deletion of a `.cri` basename.
- Detection of `crivate.exe` in the working directory.

It does not decode images or render them.

### `src/crypto.cpp/.h`

Thin Windows CNG layer:

- Cryptographically secure random bytes.
- PBKDF2-HMAC-SHA256 key derivation.
- AES-256-GCM encryption/decryption.
- Best-effort explicit memory wiping.

Keep CNG handles RAII-managed. Wipe key objects and CNG key-object storage on every
path. Do not add custom cryptographic primitives.

### `src/cri.cpp/.h`

Owns the `.cri` format and encrypted image I/O:

- Dimension and payload limits with checked 64-bit arithmetic.
- Destination basename generation.
- Same-directory encrypted temp writes and no-overwrite final rename.
- Rekey temp creation.
- Header-first, exact-size validation before payload allocation and decryption.

Treat every `.cri` as hostile input. Never allocate from unvalidated width/height
or trust the filename as a path.

### `src/image_load.cpp/.h`

Uses WIC to decode the first image frame to tightly packed top-down RGB8:

- Converts through BGRA.
- Composites alpha onto white.
- Reads EXIF orientation tag 274 and rotates/flips pixels into upright storage.
- Applies the same dimensions and byte limits as `.cri`.
- Wipes intermediate BGRA and final RGB buffers when released.

Directory expansion in `main.cpp` filters `.png`, `.jpg`, `.jpeg`, `.bmp`, `.gif`,
`.tif`, `.tiff`, and `.webp`; directly supplied files are accepted if WIC can
decode them.

### `src/viewer.cpp/.h`

Owns the Win32 window, message/render loop, grid generation, shaders, and D3D11
resources. It must remain crypto-free. Image switching is requested through
`ViewerLoadFn`; `main.cpp` retains the key and decrypts the requested image.

Rendering is deliberately not a textured quad:

- Each grid cell is a solid-color instanced square colored by average source RGB.
- Initial target square count is one per image pixel.
- W/S decreases/increases the target count by 20%, bounded by 1 and pixel count.
- Instance ranges are split into exactly 100 draw calls every frame, including
  zero-instance calls.
- Shared boundaries are stored as absolute edges (`x0/y0/x1/y1`) to avoid cracks.
- Images are letterboxed into a fixed client area; `Present(1, 0)` provides vsync.

The averaged-square presentation and exactly 100 calls are intentional product
behavior. Do not replace them with a texture or optimize away empty draw calls.

## Core data flows

```text
add:
source image -> WIC BGRA -> upright packed RGB8 -> AES-GCM -> temp -> *.cri

view:
*.cri -> strict validation -> AES-GCM -> packed RGB8 in RAM
      -> CPU cell averages -> D3D11 instance buffers -> window

password:
old .cri -> decrypt in RAM -> encrypt with new key -> staged temp files
         -> replace .cri files -> replace .crivate last
```

Only one viewed image's RGB is retained at a time. On a successful A/D switch, wipe
the previous RGB before adopting the new buffer. If loading the next image fails,
keep the current image and grid.

## On-disk formats and cryptographic contract

All integers are little-endian. The formats are version 1 and currently have no
migration framework.

`.crivate` is exactly 92 bytes:

```text
8  magic "CRIVATE1"
4  uint32 version = 1
4  uint32 PBKDF2 iterations (new folders use 600000)
16 random salt
12 AES-GCM verifier nonce
16 AES-GCM verifier tag
32 verifier ciphertext (encrypted 0xA5 canary)
```

Its AAD is the first 32 bytes: magic, version, iterations, and salt.

Each `.cri` is a 44-byte header followed by ciphertext:

```text
4  magic "CRI1"
4  uint32 version = 1
4  uint32 width
4  uint32 height
12 AES-GCM nonce
16 AES-GCM tag
N  ciphertext of width * height * 3 packed RGB bytes
```

The first 16 header bytes are AAD. Current hard limits are width/height 16384,
50,000,000 pixels, and 150,000,000 plaintext bytes. File size must be exactly
`44 + N`; trailing data is invalid.

Crypto invariants:

- Keys are 32 bytes derived with PBKDF2-HMAC-SHA256.
- Encryption is AES-256-GCM with a newly generated 12-byte nonce for every
  encryption.
- The same derived folder key protects the verifier and all images in that album.
- Never reuse a nonce with the same key, weaken iteration validation, remove AAD,
  distinguish useful authentication details to an attacker, or bypass GCM
  verification.
- Preserve compatibility unless a task explicitly designs and implements a format
  migration. Any format change requires coordinated reader, writer, rekey, limits,
  planning, and corruption tests.

## Security and privacy invariants

These take priority over convenience:

1. Never intentionally write decoded pixels to disk in an image format, including
   tests, diagnostics, thumbnails, crash helpers, or temporary exports.
2. Treat `.crivate`, `.cri`, image paths, dimensions, file sizes, and ciphertext as
   untrusted.
3. Validate fixed headers, versions, limits, checked size arithmetic, and exact file
   size before allocating payload/GPU buffers.
4. Wipe passwords, derived keys, verifier plaintext, decrypted RGB, intermediate
   image buffers, plaintext temporaries, and key-object memory on success and every
   error/early-return path. Prefer RAII guards.
5. Stage new files in the destination directory, flush complete contents, close the
   handle, then rename. Never expose a partial final `.cri`.
6. Never overwrite an existing image during `add`.
7. Keep deletion gated by successful unlock, valid selection, and exact `YES`.
8. Keep the viewer isolated from cryptography and filesystem access.
9. Do not add plaintext logging. Error messages may name files but must not contain
   passwords, keys, pixel data, tags, salts, or ciphertext.
10. Do not claim secure erasure of GPU/driver memory or protection against a
    compromised process/OS; wiping is best-effort process hygiene.

## Coding conventions

- Use wide Win32 entry points and APIs (`wmain`, `wchar_t`, `...W`) for paths and
  console input. Passwords are converted to UTF-8 bytes before derivation.
- Use small RAII wrappers for Win32, COM, CNG, and D3D handles. Disable copying for
  ownership-bearing or secret-bearing types.
- Return typed status enums from module boundaries; map them to CLI messages and
  exit codes in `main.cpp`.
- Check every Win32/HRESULT/NTSTATUS call whose failure changes correctness.
- Use fixed-width integers for formats and `uint64_t`/`size_t` with overflow checks
  for image-size math. Do not narrow until a validated bound makes it safe.
- Catch `std::bad_alloc` where allocations depend on image or directory data and
  convert it to an existing failure status.
- Keep functions focused and private helpers in unnamed namespaces.
- Follow the existing formatting: four-space indentation, braces on the same line,
  `k...` constants, PascalCase status/type names, and snake_case functions.
- Preserve `/W4` cleanliness. Do not suppress warnings globally to land a change.
- Avoid new dependencies when Windows or the standard library already provides the
  required primitive.

## Change guidance

For CLI changes, update dispatch/arity checks, usage, exit behavior, prompts, the
relevant plan if the contract changes, and manual tests together.

For image-format changes, update both read and write paths and test malformed magic,
version, zero/huge dimensions, overflow, truncation, trailing bytes, and failed GCM
authentication. Reader validation must remain header-first.

For viewer changes, preserve one window per session, current-set wrapping,
single-image no-op navigation, reset-to-full-resolution on successful image switch,
letterboxing, ignored key auto-repeat, and exactly 100 draw calls.

For password changes, remember there are two keys and potentially many plaintext
images over the command lifetime. Wipe each image before reading the next one and
wipe both keys on every exit.

Do not perform unrelated cleanup in security-sensitive code. Small focused commits
make format and lifetime changes reviewable; the existing history follows phased
feature commits and narrowly described fixes.

## Tracked issues

Consult `planning/todo.txt` for the current issue list and full reproductions; do
not duplicate issue status in other documentation. Do not work around open issues
in documentation or tests. Fix the underlying behavior when tasked, add focused
regression coverage or manual reproduction, and mark the corresponding item
`DONE` in the same commit.

## Verification checklist

At minimum after code changes:

1. Regenerate only if CMake inputs changed, then compile with no new warnings.
2. Run the command/error paths affected by the change in a disposable album.
3. Test wrong password and malformed/out-of-range input; verify no mutation.
4. Check that the album contains only `.crivate`, expected `.cri` files, and no
   newly leaked plaintext image or unexpected temp file after normal completion.
5. For add/rekey/view work, verify secret and RGB cleanup on both success and forced
   failure paths.
6. For viewer work, test W/S, A/D and arrows, wrapping, one-image no-op, Esc/close,
   mixed dimensions/aspects, and no image-file output.
7. For delete work, test rejection of `yes`, empty input, wrong password, and bad
   ranges before testing exact `YES`.
8. Review `git diff` to ensure generated files and unrelated user data are
   untouched.

When a change affects a documented command contract, use the detailed scenarios in
the applicable planning documents in addition to this checklist.
