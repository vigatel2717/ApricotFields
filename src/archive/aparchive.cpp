#include "aparchive_internal.hpp"
#include <algorithm>
#include <libdeflate.h>
#include <new>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// Whole-buffer ZIP writing and reading: one entry in, one entry out, with
// ZIP64 where sizes, offsets or the entry count need it. What isn't here
// yet is listed in APARCHIVE_TODO.md.

// ---- Errors -------------------------------------------------------------------

static void clear_error(APARCHIVE_ERROR *error)
{
    if (error)
        error->message[0] = '\0';
}

// Records [format] in [error] (if any) and returns [result].
static APRESULT fail(APARCHIVE_ERROR *error, APRESULT result, const char *format, ...)
{
    if (error)
    {
        va_list args;
        va_start(args, format);
        vsnprintf(error->message, sizeof(error->message), format, args);
        va_end(args);
    }
    return result;
}

// ---- Struct versions ----------------------------------------------------------

// Copies a caller's versioned input struct into [out] at the current version:
// the fields its struct_size covers, zeros past them. False if it's smaller
// than the struct's first version.
template <typename T> static bool read_versioned(const T *in, size_t v1_size, T &out)
{
    if (!in || in->struct_size < v1_size)
        return false;
    memset(&out, 0, sizeof(T));
    memcpy(&out, in, in->struct_size < sizeof(T) ? in->struct_size : sizeof(T));
    out.struct_size = (uint32_t)sizeof(T);
    return true;
}

// Writes [value] into a caller's versioned output struct, up to its
// struct_size, which is kept as the caller set it.
template <typename T> static bool write_versioned(T *out, size_t v1_size, const T &value)
{
    if (!out || out->struct_size < v1_size)
        return false;
    uint32_t caller_size = out->struct_size;
    memcpy(out, &value, caller_size < sizeof(T) ? caller_size : sizeof(T));
    out->struct_size = caller_size;
    return true;
}

// ---- Names --------------------------------------------------------------------

static bool is_valid_utf8(const char *text, size_t size)
{
    const uint8_t *s = (const uint8_t *)text;
    size_t i = 0;
    while (i < size)
    {
        uint8_t c = s[i];
        size_t length;
        uint32_t min;
        if (c < 0x80)
        {
            i++;
            continue;
        }
        else if ((c & 0xE0) == 0xC0)
            length = 2, min = 0x80;
        else if ((c & 0xF0) == 0xE0)
            length = 3, min = 0x800;
        else if ((c & 0xF8) == 0xF0)
            length = 4, min = 0x10000;
        else
            return false;
        if (i + length > size)
            return false;
        uint32_t code = c & (0x7F >> length);
        for (size_t k = 1; k < length; k++)
        {
            if ((s[i + k] & 0xC0) != 0x80)
                return false;
            code = (code << 6) | (s[i + k] & 0x3F);
        }
        // Overlong encodings, surrogates, and past U+10FFFF.
        if (code < min || (code >= 0xD800 && code <= 0xDFFF) || code > 0x10FFFF)
            return false;
        i += length;
    }
    return true;
}

static bool is_ascii(const char *text, size_t size)
{
    for (size_t i = 0; i < size; i++)
        if ((uint8_t)text[i] >= 0x80)
            return false;
    return true;
}

// ---- ZIP records --------------------------------------------------------------
//
// Everything is little-endian. Offsets in the comments below are from the
// start of each record.

static const uint32_t SIG_LOCAL = 0x04034b50;
static const uint32_t SIG_CENTRAL = 0x02014b50;
static const uint32_t SIG_END = 0x06054b50;
static const uint32_t SIG_END64 = 0x06064b50;
static const uint32_t SIG_LOCATOR64 = 0x07064b50;

static const size_t LOCAL_HEADER_SIZE = 30;
static const size_t CENTRAL_HEADER_SIZE = 46;
static const size_t END_SIZE = 22;
static const size_t END64_SIZE = 56;
static const size_t LOCATOR64_SIZE = 20;

static const uint16_t EXTRA_ZIP64 = 0x0001;
static const uint16_t EXTRA_PADDING = 0xD935;
// The most a central header's ZIP64 record takes: its 4-byte header and
// three 8-byte fields.
static const size_t ZIP64_EXTRA_MAX_SIZE = 28;

static const uint16_t FLAG_ENCRYPTED = 0x0001;
static const uint16_t FLAG_DATA_DESCRIPTOR = 0x0008;
static const uint16_t FLAG_PATCH_DATA = 0x0020;
static const uint16_t FLAG_STRONG_ENCRYPTION = 0x0040;
static const uint16_t FLAG_UTF8 = 0x0800;
static const uint16_t FLAG_MASKED_HEADERS = 0x2000;
static const uint16_t FLAGS_ENCRYPTION = FLAG_ENCRYPTED | FLAG_STRONG_ENCRYPTION | FLAG_MASKED_HEADERS;

// "Version made by": ZIP specification 4.5 (the one with ZIP64), host
// system 0 (MS-DOS). Host 0 because no file attributes are written: with a
// Unix host, extracting tools would take the zero attributes as "no
// permissions".
static const uint16_t VERSION_MADE_BY = 45;

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get64(const uint8_t *p)
{
    return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
}

static void put16(std::vector<uint8_t> &out, uint16_t value)
{
    out.push_back((uint8_t)value);
    out.push_back((uint8_t)(value >> 8));
}

static void put32(std::vector<uint8_t> &out, uint32_t value)
{
    put16(out, (uint16_t)value);
    put16(out, (uint16_t)(value >> 16));
}

static void put64(std::vector<uint8_t> &out, uint64_t value)
{
    put32(out, (uint32_t)value);
    put32(out, (uint32_t)(value >> 32));
}

static void put_bytes(std::vector<uint8_t> &out, const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    out.insert(out.end(), bytes, bytes + size);
}

// The version a reader needs for an entry: 4.5 for ZIP64, else what its
// method needs.
static uint16_t version_needed(uint16_t method, bool zip64)
{
    uint16_t version;
    switch (method)
    {
    case APARCHIVE_METHOD_STORED:
        version = 10;
        break;
    case APARCHIVE_METHOD_DEFLATE:
        version = 20;
        break;
    case 12: // bzip2
        version = 46;
        break;
    default: // LZMA, zstd, xz and the rest of the later methods
        version = 63;
        break;
    }
    return zip64 && version < 45 ? 45 : version;
}

