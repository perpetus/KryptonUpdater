#pragma once

#include <expected>
#include <filesystem>
#include <string>

namespace kr::updater::windows {

[[nodiscard]] bool
IsRegisteredLegacyInstallation(const std::filesystem::path &root);
[[nodiscard]] std::expected<void, std::string>
UninstallLegacyBrowser(const std::filesystem::path &root);

// Removes only managed program files. Profile folders, unknown root files and
// reparse points are preserved. Kept separate for isolated filesystem tests.
[[nodiscard]] std::expected<void, std::string>
RemoveLegacyProgramFiles(const std::filesystem::path &root);

} // namespace kr::updater::windows
