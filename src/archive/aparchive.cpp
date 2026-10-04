#include "aparchive_internal.hpp"
#include <libdeflate.h>
#include <new>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// Scaffold: codecs, name checks, CRC-32, struct versions, error messages and
// object lifetimes are real, as are every call's argument and state checks.
// Writing entries and the central directory, and reading an archive, aren't
// built yet - those calls return APRESULT_NOT_IMPLEMENTED.

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

// The checks every new entry's name and kind go through, whole, raw or
// streamed.
static APRESULT check_new_entry(aparchive_writer writer, const APARCHIVE_ENTRY_DESC &desc, bool raw,
                                APARCHIVE_ERROR *error)
{
    if (writer->failed || writer->finished)
        return fail(error, APRESULT_INVALID_ARGUMENT, "the writer is %s", writer->finished ? "finished" : "unusable after an earlier failure");
    if (writer->entry_open)
        return fail(error, APRESULT_INVALID_ARGUMENT, "a streamed entry is still open");
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
    {
        if (!writer->allow_directory_entries)
            return fail(error, APRESULT_INVALID_ARGUMENT, "'%s' is a directory entry, and they aren't allowed",
                        desc.name);
        if (desc.method != APARCHIVE_METHOD_STORED)
            return fail(error, APRESULT_INVALID_ARGUMENT, "directory entry '%s' must be STORED", desc.name);
    }
    if (writer->names.count(desc.name))
        return fail(error, APRESULT_INVALID_ARGUMENT, "'%s' is already in the archive", desc.name);
    if (desc.alignment > 1 && (desc.alignment & (desc.alignment - 1)) != 0)
        return fail(error, APRESULT_INVALID_ARGUMENT, "alignment %u isn't a power of two", desc.alignment);
    if (!desc.extra && desc.extra_size != 0)
        return fail(error, APRESULT_INVALID_ARGUMENT, "extra_size is %u but extra is null", desc.extra_size);
    if (!raw && !aparchive_codecs_has(writer->codecs, desc.method))
        return fail(error, APRESULT_APARCHIVE_UNKNOWN_METHOD, "method %u isn't in the writer's codec table",
                    desc.method);
    return APRESULT_OK;
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
        bool any_stream = c.stream_begin || c.stream_process || c.stream_end;
        bool all_stream = c.stream_begin && c.stream_process && c.stream_end;
        if (any_stream && !all_stream)
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

    uint64_t aparchive_codecs_compress_bound(aparchive_codecs codecs, uint16_t method, uint64_t in_size)
    {
        if (!codecs)
            return 0;
        if (method == APARCHIVE_METHOD_STORED)
            return in_size;
        const APARCHIVE_CODEC *codec = codecs->find(method);
        return codec && codec->compress_bound ? codec->compress_bound(codec->user, in_size) : 0;
    }

    APRESULT aparchive_codecs_compress(aparchive_codecs codecs, uint16_t method, int32_t level, const void *in,
                                       uint64_t in_size, void *out, uint64_t out_capacity, uint64_t *out_size,
                                       APARCHIVE_ERROR *out_error)
    {
        clear_error(out_error);
        if (!codecs || (!in && in_size != 0) || !out || !out_size)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "codecs, in, out or out_size is null");
        if (method == APARCHIVE_METHOD_STORED)
        {
            if (out_capacity < in_size)
                return fail(out_error, APRESULT_INVALID_ARGUMENT, "out holds %llu bytes, %llu needed",
                            (unsigned long long)out_capacity, (unsigned long long)in_size);
            if (in_size)
                memcpy(out, in, (size_t)in_size);
            *out_size = in_size;
            return APRESULT_OK;
        }
        const APARCHIVE_CODEC *codec = codecs->find(method);
        if (!codec)
            return fail(out_error, APRESULT_APARCHIVE_UNKNOWN_METHOD, "method %u isn't in the codec table", method);
        if (!codec->compress)
            return fail(out_error, APRESULT_UNSUPPORTED, "method %u's codec only decompresses", method);
        uint64_t bound = codec->compress_bound(codec->user, in_size);
        if (out_capacity < bound)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "out holds %llu bytes, compress_bound is %llu",
                        (unsigned long long)out_capacity, (unsigned long long)bound);
        if (!codec->compress(codec->user, level, in, in_size, out, out_capacity, out_size))
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "method %u failed to compress at level %d", method,
                        level);
        return APRESULT_OK;
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
        writer->allow_directory_entries = d.allow_directory_entries;
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
        APRESULT result = check_new_entry(writer, d, false, out_error);
        if (result != APRESULT_OK)
            return result;
        if (d.name[strlen(d.name) - 1] == '/' && size != 0)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "directory entry '%s' can't have data", d.name);
        // TODO: compress, CRC-32, local header (+ ZIP64 extra if needed or
        // asked, + 0xD935 alignment padding), data; record for the central
        // directory. A sink failure sets [failed].
        return fail(out_error, APRESULT_NOT_IMPLEMENTED, "aparchive_writer_add_entry isn't implemented yet");
    }

    APRESULT aparchive_writer_add_raw_entry(aparchive_writer writer, const APARCHIVE_ENTRY_DESC *desc,
                                            const void *compressed, uint64_t compressed_size,
                                            uint64_t uncompressed_size, uint32_t crc32, APARCHIVE_ERROR *out_error)
    {
        clear_error(out_error);
        (void)crc32;
        APARCHIVE_ENTRY_DESC d;
        if (!writer || !read_versioned(desc, APARCHIVE_ENTRY_DESC_V1_SIZE, d))
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "writer or desc is null, or desc's struct_size is too small");
        if (!compressed && compressed_size != 0)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "compressed is null but compressed_size is %llu",
                        (unsigned long long)compressed_size);
        APRESULT result = check_new_entry(writer, d, true, out_error);
        if (result != APRESULT_OK)
            return result;
        if (d.method == APARCHIVE_METHOD_STORED && compressed_size != uncompressed_size)
            return fail(out_error, APRESULT_INVALID_ARGUMENT,
                        "STORED entry '%s': compressed_size %llu differs from uncompressed_size %llu", d.name,
                        (unsigned long long)compressed_size, (unsigned long long)uncompressed_size);
        if (d.name[strlen(d.name) - 1] == '/' && uncompressed_size != 0)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "directory entry '%s' can't have data", d.name);
        // TODO: as add_entry, with the caller's bytes, sizes and CRC-32.
        return fail(out_error, APRESULT_NOT_IMPLEMENTED, "aparchive_writer_add_raw_entry isn't implemented yet");
    }

    APRESULT aparchive_writer_begin_entry(aparchive_writer writer, const APARCHIVE_ENTRY_DESC *desc,
                                          APARCHIVE_ERROR *out_error)
    {
        clear_error(out_error);
        APARCHIVE_ENTRY_DESC d;
        if (!writer || !read_versioned(desc, APARCHIVE_ENTRY_DESC_V1_SIZE, d))
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "writer or desc is null, or desc's struct_size is too small");
        APRESULT result = check_new_entry(writer, d, false, out_error);
        if (result != APRESULT_OK)
            return result;
        if (d.name[strlen(d.name) - 1] == '/')
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "directory entry '%s' has no data to stream", d.name);
        if (d.method != APARCHIVE_METHOD_STORED && !writer->codecs->find(d.method)->stream_begin)
            return fail(out_error, APRESULT_UNSUPPORTED, "method %u's codec can't stream", d.method);
        // TODO: local header with bit 3 (and a ZIP64 extra if [zip64]),
        // start the codec stream, set [entry_open].
        return fail(out_error, APRESULT_NOT_IMPLEMENTED, "aparchive_writer_begin_entry isn't implemented yet");
    }

    APRESULT aparchive_writer_write_entry_data(aparchive_writer writer, const void *data, uint64_t size,
                                               APARCHIVE_ERROR *out_error)
    {
        clear_error(out_error);
        if (!writer || (!data && size != 0))
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "writer is null, or data is null with a size");
        if (!writer->entry_open || writer->failed)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "no streamed entry is open");
        // TODO: CRC-32, compress through the stream, write what it produces.
        return fail(out_error, APRESULT_NOT_IMPLEMENTED, "aparchive_writer_write_entry_data isn't implemented yet");
    }

    APRESULT aparchive_writer_end_entry(aparchive_writer writer, APARCHIVE_ERROR *out_error)
    {
        clear_error(out_error);
        if (!writer)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "writer is null");
        if (!writer->entry_open || writer->failed)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "no streamed entry is open");
        // TODO: finish the stream, write the data descriptor (ZIP64 sizes
        // if the header had them; APRESULT_LIMIT if 4 GiB was reached
        // without), record for the central directory.
        return fail(out_error, APRESULT_NOT_IMPLEMENTED, "aparchive_writer_end_entry isn't implemented yet");
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
        if (writer->entry_open)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "a streamed entry is still open");
        // TODO: central directory, then ZIP64 end record + locator when
        // the entry count or any offset needs them, then the end record.
        return fail(out_error, APRESULT_NOT_IMPLEMENTED, "aparchive_writer_finish isn't implemented yet");
    }

    void aparchive_writer_destroy(aparchive_writer writer)
    {
        if (writer && writer->entry_stream && writer->entry_codec)
            writer->entry_codec->stream_end(writer->entry_codec->user, writer->entry_stream);
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
        // TODO: find the end record in the last 64 KiB + 22 bytes, follow
        // the ZIP64 locator, check the directory limits, read the central
        // directory whole, parse entries (names safe and unique, no
        // encryption, sizes and offsets in range, lower-bound overlap
        // check, the size and ratio limits), apply the strict flags, build
        // the name index if asked.
        return fail(out_error, APRESULT_NOT_IMPLEMENTED, "aparchive_reader_open isn't implemented yet");
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
        write_versioned(out_info, APARCHIVE_ENTRY_INFO_V1_SIZE, reader->entries[index].info);
        return APRESULT_OK;
    }

    APRESULT aparchive_reader_find(aparchive_reader reader, const char *name, uint32_t *out_index)
    {
        if (!reader || !name || !out_index)
            return APRESULT_INVALID_ARGUMENT;
        std::string_view wanted(name);
        if (!reader->name_index.empty())
        {
            auto it = reader->name_index.find(wanted);
            if (it == reader->name_index.end())
                return APRESULT_NOT_FOUND;
            *out_index = it->second;
            return APRESULT_OK;
        }
        for (uint32_t i = 0; i < (uint32_t)reader->entries.size(); i++)
        {
            const APARCHIVE_ENTRY_INFO &info = reader->entries[i].info;
            if (std::string_view(info.name, info.name_size) == wanted)
            {
                *out_index = i;
                return APRESULT_OK;
            }
        }
        return APRESULT_NOT_FOUND;
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
        // TODO: read the local header, check it against the central
        // directory (name, method, flags, sizes/CRC unless bit 3), check
        // the exact overlap/gap bounds, return the offset past it.
        return fail(out_error, APRESULT_NOT_IMPLEMENTED, "aparchive_reader_get_data_offset isn't implemented yet");
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
        const APARCHIVE_ENTRY_INFO &info = reader->entries[index].info;
        if (buffer_size != info.uncompressed_size)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "'%s' is %llu bytes; buffer_size is %llu", info.name,
                        (unsigned long long)info.uncompressed_size, (unsigned long long)buffer_size);
        // TODO: data offset, read the stored bytes, decompress through the
        // codec table (STORED: read straight into [buffer]), check CRC-32.
        return fail(out_error, APRESULT_NOT_IMPLEMENTED, "aparchive_reader_read_entry isn't implemented yet");
    }

    APRESULT aparchive_reader_open_entry(aparchive_reader reader, uint32_t index,
                                         aparchive_entry_reader *out_entry_reader, APARCHIVE_ERROR *out_error)
    {
        clear_error(out_error);
        if (!reader || !out_entry_reader)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "reader or out_entry_reader is null");
        *out_entry_reader = nullptr;
        if (index >= reader->entries.size())
            return fail(out_error, APRESULT_NOT_FOUND, "no entry %u (the archive has %u)", index,
                        (uint32_t)reader->entries.size());
        // TODO: data offset, start the codec stream (UNSUPPORTED if the
        // codec can't stream), create the entry reader.
        return fail(out_error, APRESULT_NOT_IMPLEMENTED, "aparchive_reader_open_entry isn't implemented yet");
    }

    APRESULT aparchive_entry_reader_read(aparchive_entry_reader entry_reader, void *buffer, uint64_t capacity,
                                         uint64_t *out_size, APARCHIVE_ERROR *out_error)
    {
        clear_error(out_error);
        if (!entry_reader || (!buffer && capacity != 0) || !out_size)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "entry_reader or out_size is null, or buffer is null with a capacity");
        *out_size = 0;
        // TODO: read stored bytes in chunks, decompress, update the CRC-32;
        // at the end, check it and the size.
        return fail(out_error, APRESULT_NOT_IMPLEMENTED, "aparchive_entry_reader_read isn't implemented yet");
    }

    void aparchive_entry_reader_close(aparchive_entry_reader entry_reader)
    {
        if (entry_reader && entry_reader->stream && entry_reader->codec)
            entry_reader->codec->stream_end(entry_reader->codec->user, entry_reader->stream);
        delete entry_reader;
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