// True if [extra] is whole records (id, size, data) end to end.
static bool extra_is_whole_records(const uint8_t *extra, size_t size)
{
    size_t at = 0;
    while (at < size)
    {
        if (size - at < 4)
            return false;
        size_t record_size = get16(extra + at + 2);
        if (size - at - 4 < record_size)
            return false;
        at += 4 + record_size;
    }
    return true;
}

// The data of the first record with [id] in an extra field. Stops at the
// first thing that isn't a whole record: other tools' extra fields aren't
// always well formed, and one that isn't only matters if it hides a record
// that was needed.
static bool find_extra(const uint8_t *extra, size_t size, uint16_t id, const uint8_t **out_data, size_t *out_size)
{
    size_t at = 0;
    while (size - at >= 4)
    {
        size_t record_size = get16(extra + at + 2);
        if (size - at - 4 < record_size)
            return false;
        if (get16(extra + at) == id)
        {
            *out_data = extra + at + 4;
            *out_size = record_size;
            return true;
        }
        at += 4 + record_size;
    }
    return false;
}

const APARCHIVE_CODEC *aparchive_codecs_t::find(uint16_t method) const
{
    for (const APARCHIVE_CODEC &codec : codecs)
        if (codec.method == method)
            return &codec;
    return nullptr;
}

// ---- DEFLATE (libdeflate) --------------------------------------------------
//
// libdeflate's compressors and decompressors aren't thread-safe, and codec
// callbacks may run on several threads at once, so each call allocates its
// own. Both are small; a per-thread cache can come later if it shows up.

static uint64_t deflate_compress_bound(void *, uint64_t in_size)
{
    return libdeflate_deflate_compress_bound(nullptr, (size_t)in_size);
}

static bool deflate_compress(void *, int32_t level, const void *in, uint64_t in_size, void *out, uint64_t out_capacity,
                             uint64_t *out_size)
{
    if (level < 0 || level > 12)
        return false;
    libdeflate_compressor *compressor = libdeflate_alloc_compressor(level);
    if (!compressor)
        return false;
    size_t size = libdeflate_deflate_compress(compressor, in, (size_t)in_size, out, (size_t)out_capacity);
    libdeflate_free_compressor(compressor);
    if (size == 0)
        return false;
    *out_size = size;
    return true;
}

static bool deflate_decompress(void *, const void *in, uint64_t in_size, void *out, uint64_t out_size)
{
    libdeflate_decompressor *decompressor = libdeflate_alloc_decompressor();
    if (!decompressor)
        return false;
    // A NULL actual_out_nbytes makes libdeflate require exactly out_size
    // bytes - so the data can't decompress past what was declared.
    libdeflate_result result =
        libdeflate_deflate_decompress(decompressor, in, (size_t)in_size, out, (size_t)out_size, nullptr);
    libdeflate_free_decompressor(decompressor);
    return result == LIBDEFLATE_SUCCESS;
}

// ---- Writing ------------------------------------------------------------------

// The checks every new entry goes through before anything is written.
static APRESULT check_new_entry(aparchive_writer writer, const APARCHIVE_ENTRY_DESC &desc, APARCHIVE_ERROR *error)
{
    if (writer->failed || writer->finished)
        return fail(error, APRESULT_INVALID_ARGUMENT, "the writer is %s", writer->finished ? "finished" : "unusable after an earlier failure");
    if (!desc.name)
        return fail(error, APRESULT_INVALID_ARGUMENT, "the entry has no name");
    size_t name_size = strlen(desc.name);
    if (!aparchive_is_safe_name(desc.name, name_size))
        return fail(error, APRESULT_INVALID_ARGUMENT, "'%s' isn't a safe entry name", desc.name);
    if (!is_valid_utf8(desc.name, name_size))
        return fail(error, APRESULT_INVALID_ARGUMENT, "'%s' isn't valid UTF-8", desc.name);
    if (!desc.utf8_name_flag && !is_ascii(desc.name, name_size))
        return fail(error, APRESULT_INVALID_ARGUMENT, "'%s' has non-ASCII characters but utf8_name_flag isn't set",
                    desc.name);
    if (desc.name[name_size - 1] == '/')
        return fail(error, APRESULT_INVALID_ARGUMENT, "'%s' is a directory entry, which the writer doesn't write yet",
                    desc.name);
    if (writer->names.count(desc.name))
        return fail(error, APRESULT_INVALID_ARGUMENT, "'%s' is already in the archive", desc.name);
    if (desc.alignment > 1 && (desc.alignment & (desc.alignment - 1)) != 0)
        return fail(error, APRESULT_INVALID_ARGUMENT, "alignment %u isn't a power of two", desc.alignment);
    if (!desc.extra && desc.extra_size != 0)
        return fail(error, APRESULT_INVALID_ARGUMENT, "extra_size is %u but extra is null", desc.extra_size);
    if (desc.extra_size != 0)
    {
        const uint8_t *extra = (const uint8_t *)desc.extra;
        const uint8_t *zip64;
        size_t zip64_size;
        if (!extra_is_whole_records(extra, desc.extra_size))
            return fail(error, APRESULT_INVALID_ARGUMENT, "'%s': extra isn't whole extra field records", desc.name);
        if (find_extra(extra, desc.extra_size, EXTRA_ZIP64, &zip64, &zip64_size))
            return fail(error, APRESULT_INVALID_ARGUMENT, "'%s': extra has a ZIP64 record, which the writer writes itself",
                        desc.name);
        // Room for the ZIP64 record the central header may need.
        if (desc.extra_size > UINT16_MAX - ZIP64_EXTRA_MAX_SIZE)
            return fail(error, APRESULT_INVALID_ARGUMENT, "'%s': extra is %u bytes, over the %u that fit", desc.name,
                        desc.extra_size, (unsigned)(UINT16_MAX - ZIP64_EXTRA_MAX_SIZE));
    }
    if (!aparchive_codecs_has(writer->codecs, desc.method))
        return fail(error, APRESULT_APARCHIVE_UNKNOWN_METHOD, "method %u isn't in the writer's codec table",
                    desc.method);
    if (writer->entries.size() >= UINT32_MAX)
        return fail(error, APRESULT_LIMIT, "the archive already has %u entries", UINT32_MAX);
    return APRESULT_OK;
}

// Appends [size] bytes to the sink. A failure leaves a partial record
// behind, so it ends the writer.
static bool emit(aparchive_writer writer, const void *data, uint64_t size)
{
    if (size == 0)
        return true;
    if (!writer->sink.write(writer->sink.user, data, size))
    {
        writer->failed = true;
        return false;
    }
    writer->offset += size;
    return true;
}

