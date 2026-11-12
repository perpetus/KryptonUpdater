#pragma once

#include <filesystem>
#include <string_view>

namespace kr::updater::windows {

class PathSanitizer {
 public:
  // Checks if target_path, when resolved relative to base_dir, remains strictly inside base_dir
  [[nodiscard]] static bool IsContainedWithin(
      const std::filesystem::path& base_dir,
      std::string_view relative_file_path) noexcept;

  // Resolves a safe canonical destination path or returns empty on traversal attempt
  [[nodiscard]] static std::filesystem::path ResolveSafePath(
      const std::filesystem::path& base_dir,
      std::string_view relative_file_path) noexcept;
};

}  // namespace kr::updater::windows
