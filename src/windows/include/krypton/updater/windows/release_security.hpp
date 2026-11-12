#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace kr::updater::windows {
[[nodiscard]] std::expected<std::string, std::string>
ReadBoundedFile(const std::filesystem::path &file, std::uint64_t limit);
[[nodiscard]] std::expected<std::string, std::string>
Sha256File(const std::filesystem::path &file);
[[nodiscard]] bool VerifyReleaseSignature(std::string_view data,
                                          std::string_view signature);
[[nodiscard]] bool VerifyReleaseFile(const std::filesystem::path &file,
                                     const std::filesystem::path &signature);
[[nodiscard]] bool FileMatches(const std::filesystem::path &file,
                               std::uint64_t size, std::string_view sha256);
[[nodiscard]] std::expected<void, std::string> ApplyReleaseDelta(
    const std::filesystem::path &base, const std::filesystem::path &patch,
    const std::filesystem::path &output, std::uint64_t expected_size,
    std::string_view algorithm = "zucchini");
} // namespace kr::updater::windows
