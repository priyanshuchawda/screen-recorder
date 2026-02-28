// test_storage_manager.cpp — Unit tests for StorageManager (T006)

#include <gtest/gtest.h>
#include "storage/storage_manager.h"
#include <filesystem>
#include <set>

using sr::StorageManager;
namespace fs = std::filesystem;

class StorageManagerTest : public ::testing::Test {
protected:
    std::wstring temp_dir;

    void SetUp() override {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        temp_dir = std::wstring(tmp) + L"sr_test_" + std::to_wstring(GetCurrentProcessId());
        fs::create_directories(temp_dir);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }
};

TEST_F(StorageManagerTest, DefaultDirectoryResolved) {
    StorageManager mgr;
    EXPECT_FALSE(mgr.outputDirectory().empty());
}

TEST_F(StorageManagerTest, SetCustomDirectory) {
    StorageManager mgr;
    EXPECT_TRUE(mgr.setOutputDirectory(temp_dir));
    EXPECT_EQ(mgr.outputDirectory(), temp_dir);
}

TEST_F(StorageManagerTest, GenerateUniqueFilenames) {
    StorageManager mgr;
    mgr.setOutputDirectory(temp_dir);

    std::set<std::wstring> names;
    for (int i = 0; i < 3; i++) {
        auto name = mgr.generateFilename();
        EXPECT_FALSE(name.empty());
        EXPECT_TRUE(name.find(L".partial.mp4") != std::wstring::npos);
        names.insert(name);

        // Create the file so next call generates a different name
        HANDLE h = CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }

    // All 3 filenames should be unique
    EXPECT_EQ(names.size(), 3u);
}

TEST_F(StorageManagerTest, FilenameFormat) {
    StorageManager mgr;
    mgr.setOutputDirectory(temp_dir);
    auto name = mgr.generateFilename();

    // Should contain ScreenRec_
    EXPECT_TRUE(name.find(L"ScreenRec_") != std::wstring::npos);
    // Should end with .partial.mp4
    EXPECT_TRUE(name.size() > 12);
    auto ext = name.substr(name.size() - 12);
    EXPECT_EQ(ext, L".partial.mp4");
}

TEST_F(StorageManagerTest, PartialToFinal) {
    auto result = StorageManager::partialToFinal(L"C:\\test\\ScreenRec_2026.partial.mp4");
    EXPECT_EQ(result, L"C:\\test\\ScreenRec_2026.mp4");
}

TEST_F(StorageManagerTest, PartialToFinalLeavesNonSuffixUnchanged) {
    auto result = StorageManager::partialToFinal(L"C:\\test\\ScreenRec_2026.partial.mp4.bak");
    EXPECT_EQ(result, L"C:\\test\\ScreenRec_2026.partial.mp4.bak");
}

TEST_F(StorageManagerTest, PartialToFinalLeavesUnrelatedPathUnchanged) {
    auto result = StorageManager::partialToFinal(L"C:\\test\\ScreenRec_2026.mp4");
    EXPECT_EQ(result, L"C:\\test\\ScreenRec_2026.mp4");
}

TEST_F(StorageManagerTest, DiskSpaceCheck) {
    StorageManager mgr;
    uint64_t free = mgr.getFreeDiskSpace();
    EXPECT_GT(free, 0u);
}

TEST_F(StorageManagerTest, FindOrphanedFiles) {
    StorageManager mgr;
    mgr.setOutputDirectory(temp_dir);

    // Create a dummy orphan
    auto orphan = temp_dir + L"\\test.partial.mp4";
    HANDLE h = CreateFileW(orphan.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);

    auto orphans = mgr.findOrphanedFiles();
    EXPECT_GE(orphans.size(), 1u);
}
