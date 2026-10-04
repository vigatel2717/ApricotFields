#ifndef APARCHIVE_INTERNAL_HPP
#define APARCHIVE_INTERNAL_HPP

#include "archive/aparchive.h"
#include <stddef.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Every opaque handle's first member is its debug name in Debug builds
// (apricore.h).

// ---- Struct versions ------------------------------------------------------
//
// The size of each versioned struct's first version: up to the end of its
// last original field. Frozen - when a field is added, these don't change.

#define APARCHIVE_V1_SIZE(type, last_field) (offsetof(type, last_field) + sizeof(((type *)nullptr)->last_field))

constexpr size_t APARCHIVE_CODEC_V1_SIZE = APARCHIVE_V1_SIZE(APARCHIVE_CODEC, stream_end);
constexpr size_t APARCHIVE_WRITER_DESC_V1_SIZE = APARCHIVE_V1_SIZE(APARCHIVE_WRITER_DESC, allow_directory_entries);
constexpr size_t APARCHIVE_ENTRY_DESC_V1_SIZE = APARCHIVE_V1_SIZE(APARCHIVE_ENTRY_DESC, zip64);
constexpr size_t APARCHIVE_READER_DESC_V1_SIZE = APARCHIVE_V1_SIZE(APARCHIVE_READER_DESC, build_name_index);
constexpr size_t APARCHIVE_ENTRY_INFO_V1_SIZE = APARCHIVE_V1_SIZE(APARCHIVE_ENTRY_INFO, is_directory);

// ---- Objects ----------------------------------------------------------------

struct aparchive_codecs_t
{
#ifdef _DEBUG
    const char *debug_name{nullptr};
#endif
    std::vector<APARCHIVE_CODEC> codecs; // never STORED
    const APARCHIVE_CODEC *find(uint16_t method) const;
};

// What the central directory needs about each entry written so far.
struct aparchive_written_entry_t
{
    std::string name;
    uint16_t method{0};
    uint16_t flags{0};
    uint16_t dos_date{0};
    uint16_t dos_time{0};
    uint32_t crc32{0};
    uint64_t compressed_size{0};
    uint64_t uncompressed_size{0};
    uint64_t local_header_offset{0};
    bool zip64{false};
    std::vector<uint8_t> extra; // the caller's records, written to the central header
};

struct aparchive_writer_t
{
#ifdef _DEBUG
    const char *debug_name{nullptr};
#endif
    APARCHIVE_SINK sink{};
    aparchive_codecs codecs{nullptr};
    bool allow_directory_entries{false};
    uint64_t offset{0}; // bytes written to the sink
    std::vector<aparchive_written_entry_t> entries;
    std::unordered_map<std::string, uint32_t> names; // for the uniqueness check
    // The streamed entry being written (begin_entry .. end_entry).
    bool entry_open{false};
    APARCHIVE_CODEC_STREAM entry_stream{nullptr};
    const APARCHIVE_CODEC *entry_codec{nullptr};
    // A failure mid-record or a finish ends the writer: nothing more may be
    // written.
    bool failed{false};
    bool finished{false};
};

// An entry as the central directory describes it. [name] and [extra] point
// into aparchive_reader_t::central_directory.
struct aparchive_read_entry_t
{
    APARCHIVE_ENTRY_INFO info{};
};

struct aparchive_reader_t
{
#ifdef _DEBUG
    const char *debug_name{nullptr};
#endif
    APARCHIVE_SOURCE source{};
    aparchive_codecs codecs{nullptr};
    APARCHIVE_READER_DESC desc{}; // as given, at the current struct version
    // The central directory as read, kept whole: entry names and extras
    // point into it.
    std::vector<uint8_t> central_directory;
    std::vector<aparchive_read_entry_t> entries;
    // Empty unless APARCHIVE_READER_DESC::build_name_index.
    std::unordered_map<std::string_view, uint32_t> name_index;
};

struct aparchive_entry_reader_t
{
#ifdef _DEBUG
    const char *debug_name{nullptr};
#endif
    aparchive_reader reader{nullptr};
    uint32_t index{0};
    uint64_t data_offset{0};       // where the stored bytes start in the source
    uint64_t compressed_read{0};   // stored bytes consumed so far
    uint64_t uncompressed_out{0};  // bytes handed to the caller so far
    uint32_t crc32{0};             // running, over what was handed out
    APARCHIVE_CODEC_STREAM stream{nullptr};
    const APARCHIVE_CODEC *codec{nullptr};
    bool done{false};
};

#endif // APARCHIVE_INTERNAL_HPP
