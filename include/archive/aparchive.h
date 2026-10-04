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
 * alignment, timestamps, extra fields, whether directory entries are
 * allowed - is the caller's, and the writer adds nothing it wasn't asked
 * for, so the same input always gives the same bytes.
 *
 * It does no I/O of its own. A reader reads through an APARCHIVE_SOURCE
 * (random access); a writer writes through an APARCHIVE_SINK (append only,
 * never seeks). It reports where each entry's data sits in the archive, so a
 * caller can map or range-read an entry's bytes itself.
 *
 * Opening reads only the end of the archive and its central directory; each
 * entry is read only when asked for. A reader's calls are safe from any
 * number of threads at once, provided the source's read callback is; each
 * entry reader is used from one thread at a time. Writers aren't
 * thread-safe.
 *
 * Out of scope: encryption (ZipCrypto, WinZip AES) and split or multi-disk
 * archives - reading one fails with APRESULT_UNSUPPORTED.
 */

/* ---- Conventions ----------------------------------------------------- */

/*
 * Results. Calls return APRESULT (apricore.h): APRESULT_OK, a general code,
 * or an APRESULT_APARCHIVE_* one. The general codes mean, here:
 *   APRESULT_INVALID_ARGUMENT  also an unsafe entry name, or a call in the
 *                              wrong state (an entry open, a writer
 *                              finished)
 *   APRESULT_IO                the source or sink callback returned false
 *   APRESULT_LIMIT             over an APARCHIVE_READER_DESC limit
 *   APRESULT_UNSUPPORTED       encryption, multiple disks, patch data, or
 *                              streaming with a codec that can't stream
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

/* A codec's streaming state, created by stream_begin. */
typedef void *APARCHIVE_CODEC_STREAM;

/* A compression method, by its ZIP method id. Methods other than STORED and
 * DEFLATE (zstd is 93, LZMA 14, ...) aren't read by most ZIP tools: an
 * archive using them only opens in software that registers them too.
 *
 * Callbacks may be called from several threads at once (each stream from
 * one thread at a time).
 *
 * Whole buffers:
 *   compress_bound: the most bytes compressing [in_size] bytes can produce.
 *   compress:       compresses [in] into [out] (at least compress_bound
 *                   bytes) at [level], whose meaning is the codec's; sets
 *                   [out_size]; false on failure.
 *   decompress:     decompresses [in] into exactly [out_size] bytes; false
 *                   if the data is malformed or decompresses to any other
 *                   size.
 *   A decode-only codec leaves compress and compress_bound NULL.
 *
 * Streams (all NULL if the codec can't stream; then entries using it can
 * only be added and read whole):
 *   stream_begin:   starts compressing at [level] ([compress] true) or
 *                   decompressing; NULL on failure.
 *   stream_process: consumes up to [in_size] bytes of [in] and produces up
 *                   to [out_capacity] bytes into [out], reporting how many
 *                   of each in [out_consumed] / [out_produced]. [finish]
 *                   says no more input will follow; [out_done] is set once
 *                   the stream has produced everything. False if the data
 *                   is malformed or the codec fails.
 *   stream_end:     frees the state, finished or not. */
typedef struct APARCHIVE_CODEC {
	uint32_t struct_size;
	uint16_t method;
	void *user;
	uint64_t (*compress_bound)(void *user, uint64_t in_size);
	bool (*compress)(void *user, int32_t level, const void *in, uint64_t in_size, void *out, uint64_t out_capacity,
	                 uint64_t *out_size);
	bool (*decompress)(void *user, const void *in, uint64_t in_size, void *out, uint64_t out_size);
	APARCHIVE_CODEC_STREAM (*stream_begin)(void *user, bool compress, int32_t level);
	bool (*stream_process)(void *user, APARCHIVE_CODEC_STREAM stream, const void *in, uint64_t in_size,
	                       uint64_t *out_consumed, void *out, uint64_t out_capacity, uint64_t *out_produced,
	                       bool finish, bool *out_done);
	void (*stream_end)(void *user, APARCHIVE_CODEC_STREAM stream);
} APARCHIVE_CODEC;

typedef struct aparchive_codecs_t *aparchive_codecs;