// May throw std::bad_alloc.
static APRESULT add_entry(aparchive_writer writer, const APARCHIVE_ENTRY_DESC &desc, const void *data, uint64_t size,
                          APARCHIVE_ERROR *error)
{
    APRESULT result = check_new_entry(writer, desc, error);
    if (result != APRESULT_OK)
        return result;
    size_t name_size = strlen(desc.name);

    // The bytes as they'll be stored.
    const void *stored = data;
    uint64_t stored_size = size;
    std::vector<uint8_t> compressed;
    if (desc.method != APARCHIVE_METHOD_STORED)
    {
        const APARCHIVE_CODEC *codec = writer->codecs->find(desc.method);
        if (!codec->compress)
            return fail(error, APRESULT_UNSUPPORTED, "method %u's codec only decompresses", desc.method);
        uint64_t bound = codec->compress_bound(codec->user, size);
        if (bound > (uint64_t)SIZE_MAX)
            return fail(error, APRESULT_OUT_OF_MEMORY, "'%s' is too big to compress in memory", desc.name);
        // Never empty, so the codec always has a buffer to write to.
        compressed.resize(bound ? (size_t)bound : 1);
        if (!codec->compress(codec->user, desc.level, data, size, compressed.data(), bound, &stored_size) ||
            stored_size > bound)
            return fail(error, APRESULT_INVALID_ARGUMENT, "method %u failed to compress '%s' at level %d", desc.method,
                        desc.name, desc.level);
        stored = compressed.data();
    }
    uint32_t crc = aparchive_crc32(0, data, size);
    bool zip64 = desc.zip64 || size >= UINT32_MAX || stored_size >= UINT32_MAX;

    // The local header's extra field: the ZIP64 sizes, the caller's
    // records, then the padding that puts the data on its alignment.
    uint64_t header_offset = writer->offset;
    std::vector<uint8_t> extra;
    if (zip64)
    {
        put16(extra, EXTRA_ZIP64);
        put16(extra, 16);
        put64(extra, size);
        put64(extra, stored_size);
    }
    if (desc.extra_size)
        put_bytes(extra, desc.extra, desc.extra_size);
    if (desc.alignment > 1)
    {
        uint64_t data_offset = header_offset + LOCAL_HEADER_SIZE + name_size + extra.size();
        uint64_t padding = (desc.alignment - data_offset % desc.alignment) % desc.alignment;
        // A record is at least its 4-byte header.
        while (padding != 0 && padding < 4)
            padding += desc.alignment;
        if (padding > UINT16_MAX || extra.size() + padding > UINT16_MAX)
            return fail(error, APRESULT_INVALID_ARGUMENT,
                        "'%s': alignment %u needs more padding than an extra field holds", desc.name, desc.alignment);
        if (padding)
        {
            put16(extra, EXTRA_PADDING);
            put16(extra, (uint16_t)(padding - 4));
            extra.insert(extra.end(), (size_t)padding - 4, (uint8_t)0);
        }
    }
    if (extra.size() > UINT16_MAX)
        return fail(error, APRESULT_INVALID_ARGUMENT, "'%s': the extra field would be %llu bytes", desc.name,
                    (unsigned long long)extra.size());

    uint16_t flags = desc.utf8_name_flag ? FLAG_UTF8 : 0;
    std::vector<uint8_t> header;
    header.reserve(LOCAL_HEADER_SIZE + name_size + extra.size());
    put32(header, SIG_LOCAL);                                     //  0
    put16(header, version_needed(desc.method, zip64));            //  4
    put16(header, flags);                                         //  6
    put16(header, desc.method);                                   //  8
    put16(header, desc.dos_time);                                 // 10
    put16(header, desc.dos_date);                                 // 12
    put32(header, crc);                                           // 14
    put32(header, zip64 ? UINT32_MAX : (uint32_t)stored_size);    // 18
    put32(header, zip64 ? UINT32_MAX : (uint32_t)size);           // 22
    put16(header, (uint16_t)name_size);                           // 26
    put16(header, (uint16_t)extra.size());                        // 28
    put_bytes(header, desc.name, name_size);
    put_bytes(header, extra.data(), extra.size());

    // Everything that can run out of memory comes before the first byte is
    // written.
    aparchive_written_entry_t entry;
    entry.name.assign(desc.name, name_size);
    entry.method = desc.method;
    entry.flags = flags;
    entry.dos_date = desc.dos_date;
    entry.dos_time = desc.dos_time;
    entry.crc32 = crc;
    entry.compressed_size = stored_size;
    entry.uncompressed_size = size;
    entry.local_header_offset = header_offset;
    entry.zip64 = desc.zip64;
    if (desc.extra_size)
        put_bytes(entry.extra, desc.extra, desc.extra_size);
    writer->names.insert(entry.name);
    writer->entries.push_back(std::move(entry));

    if (!emit(writer, header.data(), header.size()) || !emit(writer, stored, stored_size))
        return fail(error, APRESULT_IO, "the sink failed while '%s' was being written", desc.name);
    return APRESULT_OK;
}

