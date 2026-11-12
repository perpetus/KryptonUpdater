#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace kr::updater::windows {
class TemporaryInstallDirectory {
public:
  [[nodiscard]] static std::expected<TemporaryInstallDirectory, std::string>
  Create();
  ~TemporaryInstallDirectory();
  TemporaryInstallDirectory(const TemporaryInstallDirectory &) = delete;
  TemporaryInstallDirectory &
  operator=(const TemporaryInstallDirectory &) = delete;
  TemporaryInstallDirectory(TemporaryInstallDirectory &&other) noexcept;
  TemporaryInstallDirectory &operator=(TemporaryInstallDirectory &&) = delete;
  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  explicit TemporaryInstallDirectory(std::filesystem::path path);
  std::filesystem::path path_;
};

// Best effort after a confirmed install, or while holding the update worker
// lock. A pending update pins its cache. Profiles are outside this scope.
void CleanupInstallArtifacts(const std::filesystem::path &application,
                             std::string_view installed_package = {}) noexcept;
} // namespace kr::updater::windows
