#include "recovery_actions.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace {

class RecoveryActionsTest : public ::testing::Test {
protected:
    std::filesystem::path root = std::filesystem::temp_directory_path() / "fedora-screen-recorder-recovery-test";

    void SetUp() override { std::filesystem::remove_all(root); std::filesystem::create_directories(root); }
    void TearDown() override { std::filesystem::remove_all(root); }
};

TEST_F(RecoveryActionsTest, RenamesPartialAndDiagnosticsTogether) {
    const auto partial = root / "capture.partial.mp4";
    std::ofstream(partial) << "partial";
    std::ofstream(partial.string() + ".diagnostics.txt") << "diagnostics";

    EXPECT_EQ(sr::fedora::recover_partial_recording(partial), sr::fedora::RecoveryResult::Recovered);
    EXPECT_TRUE(std::filesystem::exists(root / "capture.mp4"));
    EXPECT_TRUE(std::filesystem::exists(root / "capture.mp4.diagnostics.txt"));
}

TEST_F(RecoveryActionsTest, RefusesToOverwriteCompletedRecording) {
    const auto partial = root / "capture.partial.mp4";
    std::ofstream partial_file{partial};
    std::ofstream completed_file{root / "capture.mp4"};
    EXPECT_EQ(sr::fedora::recover_partial_recording(partial), sr::fedora::RecoveryResult::Collision);
    EXPECT_TRUE(std::filesystem::exists(partial));
}

}  // namespace
