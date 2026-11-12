#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include "krypton/updater/semver.hpp"

namespace kr::updater::windows {

class VersionManager {
public:
  // Root installation path: %LOCALAPPDATA%\Programs\Krypton
  [[nodiscard]] static std::filesystem::path GetDefaultInstallRoot();

  // Versioned directory: e.g. %LOCALAPPDATA%\Programs\Krypton\app-0.2.0
  [[nodiscard]] static std::filesystem::path
  GetVersionDirectory(const std::filesystem::path &root, const SemVer &version);

  // Staging directory for ongoing extraction
  [[nodiscard]] static std::filesystem::path
  GetStagingDirectory(const std::filesystem::path &root, const SemVer &version);

  // Discovers all installed versions sorted ascending
  [[nodiscard]] static std::vector<std::pair<SemVer, std::filesystem::path>>
  GetInstalledVersions(const std::filesystem::path &root);

  // Finds newest installed version
  [[nodiscard]] static std::optional<std::pair<SemVer, std::filesystem::path>>
  GetNewestInstalledVersion(const std::filesystem::path &root);

  // Atomically moves staging directory to active version directory
  [[nodiscard]] static bool
  CommitStagedVersion(const std::filesystem::path &root, const SemVer &version);

  // Cleans up older versions if they are unlocked
  static void CleanupOldVersions(const std::filesystem::path &root,
                                 const SemVer &current_version);
  // Cleanup never traverses reparse points or removes files currently in use.
  [[nodiscard]] static bool
  IsPlainDirectory(const std::filesystem::path &path) noexcept;
  [[nodiscard]] static bool
  RemoveOwnedDirectory(const std::filesystem::path &root,
                       const std::filesystem::path &directory) noexcept;
  [[nodiscard]] static bool
  RemoveOwnedFile(const std::filesystem::path &root,
                  const std::filesystem::path &file) noexcept;
};

} // namespace kr::updater::windows
