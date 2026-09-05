#include "pg/lock.h"
#include <cassert>
#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>

using namespace pg;

void test_lock_compatibility() {
    std::cout << "[Step 1] Testing Lock Compatibility Matrix (S/S, S/X, X/X)..." << std::endl;
    LockManager lm;
    auto res = LockResource::for_row(CTID(0, 1));

    // 1. S/S Compatibility: Two transactions should both acquire SHARED locks
    assert(lm.acquire(res, 101, LockMode::SHARED, 0) == true);
    assert(lm.acquire(res, 102, LockMode::SHARED, 0) == true);
    assert(lm.locks_held() == 2);
    std::cout << " -> S/S concurrency: Tx 101 and Tx 102 concurrently hold SHARED lock.\n";

    // 2. S/X Conflict: Exclusive request must be rejected when SHARED locks are held
    assert(lm.acquire(res, 103, LockMode::EXCLUSIVE, 0) == false);
    std::cout << " -> S/X conflict: Tx 103 EXCLUSIVE lock request blocked.\n";

    // 3. Reentrancy: Same transaction re-requesting lock must succeed
    assert(lm.acquire(res, 101, LockMode::SHARED, 0) == true);

    // Release Tx 101 and Tx 102
    lm.release_all(101);
    lm.release_all(102);
    assert(lm.locks_held() == 0);

    // 4. X/X Conflict: One Exclusive lock blocks another Exclusive lock
    assert(lm.acquire(res, 201, LockMode::EXCLUSIVE, 0) == true);
    assert(lm.acquire(res, 202, LockMode::EXCLUSIVE, 0) == false);
    std::cout << " -> X/X conflict: Tx 201 holds EXCLUSIVE lock, Tx 202 blocked.\n";

    lm.release_all(201);
    assert(lm.locks_held() == 0);
}

void test_strict_2pl_lifecycle() {
    std::cout << "\n[Step 2] Testing Strict Two-Phase Locking (2PL) Bulk Release..." << std::endl;
    LockManager lm;

    auto r1 = LockResource::for_row(CTID(0, 1));
    auto r2 = LockResource::for_row(CTID(0, 2));
    auto r3 = LockResource::for_relation(42);

    // Growing phase: Tx 301 acquires multiple locks
    assert(lm.acquire(r1, 301, LockMode::EXCLUSIVE, 0) == true);
    assert(lm.acquire(r2, 301, LockMode::SHARED, 0) == true);
    assert(lm.acquire(r3, 301, LockMode::EXCLUSIVE, 0) == true);
    assert(lm.locks_held() == 3);

    // Shrinking phase (Commit / Rollback): release_all frees every resource atomically
    lm.release_all(301);
    assert(lm.locks_held() == 0);
    std::cout << " -> Strict 2PL: All 3 resources released simultaneously at transaction end.\n";
}

