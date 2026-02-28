// test_fault_tolerance.cpp — Phase 7 unit tests
// Covers: T028 (disk space polling), T029 (exclusive file lock), T030 (orphan detection)

#include <gtest/gtest.h>
#include <windows.h>
#include <filesystem>
#include <atomic>
#include <chrono>
#include <thread>
#include <string>

#include "storage/storage_manager.h"

namespace fs = std::filesystem;

// ============================================================================
// Helpers
// ============================================================================

static std::wstring make_temp_dir(const wchar_t* suffix) {
    wchar_t temp_base[MAX_PATH];
    GetTempPathW(MAX_PATH, temp_base);
    DWORD pid = GetCurrentProcessId();
    wchar_t dir[MAX_PATH];
    _snwprintf_s(dir, _countof(dir), _TRUNCATE, L"%s\\sr_ft_%u_%s", temp_base, pid, suffix);
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

static void remove_temp_dir(const std::wstring& dir) {
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ============================================================================
// T028 — Async disk space polling
// ============================================================================

// Test: callback fires when threshold is set above current free space (always-low)
TEST(DiskSpacePollingTest, CallbackFiresWhenBelowThreshold) {
    sr::StorageManager sm;

    std::atomic<int> call_count{ 0 };
    // Use UINT64_MAX as threshold — guaranteed to be "low" on any machine
    sm.startDiskSpacePolling(
        [&]() { call_count.fetch_add(1, std::memory_order_relaxed); },
        std::chrono::milliseconds(100),   // fast polling for test
        UINT64_MAX                         // always-low threshold
    );

    // Wait up to 2 seconds for at least 1 callback
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (call_count.load() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    sm.stopDiskSpacePolling();
    EXPECT_GE(call_count.load(), 1) << "Low-disk callback should have fired at least once";
}

// Test: callback does NOT fire when threshold is 0 (never-low)
TEST(DiskSpacePollingTest, CallbackDoesNotFireWhenAboveThreshold) {
    sr::StorageManager sm;

    std::atomic<int> call_count{ 0 };
    // threshold = 0 bytes — disk space will never be below 0
    sm.startDiskSpacePolling(
        [&]() { call_count.fetch_add(1, std::memory_order_relaxed); },
        std::chrono::milliseconds(100),
        0  // never fires
    );

    // Wait 500ms — callback should NOT fire
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    sm.stopDiskSpacePolling();
    EXPECT_EQ(call_count.load(), 0) << "Callback should not fire when disk is not low";
}

// Test: stopDiskSpacePolling returns cleanly; a second stop is a no-op
TEST(DiskSpacePollingTest, StopIsIdempotent) {
    sr::StorageManager sm;
    sm.startDiskSpacePolling([]() {}, std::chrono::milliseconds(500), 0);
    sm.stopDiskSpacePolling(); // first stop
    EXPECT_NO_FATAL_FAILURE(sm.stopDiskSpacePolling()); // second stop — no-op
}

// Test: polling restarts correctly after stop+start
TEST(DiskSpacePollingTest, CanRestartPolling) {
    sr::StorageManager sm;
    std::atomic<int> count1{ 0 }, count2{ 0 };

    sm.startDiskSpacePolling([&]() { count1++; },
                              std::chrono::milliseconds(100), UINT64_MAX);
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    sm.stopDiskSpacePolling();
    int first_count = count1.load();
    EXPECT_GE(first_count, 1);

    sm.startDiskSpacePolling([&]() { count2++; },
                              std::chrono::milliseconds(100), UINT64_MAX);
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    sm.stopDiskSpacePolling();
    EXPECT_GE(count2.load(), 1);
}

// ============================================================================
// T029 — Exclusive write lock on .partial.mp4
// Tests the StorageManager-level getFreeDiskSpace and verifies Windows file locking.
// The lock itself is held by MuxWriter; here we test the locking contract directly.
// ============================================================================

// Verify that a file opened with GENERIC_WRITE / FILE_SHARE_READ
// allows reads but blocks writes from other handles.
TEST(ExclusiveFileLockTest, ShareReadBlocksExternalWrite) {
    std::wstring tmp_dir = make_temp_dir(L"lock");
    std::wstring test_file = tmp_dir + L"\\test.partial.mp4";

    // Simulate MuxWriter's lock: open for write, share only read
    HANDLE lock_handle = CreateFileW(
        test_file.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,    // allow readers, deny writers
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    ASSERT_NE(lock_handle, INVALID_HANDLE_VALUE)
        << "Failed to create test file: " << GetLastError();

    // Reading from another handle should SUCCEED (FILE_SHARE_READ allows it)
    HANDLE read_handle = CreateFileW(
        test_file.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,  // we're just reading
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    // Note: read_handle might fail if MFSinkWriter isn't holding FILE_SHARE_WRITE,
    // but our own process held FILE_SHARE_READ only. Since same process opens it's 
    // implementation-defined, we test from a process-level perspective via access modes.
    // The key guarantee is that the outer world cannot write-share:
    HANDLE write_handle = CreateFileW(
        test_file.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_WRITE,   // request write sharing — should fail
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    // Because lock_handle only declared FILE_SHARE_READ, a second opener requiring
    // GENERIC_WRITE without FILE_SHARE_READ granted by us should fail:
    // (Our lock_handle holds FILE_SHARE_READ — so new openers can READ, not WRITE)
    EXPECT_EQ(write_handle, INVALID_HANDLE_VALUE)
        << "External write should be blocked by exclusive lock";

    // Cleanup
    if (read_handle != INVALID_HANDLE_VALUE) CloseHandle(read_handle);
    CloseHandle(lock_handle);
    remove_temp_dir(tmp_dir);
}

// Test: lock handle closed before rename (simulates MuxWriter::finalize)
TEST(ExclusiveFileLockTest, FileRenamableAfterLockReleased) {
    std::wstring tmp_dir  = make_temp_dir(L"rename");
    std::wstring partial  = tmp_dir + L"\\ScreenRec.partial.mp4";
    std::wstring final_p  = tmp_dir + L"\\ScreenRec.mp4";

    // Create and lock file
    HANDLE h = CreateFileW(partial.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(h, INVALID_HANDLE_VALUE);

    // Close lock
    CloseHandle(h);

    // Should rename OK now
    BOOL ok = MoveFileExW(partial.c_str(), final_p.c_str(), MOVEFILE_REPLACE_EXISTING);
    EXPECT_TRUE(ok) << "Rename should succeed after lock is released: " << GetLastError();
    EXPECT_TRUE(fs::exists(final_p));
    EXPECT_FALSE(fs::exists(partial));

    remove_temp_dir(tmp_dir);
}

// ============================================================================
// T030 — Orphan detection (findOrphanedFiles already tested in StorageManagerTest)
// Additional: verify partial -> final rename logic works correctly
// ============================================================================

TEST(OrphanDetectionTest, PartialToFinalConversion) {
    EXPECT_EQ(
        sr::StorageManager::partialToFinal(L"C:\\Recs\\ScreenRec_2026-02-28.partial.mp4"),
        L"C:\\Recs\\ScreenRec_2026-02-28.mp4");
    EXPECT_EQ(
        sr::StorageManager::partialToFinal(L"ScreenRec_001.partial.mp4"),
        L"ScreenRec_001.mp4");
}

TEST(OrphanDetectionTest, FindsOrphanedFilesInDirectory) {
    std::wstring tmp_dir = make_temp_dir(L"orphan");

    // Create some orphaned partial files and a normal mp4
    std::wstring p1     = tmp_dir + L"\\ScreenRec_2026-02-28_10-00-00.partial.mp4";
    std::wstring p2     = tmp_dir + L"\\ScreenRec_2026-02-28_11-00-00.partial.mp4";
    std::wstring normal = tmp_dir + L"\\ScreenRec_2026-02-28_09-00-00.mp4";

    for (const auto& f : { p1, p2, normal }) {
        HANDLE h = CreateFileW(f.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }

    sr::StorageManager sm;
    sm.setOutputDirectory(tmp_dir);

    auto orphans = sm.findOrphanedFiles();
    EXPECT_EQ(orphans.size(), 2u) << "Should find exactly 2 orphaned partial files";

    remove_temp_dir(tmp_dir);
}

TEST(OrphanDetectionTest, NoOrphansInCleanDirectory) {
    std::wstring tmp_dir = make_temp_dir(L"clean");

    std::wstring normal = tmp_dir + L"\\ScreenRec_2026-02-28.mp4";
    HANDLE h = CreateFileW(normal.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);

    sr::StorageManager sm;
    sm.setOutputDirectory(tmp_dir);

    auto orphans = sm.findOrphanedFiles();
    EXPECT_EQ(orphans.size(), 0u) << "No orphans should be found in a clean directory";

    remove_temp_dir(tmp_dir);
}