// May throw std::bad_alloc.
static APRESULT finish(aparchive_writer writer, APARCHIVE_ERROR *error)
{
    uint64_t directory_offset = writer->offset;
    std::vector<uint8_t> record;
    for (const aparchive_written_entry_t &entry : writer->entries)
    {
        // A ZIP64 record holds only the fields that don't fit, in this
        // order.
        bool uncompressed64 = entry.zip64 || entry.uncompressed_size >= UINT32_MAX;
        bool compressed64 = entry.zip64 || entry.compressed_size >= UINT32_MAX;
        bool offset64 = entry.local_header_offset >= UINT32_MAX;
        uint16_t zip64_size = (uint16_t)(((int)uncompressed64 + (int)compressed64 + (int)offset64) * 8);
        size_t extra_size = (zip64_size ? 4 + zip64_size : 0) + entry.extra.size();

        record.clear();
        put32(record, SIG_CENTRAL);                                                        //  0
        put16(record, VERSION_MADE_BY);                                                    //  4
        put16(record, version_needed(entry.method, zip64_size != 0));                      //  6
        put16(record, entry.flags);                                                        //  8
        put16(record, entry.method);                                                       // 10
        put16(record, entry.dos_time);                                                     // 12
        put16(record, entry.dos_date);                                                     // 14
        put32(record, entry.crc32);                                                        // 16
        put32(record, compressed64 ? UINT32_MAX : (uint32_t)entry.compressed_size);        // 20
        put32(record, uncompressed64 ? UINT32_MAX : (uint32_t)entry.uncompressed_size);    // 24
        put16(record, (uint16_t)entry.name.size());                                        // 28
        put16(record, (uint16_t)extra_size);                                               // 30
        put16(record, 0);                                                                  // 32 comment size
        put16(record, 0);                                                                  // 34 disk
        put16(record, 0);                                                                  // 36 internal attributes
        put32(record, 0);                                                                  // 38 external attributes
        put32(record, offset64 ? UINT32_MAX : (uint32_t)entry.local_header_offset);        // 42
        put_bytes(record, entry.name.data(), entry.name.size());
        if (zip64_size)
        {
            put16(record, EXTRA_ZIP64);
            put16(record, zip64_size);
            if (uncompressed64)
                put64(record, entry.uncompressed_size);
            if (compressed64)
                put64(record, entry.compressed_size);
            if (offset64)
                put64(record, entry.local_header_offset);
        }
        put_bytes(record, entry.extra.data(), entry.extra.size());
        if (!emit(writer, record.data(), record.size()))
            return fail(error, APRESULT_IO, "the sink failed while the central directory was being written");
    }
    uint64_t directory_size = writer->offset - directory_offset;
    uint64_t entry_count = writer->entries.size();

    record.clear();
    bool count64 = entry_count >= UINT16_MAX;
    bool size64 = directory_size >= UINT32_MAX;
    bool offset64 = directory_offset >= UINT32_MAX;
    if (count64 || size64 || offset64)
    {
        uint64_t end64_offset = writer->offset;
        put32(record, SIG_END64);            //  0
        put64(record, END64_SIZE - 12);      //  4 the record's size past this field
        put16(record, VERSION_MADE_BY);      // 12
        put16(record, 45);                   // 14 version needed
        put32(record, 0);                    // 16 this disk
        put32(record, 0);                    // 20 the directory's disk
        put64(record, entry_count);          // 24 entries on this disk
        put64(record, entry_count);          // 32 entries
        put64(record, directory_size);       // 40
        put64(record, directory_offset);     // 48
        put32(record, SIG_LOCATOR64);        //  0
        put32(record, 0);                    //  4 the ZIP64 end record's disk
        put64(record, end64_offset);         //  8
        put32(record, 1);                    // 16 disks
    }
    put32(record, SIG_END);                                                  //  0
    put16(record, 0);                                                        //  4 this disk
    put16(record, 0);                                                        //  6 the directory's disk
    put16(record, count64 ? UINT16_MAX : (uint16_t)entry_count);             //  8 entries on this disk
    put16(record, count64 ? UINT16_MAX : (uint16_t)entry_count);             // 10 entries
    put32(record, size64 ? UINT32_MAX : (uint32_t)directory_size);           // 12
    put32(record, offset64 ? UINT32_MAX : (uint32_t)directory_offset);       // 16
    put16(record, 0);                                                        // 20 comment size
    if (!emit(writer, record.data(), record.size()))
        return fail(error, APRESULT_IO, "the sink failed while the end records were being written");
    writer->finished = true;
    return APRESULT_OK;
}

// ---- Reading ------------------------------------------------------------------

// Reads [count] bytes at [offset]. [what] names what was being read, for
// the message.
static APRESULT read_source(const APARCHIVE_SOURCE &source, uint64_t offset, void *buffer, uint64_t count,
                            APARCHIVE_ERROR *error, const char *what)
{
    if (offset > source.size || count > source.size - offset)
        return fail(error, APRESULT_APARCHIVE_MALFORMED, "%s runs past the end of the archive", what);
    if (count != 0 && !source.read(source.user, offset, buffer, count))
        return fail(error, APRESULT_IO, "the source failed reading %s", what);
    return APRESULT_OK;
}

