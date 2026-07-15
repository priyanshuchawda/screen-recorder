#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace sr::fedora {

enum class RecoveryResult { Recovered, Missing, Collision, Failed };

inline std::filesystem::path recovered_path_for(const std::filesystem::path& partial) {
    const auto value = partial.string();
    constexpr std::string_view suffix = ".partial.mp4";
    if (!value.ends_with(suffix)) return {};
    return value.substr(0, value.size() - suffix.size()) + ".mp4";
}

inline RecoveryResult recover_partial_recording(const std::filesystem::path& partial) {
    if (!std::filesystem::exists(partial)) return RecoveryResult::Missing;
    const auto final = recovered_path_for(partial);
    if (final.empty() || std::filesystem::exists(final)) return RecoveryResult::Collision;
    std::error_code error;
    std::filesystem::rename(partial, final, error);
    if (error) return RecoveryResult::Failed;
    const auto partial_diagnostics = partial.string() + ".diagnostics.txt";
    const auto final_diagnostics = final.string() + ".diagnostics.txt";
    if (std::filesystem::exists(partial_diagnostics)) {
        std::filesystem::rename(partial_diagnostics, final_diagnostics, error);
    }
    return error ? RecoveryResult::Failed : RecoveryResult::Recovered;
}

}  // namespace sr::fedora
