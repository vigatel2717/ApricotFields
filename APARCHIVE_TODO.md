# ApArchive TODO

What `include/archive/aparchive.h` lacks compared with a good general-purpose ZIP
library. ApArchive is scaffolded: the codec tables, entry-name checks, CRC-32 and
object lifetimes are built. The ZIP writer and reader bodies aren't
(`APRESULT_NOT_IMPLEMENTED`).

Items 1, 2, 4 (directories), 9, 10 and 11 are in the header: "designed" means the
API, its argument and state checks, and its tests exist, but the calls behind it
are part of the unbuilt writer and reader. Streaming DEFLATE also needs a
streaming deflate implementation: libdeflate has none, so the built-in DEFLATE
codec is whole-buffer only (`APRESULT_UNSUPPORTED` when streamed). Symlinks wait
on item 5 (file attributes). Item 8's UTF-8 checks on written names are in too.

The header covers Trellis's needs well. The gaps below are about ApArchive being
reusable by other Apricot products, such as a game engine or mechanical CAD.

## Real gaps: things a good library has

### 1. No streaming entries - designed

`aparchive_writer_add_entry` and `aparchive_reader_read_entry` only work on whole
buffers. You can't write an entry bigger than memory, or one whose size isn't known
up front (compressing a stream as it's produced), and you can't read one in chunks.

- **Fix:** `begin_entry` / `write` / `end_entry` for writing, using a data descriptor
  when the size isn't known, and an entry-reader handle for reading.
- **The codec interface needs it too:** `APARCHIVE_CODEC` only compresses a whole
  buffer in one call, so it needs a streaming version as well.

This is the biggest gap. The game-engine and mechanical-CAD scenarios both needed
it.

### 2. No raw (pre-compressed) entries - designed

There's no way to add data that's already compressed. That blocks:

- copying entries between archives without recompressing, which updating,
  repacking and merging all depend on,
- **compressing entries in parallel:** the writer compresses inside `add_entry`,
  one entry at a time. With a raw add, callers compress on worker threads and the
  writer only writes in order.

The read side already has this through `aparchive_reader_get_data_offset` plus
`compressed_size`, but the header doesn't say so.

### 3. No updating an existing archive

You can't open an archive, append or replace entries, and write a new central
directory. libzip and minizip-ng both do this, and the MCAD case needed it.

### 4. Its name rules are Trellis policy - directories designed

That breaks the "no hidden choices" rule. The writer refuses:

- **directory entries:** a general archiver must be able to store empty
  directories,
- **symlinks:** there's no way to set file attributes at all.

The "no `..`, no absolute paths" checks are real safety rules and should stay.
Refusing directories should be a caller option.

### 5. No file attributes or "version made by"

Both are written as constants. Packaging tools need Unix permissions (the
executable bit), symlinks and the creating OS. Make them fields in
`APARCHIVE_ENTRY_DESC`, and report them in `APARCHIVE_ENTRY_INFO`.

### 6. The local and central extra fields can't differ

ZIP allows them to, and some uses need it: local-only padding, or central-only
information. The caller's extra data (`APARCHIVE_ENTRY_DESC::extra`) currently goes
into both.

### 7. No comments

There's no archive comment (`aparchive_writer_finish` takes none) and no per-entry
comment, for writing or reading. They're rare, but every complete library supports
them.

### 8. Little help with metadata

There are no helpers for the standard extra fields, so every caller has to
hand-parse:

- the ZIP64 field,
- the extended timestamp (`0x5455`, UTC with 1-second precision; DOS time is local
  time at 2-second precision),
- the NTFS timestamp,
- the Unicode path field.

At least add a "find extra record by ID" helper and encode/decode for timestamps.

The writer also doesn't check that names are valid UTF-8. And a non-ASCII name
written without `utf8_name_flag` will be read as CP437, the old DOS code page,
which is silently wrong. That case should be rejected.

### 9. Error details only come from `open` - designed

When `aparchive_reader_read_entry` or `aparchive_reader_get_data_offset` fails on a
corrupt file, all you get is a code. Add an optional message buffer to each call; a
per-call buffer keeps the reader thread-safe.

### 10. Zip-bomb protection depends on the caller - designed

`aparchive_reader_read_entry` makes the caller allocate `uncompressed_size` bytes
before calling it, and that number comes from the archive itself. Add reader limits
(in `APARCHIVE_READER_DESC`) on total and per-entry uncompressed size, and on
compression ratio, so a 4 GB claim fails at open instead of at allocation.

### 11. No way to evolve the structs without breaking compatibility - done

The desc structs have no size or version field. Adding a field later breaks every
separately built binary, and ApricotFields is meant to be reused. Add a leading
`struct_size` field now (the same issue is listed in trellislib's
`docs/bim/open_questions.md`).

## Smaller additions

- **Custom memory allocator.** Game engines expect to supply their own; today it's
  always the global allocator.
- **Progress and cancellation callbacks** for long compressions and reads.
- **Verifying an entry's CRC without returning its data,** for callers that
  memory-map stored entries and still want them checked.
- **Archive-level info:** the comment, whether ZIP64 is used, and how many bytes come
  before the first entry (for self-extracting archives).
- **Case-insensitive collision detection** when extracting to disk on Windows or
  macOS (`A.txt` and `a.txt`), as an option. Unicode normalization (NFC vs NFD)
  only needs a documented note.
- **Entries written with data descriptors by other tools** (general-purpose bit 3).
  The reader will meet them, so the header should say how it handles them.
- **Codec thread-safety:** the header doesn't say whether adding to a codec table
  while readers use it is allowed. Say it isn't.

## Fine to leave out, but say so in the header

- **Encryption** (ZipCrypto or WinZip AES). It's common in general archivers but out
  of scope here.
- **Split and multi-disk archives.**
- **Compression methods other than stored and deflate,** which the codec table
  already covers.

## Order

1. **Before anything ships:** 11 (struct versioning).
2. **Before the writer and reader get built:** 1, 2 and 9. They change the shape of
   the API.
3. **Field additions, any time:** 4, 5, 6 and 8.
4. **When a product needs them:** 3 and the smaller additions.
