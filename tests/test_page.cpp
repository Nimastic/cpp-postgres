#include "pg/page.h"
#include "pg/pager.h"
#include <iostream>
#include <vector>
#include <string>
#include <cassert>
#include <filesystem>
#include <cstring>

namespace fs = std::filesystem;

void run_page_tests() {
    std::cout << "[Test 1] Initializing blank slotted page..." << std::endl;
    pg::PageBuffer page_buf;
    pg::Page& page = *page_buf;
    assert(page.num_slots() == 0);
    assert(page.header().pd_lower == sizeof(pg::PageHeaderData));
    assert(page.header().pd_upper == pg::PAGE_SIZE);
    assert(page.header().pd_special == pg::PAGE_SIZE);
    assert(page.free_space() == pg::PAGE_SIZE - sizeof(pg::PageHeaderData) - sizeof(pg::LinePointer));
    std::cout << " -> Initialized page. Free space: " << page.free_space() << " bytes.\n";

    std::cout << "[Test 2] Inserting Tuple 1 (50-byte text payload)..." << std::endl;
    std::string payload1 = "PostgreSQL Slotted Page Architecture: Tuple Slot 1";
    pg::slot_id_t slot1 = page.insert_tuple(payload1.data(), payload1.size());
    assert(slot1 == 1);
    assert(page.num_slots() == 1);
    assert(page.header().pd_lower == sizeof(pg::PageHeaderData) + sizeof(pg::LinePointer));
    assert(page.header().pd_upper == pg::PAGE_SIZE - payload1.size());

    auto lp1_opt = page.get_line_pointer(slot1);
    assert(lp1_opt.has_value());
    assert(lp1_opt->length() == payload1.size());
    assert(lp1_opt->lp_offset == pg::PAGE_SIZE - payload1.size());
    assert(lp1_opt->flags() == pg::ItemFlags::NORMAL);
    std::cout << " -> Inserted Slot 1 at offset " << lp1_opt->lp_offset << ", length " << lp1_opt->length() << ".\n";

    std::cout << "[Test 3] Inserting Tuple 2 (100-byte binary pattern)..." << std::endl;
    std::vector<uint8_t> payload2(100, 0x5A);
    std::memcpy(payload2.data(), "Second tuple row record data", 28);
    pg::slot_id_t slot2 = page.insert_tuple(payload2.data(), payload2.size());
    assert(slot2 == 2);
    assert(page.num_slots() == 2);
    assert(page.header().pd_lower == sizeof(pg::PageHeaderData) + 2 * sizeof(pg::LinePointer));
    assert(page.header().pd_upper == pg::PAGE_SIZE - payload1.size() - payload2.size());

    auto lp2_opt = page.get_line_pointer(slot2);
    assert(lp2_opt.has_value());
    assert(lp2_opt->length() == 100);
    assert(lp2_opt->lp_offset == pg::PAGE_SIZE - payload1.size() - 100);
    std::cout << " -> Inserted Slot 2 at offset " << lp2_opt->lp_offset << ", length " << lp2_opt->length() << ".\n";

    std::cout << "[Test 4] Verifying retrieved tuple payloads..." << std::endl;
    auto read_tuple1 = page.get_tuple(slot1);
    assert(read_tuple1.has_value());
    assert(read_tuple1->size() == payload1.size());
    assert(std::memcmp(read_tuple1->data(), payload1.data(), payload1.size()) == 0);

    auto read_tuple2 = page.get_tuple(slot2);
    assert(read_tuple2.has_value());
    assert(read_tuple2->size() == payload2.size());
    assert(std::memcmp(read_tuple2->data(), payload2.data(), payload2.size()) == 0);
    std::cout << " -> Both tuple contents retrieved and matched byte-for-byte!\n";

    std::cout << "\n[Test 5] Visual Slotted Page Dump:\n" << page.dump() << std::endl;

    std::cout << "[Test 6] Inserting until page space is exhausted..." << std::endl;
    size_t inserted_count = 2;
    std::vector<uint8_t> chunk(200, 0xEE);
    while (true) {
        pg::slot_id_t s = page.insert_tuple(chunk.data(), chunk.size());
        if (s == pg::INVALID_SLOT_ID) {
            break;
        }
        inserted_count++;
    }
    std::cout << " -> Successfully filled page with " << inserted_count 
              << " tuples. Remaining free space: " << page.free_space() << " bytes.\n";
    assert(page.insert_tuple(chunk.data(), chunk.size()) == pg::INVALID_SLOT_ID);

    std::cout << "[Test 7] Verifying disk persistence of Slotted Page with Pager..." << std::endl;
    const std::string db_name = "test_slotted_page.db";
    if (fs::exists(db_name)) fs::remove(db_name);

    {
        auto pager = pg::Pager::open(db_name);
        pg::page_id_t pid = pager->allocate_page();
        pager->write_page(pid, page.data());
        pager->flush();
        pager->close();
    }

    {
        auto pager = pg::Pager::open(db_name);
        std::vector<uint8_t> disk_buffer(pg::PAGE_SIZE, 0);
        pager->read_page(0, disk_buffer.data());

        pg::Page reloaded_page(disk_buffer.data());
        assert(reloaded_page.num_slots() == inserted_count);
        auto t1 = reloaded_page.get_tuple(slot1);
        assert(t1.has_value());
        assert(std::memcmp(t1->data(), payload1.data(), payload1.size()) == 0);
        std::cout << " -> Slotted page reloaded from disk; verified " 
                  << reloaded_page.num_slots() << " slots and tuple contents intact!\n";
    }

    fs::remove(db_name);
    std::cout << "\n>>> ITEM 2 (SLOTTED PAGE) TESTS PASSED SUCCESSFULLY! <<<\n" << std::endl;
}

int main() {
    try {
        run_page_tests();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Page test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
