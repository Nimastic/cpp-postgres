#include "s3/types.h"
#include "s3/crc64.h"
#include "s3/chunk_store.h"
#include "s3/extent_index.h"
#include "s3/multipart.h"
#include <iostream>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

void test_crc64_nvme() {
    std::cout << "[S3 Test 1] Verifying CRC64-NVME calculation..." << std::endl;
    const char* str = "123456789";
    uint64_t crc = s3::crc64_nvme(str, std::strlen(str));
    assert(crc != 0);

    // Verify determinism
    uint64_t crc2 = s3::crc64_nvme(str, std::strlen(str));
    assert(crc == crc2);

    // Verify bit sensitivity
    const char* str_mod = "123456788";
    uint64_t crc_mod = s3::crc64_nvme(str_mod, std::strlen(str_mod));
    assert(crc != crc_mod);
    std::cout << "  CRC64-NVME verified successfully (checksum: 0x" << std::hex << crc << std::dec << ")." << std::endl;
}

void test_chunk_store_allocation_and_scrubbing() {
    std::cout << "[S3 Test 2] Verifying physical ChunkStore allocation and bit-rot scrubber..." << std::endl;
    std::string test_path = "test_s3_chunks.db";
    if (fs::exists(test_path)) {
        fs::remove(test_path);
    }

    const size_t slot_cap = 64 * 1024; // 64 KB slot for fast test
    auto store = s3::ChunkStore::open(test_path, slot_cap);
    assert(store != nullptr);
    assert(store->num_chunks() == 0);

    // Append chunk 0
    std::string p0 = "Amazon S3 physical chunk storage engine Block 1";
    s3::chunk_id_t c0 = store->append_chunk(p0.data(), p0.size());
    assert(c0 == 0);
    assert(store->num_chunks() == 1);

    // Append chunk 1
    std::vector<uint8_t> p1(32 * 1024, 0xAB);
    s3::chunk_id_t c1 = store->append_chunk(p1.data(), p1.size());
    assert(c1 == 1);
    assert(store->num_chunks() == 2);

    // Verify read
    char buf[1024];
    s3::ChunkHeader hdr;
    size_t rlen = store->read_chunk(0, buf, sizeof(buf), &hdr);
    assert(rlen == p0.size());
    assert(std::memcmp(buf, p0.data(), rlen) == 0);
    assert(hdr.magic == s3::S3_CHUNK_MAGIC);
    assert(hdr.payload_len == p0.size());

    // Scrub intact data
    size_t corrupted = store->scrub([](s3::chunk_id_t id, bool ok) {
        assert(ok);
        (void)id;
    });
    assert(corrupted == 0);

    // Deliberately corrupt a byte in chunk 1 payload (simulate cosmic ray / bit-rot)
    bool corrupt_ok = store->corrupt_chunk_payload(1, 100);
    assert(corrupt_ok);

    // Scrubber must detect corruption
    size_t corrupt_count = 0;
    corrupted = store->scrub([&corrupt_count](s3::chunk_id_t id, bool ok) {
        if (!ok) {
            assert(id == 1);
            corrupt_count++;
        }
    });
    assert(corrupted == 1);
    assert(corrupt_count == 1);
    assert(!store->verify_chunk(1));

    // Mark as tombstone
    assert(store->mark_tombstone(1));
    assert(store->verify_chunk(1)); // Tombstones are bypassed/treated as ok

    store->close();
    fs::remove(test_path);
    std::cout << "  Physical ChunkStore and bit-rot scrubbing verified successfully." << std::endl;
}

void test_extent_index_and_range_queries() {
    std::cout << "[S3 Test 3] Verifying ExtentIndex range resolution and object metadata..." << std::endl;
    s3::ExtentIndex index;

    s3::ObjectMetadata obj;
    obj.bucket = "backup-vault";
    obj.key = "archive/db_dump.tar";
    obj.size = 10000;
    // Split into 3 extents: [0..4000), [4000..8000), [8000..10000)
    obj.extents.push_back(s3::Extent{.chunk_id = 10, .offset_in_chunk = 0, .length = 4000});
    obj.extents.push_back(s3::Extent{.chunk_id = 11, .offset_in_chunk = 0, .length = 4000});
    obj.extents.push_back(s3::Extent{.chunk_id = 12, .offset_in_chunk = 0, .length = 2000});

    index.put_object(obj);
    assert(index.size() == 1);

    auto retrieved = index.get_object("backup-vault", "archive/db_dump.tar");
    assert(retrieved.has_value());
    assert(retrieved->size == 10000);
    assert(retrieved->extents.size() == 3);

    // Range Query: Read bytes 3500 to 8500 (total 5000 bytes) spanning all 3 chunks!
    // Extent 10: [3500..4000) -> 500 bytes (offset in chunk: 3500)
    // Extent 11: [4000..8000) -> 4000 bytes (offset in chunk: 0)
    // Extent 12: [8000..8500) -> 500 bytes (offset in chunk: 0)
    auto slices = index.resolve_range("backup-vault", "archive/db_dump.tar", 3500, 5000);
    assert(slices.size() == 3);

    assert(slices[0].chunk_id == 10);
    assert(slices[0].offset_in_chunk == 3500);
    assert(slices[0].length == 500);
    assert(slices[0].object_offset == 3500);

    assert(slices[1].chunk_id == 11);
    assert(slices[1].offset_in_chunk == 0);
    assert(slices[1].length == 4000);
    assert(slices[1].object_offset == 4000);

    assert(slices[2].chunk_id == 12);
    assert(slices[2].offset_in_chunk == 0);
    assert(slices[2].length == 500);
    assert(slices[2].object_offset == 8000);

    // Listing test
    auto list = index.list_objects("backup-vault", "archive/");
    assert(list.size() == 1);
    assert(list[0].key == "archive/db_dump.tar");

    // Soft delete test
    bool del_ok = index.delete_object("backup-vault", "archive/db_dump.tar", true);
    assert(del_ok);
    assert(!index.get_object("backup-vault", "archive/db_dump.tar").has_value());

    std::cout << "  ExtentIndex and multi-chunk range query translation verified successfully." << std::endl;
}

