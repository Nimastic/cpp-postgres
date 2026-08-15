#include "pg/pager.h"
#include <iostream>
#include <vector>
#include <cassert>
#include <filesystem>
#include <cstring>

namespace fs = std::filesystem;

void run_pager_tests() {
    const std::string test_filename = "test_items.db";

    // Clean up pre-existing test file
    if (fs::exists(test_filename)) {
        fs::remove(test_filename);
    }

    std::cout << "[Test 1] Creating new database file..." << std::endl;
    {
        auto pager = pg::Pager::open(test_filename);
        assert(pager->is_open());
        assert(pager->num_pages() == 0);
        std::cout << " -> Pager created successfully. Num pages: " << pager->num_pages() << std::endl;

        std::cout << "[Test 2] Allocating Page 0 and Page 1..." << std::endl;
        pg::page_id_t p0 = pager->allocate_page();
        pg::page_id_t p1 = pager->allocate_page();
        assert(p0 == 0);
        assert(p1 == 1);
        assert(pager->num_pages() == 2);
        std::cout << " -> Allocated page 0 and 1. Num pages: " << pager->num_pages() << std::endl;

        std::cout << "[Test 3] Writing binary patterns to Page 0 and Page 1..." << std::endl;
        std::vector<char> page0_data(pg::PAGE_SIZE, 0xAA);
        std::vector<char> page1_data(pg::PAGE_SIZE, 0xBB);

        // Put unique markers inside the pages
        std::strcpy(page0_data.data(), "POSTGRES_PAGE_0_HEADER_MARKER");
        std::strcpy(page1_data.data(), "POSTGRES_PAGE_1_HEADER_MARKER");

        pager->write_page(p0, page0_data.data());
        pager->write_page(p1, page1_data.data());

        pager->flush();
        pager->close();
    }

    std::cout << "[Test 4] Verifying file size on disk..." << std::endl;
    uintmax_t file_size = fs::file_size(test_filename);
    assert(file_size == 2 * pg::PAGE_SIZE); // Exactly 16KB
    std::cout << " -> Disk file size is exactly " << file_size << " bytes (16KB)." << std::endl;

    std::cout << "[Test 5] Reopening database file and checking persistence..." << std::endl;
    {
        auto pager = pg::Pager::open(test_filename);
        assert(pager->is_open());
        assert(pager->num_pages() == 2);

        std::vector<char> read_buf0(pg::PAGE_SIZE, 0);
        std::vector<char> read_buf1(pg::PAGE_SIZE, 0);

        pager->read_page(0, read_buf0.data());
        pager->read_page(1, read_buf1.data());

        assert(std::strcmp(read_buf0.data(), "POSTGRES_PAGE_0_HEADER_MARKER") == 0);
        assert(std::strcmp(read_buf1.data(), "POSTGRES_PAGE_1_HEADER_MARKER") == 0);
        assert(static_cast<unsigned char>(read_buf0[100]) == 0xAA);
        assert(static_cast<unsigned char>(read_buf1[100]) == 0xBB);

        std::cout << " -> Bytes read back from disk match written patterns perfectly!" << std::endl;

        std::cout << "[Test 6] Verifying out-of-bounds error handling..." << std::endl;
        bool caught_exception = false;
        try {
            pager->read_page(2, read_buf0.data());
        } catch (const std::out_of_range& e) {
            caught_exception = true;
            std::cout << " -> Out-of-bounds exception correctly caught: " << e.what() << std::endl;
        }
        assert(caught_exception);
    }

    // Clean up
    fs::remove(test_filename);
    std::cout << "\n>>> ITEM 1 (PAGERT) TESTS PASSED SUCCESSFULLY! <<<\n" << std::endl;
}

int main() {
    try {
        run_pager_tests();
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Test failed with exception: " << ex.what() << std::endl;
        return 1;
    }
}