// Reads the end records and the central directory into [reader]. May throw
// std::bad_alloc.
static APRESULT open_reader(const APARCHIVE_READER_DESC &desc, aparchive_reader reader, APARCHIVE_ERROR *error)
{
    const APARCHIVE_SOURCE &source = desc.source;
    if (source.size < END_SIZE)
        return fail(error, APRESULT_APARCHIVE_NOT_AN_ARCHIVE, "%llu bytes is too small to be a ZIP archive",
                    (unsigned long long)source.size);

    // The end record is the last thing in the archive, followed only by its
    // comment (up to 65,535 bytes): search backwards for a signature whose
    // comment size reaches exactly the end.
    size_t tail_size = source.size < END_SIZE + UINT16_MAX ? (size_t)source.size : END_SIZE + UINT16_MAX;
    std::vector<uint8_t> tail(tail_size);
    APRESULT result = read_source(source, source.size - tail_size, tail.data(), tail_size, error, "the end record");
    if (result != APRESULT_OK)
        return result;
    size_t end_at = tail_size;
    for (size_t i = tail_size - END_SIZE + 1; i-- > 0;)
    {
        if (get32(tail.data() + i) == SIG_END && get16(tail.data() + i + 20) == tail_size - i - END_SIZE)
        {
            end_at = i;
            break;
        }
    }
    if (end_at == tail_size)
        return fail(error, APRESULT_APARCHIVE_NOT_AN_ARCHIVE, "no end of central directory record: not a ZIP archive");
    const uint8_t *end = tail.data() + end_at;
    uint64_t end_offset = source.size - tail_size + end_at;

    uint32_t disk = get16(end + 4);
    uint32_t directory_disk = get16(end + 6);
    uint64_t disk_entry_count = get16(end + 8);
    uint64_t entry_count = get16(end + 10);
    uint64_t directory_size = get32(end + 12);
    uint64_t directory_offset = get32(end + 16);
    // Where the directory and everything before it must end.
    uint64_t directory_limit = end_offset;

    // A field at its largest value means "see the ZIP64 end record", which
    // a locator just before the end record points to. With no locator the
    // values are literal.
    bool any_marker = disk == UINT16_MAX || directory_disk == UINT16_MAX || disk_entry_count == UINT16_MAX ||
                      entry_count == UINT16_MAX || directory_size == UINT32_MAX || directory_offset == UINT32_MAX;
    if (any_marker && end_offset >= LOCATOR64_SIZE)
    {
        uint8_t locator[LOCATOR64_SIZE];
        uint64_t locator_offset = end_offset - LOCATOR64_SIZE;
        result = read_source(source, locator_offset, locator, sizeof(locator), error, "the ZIP64 locator");
        if (result != APRESULT_OK)
            return result;
        if (get32(locator) == SIG_LOCATOR64)
        {
            if (get32(locator + 4) != 0 || get32(locator + 16) > 1)
                return fail(error, APRESULT_UNSUPPORTED, "the archive is split over several disks");
            uint64_t end64_offset = get64(locator + 8);
            if (end64_offset > locator_offset || locator_offset - end64_offset < END64_SIZE)
                return fail(error, APRESULT_APARCHIVE_MALFORMED, "the ZIP64 end record isn't where its locator says");
            uint8_t end64[END64_SIZE];
            result = read_source(source, end64_offset, end64, sizeof(end64), error, "the ZIP64 end record");
            if (result != APRESULT_OK)
                return result;
            // Its size field counts what follows it (12 bytes in); the
            // record may be longer than the fields read here.
            uint64_t end64_size = get64(end64 + 4);
            if (get32(end64) != SIG_END64 || end64_size < END64_SIZE - 12 ||
                end64_size > locator_offset - end64_offset - 12)
                return fail(error, APRESULT_APARCHIVE_MALFORMED, "the ZIP64 end record is malformed");
            disk = get32(end64 + 16);
            directory_disk = get32(end64 + 20);
            disk_entry_count = get64(end64 + 24);
            entry_count = get64(end64 + 32);
            directory_size = get64(end64 + 40);
            directory_offset = get64(end64 + 48);
            directory_limit = end64_offset;
        }
    }
    if (disk != 0 || directory_disk != 0 || disk_entry_count != entry_count)
        return fail(error, APRESULT_UNSUPPORTED, "the archive is split over several disks");
    if (directory_offset > directory_limit || directory_size > directory_limit - directory_offset)
        return fail(error, APRESULT_APARCHIVE_MALFORMED, "the central directory doesn't fit in the archive");

    // The limits on the directory, before any of it is read.
    if (entry_count > desc.max_entry_count)
        return fail(error, APRESULT_LIMIT, "%llu entries, over the limit of %u", (unsigned long long)entry_count,
                    desc.max_entry_count);
    if (directory_size > desc.max_central_directory_size)
        return fail(error, APRESULT_LIMIT, "a central directory of %llu bytes, over the limit of %llu",
                    (unsigned long long)directory_size, (unsigned long long)desc.max_central_directory_size);
    if (entry_count > directory_size / CENTRAL_HEADER_SIZE)
        return fail(error, APRESULT_APARCHIVE_MALFORMED, "%llu entries can't fit in a central directory of %llu bytes",
                    (unsigned long long)entry_count, (unsigned long long)directory_size);
    if (directory_size > (uint64_t)SIZE_MAX)
        return fail(error, APRESULT_OUT_OF_MEMORY, "the central directory is too big to hold in memory");

    reader->source = source;
    reader->codecs = desc.codecs;
    reader->directory_offset = directory_offset;
    reader->central_directory.resize((size_t)directory_size);
    result = read_source(source, directory_offset, reader->central_directory.data(), directory_size, error,
                         "the central directory");
    if (result != APRESULT_OK)
        return result;

    // Each entry takes at least its fixed header, so the names together
    // take at most the rest, plus a NUL each. Reserved once: entries point
    // into it.
    reader->names.reserve((size_t)(directory_size - entry_count * CENTRAL_HEADER_SIZE + entry_count));
    reader->entries.reserve((size_t)entry_count);
    reader->name_index.reserve((size_t)entry_count);

    const uint8_t *directory = reader->central_directory.data();
    size_t at = 0;
    uint64_t total_uncompressed = 0;
    for (uint64_t k = 0; k < entry_count; k++)
    {
        size_t left = (size_t)directory_size - at;
        const uint8_t *header = directory + at;
        if (left < CENTRAL_HEADER_SIZE || get32(header) != SIG_CENTRAL)
            return fail(error, APRESULT_APARCHIVE_MALFORMED, "central directory entry %llu is malformed",
                        (unsigned long long)k);
        uint16_t flags = get16(header + 8);
        uint16_t method = get16(header + 10);
        uint64_t compressed_size = get32(header + 20);
        uint64_t uncompressed_size = get32(header + 24);
        size_t name_size = get16(header + 28);
        size_t extra_size = get16(header + 30);
        size_t comment_size = get16(header + 32);
        uint32_t entry_disk = get16(header + 34);
        uint64_t header_offset = get32(header + 42);
        if (left - CENTRAL_HEADER_SIZE < name_size + extra_size + comment_size)
            return fail(error, APRESULT_APARCHIVE_MALFORMED, "central directory entry %llu runs past the directory",
                        (unsigned long long)k);
        const char *stored_name = (const char *)header + CENTRAL_HEADER_SIZE;
        const uint8_t *extra = header + CENTRAL_HEADER_SIZE + name_size;
        at += CENTRAL_HEADER_SIZE + name_size + extra_size + comment_size;

        // The name, copied out so it can be NUL-terminated.
        size_t name_at = reader->names.size();
        reader->names.insert(reader->names.end(), stored_name, stored_name + name_size);
        reader->names.push_back('\0');
        const char *name = reader->names.data() + name_at;
        if (!aparchive_is_safe_name(stored_name, name_size))
            return fail(error, APRESULT_APARCHIVE_MALFORMED, "entry %llu's name '%s' isn't safe", (unsigned long long)k,
                        name);

        if (flags & FLAGS_ENCRYPTION)
            return fail(error, APRESULT_UNSUPPORTED, "'%s' is encrypted", name);
        if (flags & FLAG_PATCH_DATA)
            return fail(error, APRESULT_UNSUPPORTED, "'%s' is compressed patch data", name);

        // ZIP64: a field at its largest value has its real value in the
        // ZIP64 record, which holds only those fields, in this order.
        if (uncompressed_size == UINT32_MAX || compressed_size == UINT32_MAX || header_offset == UINT32_MAX ||
            entry_disk == UINT16_MAX)
        {
            const uint8_t *zip64;
            size_t zip64_size;
            if (find_extra(extra, extra_size, EXTRA_ZIP64, &zip64, &zip64_size))
            {
                size_t zip64_at = 0;
                auto take = [&](uint64_t &value) {
                    if (zip64_size - zip64_at < 8)
                        return false;
                    value = get64(zip64 + zip64_at);
                    zip64_at += 8;
                    return true;
                };
                if ((uncompressed_size == UINT32_MAX && !take(uncompressed_size)) ||
                    (compressed_size == UINT32_MAX && !take(compressed_size)) ||
                    (header_offset == UINT32_MAX && !take(header_offset)))
                    return fail(error, APRESULT_APARCHIVE_MALFORMED, "'%s' has a ZIP64 record that's too short", name);
                if (entry_disk == UINT16_MAX)
                {
                    if (zip64_size - zip64_at < 4)
                        return fail(error, APRESULT_APARCHIVE_MALFORMED, "'%s' has a ZIP64 record that's too short",
                                    name);
                    entry_disk = get32(zip64 + zip64_at);
                }
            }
        }
        if (entry_disk != 0)
            return fail(error, APRESULT_UNSUPPORTED, "'%s' is on another disk of a split archive", name);

        // Sizes that can't be true, then the limits.
        if (method == APARCHIVE_METHOD_STORED && compressed_size != uncompressed_size)
            return fail(error, APRESULT_APARCHIVE_MALFORMED, "'%s' is STORED but its sizes differ", name);
        if (uncompressed_size != 0 && compressed_size == 0)
            return fail(error, APRESULT_APARCHIVE_MALFORMED, "'%s' has data but no stored bytes", name);
        if (header_offset > directory_offset || directory_offset - header_offset < LOCAL_HEADER_SIZE + name_size ||
            directory_offset - header_offset - LOCAL_HEADER_SIZE - name_size < compressed_size)
            return fail(error, APRESULT_APARCHIVE_MALFORMED, "'%s' doesn't fit before the central directory", name);
        if (uncompressed_size > desc.max_entry_uncompressed_size)
            return fail(error, APRESULT_LIMIT, "'%s' is %llu bytes, over the limit of %llu", name,
                        (unsigned long long)uncompressed_size, (unsigned long long)desc.max_entry_uncompressed_size);
        if (uncompressed_size > UINT64_MAX - total_uncompressed ||
            total_uncompressed + uncompressed_size > desc.max_total_uncompressed_size)
            return fail(error, APRESULT_LIMIT, "the entries together are over the limit of %llu bytes",
                        (unsigned long long)desc.max_total_uncompressed_size);
        total_uncompressed += uncompressed_size;
        if (compressed_size != 0)
        {
            uint64_t ratio = uncompressed_size / compressed_size + (uncompressed_size % compressed_size != 0);
            if (ratio > desc.max_compression_ratio)
                return fail(error, APRESULT_LIMIT, "'%s' compresses %llu:1, over the limit of %u:1", name,
                            (unsigned long long)ratio, desc.max_compression_ratio);
        }

        APARCHIVE_ENTRY_INFO info{};
        info.struct_size = (uint32_t)sizeof(info);
        info.name = name;
        info.name_size = (uint16_t)name_size;
        info.method = method;
        info.flags = flags;
        info.dos_time = get16(header + 12);
        info.dos_date = get16(header + 14);
        info.crc32 = get32(header + 16);
        info.compressed_size = compressed_size;
        info.uncompressed_size = uncompressed_size;
        info.local_header_offset = header_offset;
        info.extra = extra_size ? extra : nullptr;
        info.extra_size = (uint16_t)extra_size;
        info.is_directory = stored_name[name_size - 1] == '/';
        if (!reader->name_index.emplace(std::string_view(name, name_size), (uint32_t)k).second)
            return fail(error, APRESULT_APARCHIVE_MALFORMED, "'%s' is in the archive twice", name);
        reader->entries.push_back(info);
    }

    // No two entries may share bytes: one small entry listed many times
    // over is how an archive claims far more data than it holds. Each
    // entry takes at least its local header, name and stored bytes.
    std::vector<uint32_t> order(reader->entries.size());
    for (uint32_t i = 0; i < (uint32_t)order.size(); i++)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        return reader->entries[a].local_header_offset < reader->entries[b].local_header_offset;
    });
    for (size_t i = 1; i < order.size(); i++)
    {
        const APARCHIVE_ENTRY_INFO &before = reader->entries[order[i - 1]];
        const APARCHIVE_ENTRY_INFO &after = reader->entries[order[i]];
        uint64_t before_end = before.local_header_offset + LOCAL_HEADER_SIZE + before.name_size + before.compressed_size;
        if (before_end > after.local_header_offset)
            return fail(error, APRESULT_APARCHIVE_MALFORMED, "'%s' and '%s' overlap", before.name, after.name);
    }
    return APRESULT_OK;
}

