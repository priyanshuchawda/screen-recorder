#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include "utils/logging.h"

TEST(PersistentLogWriterTest, FlushPersistsQueuedLines) {
    const auto path = std::filesystem::temp_directory_path() /
        (L"screen-recorder-log-test-" + std::to_wstring(GetCurrentProcessId()) + L".txt");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    sr::append_persistent_log_line(path.wstring(), L"event=queued_test value=ok");
    ASSERT_TRUE(sr::flush_persistent_log(std::chrono::seconds(3)));

    std::ifstream file(path, std::ios::binary);
    ASSERT_TRUE(file.good());
    const std::string contents((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
    EXPECT_NE(contents.find("event=queued_test value=ok"), std::string::npos);
    file.close();
    std::filesystem::remove(path, ec);
}
