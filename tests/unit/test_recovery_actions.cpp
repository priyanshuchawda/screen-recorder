#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "app/recovery_actions.h"

namespace {

std::wstring make_temp_dir(const wchar_t* prefix) {
    wchar_t temp_path[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp_path);

    wchar_t guid_text[64]{};
    GUID guid{};
    CoCreateGuid(&guid);
    StringFromGUID2(guid, guid_text, static_cast<int>(_countof(guid_text)));

    std::wstring dir = std::wstring(temp_path) + prefix + L"_" + guid_text;
    std::filesystem::create_directories(dir);
    return dir;
}

void write_dummy_file(const std::wstring& path) {
    std::ofstream out(std::filesystem::path(path), std::ios::binary);
    out << "partial";
}

} // namespace

TEST(RecoveryActionsTest, RecoverOrphanPartialRenamesToFinalPath) {
    const std::wstring dir = make_temp_dir(L"sr_recover");
    const std::wstring partial = dir + L"\\ScreenRec.partial.mp4";
    const std::wstring final_path = dir + L"\\ScreenRec.mp4";
    write_dummy_file(partial);

    const auto result = sr::recover_orphan_partial(partial);

    EXPECT_TRUE(result.succeeded);
    EXPECT_EQ(result.error_code, 0u);
    EXPECT_EQ(result.final_path, final_path);
    EXPECT_FALSE(std::filesystem::exists(partial));
    EXPECT_TRUE(std::filesystem::exists(final_path));

    std::filesystem::remove_all(dir);
}

TEST(RecoveryActionsTest, RecoverOrphanPartialReportsMissingFileFailure) {
    const std::wstring dir = make_temp_dir(L"sr_recover_missing");
    const std::wstring partial = dir + L"\\Missing.partial.mp4";

    const auto result = sr::recover_orphan_partial(partial);

    EXPECT_FALSE(result.succeeded);
    EXPECT_NE(result.error_code, 0u);
    EXPECT_EQ(result.final_path, dir + L"\\Missing.mp4");

    std::filesystem::remove_all(dir);
}

TEST(RecoveryActionsTest, DeleteOrphanPartialRemovesFile) {
    const std::wstring dir = make_temp_dir(L"sr_delete");
    const std::wstring partial = dir + L"\\DeleteMe.partial.mp4";
    write_dummy_file(partial);

    const auto result = sr::delete_orphan_partial(partial);

    EXPECT_TRUE(result.succeeded);
    EXPECT_EQ(result.error_code, 0u);
    EXPECT_FALSE(std::filesystem::exists(partial));

    std::filesystem::remove_all(dir);
}
