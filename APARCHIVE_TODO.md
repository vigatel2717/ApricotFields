# ApArchive TODO

`include/archive/aparchive.h` reads and writes ZIP archives one whole entry at a
time: STORED and DEFLATE (or any registered codec), ZIP64, aligned data, caller
extra fields, reader limits. That covers any format built as a manifest, text
entries and binary blobs to map in place.

Below is what a general-purpose ZIP library has that this one doesn't yet, for the
products that will need it (a game engine's asset packs, CGI scene and cache
files, mechanical CAD).

## The rule for adding any of it

Nothing here may change existing files or existing callers:

- **Files:** today's calls keep writing exactly the bytes they write now, and
  today's reader keeps opening every archive it opens now. The writer's output is
  plain ZIP, so a later feature is a different set of standard ZIP records, used
  only by the entries that ask for it.
- **Callers:** a feature is a new function, or a new field at the end of a struct.
  Every struct that can grow starts with `struct_size`, so a field added later
  reads as zero from older callers, and zero must mean "as before".

Each item says how it fits that rule.

## Not built

### 1. Streamed entries

An entry bigger than memory, or one whose size isn't known up front, can't be
written, and an entry can't be read in pieces.

- **Writing:** `aparchive_writer_begin_entry` / `write_entry_data` / `end_entry`.
  The local header goes out with sizes unknown (general-purpose bit 3) and a data
  descriptor follows the data. `APARCHIVE_ENTRY_DESC::zip64` already exists for the
  case where a streamed entry may pass 4 GiB.
- **Reading:** an entry-reader handle (`aparchive_reader_open_entry`, `read`,
  `close`).
- **Codecs:** `APARCHIVE_CODEC` gains stream callbacks (`stream_begin`,
  `stream_process`, `stream_end`) at its end. A codec without them only works
  whole, as now. libdeflate has no streaming, so streamed DEFLATE needs a second
  implementation (zlib-ng, or miniz's `tdefl`/`tinfl`); its output won't match
  libdeflate's byte for byte, which is fine because only new calls use it.
- **Fit:** new calls and new trailing fields. The reader already reads entries
  other tools wrote with data descriptors.

This is the biggest gap for asset packs and CGI caches.

### 2. Raw (pre-compressed) entries

Adding data that's already compressed: `aparchive_writer_add_raw_entry`, taking the
compressed bytes, the uncompressed size and the CRC-32. It allows:

- copying entries between archives without recompressing (repacking, merging),
- compressing entries in parallel on the caller's threads, with the writer only
  writing in order. That wants `aparchive_codecs_compress` / `compress_bound` too.

The read side has it today: `aparchive_reader_get_data_offset` plus
`compressed_size` give an entry's stored bytes.

- **Fit:** new calls.

### 3. Updating an existing archive

Opening an archive, appending or replacing entries, and writing a new central
directory. Needs a sink that can also read and truncate.

- **Fit:** new calls. Depends on item 2.

### 4. Directory entries and symlinks, on write

The writer refuses names ending in `/`: directories exist only as name prefixes.
A general archiver must be able to store an empty directory.

- **Directories:** `APARCHIVE_WRITER_DESC::allow_directory_entries`, added at the
  end, off by default.
- **Symlinks:** wait on item 5.
- **Fit:** a trailing field whose zero is today's behaviour. The reader already
  reports directory entries (`APARCHIVE_ENTRY_INFO::is_directory`).

### 5. File attributes and "version made by"

Both are written as constants: host system 0 (MS-DOS), no attributes. Packaging
tools need Unix permissions (the executable bit) and symlinks. Add them to
`APARCHIVE_ENTRY_DESC` and `APARCHIVE_ENTRY_INFO`.

- **Fit:** trailing fields; zero keeps host 0 and no attributes.

### 6. Separate local and central extra fields

`APARCHIVE_ENTRY_DESC::extra` goes into both headers. ZIP lets them differ, and
some uses need it (central-only information).

- **Fit:** a trailing `central_extra` field; NULL means "the same as `extra`".

### 7. Comments

No archive comment and no per-entry comment can be written, and the reader
doesn't return the ones it finds (it accepts and skips them).

- **Fit:** trailing fields and a new getter.

### 8. Help with standard extra fields

Callers hand-parse the extended timestamp (`0x5455`), the NTFS timestamp and the
Unicode path field. Add a "find extra record by id" helper and encode/decode for
timestamps.

- **Fit:** new calls.

### 9. Strict reading

The reader accepts what ZIP allows. A caller that wants to refuse an archive
comment, directory entries, or bytes no entry accounts for (gaps between entries)
has to check for itself, and can't see the last one at all.

- **Fit:** a trailing `strict` flags field in `APARCHIVE_READER_DESC`; zero is
  today's lenient reading.

### 10. Archives with data in front

A self-extracting archive whose offsets don't account for the stub before the
first entry isn't read. Most readers find the shift from where the central
directory actually sits.

- **Fit:** reader-only; it opens more archives, never fewer.

## Smaller additions

- **Custom memory allocator.** Game engines expect to supply their own; today it's
  always the global allocator.
- **Progress and cancellation callbacks** for long compressions and reads.
- **Verifying an entry's CRC without returning its data,** for callers that
  memory-map stored entries and still want them checked.
- **Archive-level info:** whether ZIP64 is used, how many bytes come before the
  first entry.
- **Case-insensitive collision detection** when extracting to disk on Windows or
  macOS (`A.txt` and `a.txt`), as an option. Unicode normalization (NFC vs NFD)
  only needs a documented note.
- **A decompressor cache.** The DEFLATE codec allocates a libdeflate compressor or
  decompressor per call so it's safe from any thread; a per-thread cache would
  save that for archives with many small entries.

## Testing still owed

- **Fuzz the reader** (libFuzzer over `aparchive_reader_open` and
  `aparchive_reader_read_entry`): it parses untrusted files.
- **Other tools:** CI opens what the tests write with Python's `zipfile` and
  Info-ZIP `unzip -t`, and ApArchive opens archives those tools wrote.
- **Real ZIP64 sizes:** the tests reach the ZIP64 records through an entry count
  over 65,535 and through `APARCHIVE_ENTRY_DESC::zip64`, never through an entry or
  an archive over 4 GiB.

## Out of scope, and the header says so

- **Encryption** (ZipCrypto or WinZip AES).
- **Split and multi-disk archives.**