void test_multipart_upload_lifecycle() {
    std::cout << "[S3 Test 4] Verifying MultipartManager upload slicing, out-of-order parts, and assembly..." << std::endl;
    std::string test_path = "test_s3_mp.db";
    if (fs::exists(test_path)) {
        fs::remove(test_path);
    }

    const size_t slot_cap = 16 * 1024; // 16 KB chunk capacity
    auto store = s3::ChunkStore::open(test_path, slot_cap);
    s3::ExtentIndex index;
    s3::MultipartManager mp;

    std::string upload_id = mp.initiate_upload("photos", "raw/img001.raw");
    assert(!upload_id.empty());
    assert(mp.has_upload(upload_id));
    assert(mp.active_uploads_count() == 1);

    // Upload Part 1 (10 KB)
    std::vector<char> part1_data(10 * 1024, 'A');
    auto p1 = mp.upload_part(upload_id, 1, part1_data.data(), part1_data.size(), *store);
    assert(p1.part_number == 1);
    assert(p1.size == 10 * 1024);

    // Upload Part 3 intentionally before Part 2 (out-of-order upload)
    std::vector<char> part3_data(5 * 1024, 'C');
    auto p3 = mp.upload_part(upload_id, 3, part3_data.data(), part3_data.size(), *store);
    assert(p3.part_number == 3);

    // Upload Part 2 (20 KB - requires 2 chunks since slot_cap is 16KB)
    std::vector<char> part2_data(20 * 1024, 'B');
    auto p2 = mp.upload_part(upload_id, 2, part2_data.data(), part2_data.size(), *store);
    assert(p2.part_number == 2);
    assert(p2.extents.size() == 2); // 16KB + 4KB

    // List parts, verify sorted by part number
    auto parts = mp.list_parts(upload_id);
    assert(parts.size() == 3);
    assert(parts[0].part_number == 1);
    assert(parts[1].part_number == 2);
    assert(parts[2].part_number == 3);

    // Complete upload
    auto final_obj = mp.complete_upload(upload_id, {p1, p2, p3}, index);
    assert(final_obj.bucket == "photos");
    assert(final_obj.key == "raw/img001.raw");
    assert(final_obj.size == (10 + 20 + 5) * 1024);
    assert(final_obj.extents.size() == 4); // 1 (p1) + 2 (p2) + 1 (p3)
    assert(!mp.has_upload(upload_id));
    assert(mp.active_uploads_count() == 0);

    // Verify object now lives in ExtentIndex
    auto stored = index.get_object("photos", "raw/img001.raw");
    assert(stored.has_value());
    assert(stored->size == (35 * 1024));

    // Test Abort Flow
    std::string abort_upload_id = mp.initiate_upload("photos", "cancelled.raw");
    mp.upload_part(abort_upload_id, 1, part1_data.data(), part1_data.size(), *store);
    assert(mp.active_uploads_count() == 1);
    bool aborted = mp.abort_upload(abort_upload_id, *store);
    assert(aborted);
    assert(!mp.has_upload(abort_upload_id));
    assert(mp.active_uploads_count() == 0);

    store->close();
    fs::remove(test_path);
    std::cout << "  Multipart upload state machine and out-of-order assembly verified successfully." << std::endl;
}

int main() {
    std::cout << "=== Running S3 Physical Storage Engine (Block 1) Test Suite ===" << std::endl;
    test_crc64_nvme();
    test_chunk_store_allocation_and_scrubbing();
    test_extent_index_and_range_queries();
    test_multipart_upload_lifecycle();
    std::cout << "\n>>> ALL S3 STORAGE ENGINE TESTS PASSED! <<<" << std::endl;
    return 0;
}