// Where entry [index]'s stored bytes start, after checking its local header
// against the central directory. May throw std::bad_alloc.
static APRESULT locate_data(aparchive_reader reader, uint32_t index, uint64_t *out_offset, APARCHIVE_ERROR *error)
{
    const APARCHIVE_ENTRY_INFO &info = reader->entries[index];
    uint8_t header[LOCAL_HEADER_SIZE];
    APRESULT result = read_source(reader->source, info.local_header_offset, header, sizeof(header), error, info.name);
    if (result != APRESULT_OK)
        return result;
    if (get32(header) != SIG_LOCAL)
        return fail(error, APRESULT_APARCHIVE_MALFORMED, "'%s' has no local header where the central directory says",
                    info.name);
    uint16_t flags = get16(header + 6);
    uint16_t method = get16(header + 8);
    uint32_t crc = get32(header + 14);
    uint64_t compressed_size = get32(header + 18);
    uint64_t uncompressed_size = get32(header + 22);
    size_t name_size = get16(header + 26);
    size_t extra_size = get16(header + 28);
    if ((flags & FLAGS_ENCRYPTION) || method != info.method || name_size != info.name_size)
        return fail(error, APRESULT_APARCHIVE_MALFORMED, "'%s': the local header disagrees with the central directory",
                    info.name);

    // The name and the extra field (the name is never empty).
    std::vector<uint8_t> rest(name_size + extra_size);
    result = read_source(reader->source, info.local_header_offset + LOCAL_HEADER_SIZE, rest.data(), rest.size(), error,
                         info.name);
    if (result != APRESULT_OK)
        return result;
    if (memcmp(rest.data(), info.name, name_size) != 0)
        return fail(error, APRESULT_APARCHIVE_MALFORMED, "'%s': the local header has another name", info.name);

    // With a data descriptor the local header was written before the sizes
    // and CRC-32 were known, and has nothing to compare.
    if (!(flags & FLAG_DATA_DESCRIPTOR))
    {
        bool uncompressed64 = uncompressed_size == UINT32_MAX;
        bool compressed64 = compressed_size == UINT32_MAX;
        const uint8_t *zip64;
        size_t zip64_size;
        if ((uncompressed64 || compressed64) &&
            find_extra(rest.data() + name_size, extra_size, EXTRA_ZIP64, &zip64, &zip64_size))
        {
            // A local ZIP64 record should hold both sizes, uncompressed
            // first; some writers store only the one that didn't fit.
            if (zip64_size >= 16)
            {
                if (uncompressed64)
                    uncompressed_size = get64(zip64);
                if (compressed64)
                    compressed_size = get64(zip64 + 8);
            }
            else if (zip64_size >= 8 && uncompressed64 != compressed64)
                (uncompressed64 ? uncompressed_size : compressed_size) = get64(zip64);
            else
                return fail(error, APRESULT_APARCHIVE_MALFORMED, "'%s' has a ZIP64 record that's too short", info.name);
        }
        if (crc != info.crc32 || compressed_size != info.compressed_size ||
            uncompressed_size != info.uncompressed_size)
            return fail(error, APRESULT_APARCHIVE_MALFORMED,
                        "'%s': the local header's sizes or CRC-32 disagree with the central directory", info.name);
    }

    uint64_t data_offset = info.local_header_offset + LOCAL_HEADER_SIZE + name_size + extra_size;
    if (data_offset > reader->directory_offset || info.compressed_size > reader->directory_offset - data_offset)
        return fail(error, APRESULT_APARCHIVE_MALFORMED, "'%s' runs into the central directory", info.name);
    *out_offset = data_offset;
    return APRESULT_OK;
}

