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

static void test_compress()
{
    current_test = "compress";
    aparchive_codecs codecs = nullptr;
    CHECK(aparchive_codecs_create(&codecs) == APRESULT_OK);
    std::vector<char> in(4096, 'a');
    APARCHIVE_ERROR error;
    // Not in the table yet.
    uint64_t size = 0;
    std::vector<char> out(8192);
    CHECK(aparchive_codecs_compress(codecs, APARCHIVE_METHOD_DEFLATE, 6, in.data(), in.size(), out.data(), out.size(),
                                    &size, &error) == APRESULT_APARCHIVE_UNKNOWN_METHOD);
    CHECK(error.message[0] != '\0');
    CHECK(aparchive_codecs_add_deflate(codecs) == APRESULT_OK);
    // STORED copies.
    CHECK(aparchive_codecs_compress_bound(codecs, APARCHIVE_METHOD_STORED, 4096) == 4096);
    CHECK(aparchive_codecs_compress(codecs, APARCHIVE_METHOD_STORED, 0, in.data(), in.size(), out.data(), out.size(),
                                    &size, &error) == APRESULT_OK);
    CHECK(size == 4096 && memcmp(out.data(), in.data(), 4096) == 0 && error.message[0] == '\0');
    // DEFLATE shrinks repetitive data, and is deterministic.
    uint64_t bound = aparchive_codecs_compress_bound(codecs, APARCHIVE_METHOD_DEFLATE, in.size());
    CHECK(bound >= in.size());
    std::vector<char> a(bound), b(bound);
    uint64_t size_a = 0, size_b = 0;
    CHECK(aparchive_codecs_compress(codecs, APARCHIVE_METHOD_DEFLATE, 6, in.data(), in.size(), a.data(), a.size(),
                                    &size_a, nullptr) == APRESULT_OK);
    CHECK(aparchive_codecs_compress(codecs, APARCHIVE_METHOD_DEFLATE, 6, in.data(), in.size(), b.data(), b.size(),
                                    &size_b, nullptr) == APRESULT_OK);
    CHECK(size_a > 0 && size_a < 100 && size_a == size_b && memcmp(a.data(), b.data(), size_a) == 0);
    // An output buffer below the bound is refused.
    CHECK(aparchive_codecs_compress(codecs, APARCHIVE_METHOD_DEFLATE, 6, in.data(), in.size(), a.data(), 10, &size_a,
                                    nullptr) == APRESULT_INVALID_ARGUMENT);
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

static bool discard_write(void *, const void *, uint64_t)
{
    return true;
}

static APARCHIVE_ENTRY_DESC entry_desc(const char *name, uint16_t method)
{
    APARCHIVE_ENTRY_DESC entry{};
    entry.struct_size = sizeof(entry);
    entry.name = name;
    entry.method = method;
    return entry;
}

static void test_writer_checks()
{
    current_test = "writer checks";
    aparchive_codecs codecs = nullptr;
    CHECK(aparchive_codecs_create(&codecs) == APRESULT_OK);
    CHECK(aparchive_codecs_add_deflate(codecs) == APRESULT_OK);
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
    // Directory entries are off unless the writer allows them.
    entry = entry_desc("dir/", APARCHIVE_METHOD_STORED);
    CHECK(aparchive_writer_add_entry(writer, &entry, nullptr, 0, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(strstr(error.message, "directory") != nullptr);
    entry = entry_desc("a", APARCHIVE_METHOD_STORED);
    entry.alignment = 48; // not a power of two
    CHECK(aparchive_writer_add_entry(writer, &entry, "x", 1, nullptr) == APRESULT_INVALID_ARGUMENT);
    entry = entry_desc("a", 93); // not in the table
    CHECK(aparchive_writer_add_entry(writer, &entry, "x", 1, nullptr) == APRESULT_APARCHIVE_UNKNOWN_METHOD);
    // Raw entries take any method, but STORED sizes must agree.
    entry = entry_desc("a", APARCHIVE_METHOD_STORED);
    CHECK(aparchive_writer_add_raw_entry(writer, &entry, "xy", 2, 3, 0, nullptr) == APRESULT_INVALID_ARGUMENT);
    // DEFLATE can't stream yet; streaming calls need an open entry.
    entry = entry_desc("big", APARCHIVE_METHOD_DEFLATE);
    CHECK(aparchive_writer_begin_entry(writer, &entry, nullptr) == APRESULT_UNSUPPORTED);
    CHECK(aparchive_writer_write_entry_data(writer, "x", 1, nullptr) == APRESULT_INVALID_ARGUMENT);
    CHECK(aparchive_writer_end_entry(writer, nullptr) == APRESULT_INVALID_ARGUMENT);
    aparchive_writer_destroy(writer);

    // With directory entries allowed: STORED and empty only.
    desc.allow_directory_entries = true;
    CHECK(aparchive_writer_create(&desc, &writer) == APRESULT_OK);
    entry = entry_desc("dir/", APARCHIVE_METHOD_DEFLATE);
    CHECK(aparchive_writer_add_entry(writer, &entry, nullptr, 0, nullptr) == APRESULT_INVALID_ARGUMENT);
    entry = entry_desc("dir/", APARCHIVE_METHOD_STORED);
    CHECK(aparchive_writer_add_entry(writer, &entry, "x", 1, nullptr) == APRESULT_INVALID_ARGUMENT);
    aparchive_writer_destroy(writer);
    aparchive_codecs_destroy(codecs);
}

static bool fail_read(void *, uint64_t, void *, uint64_t)
{
    return false;
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
    aparchive_codecs_destroy(codecs);
}

int main()
{
    test_codecs();
    test_compress();
    test_safe_names();
    test_crc32();
    test_writer_checks();
    test_reader_checks();
    if (failures)
    {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("All aparchive tests passed\n");
    return 0;
}