/* An empty table: STORED only. */
APRESULT aparchive_codecs_create(aparchive_codecs *out_codecs);
/* Copies [codec] into the table. APRESULT_INVALID_ARGUMENT for method
 * STORED, a method already in the table, no decompress, or a partial set of
 * compress or stream callbacks. */
APRESULT aparchive_codecs_add(aparchive_codecs codecs, const APARCHIVE_CODEC *codec);
/* Adds DEFLATE, built on libdeflate. Levels 0-12: 0 stores without
 * compressing (still as DEFLATE), 1 is fastest, 12 smallest. Its output
 * for a given input and level is fixed by the libdeflate version
 * ApricotFields builds with - pinned, so archives stay byte-identical.
 * Whole buffers only: libdeflate has no streaming, so DEFLATE entries
 * can't be streamed yet. */
APRESULT aparchive_codecs_add_deflate(aparchive_codecs codecs);
/* True if [method] is STORED or in the table. */
bool aparchive_codecs_has(aparchive_codecs codecs, uint16_t method);

/* Compresses with a method in the table, exactly as the writer would - so
 * callers can compress entries in parallel on their own threads and add
 * them with aparchive_writer_add_raw_entry(). [out] must hold
 * aparchive_codecs_compress_bound() bytes. STORED copies. */
uint64_t aparchive_codecs_compress_bound(aparchive_codecs codecs, uint16_t method, uint64_t in_size);
APRESULT aparchive_codecs_compress(aparchive_codecs codecs, uint16_t method, int32_t level, const void *in,
                                   uint64_t in_size, void *out, uint64_t out_capacity, uint64_t *out_size,
                                   APARCHIVE_ERROR *out_error);

/* Destroy every reader and writer using the table first. */
void aparchive_codecs_destroy(aparchive_codecs codecs);

/* ---- Writing --------------------------------------------------------- */

typedef struct APARCHIVE_WRITER_DESC {
	uint32_t struct_size;
	APARCHIVE_SINK sink;
	/* Must outlive the writer. */
	aparchive_codecs codecs;
	/* Accept directory entries: names ending in '/', STORED, no data.
	 * Off, they're refused, and directories exist only as name
	 * prefixes. */
	bool allow_directory_entries;
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
	 * [utf8_name_flag]: without it, readers take the name as CP437. A
	 * trailing '/' makes it a directory entry, which needs
	 * APARCHIVE_WRITER_DESC::allow_directory_entries. */
	const char *name;
	/* STORED, or a method in the writer's codec table (any method for a
	 * raw entry). */
	uint16_t method;
	/* The codec's level; ignored for STORED and raw entries. */
	int32_t level;
	/* Where the entry's data starts in the archive: a power of two, 0 or 1
	 * for anywhere. Reached by padding the local header with one extra
	 * field record (id 0xD935, the one Android's zipalign uses), so
	 * alignment adds at least 4 bytes whenever padding is needed. */
	uint32_t alignment;
	/* MS-DOS date and time, written as given (date 0x0021 / time 0 is
	 * 1980-01-01 00:00:00, the earliest representable). */
	uint16_t dos_date;
	uint16_t dos_time;
	/* Sets general-purpose flag bit 11: the name is UTF-8. */
	bool utf8_name_flag;
	/* Caller extra field records (id, size, data - already encoded),
	 * written into both the local and the central header. NULL/0 for
	 * none. With no extra, no alignment padding and sizes under 4 GiB, a
	 * local header has no extra field at all. */
	const void *extra;
	uint16_t extra_size;
	/* Write ZIP64 size fields for this entry even if its sizes turn out
	 * small. Only matters for streamed entries, whose sizes aren't known
	 * when the local header is written: one that reaches 4 GiB without it
	 * fails at aparchive_writer_end_entry() with APRESULT_LIMIT. */
	bool zip64;
} APARCHIVE_ENTRY_DESC;

/* Compresses [data] (whole) with the entry's method and appends the entry.
 * Sizes and CRC-32 are known before the header is written, so no data
 * descriptor is used. ZIP64 fields are written for an entry only when its
 * sizes or offset need them (or [zip64] asks). */
APRESULT aparchive_writer_add_entry(aparchive_writer writer, const APARCHIVE_ENTRY_DESC *desc, const void *data,
                                    uint64_t size, APARCHIVE_ERROR *out_error);