// May throw std::bad_alloc.
static APRESULT read_entry(aparchive_reader reader, uint32_t index, void *buffer, APARCHIVE_ERROR *error)
{
    const APARCHIVE_ENTRY_INFO &info = reader->entries[index];
    uint64_t data_offset;
    APRESULT result = locate_data(reader, index, &data_offset, error);
    if (result != APRESULT_OK)
        return result;
    if (info.method == APARCHIVE_METHOD_STORED)
    {
        result = read_source(reader->source, data_offset, buffer, info.uncompressed_size, error, info.name);
        if (result != APRESULT_OK)
            return result;
    }
    else
    {
        const APARCHIVE_CODEC *codec = reader->codecs->find(info.method);
        if (!codec)
            return fail(error, APRESULT_APARCHIVE_UNKNOWN_METHOD, "'%s' uses method %u, which isn't in the codec table",
                        info.name, info.method);
        // No bigger than the archive itself: the directory was checked to
        // fit on open.
        if (info.compressed_size > (uint64_t)SIZE_MAX)
            return fail(error, APRESULT_OUT_OF_MEMORY, "'%s' is too big to decompress in memory", info.name);
        std::vector<uint8_t> compressed((size_t)info.compressed_size);
        result = read_source(reader->source, data_offset, compressed.data(), compressed.size(), error, info.name);
        if (result != APRESULT_OK)
            return result;
        if (!codec->decompress(codec->user, compressed.data(), compressed.size(), buffer, info.uncompressed_size))
            return fail(error, APRESULT_CORRUPT_DATA, "'%s' doesn't decompress to its declared %llu bytes", info.name,
                        (unsigned long long)info.uncompressed_size);
    }
    if (aparchive_crc32(0, buffer, info.uncompressed_size) != info.crc32)
        return fail(error, APRESULT_CORRUPT_DATA, "'%s' fails its CRC-32", info.name);
    return APRESULT_OK;
}

