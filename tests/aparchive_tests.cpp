// Headless tests for ApArchive (include/archive/aparchive.h).

#include "archive/aparchive.h"
#include <stdio.h>
#include <string.h>
#include <vector>

static int failures = 0;
static const char *current_test = "";

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            printf("FAIL %s (%s:%d): %s\n", current_test, __FILE__, __LINE__, #condition);                           \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// ---- Helpers ------------------------------------------------------------------

// An archive in memory, as a sink and as a source.
static bool memory_write(void *user, const void *data, uint64_t count)
{
    std::vector<uint8_t> &out = *(std::vector<uint8_t> *)user;
    const uint8_t *bytes = (const uint8_t *)data;
    out.insert(out.end(), bytes, bytes + count);
    return true;
}

static bool memory_read(void *user, uint64_t offset, void *buffer, uint64_t count)
{
    const std::vector<uint8_t> &in = *(const std::vector<uint8_t> *)user;
    if (offset > in.size() || count > in.size() - offset)
        return false;
    memcpy(buffer, in.data() + offset, (size_t)count);
    return true;
}

static bool discard_write(void *, const void *, uint64_t)
{
    return true;
}

static bool fail_write(void *, const void *, uint64_t)
{
    return false;
}

static bool fail_read(void *, uint64_t, void *, uint64_t)
{
    return false;
}

static aparchive_codecs deflate_codecs()
{
    aparchive_codecs codecs = nullptr;
    CHECK(aparchive_codecs_create(&codecs) == APRESULT_OK);
    CHECK(aparchive_codecs_add_deflate(codecs) == APRESULT_OK);
    return codecs;
}

static aparchive_writer memory_writer(std::vector<uint8_t> &out, aparchive_codecs codecs)
{
    APARCHIVE_WRITER_DESC desc{};
    desc.struct_size = sizeof(desc);
    desc.sink.user = &out;
    desc.sink.write = memory_write;
    desc.codecs = codecs;
    aparchive_writer writer = nullptr;
    CHECK(aparchive_writer_create(&desc, &writer) == APRESULT_OK);
    return writer;
}

// A reader desc over [archive] with no limits.
static APARCHIVE_READER_DESC reader_desc(const std::vector<uint8_t> &archive, aparchive_codecs codecs)
{
    APARCHIVE_READER_DESC desc{};
    desc.struct_size = sizeof(desc);
    desc.source.user = (void *)&archive;
    desc.source.size = archive.size();
    desc.source.read = memory_read;
    desc.codecs = codecs;
    desc.max_entry_count = UINT32_MAX;
    desc.max_central_directory_size = UINT64_MAX;
    desc.max_entry_uncompressed_size = UINT64_MAX;
    desc.max_total_uncompressed_size = UINT64_MAX;
    desc.max_compression_ratio = UINT32_MAX;
    return desc;
}

static APARCHIVE_ENTRY_DESC entry_desc(const char *name, uint16_t method)
{
    APARCHIVE_ENTRY_DESC entry{};
    entry.struct_size = sizeof(entry);
    entry.name = name;
    entry.method = method;
    entry.dos_date = 0x0021; // 1980-01-01
    return entry;
}

static APARCHIVE_ENTRY_INFO entry_info(aparchive_reader reader, uint32_t index)
{
    APARCHIVE_ENTRY_INFO info{};
    info.struct_size = sizeof(info);
    CHECK(aparchive_reader_get_entry(reader, index, &info) == APRESULT_OK);
    return info;
}

// Reads the entry named [name] whole; false if it isn't there or fails.
static bool read_named(aparchive_reader reader, const char *name, std::vector<uint8_t> &out)
{
    uint32_t index;
    if (aparchive_reader_find(reader, name, &index) != APRESULT_OK)
        return false;
    APARCHIVE_ENTRY_INFO info = entry_info(reader, index);
    out.resize((size_t)info.uncompressed_size);
    return aparchive_reader_read_entry(reader, index, out.data(), out.size(), nullptr) == APRESULT_OK;
}

// ZIP records written by hand, for archives ApArchive's writer never
// produces. Every entry is STORED.
struct bytes_t
{
    std::vector<uint8_t> data;
    void u16(uint16_t value)
    {
        data.push_back((uint8_t)value);
        data.push_back((uint8_t)(value >> 8));
    }
    void u32(uint32_t value)
    {
        u16((uint16_t)value);
        u16((uint16_t)(value >> 16));
    }
    void u64(uint64_t value)
    {
        u32((uint32_t)value);
        u32((uint32_t)(value >> 32));
    }
    void text(const char *value)
    {
        data.insert(data.end(), value, value + strlen(value));
    }
    void bytes(const bytes_t &value)
    {
        data.insert(data.end(), value.data.begin(), value.data.end());
    }
    uint32_t size() const
    {
        return (uint32_t)data.size();
    }
};

static void local_header(bytes_t &out, const char *name, uint16_t flags, uint32_t crc, uint32_t compressed_size,
                         uint32_t uncompressed_size, const bytes_t &extra = bytes_t())
{
    out.u32(0x04034b50);
    out.u16(20);
    out.u16(flags);
    out.u16(APARCHIVE_METHOD_STORED);
    out.u16(0);
    out.u16(0x0021);
    out.u32(crc);
    out.u32(compressed_size);
    out.u32(uncompressed_size);
    out.u16((uint16_t)strlen(name));
    out.u16((uint16_t)extra.size());
    out.text(name);
    out.bytes(extra);
}

static void central_header(bytes_t &out, const char *name, uint16_t flags, uint32_t crc, uint32_t compressed_size,
                           uint32_t uncompressed_size, uint32_t local_header_offset, const bytes_t &extra = bytes_t())
{
    out.u32(0x02014b50);
    out.u16(20);
    out.u16(20);
    out.u16(flags);
    out.u16(APARCHIVE_METHOD_STORED);
    out.u16(0);
    out.u16(0x0021);
    out.u32(crc);
    out.u32(compressed_size);
    out.u32(uncompressed_size);
    out.u16((uint16_t)strlen(name));
    out.u16((uint16_t)extra.size());
    out.u16(0);
    out.u16(0);
    out.u16(0);
    out.u32(0);
    out.u32(local_header_offset);
    out.text(name);
    out.bytes(extra);
}

static void end_record(bytes_t &out, uint16_t entry_count, uint32_t directory_size, uint32_t directory_offset,
                       const char *comment = "")
{
    out.u32(0x06054b50);
    out.u16(0);
    out.u16(0);
    out.u16(entry_count);
    out.u16(entry_count);
    out.u32(directory_size);
    out.u32(directory_offset);
    out.u16((uint16_t)strlen(comment));
    out.text(comment);
}

// ---- Tests --------------------------------------------------------------------

static void test_codecs()
{
    current_test = "codecs";
    aparchive_codecs codecs = nullptr;
    CHECK(aparchive_codecs_create(&codecs) == APRESULT_OK);
    // A new table has STORED only.
    CHECK(aparchive_codecs_has(codecs, APARCHIVE_METHOD_STORED));
    CHECK(!aparchive_codecs_has(codecs, APARCHIVE_METHOD_DEFLATE));
    CHECK(aparchive_codecs_add_deflate(codecs) == APRESULT_OK);
    CHECK(aparchive_codecs_has(codecs, APARCHIVE_METHOD_DEFLATE));
    // No duplicates, no STORED, no codec without decompress.
    CHECK(aparchive_codecs_add_deflate(codecs) == APRESULT_INVALID_ARGUMENT);
    APARCHIVE_CODEC stored{};
    stored.struct_size = sizeof(stored);
    stored.method = APARCHIVE_METHOD_STORED;
    CHECK(aparchive_codecs_add(codecs, &stored) == APRESULT_INVALID_ARGUMENT);
    APARCHIVE_CODEC zstd{};
    zstd.struct_size = sizeof(zstd);
    zstd.method = 93;
    CHECK(aparchive_codecs_add(codecs, &zstd) == APRESULT_INVALID_ARGUMENT);
    // Tables are independent: no global registration.
    aparchive_codecs other = nullptr;
    CHECK(aparchive_codecs_create(&other) == APRESULT_OK);
    CHECK(!aparchive_codecs_has(other, APARCHIVE_METHOD_DEFLATE));
    aparchive_codecs_destroy(other);
    aparchive_codecs_destroy(codecs);
}

static void test_safe_names()
{
    current_test = "safe names";
    auto safe = [](const char *name) { return aparchive_is_safe_name(name, strlen(name)); };
    CHECK(safe("mimetype"));
    CHECK(safe("model/elements.jsonl"));
    CHECK(safe("a.b/c..d/.e"));
    CHECK(safe("dir/")); // a directory entry
    CHECK(safe("a/b/"));
    CHECK(safe("caf\xc3\xa9")); // UTF-8
    CHECK(safe("caf\x82"));     // CP437: judged encoding-blind
    CHECK(!safe(""));
    CHECK(!safe("/"));
    CHECK(!safe("/etc/passwd"));
    CHECK(!safe("C:/x"));
    CHECK(!safe("a\\b"));
    CHECK(!safe("../x"));
    CHECK(!safe("a/../b"));
    CHECK(!safe("a/./b"));
    CHECK(!safe("a//b"));
    CHECK(!safe("dir//"));
    CHECK(!aparchive_is_safe_name("a\0b", 3));
}

static void test_crc32()
{
    current_test = "crc32";
    // The standard CRC-32 check value.
    CHECK(aparchive_crc32(0, "123456789", 9) == 0xCBF43926u);
    // Running updates match one pass.
    CHECK(aparchive_crc32(aparchive_crc32(0, "1234", 4), "56789", 5) == 0xCBF43926u);
}

static void test_writer_checks()
{
    current_test = "writer checks";
    aparchive_codecs codecs = deflate_codecs();
    APARCHIVE_WRITER_DESC desc{};
    desc.sink.write = discard_write;
    desc.codecs = codecs;
    aparchive_writer writer = nullptr;
    // A struct_size below the first version is refused.
    desc.struct_size = 4;
    CHECK(aparchive_writer_create(&desc, &writer) == APRESULT_INVALID_ARGUMENT && writer == nullptr);
    desc.struct_size = sizeof(desc);
    CHECK(aparchive_writer_create(&desc, &writer) == APRESULT_OK);
    CHECK(aparchive_writer_get_offset(writer) == 0);

    APARCHIVE_ERROR error;
    APARCHIVE_ENTRY_DESC entry = entry_desc("../escape", APARCHIVE_METHOD_STORED);
    CHECK(aparchive_writer_add_entry(writer, &entry, "x", 1, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(strstr(error.message, "../escape") != nullptr);
    // Non-ASCII names need the UTF-8 flag, and must be valid UTF-8.
    entry = entry_desc("caf\xc3\xa9", APARCHIVE_METHOD_STORED);
    CHECK(aparchive_writer_add_entry(writer, &entry, "x", 1, nullptr) == APRESULT_INVALID_ARGUMENT);
    entry = entry_desc("caf\x82", APARCHIVE_METHOD_STORED);
    entry.utf8_name_flag = true;
    CHECK(aparchive_writer_add_entry(writer, &entry, "x", 1, nullptr) == APRESULT_INVALID_ARGUMENT);
    // Directory entries aren't written yet.
    entry = entry_desc("dir/", APARCHIVE_METHOD_STORED);
    CHECK(aparchive_writer_add_entry(writer, &entry, nullptr, 0, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(strstr(error.message, "directory") != nullptr);
    entry = entry_desc("a", APARCHIVE_METHOD_STORED);
    entry.alignment = 48; // not a power of two
    CHECK(aparchive_writer_add_entry(writer, &entry, "x", 1, nullptr) == APRESULT_INVALID_ARGUMENT);
    entry = entry_desc("a", 93); // not in the table
    CHECK(aparchive_writer_add_entry(writer, &entry, "x", 1, nullptr) == APRESULT_APARCHIVE_UNKNOWN_METHOD);
    // Extra must be whole records, and not ZIP64's.
    const uint8_t cut_short[] = {0xFE, 0xCA, 0x05, 0x00, 'x'};
    entry = entry_desc("a", APARCHIVE_METHOD_STORED);
    entry.extra = cut_short;
    entry.extra_size = sizeof(cut_short);
    CHECK(aparchive_writer_add_entry(writer, &entry, "x", 1, nullptr) == APRESULT_INVALID_ARGUMENT);
    const uint8_t zip64[] = {0x01, 0x00, 0x00, 0x00};
    entry.extra = zip64;
    entry.extra_size = sizeof(zip64);
    CHECK(aparchive_writer_add_entry(writer, &entry, "x", 1, nullptr) == APRESULT_INVALID_ARGUMENT);
    // A refused entry writes nothing and leaves the writer usable.
    CHECK(aparchive_writer_get_offset(writer) == 0);
    entry = entry_desc("a", APARCHIVE_METHOD_STORED);
    CHECK(aparchive_writer_add_entry(writer, &entry, "x", 1, &error) == APRESULT_OK && error.message[0] == '\0');
    // No name twice.
    CHECK(aparchive_writer_add_entry(writer, &entry, "y", 1, nullptr) == APRESULT_INVALID_ARGUMENT);
    // Nothing after a finish.
    CHECK(aparchive_writer_finish(writer, nullptr) == APRESULT_OK);
    entry = entry_desc("b", APARCHIVE_METHOD_STORED);
    CHECK(aparchive_writer_add_entry(writer, &entry, "x", 1, nullptr) == APRESULT_INVALID_ARGUMENT);
    CHECK(aparchive_writer_finish(writer, nullptr) == APRESULT_INVALID_ARGUMENT);
    aparchive_writer_destroy(writer);

    // A sink failure ends the writer.
    desc.sink.write = fail_write;
    CHECK(aparchive_writer_create(&desc, &writer) == APRESULT_OK);
    entry = entry_desc("a", APARCHIVE_METHOD_STORED);
    CHECK(aparchive_writer_add_entry(writer, &entry, "x", 1, &error) == APRESULT_IO && error.message[0] != '\0');
    entry = entry_desc("b", APARCHIVE_METHOD_STORED);
    CHECK(aparchive_writer_add_entry(writer, &entry, "x", 1, nullptr) == APRESULT_INVALID_ARGUMENT);
    CHECK(aparchive_writer_finish(writer, nullptr) == APRESULT_INVALID_ARGUMENT);
    aparchive_writer_destroy(writer);
    aparchive_codecs_destroy(codecs);
}

static void test_reader_checks()
{
    current_test = "reader checks";
    aparchive_codecs codecs = nullptr;
    CHECK(aparchive_codecs_create(&codecs) == APRESULT_OK);
    APARCHIVE_READER_DESC desc{};
    desc.source.read = fail_read;
    desc.codecs = codecs;
    aparchive_reader reader = nullptr;
    APARCHIVE_ERROR error;
    // struct_size unset (0) is refused, with a message saying so.
    CHECK(aparchive_reader_open(&desc, &reader, &error) == APRESULT_INVALID_ARGUMENT && reader == nullptr);
    CHECK(strstr(error.message, "struct_size") != nullptr);
    // A source that fails.
    desc.struct_size = sizeof(desc);
    desc.source.size = 1000;
    CHECK(aparchive_reader_open(&desc, &reader, &error) == APRESULT_IO && reader == nullptr);
    // Not archives: too small, and no end record.
    std::vector<uint8_t> tiny(10, 'x'), junk(100000, 'x');
    desc = reader_desc(tiny, codecs);
    CHECK(aparchive_reader_open(&desc, &reader, nullptr) == APRESULT_APARCHIVE_NOT_AN_ARCHIVE && reader == nullptr);
    desc = reader_desc(junk, codecs);
    CHECK(aparchive_reader_open(&desc, &reader, &error) == APRESULT_APARCHIVE_NOT_AN_ARCHIVE && reader == nullptr);
    CHECK(error.message[0] != '\0');
    aparchive_codecs_destroy(codecs);
}

// The contents the round-trip tests write.
static const char MIMETYPE[] = "application/vnd.test+zip";
static const uint8_t CUSTOM_EXTRA[] = {0xFE, 0xCA, 0x02, 0x00, 'x', 'y'};

static std::vector<uint8_t> text_data()
{
    std::vector<uint8_t> data;
    for (int i = 0; i < 400; i++)
    {
        const char *line = "{\"guid\":\"3f2c9a4e\",\"category\":\"wall\"}\n";
        data.insert(data.end(), line, line + strlen(line));
    }
    return data;
}

static std::vector<uint8_t> blob_data()
{
    std::vector<uint8_t> data(1000);
    for (size_t i = 0; i < data.size(); i++)
        data[i] = (uint8_t)(i * 7 + 3);
    return data;
}

// An archive shaped like a document: an identifying first entry, compressed
// text, an aligned binary entry, an empty entry and a UTF-8 name.
static std::vector<uint8_t> write_sample(aparchive_codecs codecs)
{
    std::vector<uint8_t> archive;
    aparchive_writer writer = memory_writer(archive, codecs);
    std::vector<uint8_t> text = text_data(), blob = blob_data();
    APARCHIVE_ERROR error;

    APARCHIVE_ENTRY_DESC entry = entry_desc("mimetype", APARCHIVE_METHOD_STORED);
    CHECK(aparchive_writer_add_entry(writer, &entry, MIMETYPE, strlen(MIMETYPE), &error) == APRESULT_OK);
    entry = entry_desc("model/data.json", APARCHIVE_METHOD_DEFLATE);
    entry.level = 6;
    CHECK(aparchive_writer_add_entry(writer, &entry, text.data(), text.size(), &error) == APRESULT_OK);
    entry = entry_desc("geometry/blob.bin", APARCHIVE_METHOD_STORED);
    entry.alignment = 64;
    CHECK(aparchive_writer_add_entry(writer, &entry, blob.data(), blob.size(), &error) == APRESULT_OK);
    entry = entry_desc("empty", APARCHIVE_METHOD_STORED);
    CHECK(aparchive_writer_add_entry(writer, &entry, nullptr, 0, &error) == APRESULT_OK);
    entry = entry_desc("caf\xc3\xa9.txt", APARCHIVE_METHOD_DEFLATE);
    entry.level = 9;
    entry.utf8_name_flag = true;
    entry.extra = CUSTOM_EXTRA;
    entry.extra_size = sizeof(CUSTOM_EXTRA);
    CHECK(aparchive_writer_add_entry(writer, &entry, "bonjour", 7, &error) == APRESULT_OK);

    CHECK(aparchive_writer_finish(writer, &error) == APRESULT_OK && error.message[0] == '\0');
    CHECK(aparchive_writer_get_offset(writer) == archive.size());
    aparchive_writer_destroy(writer);
    return archive;
}

static void test_round_trip()
{
    current_test = "round trip";
    aparchive_codecs codecs = deflate_codecs();
    std::vector<uint8_t> archive = write_sample(codecs);
    std::vector<uint8_t> text = text_data(), blob = blob_data();

    // Starts with a local header, and the first entry's data is at a fixed
    // offset: 30 bytes of header and the 8 of "mimetype", no extra field.
    CHECK(archive.size() > 38 + strlen(MIMETYPE));
    CHECK(memcmp(archive.data(), "PK\x03\x04", 4) == 0);
    CHECK(memcmp(archive.data() + 38, MIMETYPE, strlen(MIMETYPE)) == 0);
    // The same input gives the same bytes.
    CHECK(write_sample(codecs) == archive);

    APARCHIVE_READER_DESC desc = reader_desc(archive, codecs);
    aparchive_reader reader = nullptr;
    APARCHIVE_ERROR error;
    CHECK(aparchive_reader_open(&desc, &reader, &error) == APRESULT_OK && error.message[0] == '\0');
    if (!reader)
    {
        aparchive_codecs_destroy(codecs);
        return;
    }
    CHECK(aparchive_reader_get_entry_count(reader) == 5);

    // Entries come back in the order written, as written.
    APARCHIVE_ENTRY_INFO info = entry_info(reader, 0);
    CHECK(strcmp(info.name, "mimetype") == 0 && info.name_size == 8);
    CHECK(info.method == APARCHIVE_METHOD_STORED && info.flags == 0);
    CHECK(info.dos_date == 0x0021 && info.dos_time == 0);
    CHECK(info.uncompressed_size == strlen(MIMETYPE) && info.compressed_size == strlen(MIMETYPE));
    CHECK(info.crc32 == aparchive_crc32(0, MIMETYPE, strlen(MIMETYPE)));
    CHECK(info.local_header_offset == 0 && info.extra == nullptr && info.extra_size == 0 && !info.is_directory);
    info = entry_info(reader, 1);
    CHECK(strcmp(info.name, "model/data.json") == 0 && info.method == APARCHIVE_METHOD_DEFLATE);
    CHECK(info.uncompressed_size == text.size() && info.compressed_size < text.size() / 10);
    info = entry_info(reader, 4);
    CHECK(strcmp(info.name, "caf\xc3\xa9.txt") == 0 && (info.flags & 0x0800));
    CHECK(info.extra_size == sizeof(CUSTOM_EXTRA) && info.extra &&
          memcmp(info.extra, CUSTOM_EXTRA, sizeof(CUSTOM_EXTRA)) == 0);
    APARCHIVE_ENTRY_INFO unset{};
    CHECK(aparchive_reader_get_entry(reader, 0, &unset) == APRESULT_INVALID_ARGUMENT); // no struct_size
    CHECK(aparchive_reader_get_entry(reader, 5, &info) == APRESULT_NOT_FOUND);

    // Every entry reads back.
    std::vector<uint8_t> data;
    CHECK(read_named(reader, "mimetype", data) && data.size() == strlen(MIMETYPE) &&
          memcmp(data.data(), MIMETYPE, data.size()) == 0);
    CHECK(read_named(reader, "model/data.json", data) && data == text);
    CHECK(read_named(reader, "geometry/blob.bin", data) && data == blob);
    CHECK(read_named(reader, "empty", data) && data.empty());
    CHECK(read_named(reader, "caf\xc3\xa9.txt", data) && data.size() == 7 && memcmp(data.data(), "bonjour", 7) == 0);
    uint32_t index = 99;
    CHECK(aparchive_reader_find(reader, "Mimetype", &index) == APRESULT_NOT_FOUND); // exact, case and all
    CHECK(aparchive_reader_find(reader, "geometry/blob.bin", &index) == APRESULT_OK && index == 2);

    // The aligned entry's data is on its boundary, and there in the archive
    // as stored.
    uint64_t offset = 0;
    CHECK(aparchive_reader_get_data_offset(reader, index, &offset, &error) == APRESULT_OK);
    CHECK(offset % 64 == 0 && offset + blob.size() <= archive.size());
    CHECK(memcmp(archive.data() + offset, blob.data(), blob.size()) == 0);
    CHECK(aparchive_reader_get_data_offset(reader, 0, &offset, nullptr) == APRESULT_OK && offset == 38);

    // The buffer must be the entry's size exactly.
    data.resize(blob.size() + 1);
    CHECK(aparchive_reader_read_entry(reader, index, data.data(), data.size(), &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(aparchive_reader_read_entry(reader, 5, data.data(), data.size(), nullptr) == APRESULT_NOT_FOUND);
    aparchive_reader_destroy(reader);

    // Without DEFLATE in the table, the archive opens and its STORED
    // entries read; the DEFLATE ones don't.
    aparchive_codecs stored_only = nullptr;
    CHECK(aparchive_codecs_create(&stored_only) == APRESULT_OK);
    desc = reader_desc(archive, stored_only);
    CHECK(aparchive_reader_open(&desc, &reader, nullptr) == APRESULT_OK);
    if (reader)
    {
        CHECK(read_named(reader, "geometry/blob.bin", data) && data == blob);
        data.resize(text.size());
        CHECK(aparchive_reader_read_entry(reader, 1, data.data(), data.size(), &error) ==
              APRESULT_APARCHIVE_UNKNOWN_METHOD);
        CHECK(strstr(error.message, "model/data.json") != nullptr);
        aparchive_reader_destroy(reader);
    }
    aparchive_codecs_destroy(stored_only);
    aparchive_codecs_destroy(codecs);
}

static void test_empty_archive()
{
    current_test = "empty archive";
    aparchive_codecs codecs = deflate_codecs();
    std::vector<uint8_t> archive;
    aparchive_writer writer = memory_writer(archive, codecs);
    CHECK(aparchive_writer_finish(writer, nullptr) == APRESULT_OK);
    aparchive_writer_destroy(writer);
    CHECK(archive.size() == 22); // the end record alone
    APARCHIVE_READER_DESC desc = reader_desc(archive, codecs);
    aparchive_reader reader = nullptr;
    CHECK(aparchive_reader_open(&desc, &reader, nullptr) == APRESULT_OK);
    CHECK(aparchive_reader_get_entry_count(reader) == 0);
    aparchive_reader_destroy(reader);
    aparchive_codecs_destroy(codecs);
}

// Small alignments, where the padding wanted can be smaller than a record's
// own 4-byte header.
static void test_alignment()
{
    current_test = "alignment";
    aparchive_codecs codecs = deflate_codecs();
    const char *names[] = {"a", "ab", "abc", "abcd", "abcde", "abcdef", "abcdefg", "abcdefgh"};
    const uint32_t alignments[] = {2, 4, 8, 4096};
    for (uint32_t alignment : alignments)
    {
        std::vector<uint8_t> archive;
        aparchive_writer writer = memory_writer(archive, codecs);
        for (const char *name : names)
        {
            APARCHIVE_ENTRY_DESC entry = entry_desc(name, APARCHIVE_METHOD_STORED);
            entry.alignment = alignment;
            CHECK(aparchive_writer_add_entry(writer, &entry, name, strlen(name), nullptr) == APRESULT_OK);
        }
        CHECK(aparchive_writer_finish(writer, nullptr) == APRESULT_OK);
        aparchive_writer_destroy(writer);

        APARCHIVE_READER_DESC desc = reader_desc(archive, codecs);
        aparchive_reader reader = nullptr;
        CHECK(aparchive_reader_open(&desc, &reader, nullptr) == APRESULT_OK);
        if (!reader)
            continue;
        for (uint32_t i = 0; i < 8; i++)
        {
            uint64_t offset = 1;
            CHECK(aparchive_reader_get_data_offset(reader, i, &offset, nullptr) == APRESULT_OK);
            CHECK(offset % alignment == 0);
            std::vector<uint8_t> data;
            CHECK(read_named(reader, names[i], data) && data.size() == strlen(names[i]) &&
                  memcmp(data.data(), names[i], data.size()) == 0);
        }
        aparchive_reader_destroy(reader);
    }
    aparchive_codecs_destroy(codecs);
}

static void test_limits()
{
    current_test = "limits";
    aparchive_codecs codecs = deflate_codecs();
    std::vector<uint8_t> archive = write_sample(codecs);
    aparchive_reader reader = nullptr;
    APARCHIVE_ERROR error;

    APARCHIVE_READER_DESC desc = reader_desc(archive, codecs);
    desc.max_entry_count = 4;
    CHECK(aparchive_reader_open(&desc, &reader, &error) == APRESULT_LIMIT && reader == nullptr);
    CHECK(error.message[0] != '\0');
    desc = reader_desc(archive, codecs);
    desc.max_central_directory_size = 10;
    CHECK(aparchive_reader_open(&desc, &reader, nullptr) == APRESULT_LIMIT && reader == nullptr);
    desc = reader_desc(archive, codecs);
    desc.max_entry_uncompressed_size = 999; // the blob is 1000
    CHECK(aparchive_reader_open(&desc, &reader, &error) == APRESULT_LIMIT && reader == nullptr);
    desc = reader_desc(archive, codecs);
    desc.max_total_uncompressed_size = 2000;
    CHECK(aparchive_reader_open(&desc, &reader, nullptr) == APRESULT_LIMIT && reader == nullptr);
    desc = reader_desc(archive, codecs);
    desc.max_compression_ratio = 5; // the repeated text compresses far more
    CHECK(aparchive_reader_open(&desc, &reader, &error) == APRESULT_LIMIT && reader == nullptr);
    CHECK(strstr(error.message, "model/data.json") != nullptr);
    // At the limits exactly, it opens.
    desc = reader_desc(archive, codecs);
    desc.max_entry_count = 5;
    desc.max_entry_uncompressed_size = text_data().size();
    CHECK(aparchive_reader_open(&desc, &reader, nullptr) == APRESULT_OK);
    aparchive_reader_destroy(reader);
    aparchive_codecs_destroy(codecs);
}

static void test_corruption()
{
    current_test = "corruption";
    aparchive_codecs codecs = deflate_codecs();
    const std::vector<uint8_t> archive = write_sample(codecs);
    aparchive_reader reader = nullptr;
    APARCHIVE_ERROR error;

    // Where two entries' data sit.
    uint64_t blob_offset = 0, text_offset = 0;
    APARCHIVE_READER_DESC desc = reader_desc(archive, codecs);
    CHECK(aparchive_reader_open(&desc, &reader, nullptr) == APRESULT_OK);
    if (!reader)
    {
        aparchive_codecs_destroy(codecs);
        return;
    }
    CHECK(aparchive_reader_get_data_offset(reader, 2, &blob_offset, nullptr) == APRESULT_OK);
    CHECK(aparchive_reader_get_data_offset(reader, 1, &text_offset, nullptr) == APRESULT_OK);
    APARCHIVE_ENTRY_INFO text_info = entry_info(reader, 1);
    aparchive_reader_destroy(reader);
    reader = nullptr;

    // A changed byte in a STORED entry fails its CRC-32; the other entries
    // still read.
    std::vector<uint8_t> damaged = archive;
    damaged[(size_t)blob_offset + 10] ^= 0x01;
    desc = reader_desc(damaged, codecs);
    CHECK(aparchive_reader_open(&desc, &reader, nullptr) == APRESULT_OK);
    if (reader)
    {
        std::vector<uint8_t> data(1000);
        CHECK(aparchive_reader_read_entry(reader, 2, data.data(), data.size(), &error) == APRESULT_CORRUPT_DATA);
        CHECK(strstr(error.message, "geometry/blob.bin") != nullptr);
        CHECK(read_named(reader, "model/data.json", data) && data == text_data());
        aparchive_reader_destroy(reader);
        reader = nullptr;
    }

    // Changed bytes in a DEFLATE entry: it fails to decompress or fails its
    // CRC-32, either way as corrupt data.
    damaged = archive;
    for (size_t i = 0; i < 8; i++)
        damaged[(size_t)text_offset + (size_t)text_info.compressed_size / 2 + i] ^= 0xFF;
    desc = reader_desc(damaged, codecs);
    CHECK(aparchive_reader_open(&desc, &reader, nullptr) == APRESULT_OK);
    if (reader)
    {
        std::vector<uint8_t> data((size_t)text_info.uncompressed_size);
        CHECK(aparchive_reader_read_entry(reader, 1, data.data(), data.size(), nullptr) == APRESULT_CORRUPT_DATA);
        aparchive_reader_destroy(reader);
        reader = nullptr;
    }

    // A local header whose name isn't the central directory's.
    damaged = archive;
    damaged[30] = 'n'; // "mimetype" -> "nimetype"
    desc = reader_desc(damaged, codecs);
    CHECK(aparchive_reader_open(&desc, &reader, nullptr) == APRESULT_OK);
    if (reader)
    {
        uint64_t offset;
        CHECK(aparchive_reader_get_data_offset(reader, 0, &offset, &error) == APRESULT_APARCHIVE_MALFORMED);
        CHECK(strstr(error.message, "mimetype") != nullptr);
        aparchive_reader_destroy(reader);
        reader = nullptr;
    }

    // A local header that isn't one.
    damaged = archive;
    damaged[0] = 'X';
    desc = reader_desc(damaged, codecs);
    CHECK(aparchive_reader_open(&desc, &reader, nullptr) == APRESULT_OK);
    if (reader)
    {
        std::vector<uint8_t> data(strlen(MIMETYPE));
        CHECK(aparchive_reader_read_entry(reader, 0, data.data(), data.size(), nullptr) == APRESULT_APARCHIVE_MALFORMED);
        aparchive_reader_destroy(reader);
        reader = nullptr;
    }

    // Cut short: the end record is gone, or the directory no longer fits.
    for (size_t cut : {(size_t)1, (size_t)22, (size_t)60, archive.size() / 2})
    {
        damaged.assign(archive.begin(), archive.end() - cut);
        desc = reader_desc(damaged, codecs);
        CHECK(aparchive_reader_open(&desc, &reader, &error) != APRESULT_OK && reader == nullptr);
        CHECK(error.message[0] != '\0');
    }
    aparchive_codecs_destroy(codecs);
}

// Archives as other tools write them, which ApArchive's writer never does:
// an archive comment, a directory entry, and an entry written with a data
// descriptor (its local header has no sizes or CRC-32).
static void test_foreign_archive()
{
    current_test = "foreign archive";
    aparchive_codecs codecs = deflate_codecs();
    uint32_t crc = aparchive_crc32(0, "hello", 5);
    bytes_t zip;
    local_header(zip, "dir/", 0, 0, 0, 0);
    uint32_t file_offset = zip.size();
    local_header(zip, "dir/hello.txt", 0x0008, 0, 0, 0);
    zip.text("hello");
    zip.u32(0x08074b50); // the data descriptor
    zip.u32(crc);
    zip.u32(5);
    zip.u32(5);
    uint32_t directory_offset = zip.size();
    central_header(zip, "dir/", 0, 0, 0, 0, 0);
    central_header(zip, "dir/hello.txt", 0x0008, crc, 5, 5, file_offset);
    end_record(zip, 2, zip.size() - directory_offset, directory_offset, "made elsewhere");

    APARCHIVE_READER_DESC desc = reader_desc(zip.data, codecs);
    aparchive_reader reader = nullptr;
    APARCHIVE_ERROR error;
    CHECK(aparchive_reader_open(&desc, &reader, &error) == APRESULT_OK);
    if (reader)
    {
        CHECK(aparchive_reader_get_entry_count(reader) == 2);
        APARCHIVE_ENTRY_INFO info = entry_info(reader, 0);
        CHECK(info.is_directory && info.uncompressed_size == 0);
        CHECK(aparchive_reader_read_entry(reader, 0, nullptr, 0, nullptr) == APRESULT_OK);
        info = entry_info(reader, 1);
        CHECK(!info.is_directory && (info.flags & 0x0008));
        std::vector<uint8_t> data;
        CHECK(read_named(reader, "dir/hello.txt", data) && data.size() == 5 && memcmp(data.data(), "hello", 5) == 0);
        aparchive_reader_destroy(reader);
        reader = nullptr;
    }

    // Two names for the same bytes: refused on open.
    bytes_t twice;
    crc = aparchive_crc32(0, "abc", 3);
    local_header(twice, "a", 0, crc, 3, 3);
    twice.text("abc");
    directory_offset = twice.size();
    central_header(twice, "a", 0, crc, 3, 3, 0);
    central_header(twice, "b", 0, crc, 3, 3, 0);
    end_record(twice, 2, twice.size() - directory_offset, directory_offset);
    desc = reader_desc(twice.data, codecs);
    CHECK(aparchive_reader_open(&desc, &reader, &error) == APRESULT_APARCHIVE_MALFORMED && reader == nullptr);
    CHECK(strstr(error.message, "overlap") != nullptr);

    // The same name twice.
    bytes_t repeated;
    local_header(repeated, "a", 0, crc, 3, 3);
    repeated.text("abc");
    uint32_t second_offset = repeated.size();
    local_header(repeated, "a", 0, crc, 3, 3);
    repeated.text("abc");
    directory_offset = repeated.size();
    central_header(repeated, "a", 0, crc, 3, 3, 0);
    central_header(repeated, "a", 0, crc, 3, 3, second_offset);
    end_record(repeated, 2, repeated.size() - directory_offset, directory_offset);
    desc = reader_desc(repeated.data, codecs);
    CHECK(aparchive_reader_open(&desc, &reader, nullptr) == APRESULT_APARCHIVE_MALFORMED && reader == nullptr);

    // A name that would leave the directory it's extracted into.
    bytes_t escape;
    local_header(escape, "../x", 0, crc, 3, 3);
    escape.text("abc");
    directory_offset = escape.size();
    central_header(escape, "../x", 0, crc, 3, 3, 0);
    end_record(escape, 1, escape.size() - directory_offset, directory_offset);
    desc = reader_desc(escape.data, codecs);
    CHECK(aparchive_reader_open(&desc, &reader, &error) == APRESULT_APARCHIVE_MALFORMED && reader == nullptr);
    CHECK(strstr(error.message, "../x") != nullptr);

    // An encrypted entry.
    bytes_t encrypted;
    local_header(encrypted, "a", 0x0001, crc, 3, 3);
    encrypted.text("abc");
    directory_offset = encrypted.size();
    central_header(encrypted, "a", 0x0001, crc, 3, 3, 0);
    end_record(encrypted, 1, encrypted.size() - directory_offset, directory_offset);
    desc = reader_desc(encrypted.data, codecs);
    CHECK(aparchive_reader_open(&desc, &reader, nullptr) == APRESULT_UNSUPPORTED && reader == nullptr);

    // A local header claiming other sizes than the central directory.
    bytes_t disagree;
    local_header(disagree, "a", 0, crc, 2, 2);
    disagree.text("abc");
    directory_offset = disagree.size();
    central_header(disagree, "a", 0, crc, 3, 3, 0);
    end_record(disagree, 1, disagree.size() - directory_offset, directory_offset);
    desc = reader_desc(disagree.data, codecs);
    CHECK(aparchive_reader_open(&desc, &reader, nullptr) == APRESULT_OK);
    if (reader)
    {
        uint8_t data[3];
        CHECK(aparchive_reader_read_entry(reader, 0, data, 3, nullptr) == APRESULT_APARCHIVE_MALFORMED);
        aparchive_reader_destroy(reader);
        reader = nullptr;
    }
    aparchive_codecs_destroy(codecs);
}

// ZIP64 records on an archive small enough not to need them, which the
// format allows: written by hand, and by the writer when asked.
static void test_zip64()
{
    current_test = "zip64";
    aparchive_codecs codecs = deflate_codecs();
    aparchive_reader reader = nullptr;
    APARCHIVE_ERROR error;

    // By hand: every size, offset and count deferred to ZIP64 records.
    uint32_t crc = aparchive_crc32(0, "abc", 3);
    bytes_t local_extra, central_extra, zip;
    local_extra.u16(0x0001);
    local_extra.u16(16);
    local_extra.u64(3); // uncompressed
    local_extra.u64(3); // compressed
    central_extra.u16(0x0001);
    central_extra.u16(24);
    central_extra.u64(3); // uncompressed
    central_extra.u64(3); // compressed
    central_extra.u64(0); // local header offset
    local_header(zip, "a", 0, crc, UINT32_MAX, UINT32_MAX, local_extra);
    zip.text("abc");
    uint32_t directory_offset = zip.size();
    central_header(zip, "a", 0, crc, UINT32_MAX, UINT32_MAX, UINT32_MAX, central_extra);
    uint32_t directory_size = zip.size() - directory_offset;
    uint32_t end64_offset = zip.size();
    zip.u32(0x06064b50); // the ZIP64 end record
    zip.u64(44);
    zip.u16(45);
    zip.u16(45);
    zip.u32(0);
    zip.u32(0);
    zip.u64(1);
    zip.u64(1);
    zip.u64(directory_size);
    zip.u64(directory_offset);
    zip.u32(0x07064b50); // its locator
    zip.u32(0);
    zip.u64(end64_offset);
    zip.u32(1);
    end_record(zip, UINT16_MAX, UINT32_MAX, UINT32_MAX);

    APARCHIVE_READER_DESC desc = reader_desc(zip.data, codecs);
    CHECK(aparchive_reader_open(&desc, &reader, &error) == APRESULT_OK);
    if (reader)
    {
        CHECK(aparchive_reader_get_entry_count(reader) == 1);
        APARCHIVE_ENTRY_INFO info = entry_info(reader, 0);
        CHECK(info.uncompressed_size == 3 && info.compressed_size == 3 && info.local_header_offset == 0);
        std::vector<uint8_t> data;
        CHECK(read_named(reader, "a", data) && data.size() == 3 && memcmp(data.data(), "abc", 3) == 0);
        aparchive_reader_destroy(reader);
        reader = nullptr;
    }

    // The writer, asked for ZIP64 size fields on small entries.
    std::vector<uint8_t> archive;
    aparchive_writer writer = memory_writer(archive, codecs);
    std::vector<uint8_t> text = text_data();
    APARCHIVE_ENTRY_DESC entry = entry_desc("stored", APARCHIVE_METHOD_STORED);
    entry.zip64 = true;
    entry.alignment = 64;
    CHECK(aparchive_writer_add_entry(writer, &entry, "abc", 3, nullptr) == APRESULT_OK);
    entry = entry_desc("deflated", APARCHIVE_METHOD_DEFLATE);
    entry.zip64 = true;
    entry.level = 6;
    CHECK(aparchive_writer_add_entry(writer, &entry, text.data(), text.size(), nullptr) == APRESULT_OK);
    CHECK(aparchive_writer_finish(writer, nullptr) == APRESULT_OK);
    aparchive_writer_destroy(writer);
    // The first local header: version 4.5 needed, sizes deferred.
    CHECK(archive.size() > 30 && archive[4] == 45 && archive[5] == 0);
    CHECK(memcmp(archive.data() + 18, "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 8) == 0);
    desc = reader_desc(archive, codecs);
    CHECK(aparchive_reader_open(&desc, &reader, &error) == APRESULT_OK);
    if (reader)
    {
        APARCHIVE_ENTRY_INFO info = entry_info(reader, 1);
        CHECK(info.uncompressed_size == text.size() && info.compressed_size < text.size());
        uint64_t offset = 1;
        CHECK(aparchive_reader_get_data_offset(reader, 0, &offset, nullptr) == APRESULT_OK && offset % 64 == 0);
        std::vector<uint8_t> data;
        CHECK(read_named(reader, "stored", data) && data.size() == 3 && memcmp(data.data(), "abc", 3) == 0);
        CHECK(read_named(reader, "deflated", data) && data == text);
        aparchive_reader_destroy(reader);
        reader = nullptr;
    }
    aparchive_codecs_destroy(codecs);
}

// More entries than the plain end record can count: the writer adds the
// ZIP64 end records, and the reader follows them.
static void test_many_entries()
{
    current_test = "many entries";
    aparchive_codecs codecs = deflate_codecs();
    const uint32_t count = 65536;
    std::vector<uint8_t> archive;
    aparchive_writer writer = memory_writer(archive, codecs);
    char name[16];
    bool added = true;
    for (uint32_t i = 0; i < count; i++)
    {
        snprintf(name, sizeof(name), "e/%05u", i);
        APARCHIVE_ENTRY_DESC entry = entry_desc(name, APARCHIVE_METHOD_STORED);
        added = added && aparchive_writer_add_entry(writer, &entry, name, strlen(name), nullptr) == APRESULT_OK;
    }
    CHECK(added);
    CHECK(aparchive_writer_finish(writer, nullptr) == APRESULT_OK);
    aparchive_writer_destroy(writer);
    // The archive ends with the ZIP64 locator and the plain end record,
    // whose entry counts say "see ZIP64".
    CHECK(archive.size() > 42);
    if (archive.size() > 42)
    {
        const uint8_t *end = archive.data() + archive.size() - 22;
        CHECK(memcmp(end - 20, "PK\x06\x07", 4) == 0);
        CHECK(memcmp(end, "PK\x05\x06", 4) == 0);
        CHECK(end[8] == 0xFF && end[9] == 0xFF && end[10] == 0xFF && end[11] == 0xFF);
    }

    APARCHIVE_READER_DESC desc = reader_desc(archive, codecs);
    aparchive_reader reader = nullptr;
    APARCHIVE_ERROR error;
    CHECK(aparchive_reader_open(&desc, &reader, &error) == APRESULT_OK);
    if (reader)
    {
        CHECK(aparchive_reader_get_entry_count(reader) == count);
        uint32_t index = 0;
        CHECK(aparchive_reader_find(reader, "e/65535", &index) == APRESULT_OK && index == 65535);
        CHECK(aparchive_reader_find(reader, "e/65536", &index) == APRESULT_NOT_FOUND);
        std::vector<uint8_t> data;
        CHECK(read_named(reader, "e/00000", data) && data.size() == 7 && memcmp(data.data(), "e/00000", 7) == 0);
        CHECK(read_named(reader, "e/65535", data) && data.size() == 7 && memcmp(data.data(), "e/65535", 7) == 0);
        aparchive_reader_destroy(reader);
    }
    // One entry fewer fits the plain end record: no ZIP64 records.
    archive.clear();
    writer = memory_writer(archive, codecs);
    added = true;
    for (uint32_t i = 0; i < 65534; i++)
    {
        snprintf(name, sizeof(name), "e/%05u", i);
        APARCHIVE_ENTRY_DESC entry = entry_desc(name, APARCHIVE_METHOD_STORED);
        added = added && aparchive_writer_add_entry(writer, &entry, nullptr, 0, nullptr) == APRESULT_OK;
    }
    CHECK(added);
    CHECK(aparchive_writer_finish(writer, nullptr) == APRESULT_OK);
    aparchive_writer_destroy(writer);
    CHECK(archive.size() > 42 && memcmp(archive.data() + archive.size() - 42, "PK\x06\x07", 4) != 0);
    desc = reader_desc(archive, codecs);
    reader = nullptr;
    CHECK(aparchive_reader_open(&desc, &reader, nullptr) == APRESULT_OK);
    CHECK(aparchive_reader_get_entry_count(reader) == 65534);
    aparchive_reader_destroy(reader);
    aparchive_codecs_destroy(codecs);
}

int main()
{
    test_codecs();
    test_safe_names();
    test_crc32();
    test_writer_checks();
    test_reader_checks();
    test_round_trip();
    test_empty_archive();
    test_alignment();
    test_limits();
    test_corruption();
    test_foreign_archive();
    test_zip64();
    test_many_entries();
    if (failures)
    {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("All aparchive tests passed\n");
    return 0;
}