/* Appends an entry whose data is already compressed with [desc->method] -
 * copied from another archive (see aparchive_reader_get_data_offset()), or
 * compressed by the caller (aparchive_codecs_compress()). The method needn't
 * be in the codec table. [uncompressed_size] and [crc32] describe the
 * uncompressed data; they can't be checked without decompressing, so
 * they're written as given and a wrong one only shows when the entry is
 * read. For STORED, [compressed_size] must equal [uncompressed_size]. */
APRESULT aparchive_writer_add_raw_entry(aparchive_writer writer, const APARCHIVE_ENTRY_DESC *desc,
                                        const void *compressed, uint64_t compressed_size, uint64_t uncompressed_size,
                                        uint32_t crc32, APARCHIVE_ERROR *out_error);

/* Streams an entry of any size: begin, write its uncompressed data in
 * pieces, end. The local header goes out first with sizes unknown
 * (general-purpose bit 3), and a data descriptor follows the data. The
 * method must be STORED or a codec that can stream. Only one entry is open
 * at a time; nothing else may be added until it's ended. */
APRESULT aparchive_writer_begin_entry(aparchive_writer writer, const APARCHIVE_ENTRY_DESC *desc,
                                      APARCHIVE_ERROR *out_error);
APRESULT aparchive_writer_write_entry_data(aparchive_writer writer, const void *data, uint64_t size,
                                           APARCHIVE_ERROR *out_error);
APRESULT aparchive_writer_end_entry(aparchive_writer writer, APARCHIVE_ERROR *out_error);

/* Bytes written to the sink so far - where the next entry's local header
 * will start. */
uint64_t aparchive_writer_get_offset(aparchive_writer writer);

/* Writes the central directory and end records (ZIP64 ones only when the
 * entry count or offsets need them), with no archive comment. Nothing can
 * be added afterwards. */
APRESULT aparchive_writer_finish(aparchive_writer writer, APARCHIVE_ERROR *out_error);

/* Any failure while an entry or the central directory was being written
 * leaves the writer unusable - what follows would sit after a partial
 * record - and every later call fails with APRESULT_INVALID_ARGUMENT.
 * Without a finish, what was written isn't a valid archive. */
void aparchive_writer_destroy(aparchive_writer writer);

/* ---- Reading --------------------------------------------------------- */

/* Checks a lenient reader skips. Some checks are always made, because they
 * keep any caller safe: unsafe or duplicate names, overlapping entries,
 * local headers that disagree with the central directory, declared sizes
 * and CRC-32, and the limits below. */
typedef uint32_t APARCHIVE_STRICT_FLAGS;
enum {
	APARCHIVE_STRICT_NONE = 0,
	/* The first entry starts at offset 0 (no prepended data, as in a
	 * self-extracting archive). */
	APARCHIVE_STRICT_NO_PREFIX = 0x01,
	/* No archive comment. */
	APARCHIVE_STRICT_NO_COMMENT = 0x02,
	/* No directory entries (names ending in '/'). */
	APARCHIVE_STRICT_NO_DIRECTORIES = 0x04,
	/* Entries follow each other with no unaccounted bytes between them or
	 * before the central directory. Checked as far as the central
	 * directory allows on open, exactly as each entry is read. */
	APARCHIVE_STRICT_NO_GAPS = 0x08,
	APARCHIVE_STRICT_ALL = 0x0F,
};

typedef struct APARCHIVE_READER_DESC {
	uint32_t struct_size;
	APARCHIVE_SOURCE source;
	/* Must outlive the reader. */
	aparchive_codecs codecs;
	APARCHIVE_STRICT_FLAGS strict;
	/* Limits, checked on open against what the central directory
	 * declares - so an archive claiming more fails before the caller
	 * allocates anything - and again as data is read, since a declared
	 * size is only trusted once decompression matches it exactly. Each
	 * fails with APRESULT_LIMIT; UINT32_MAX / UINT64_MAX for none.
	 *   max_entry_count, max_central_directory_size: the directory.
	 *   max_entry_uncompressed_size: any one entry.
	 *   max_total_uncompressed_size: every entry together.
	 *   max_compression_ratio: an entry's uncompressed size over its
	 *     compressed size, rounded up (a zip bomb compresses far beyond
	 *     what real data does; DEFLATE peaks near 1032:1). An empty
	 *     entry never exceeds it; a non-empty one with no compressed
	 *     bytes always does. */
	uint32_t max_entry_count;
	uint64_t max_central_directory_size;
	uint64_t max_entry_uncompressed_size;
	uint64_t max_total_uncompressed_size;
	uint32_t max_compression_ratio;
	/* Builds a hash index of the names on open, so aparchive_reader_find()
	 * is O(1) instead of a linear search - worth it for archives with many
	 * entries that are looked up by name. */
	bool build_name_index;
} APARCHIVE_READER_DESC;