extern "C"
{
    // ---- Codecs -------------------------------------------------------------

    APRESULT aparchive_codecs_create(aparchive_codecs *out_codecs)
    {
        if (!out_codecs)
            return APRESULT_INVALID_ARGUMENT;
        *out_codecs = new (std::nothrow) aparchive_codecs_t();
        return *out_codecs ? APRESULT_OK : APRESULT_OUT_OF_MEMORY;
    }

    APRESULT aparchive_codecs_add(aparchive_codecs codecs, const APARCHIVE_CODEC *codec)
    {
        APARCHIVE_CODEC c;
        if (!codecs || !read_versioned(codec, APARCHIVE_CODEC_V1_SIZE, c))
            return APRESULT_INVALID_ARGUMENT;
        if (c.method == APARCHIVE_METHOD_STORED || !c.decompress)
            return APRESULT_INVALID_ARGUMENT;
        if ((c.compress == nullptr) != (c.compress_bound == nullptr))
            return APRESULT_INVALID_ARGUMENT;
        if (codecs->find(c.method))
            return APRESULT_INVALID_ARGUMENT;
        try
        {
            codecs->codecs.push_back(c);
        }
        catch (const std::bad_alloc &)
        {
            return APRESULT_OUT_OF_MEMORY;
        }
        return APRESULT_OK;
    }

    APRESULT aparchive_codecs_add_deflate(aparchive_codecs codecs)
    {
        APARCHIVE_CODEC codec{};
        codec.struct_size = sizeof(codec);
        codec.method = APARCHIVE_METHOD_DEFLATE;
        codec.compress_bound = deflate_compress_bound;
        codec.compress = deflate_compress;
        codec.decompress = deflate_decompress;
        return aparchive_codecs_add(codecs, &codec);
    }

    bool aparchive_codecs_has(aparchive_codecs codecs, uint16_t method)
    {
        if (!codecs)
            return false;
        return method == APARCHIVE_METHOD_STORED || codecs->find(method) != nullptr;
    }

    void aparchive_codecs_destroy(aparchive_codecs codecs)
    {
        delete codecs;
    }

    // ---- Writing ------------------------------------------------------------

    APRESULT aparchive_writer_create(const APARCHIVE_WRITER_DESC *desc, aparchive_writer *out_writer)
    {
        if (!out_writer)
            return APRESULT_INVALID_ARGUMENT;
        *out_writer = nullptr;
        APARCHIVE_WRITER_DESC d;
        if (!read_versioned(desc, APARCHIVE_WRITER_DESC_V1_SIZE, d) || !d.sink.write || !d.codecs)
            return APRESULT_INVALID_ARGUMENT;
        aparchive_writer_t *writer = new (std::nothrow) aparchive_writer_t();
        if (!writer)
            return APRESULT_OUT_OF_MEMORY;
        writer->sink = d.sink;
        writer->codecs = d.codecs;
        *out_writer = writer;
        return APRESULT_OK;
    }

    APRESULT aparchive_writer_add_entry(aparchive_writer writer, const APARCHIVE_ENTRY_DESC *desc, const void *data,
                                        uint64_t size, APARCHIVE_ERROR *out_error)
    {
        clear_error(out_error);
        APARCHIVE_ENTRY_DESC d;
        if (!writer || !read_versioned(desc, APARCHIVE_ENTRY_DESC_V1_SIZE, d))
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "writer or desc is null, or desc's struct_size is too small");
        if (!data && size != 0)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "data is null but size is %llu", (unsigned long long)size);
        try
        {
            return add_entry(writer, d, data, size, out_error);
        }
        catch (const std::bad_alloc &)
        {
            // The entry may be half recorded.
            writer->failed = true;
            return fail(out_error, APRESULT_OUT_OF_MEMORY, "out of memory adding the entry");
        }
    }

    uint64_t aparchive_writer_get_offset(aparchive_writer writer)
    {
        return writer ? writer->offset : 0;
    }

    APRESULT aparchive_writer_finish(aparchive_writer writer, APARCHIVE_ERROR *out_error)
    {
        clear_error(out_error);
        if (!writer)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "writer is null");
        if (writer->failed || writer->finished)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "the writer is %s",
                        writer->finished ? "finished" : "unusable after an earlier failure");
        try
        {
            return finish(writer, out_error);
        }
        catch (const std::bad_alloc &)
        {
            writer->failed = true;
            return fail(out_error, APRESULT_OUT_OF_MEMORY, "out of memory writing the central directory");
        }
    }

    void aparchive_writer_destroy(aparchive_writer writer)
    {
        delete writer;
    }

    // ---- Reading ------------------------------------------------------------

    APRESULT aparchive_reader_open(const APARCHIVE_READER_DESC *desc, aparchive_reader *out_reader,
                                   APARCHIVE_ERROR *out_error)
    {
        clear_error(out_error);
        if (!out_reader)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "out_reader is null");
        *out_reader = nullptr;
        APARCHIVE_READER_DESC d;
        if (!read_versioned(desc, APARCHIVE_READER_DESC_V1_SIZE, d))
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is null, or its struct_size is too small");
        if (!d.source.read || !d.codecs)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc's source read callback or codecs is null");
        aparchive_reader_t *reader = new (std::nothrow) aparchive_reader_t();
        if (!reader)
            return fail(out_error, APRESULT_OUT_OF_MEMORY, "out of memory opening the archive");
        APRESULT result;
        try
        {
            result = open_reader(d, reader, out_error);
        }
        catch (const std::bad_alloc &)
        {
            result = fail(out_error, APRESULT_OUT_OF_MEMORY, "out of memory reading the central directory");
        }
        if (result != APRESULT_OK)
        {
            delete reader;
            return result;
        }
        *out_reader = reader;
        return APRESULT_OK;
    }

    uint32_t aparchive_reader_get_entry_count(aparchive_reader reader)
    {
        return reader ? (uint32_t)reader->entries.size() : 0;
    }

    APRESULT aparchive_reader_get_entry(aparchive_reader reader, uint32_t index, APARCHIVE_ENTRY_INFO *out_info)
    {
        if (!reader || !out_info || out_info->struct_size < APARCHIVE_ENTRY_INFO_V1_SIZE)
            return APRESULT_INVALID_ARGUMENT;
        if (index >= reader->entries.size())
            return APRESULT_NOT_FOUND;
        write_versioned(out_info, APARCHIVE_ENTRY_INFO_V1_SIZE, reader->entries[index]);
        return APRESULT_OK;
    }

    APRESULT aparchive_reader_find(aparchive_reader reader, const char *name, uint32_t *out_index)
    {
        if (!reader || !name || !out_index)
            return APRESULT_INVALID_ARGUMENT;
        auto it = reader->name_index.find(std::string_view(name));
        if (it == reader->name_index.end())
            return APRESULT_NOT_FOUND;
        *out_index = it->second;
        return APRESULT_OK;
    }

    APRESULT aparchive_reader_get_data_offset(aparchive_reader reader, uint32_t index, uint64_t *out_offset,
                                              APARCHIVE_ERROR *out_error)
    {
        clear_error(out_error);
        if (!reader || !out_offset)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "reader or out_offset is null");
        if (index >= reader->entries.size())
            return fail(out_error, APRESULT_NOT_FOUND, "no entry %u (the archive has %u)", index,
                        (uint32_t)reader->entries.size());
        try
        {
            return locate_data(reader, index, out_offset, out_error);
        }
        catch (const std::bad_alloc &)
        {
            return fail(out_error, APRESULT_OUT_OF_MEMORY, "out of memory reading the entry's local header");
        }
    }

    APRESULT aparchive_reader_read_entry(aparchive_reader reader, uint32_t index, void *buffer, uint64_t buffer_size,
                                         APARCHIVE_ERROR *out_error)
    {
        clear_error(out_error);
        if (!reader || (!buffer && buffer_size != 0))
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "reader is null, or buffer is null with a size");
        if (index >= reader->entries.size())
            return fail(out_error, APRESULT_NOT_FOUND, "no entry %u (the archive has %u)", index,
                        (uint32_t)reader->entries.size());
        const APARCHIVE_ENTRY_INFO &info = reader->entries[index];
        if (buffer_size != info.uncompressed_size)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "'%s' is %llu bytes; buffer_size is %llu", info.name,
                        (unsigned long long)info.uncompressed_size, (unsigned long long)buffer_size);
        try
        {
            return read_entry(reader, index, buffer, out_error);
        }
        catch (const std::bad_alloc &)
        {
            return fail(out_error, APRESULT_OUT_OF_MEMORY, "out of memory reading '%s'", info.name);
        }
    }

    void aparchive_reader_destroy(aparchive_reader reader)
    {
        delete reader;
    }

    // ---- Utilities ----------------------------------------------------------

    bool aparchive_is_safe_name(const char *name, uint64_t name_size)
    {
        if (!name || name_size == 0 || name_size > UINT16_MAX)
            return false;
        // A drive letter ("C:") or a leading '/' makes the name absolute.
        if (name[0] == '/' || (name_size >= 2 && name[1] == ':'))
            return false;
        // One trailing '/' marks a directory entry; judge the path before it.
        uint64_t path_size = name[name_size - 1] == '/' ? name_size - 1 : name_size;
        if (path_size == 0)
            return false;
        uint64_t segment_start = 0;
        for (uint64_t i = 0; i <= path_size; i++)
        {
            char c = i < path_size ? name[i] : '/';
            if (i < path_size && (c == '\0' || c == '\\'))
                return false;
            if (c != '/')
                continue;
            uint64_t length = i - segment_start;
            const char *segment = name + segment_start;
            // Empty segments ("a//b"), ".", and "..".
            if (length == 0 || (length == 1 && segment[0] == '.') ||
                (length == 2 && segment[0] == '.' && segment[1] == '.'))
                return false;
            segment_start = i + 1;
        }
        return true;
    }

    uint32_t aparchive_crc32(uint32_t crc, const void *data, uint64_t size)
    {
        const uint8_t *bytes = (const uint8_t *)data;
        // libdeflate_crc32 takes a size_t; feed huge inputs in pieces.
        while (size > 0)
        {
            size_t chunk = size > (uint64_t)SIZE_MAX ? SIZE_MAX : (size_t)size;
            crc = libdeflate_crc32(crc, bytes, chunk);
            bytes += chunk;
            size -= chunk;
        }
        return crc;
    }
}