void test_fifo_wait_queue_and_unblocking() {
    std::cout << "\n[Step 3] Testing FIFO Lock Wait Queue with Worker Threads..." << std::endl;
    LockManager lm;
    auto res = LockResource::for_row(CTID(1, 5));

    // Tx 401 holds exclusive lock
    assert(lm.acquire(res, 401, LockMode::EXCLUSIVE, 0) == true);

    std::atomic<bool> tx402_acquired{false};
    std::atomic<bool> tx403_acquired{false};

    // Worker 1: Tx 402 enqueues to acquire exclusive lock (timeout 2000ms)
    std::thread t2([&]() {
        bool ok = lm.acquire(res, 402, LockMode::EXCLUSIVE, 2000);
        tx402_acquired = ok;
    });

    // Give t2 time to enqueue
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    assert(lm.waiters_count() == 1);

    // Worker 2: Tx 403 enqueues behind Tx 402 (timeout 2000ms)
    std::thread t3([&]() {
        bool ok = lm.acquire(res, 403, LockMode::EXCLUSIVE, 2000);
        tx403_acquired = ok;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    assert(lm.waiters_count() == 2);
    assert(!tx402_acquired);
    assert(!tx403_acquired);

    // Release Tx 401: Tx 402 should be granted lock next (FIFO order)
    lm.release_all(401);

    // Wait for t2 to acquire lock
    for (int i = 0; i < 50 && !tx402_acquired; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    assert(tx402_acquired);
    assert(!tx403_acquired); // Tx 403 must still be waiting!
    std::cout << " -> FIFO queue: Tx 402 promoted to holder, Tx 403 still queued.\n";

    // Release Tx 402: Tx 403 is now unblocked
    lm.release_all(402);
    t2.join();

    for (int i = 0; i < 50 && !tx403_acquired; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    assert(tx403_acquired);
    std::cout << " -> FIFO queue: Tx 403 unblocked and acquired lock.\n";

    lm.release_all(403);
    t3.join();
}

void test_deadlock_detection_two_tx() {
    std::cout << "\n[Step 4] Testing 2-Transaction Mutual Deadlock Detection..." << std::endl;
    LockManager lm;
    auto rA = LockResource::for_row(CTID(0, 10));
    auto rB = LockResource::for_row(CTID(0, 20));

    // Tx 501 acquires Resource A
    assert(lm.acquire(rA, 501, LockMode::EXCLUSIVE, 0) == true);
    // Tx 502 acquires Resource B
    assert(lm.acquire(rB, 502, LockMode::EXCLUSIVE, 0) == true);

    std::atomic<bool> t1_started{false};
    std::atomic<bool> t1_finished{false};

    // Thread for Tx 501: requests Resource B (will block waiting for Tx 502)
    std::thread t1([&]() {
        t1_started = true;
        try {
            lm.acquire(rB, 501, LockMode::EXCLUSIVE, 2000);
        } catch (...) {}
        t1_finished = true;
    });

    while (!t1_started || lm.waiters_count() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Now Tx 501 is waiting on Tx 502 (edge: 501 -> 502).
    // Tx 502 now requests Resource A!
    // Adding edge 502 -> 501 closes the cycle: 501 -> 502 -> 501!
    bool caught_deadlock = false;
    try {
        lm.acquire(rA, 502, LockMode::EXCLUSIVE, 1000);
    } catch (const DeadlockException& e) {
        caught_deadlock = true;
        std::cout << " -> Caught expected DeadlockException: " << e.what() << std::endl;
        const auto& cycle = e.cycle();
        assert(cycle.size() >= 2);
    }

    assert(caught_deadlock);

    // Since Tx 502 was aborted as victim, it releases all locks
    lm.release_all(502);

    // With Tx 502 aborted and B released, Tx 501 should be unblocked!
    t1.join();
    assert(t1_finished);

    lm.release_all(501);
    assert(lm.locks_held() == 0);
    std::cout << " -> 2-Tx deadlock resolved cleanly by victim abort.\n";
}

void test_deadlock_detection_three_tx_cycle() {
    std::cout << "\n[Step 5] Testing 3-Transaction Circular Deadlock Detection (T1->T2->T3->T1)..." << std::endl;
    LockManager lm;
    auto rA = LockResource::for_row(CTID(1, 1));
    auto rB = LockResource::for_row(CTID(1, 2));
    auto rC = LockResource::for_row(CTID(1, 3));

    // Tx 601 holds rA
    assert(lm.acquire(rA, 601, LockMode::EXCLUSIVE, 0) == true);
    // Tx 602 holds rB
    assert(lm.acquire(rB, 602, LockMode::EXCLUSIVE, 0) == true);
    // Tx 603 holds rC
    assert(lm.acquire(rC, 603, LockMode::EXCLUSIVE, 0) == true);

    // Thread 1: Tx 601 requests rB (waits on Tx 602)
    std::thread t1([&]() {
        try { lm.acquire(rB, 601, LockMode::EXCLUSIVE, 3000); } catch (...) {}
    });

    // Thread 2: Tx 602 requests rC (waits on Tx 603)
    std::thread t2([&]() {
        try { lm.acquire(rC, 602, LockMode::EXCLUSIVE, 3000); } catch (...) {}
    });

    // Wait until both Tx 601 and Tx 602 are registered in wait queue
    while (lm.waiters_count() < 2) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Now:
    // Tx 601 -> Tx 602
    // Tx 602 -> Tx 603
    // Tx 603 now requests rA (held by Tx 601)!
    // This creates circular dependency: Tx 603 -> Tx 601 -> Tx 602 -> Tx 603!
    bool caught_3way_deadlock = false;
    try {
        lm.acquire(rA, 603, LockMode::EXCLUSIVE, 1000);
    } catch (const DeadlockException& e) {
        caught_3way_deadlock = true;
        std::cout << " -> Caught 3-way circular deadlock: " << e.what() << std::endl;
    }

    assert(caught_3way_deadlock);

    // Abort victim Tx 603
    lm.release_all(603);

    // Clean up remaining threads
    lm.release_all(602);
    lm.release_all(601);

    t1.join();
    t2.join();

    assert(lm.locks_held() == 0);
    std::cout << " -> 3-way circular deadlock successfully detected and resolved.\n";
}

int main() {
    try {
        std::cout << "\n--- TESTING TWO-PHASE LOCKING (2PL) & DEADLOCK DETECTION (MILESTONE 25) ---" << std::endl;
        test_lock_compatibility();
        test_strict_2pl_lifecycle();
        test_fifo_wait_queue_and_unblocking();
        test_deadlock_detection_two_tx();
        test_deadlock_detection_three_tx_cycle();

        std::cout << "\n>>> ITEM 25 (2PL & WAIT-FOR GRAPH DEADLOCK DETECTION) TESTS PASSED! <<<\n" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "LockManager test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
