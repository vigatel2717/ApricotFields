#ifndef APARCHIVE_H
#define APARCHIVE_H

#include "../apricore.h"
#include <stdbool.h>
#include <stdint.h>

#if __cplusplus
extern "C" {
#endif

/*
 * ApArchive - ZIP archives (with ZIP64), read and written through caller
 * callbacks.
 *
 * General purpose: it knows nothing about what an archive holds. Every
 * layout decision - entry order, compression method and level, data
 * alignment, timestamps, extra fields - is the caller's, and the writer adds
 * nothing it wasn't asked for, so the same input always gives the same
 * bytes.
 *
 * It does no I/O of its own. A reader reads through an APARCHIVE_SOURCE
 * (random access); a writer writes through an APARCHIVE_SINK (append only,
 * never seeks). It reports where each entry's data sits in the archive, so a
 * caller can map or range-read an entry's bytes itself.
 *
 * Entries are written and read whole: one buffer in, one buffer out.
 *
 * Opening reads only the end of the archive and its central directory; each
 * entry is read only when asked for. A reader's calls are safe from any
 * number of threads at once, provided the source's read callback is.
 * Writers aren't thread-safe.
 *
 * What it writes is plain ZIP: any ZIP tool reads it. What it reads is
 * plain ZIP from any tool, including what its own writer never produces -
 * an archive comment, directory entries, entries written with a data
 * descriptor, extra field records it doesn't know.
 *
 * Not built yet (APARCHIVE_TODO.md): entries streamed in pieces, entries
 * added already compressed, writing directory entries, updating an archive
 * in place. Each is a new call or a new field at the end of a struct. None
 * changes the bytes today's calls write, or which archives today's reader
 * opens.
 *
 * Out of scope: encryption (ZipCrypto, WinZip AES) and split or multi-disk
 * archives - reading one fails with APRESULT_UNSUPPORTED. Archives with
 * data before the first entry whose offsets don't account for it (some
 * self-extracting archives) aren't read.
 */

/* ---- Conventions ----------------------------------------------------- */

/*
 * Results. Calls return APRESULT (apricore.h): APRESULT_OK, a general code,
 * or an APRESULT_APARCHIVE_* one. The general codes mean, here:
 *   APRESULT_INVALID_ARGUMENT  also an unsafe entry name, or a call in the
 *                              wrong state (a writer finished or failed)
 *   APRESULT_IO                the source or sink callback returned false
 *   APRESULT_LIMIT             over an APARCHIVE_READER_DESC limit
 *   APRESULT_UNSUPPORTED       encryption, multiple disks, patch data, or
 *                              compressing with a decode-only codec
 *   APRESULT_CORRUPT_DATA      an entry that fails its CRC-32 or doesn't
 *                              decompress to its declared size
 *
 * Error messages. Every call that can fail for a reason worth a sentence
 * takes an APARCHIVE_ERROR pointer (NULL to skip it): on failure it holds a
 * description naming the entry and what was wrong; on success its message
 * is empty. Being per call, it keeps readers thread-safe.
 *
 * Struct versions. Every struct that may grow starts with [struct_size],
 * which the caller sets to sizeof(the struct) as it was compiled. Fields
 * past it are treated as zero on input and not written on output, so a
 * binary built against an older aparchive.h keeps working with a newer
 * ApricotFields. Fields are only ever added at the end. A [struct_size]
 * smaller than the first version of the struct fails with
 * APRESULT_INVALID_ARGUMENT. APARCHIVE_SOURCE, APARCHIVE_SINK and
 * APARCHIVE_ERROR are frozen and have none.
 */

#define APARCHIVE_ERROR_MESSAGE_SIZE 256

typedef struct APARCHIVE_ERROR {
	char message[APARCHIVE_ERROR_MESSAGE_SIZE]; /* NUL-terminated, truncated to fit */
} APARCHIVE_ERROR;

/* ---- I/O ------------------------------------------------------------- */

/* Random-access input. [size] is the whole archive's size in bytes. [read]
 * copies exactly [count] bytes at [offset] into [buffer] and returns false
 * on any failure, including a short read. Offsets come in any order, and
 * from several threads at once if the reader is used that way. */
typedef struct APARCHIVE_SOURCE {
	void *user;
	uint64_t size;
	bool (*read)(void *user, uint64_t offset, void *buffer, uint64_t count);
} APARCHIVE_SOURCE;

/* Append-only output. [write] appends [count] bytes and returns false on
 * failure, which fails the call that was writing. */
typedef struct APARCHIVE_SINK {
	void *user;
	bool (*write)(void *user, const void *data, uint64_t count);
} APARCHIVE_SINK;

/* ---- Codecs ---------------------------------------------------------- */

/*
 * A codec table says which compression methods a reader can decode and a
 * writer can encode. Each reader or writer is given one; there's no global
 * table, so what one product registers never changes what another product's
 * archives accept. A table must not change while a reader or writer uses
 * it: add every codec first.
 *
 * STORED (no compression) is part of ZIP itself and always available. A new
 * table has nothing else - add DEFLATE with aparchive_codecs_add_deflate(),
 * and any other method with aparchive_codecs_add().
 */

#define APARCHIVE_METHOD_STORED 0
#define APARCHIVE_METHOD_DEFLATE 8

/* A compression method, by its ZIP method id. Methods other than STORED and
 * DEFLATE (zstd is 93, LZMA 14, ...) aren't read by most ZIP tools: an
 * archive using them only opens in software that registers them too.
 *
 * Callbacks may be called from several threads at once.
 *
 *   compress_bound: the most bytes compressing [in_size] bytes can produce.
 *   compress:       compresses [in] into [out] (at least compress_bound
 *                   bytes) at [level], whose meaning is the codec's; sets
 *                   [out_size]; false on failure.
 *   decompress:     decompresses [in] into exactly [out_size] bytes; false
 *                   if the data is malformed or decompresses to any other
 *                   size.
 *   A decode-only codec leaves compress and compress_bound NULL. */
typedef struct APARCHIVE_CODEC {
	uint32_t struct_size;
	uint16_t method;
	void *user;
	uint64_t (*compress_bound)(void *user, uint64_t in_size);
	bool (*compress)(void *user, int32_t level, const void *in, uint64_t in_size, void *out, uint64_t out_capacity,
	                 uint64_t *out_size);
	bool (*decompress)(void *user, const void *in, uint64_t in_size, void *out, uint64_t out_size);
} APARCHIVE_CODEC;

typedef struct aparchive_codecs_t *aparchive_codecs;

/* An empty table: STORED only. */
APRESULT aparchive_codecs_create(aparchive_codecs *out_codecs);
/* Copies [codec] into the table. APRESULT_INVALID_ARGUMENT for method
 * STORED, a method already in the table, no decompress, or only one of
 * compress and compress_bound. */
APRESULT aparchive_codecs_add(aparchive_codecs codecs, const APARCHIVE_CODEC *codec);
/* Adds DEFLATE, built on libdeflate. Levels 0-12: 0 stores without
 * compressing (still as DEFLATE), 1 is fastest, 12 smallest. Its output
 * for a given input and level is fixed by the libdeflate version
 * ApricotFields builds with - pinned, so archives stay byte-identical. */
APRESULT aparchive_codecs_add_deflate(aparchive_codecs codecs);
/* True if [method] is STORED or in the table. */
bool aparchive_codecs_has(aparchive_codecs codecs, uint16_t method);

/* Destroy every reader and writer using the table first. */
void aparchive_codecs_destroy(aparchive_codecs codecs);

/* ---- Writing --------------------------------------------------------- */

typedef struct APARCHIVE_WRITER_DESC {
	uint32_t struct_size;
	APARCHIVE_SINK sink;
	/* Must outlive the writer. */
	aparchive_codecs codecs;
} APARCHIVE_WRITER_DESC;

typedef struct aparchive_writer_t *aparchive_writer;

APRESULT aparchive_writer_create(const APARCHIVE_WRITER_DESC *desc, aparchive_writer *out_writer);

/* One entry. Every header field ApArchive writes comes from here or is a
 * fixed value - nothing is taken from the clock, the file system or the
 * platform. */
typedef struct APARCHIVE_ENTRY_DESC {
	uint32_t struct_size;
	/* Valid UTF-8, '/'-separated, passing aparchive_is_safe_name(), and not
	 * a name already in the archive. A name with any non-ASCII byte needs
	 * [utf8_name_flag]: without it, readers take the name as CP437. A name
	 * ending in '/' would be a directory entry, which the writer doesn't
	 * write yet: directories exist only as name prefixes. */
	const char *name;
	/* STORED, or a method in the writer's codec table. */
	uint16_t method;
	/* The codec's level; ignored for STORED. */
	int32_t level;
	/* Where the entry's data starts in the archive: a power of two, 0 or 1
	 * for anywhere. Reached by padding the local header with one extra
	 * field record of zeros (id 0xD935, the id Android's zipalign uses),
	 * so alignment adds at least 4 bytes whenever padding is needed. */
	uint32_t alignment;
	/* MS-DOS date and time, written as given (date 0x0021 / time 0 is
	 * 1980-01-01 00:00:00, the earliest representable). */
	uint16_t dos_date;
	uint16_t dos_time;
	/* Sets general-purpose flag bit 11: the name is UTF-8. */
	bool utf8_name_flag;
	/* Caller extra field records (id, size, data - already encoded),
	 * written into both the local and the central header. NULL/0 for
	 * none. Must be whole records, none with id 0x0001 (ZIP64, which
	 * ApArchive writes itself). With no extra, no alignment padding and
	 * sizes under 4 GiB, a local header has no extra field at all. */
	const void *extra;
	uint16_t extra_size;
	/* Write this entry's sizes in ZIP64 fields even though they'd fit
	 * without. Off, an entry gets ZIP64 fields only when its sizes or
	 * offset need them. */
	bool zip64;
} APARCHIVE_ENTRY_DESC;

/* Compresses [data] (whole) with the entry's method and appends the entry.
 * Sizes and CRC-32 are known before the header is written, so no data
 * descriptor is used. */
APRESULT aparchive_writer_add_entry(aparchive_writer writer, const APARCHIVE_ENTRY_DESC *desc, const void *data,
                                    uint64_t size, APARCHIVE_ERROR *out_error);

/* Bytes written to the sink so far - where the next entry's local header
 * will start. */
uint64_t aparchive_writer_get_offset(aparchive_writer writer);

/* Writes the central directory and end records (ZIP64 ones only when the
 * entry count or offsets need them), with no archive comment. Nothing can
 * be added afterwards. */
APRESULT aparchive_writer_finish(aparchive_writer writer, APARCHIVE_ERROR *out_error);

/* A sink failure, or running out of memory, while an entry or the central
 * directory was being written leaves the writer unusable - what follows
 * would sit after a partial record - and every later call fails with
 * APRESULT_INVALID_ARGUMENT. An entry refused for its arguments writes
 * nothing and leaves the writer usable. Without a finish, what was written
 * isn't a valid archive. */
void aparchive_writer_destroy(aparchive_writer writer);

/* ---- Reading --------------------------------------------------------- */

typedef struct APARCHIVE_READER_DESC {
	uint32_t struct_size;
	APARCHIVE_SOURCE source;
	/* Must outlive the reader. */
	aparchive_codecs codecs;
	/* Limits, checked on open against what the central directory
	 * declares - so an archive claiming more fails before the caller
	 * allocates anything. A declared size is exact: reading an entry
	 * fails unless it decompresses to precisely that many bytes. Each
	 * limit fails with APRESULT_LIMIT; UINT32_MAX / UINT64_MAX for none.
	 *   max_entry_count, max_central_directory_size: the directory.
	 *   max_entry_uncompressed_size: any one entry.
	 *   max_total_uncompressed_size: every entry together.
	 *   max_compression_ratio: an entry's uncompressed size over its
	 *     compressed size, rounded up (a zip bomb compresses far beyond
	 *     what real data does; DEFLATE peaks near 1032:1). An empty
	 *     entry never exceeds it. */
	uint32_t max_entry_count;
	uint64_t max_central_directory_size;
	uint64_t max_entry_uncompressed_size;
	uint64_t max_total_uncompressed_size;
	uint32_t max_compression_ratio;
} APARCHIVE_READER_DESC;

typedef struct aparchive_reader_t *aparchive_reader;

/* Reads the end records and the central directory - nothing else. Some
 * checks are always made, because they keep any caller safe: an archive
 * with an unsafe name (aparchive_is_safe_name()), a duplicate name,
 * overlapping entries, or sizes and offsets that don't fit the file fails
 * with APRESULT_APARCHIVE_MALFORMED. APRESULT_APARCHIVE_NOT_AN_ARCHIVE if
 * there's no end record. */
APRESULT aparchive_reader_open(const APARCHIVE_READER_DESC *desc, aparchive_reader *out_reader,
                               APARCHIVE_ERROR *out_error);

uint32_t aparchive_reader_get_entry_count(aparchive_reader reader);

/* What the central directory says about an entry. [name] and [extra] point
 * into the reader and stay valid until it's destroyed. */
typedef struct APARCHIVE_ENTRY_INFO {
	uint32_t struct_size;
	const char *name; /* NUL-terminated, as stored: UTF-8 if flags bit 11, else CP437 */
	uint16_t name_size;
	uint16_t method;
	uint16_t flags; /* general-purpose bit flags; bit 3: a data descriptor follows the data */
	uint16_t dos_date;
	uint16_t dos_time;
	uint32_t crc32;
	uint64_t compressed_size;
	uint64_t uncompressed_size;
	uint64_t local_header_offset;
	const void *extra; /* the central header's extra field */
	uint16_t extra_size;
	bool is_directory; /* the name ends in '/' */
} APARCHIVE_ENTRY_INFO;

/* Fills [out_info] up to its struct_size, which the caller sets. Entries
 * are in central directory order. */
APRESULT aparchive_reader_get_entry(aparchive_reader reader, uint32_t index, APARCHIVE_ENTRY_INFO *out_info);

/* The index of the entry named [name] (exact, byte for byte - no case
 * folding or Unicode normalization); APRESULT_NOT_FOUND if there's none. */
APRESULT aparchive_reader_find(aparchive_reader reader, const char *name, uint32_t *out_index);

/* Where entry [index]'s data starts in the archive: past its local header,
 * which this reads and checks against the central directory
 * (APRESULT_APARCHIVE_MALFORMED if they disagree). The entry's bytes as
 * stored - uncompressed for STORED, compressed otherwise - are
 * [offset, offset + compressed_size) of the source: for the caller to map
 * or range-read, or to decompress itself (on the GPU, say). Nothing read
 * this way is checked against the CRC-32. */
APRESULT aparchive_reader_get_data_offset(aparchive_reader reader, uint32_t index, uint64_t *out_offset,
                                          APARCHIVE_ERROR *out_error);

/* Reads entry [index] whole into [buffer], decompressing it, and checks its
 * CRC-32. [buffer_size] must be its uncompressed_size exactly, so nothing
 * can decompress past what the central directory declared.
 * APRESULT_APARCHIVE_UNKNOWN_METHOD if its method isn't in the codec
 * table. */
APRESULT aparchive_reader_read_entry(aparchive_reader reader, uint32_t index, void *buffer, uint64_t buffer_size,
                                     APARCHIVE_ERROR *out_error);

void aparchive_reader_destroy(aparchive_reader reader);

/* ---- Utilities ------------------------------------------------------- */

/* True if [name] is a safe entry name: not empty, no NUL, no leading '/',
 * no '\\', no drive letter, and no empty, "." or ".." segments - except
 * that it may end in one '/', which makes it a directory entry's name.
 * Encoding-blind, so it judges names from any archive, UTF-8 or CP437; the
 * writer also requires UTF-8, and names that are unique and not a
 * directory's. */
bool aparchive_is_safe_name(const char *name, uint64_t name_size);

/* Updates a running CRC-32 (ZIP's polynomial) with [size] bytes; start
 * from 0. */
uint32_t aparchive_crc32(uint32_t crc, const void *data, uint64_t size);

#if __cplusplus
}
#endif

#endif // APARCHIVE_H