typedef struct aparchive_reader_t *aparchive_reader;

/* Reads the end records and the central directory - nothing else. */
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

/* Fills [out_info] up to its struct_size, which the caller sets. */
APRESULT aparchive_reader_get_entry(aparchive_reader reader, uint32_t index, APARCHIVE_ENTRY_INFO *out_info);

/* The index of the entry named [name] (exact, byte for byte - no case
 * folding or Unicode normalization); APRESULT_NOT_FOUND if there's none. */
APRESULT aparchive_reader_find(aparchive_reader reader, const char *name, uint32_t *out_index);

/* Where entry [index]'s data starts in the archive: past its local header,
 * which this reads and checks against the central directory. The entry's
 * bytes as stored - uncompressed for STORED, compressed otherwise - are
 * [offset, offset + compressed_size) of the source: for the caller to map
 * or range-read, to decompress itself (on the GPU, say), or to copy into
 * another archive with aparchive_writer_add_raw_entry(). Nothing read this
 * way is checked against the CRC-32. */
APRESULT aparchive_reader_get_data_offset(aparchive_reader reader, uint32_t index, uint64_t *out_offset,
                                          APARCHIVE_ERROR *out_error);

/* Reads entry [index] whole into [buffer], decompressing it, and checks its
 * CRC-32. [buffer_size] must be its uncompressed_size exactly, so nothing
 * can decompress past what the central directory declared. */
APRESULT aparchive_reader_read_entry(aparchive_reader reader, uint32_t index, void *buffer, uint64_t buffer_size,
                                     APARCHIVE_ERROR *out_error);

/* Reads an entry in pieces, for entries too big to hold at once. Each
 * entry reader is independent (several may be open on one reader, on
 * different threads), and must be closed before its reader is destroyed.
 * The method must be STORED or a codec that can stream. */
typedef struct aparchive_entry_reader_t *aparchive_entry_reader;

APRESULT aparchive_reader_open_entry(aparchive_reader reader, uint32_t index, aparchive_entry_reader *out_entry_reader,
                                     APARCHIVE_ERROR *out_error);
/* Decompresses up to [capacity] bytes into [buffer] and sets [out_size];
 * 0 means the entry is done. The CRC-32 and the declared size are checked
 * as the data ends: the read that would return the last bytes fails with
 * APRESULT_CORRUPT_DATA instead if they don't match. */
APRESULT aparchive_entry_reader_read(aparchive_entry_reader entry_reader, void *buffer, uint64_t capacity,
                                     uint64_t *out_size, APARCHIVE_ERROR *out_error);
void aparchive_entry_reader_close(aparchive_entry_reader entry_reader);

void aparchive_reader_destroy(aparchive_reader reader);

/* ---- Utilities ------------------------------------------------------- */

/* True if [name] is a safe entry name: not empty, no NUL, no leading '/',
 * no '\\', no drive letter, and no empty, "." or ".." segments - except
 * that it may end in one '/', which makes it a directory entry's name.
 * Encoding-blind, so it judges names from any archive, UTF-8 or CP437; the
 * writer also requires UTF-8, and checks directory entries are allowed and
 * names unique. */
bool aparchive_is_safe_name(const char *name, uint64_t name_size);

/* Updates a running CRC-32 (ZIP's polynomial) with [size] bytes; start
 * from 0. */
uint32_t aparchive_crc32(uint32_t crc, const void *data, uint64_t size);

#if __cplusplus
}
#endif

#endif // APARCHIVE_H
