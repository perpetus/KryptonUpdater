#include "krypton/updater/windows/path_sanitizer.hpp"

#include <windows.h>
#include <algorithm>

namespace kr::updater::windows {

bool PathSanitizer::IsContainedWithin(
    const std::filesystem::path& base_dir,
    std::string_view relative_file_path) noexcept {
  auto resolved = ResolveSafePath(base_dir, relative_file_path);
  return !resolved.empty();
}

std::filesystem::path PathSanitizer::ResolveSafePath(
    const std::filesystem::path& base_dir,
    std::string_view relative_file_path) noexcept {
  if (relative_file_path.empty()) return {};

  // Reject paths containing colons (stream syntax or drives)
  if (relative_file_path.find(':') != std::string_view::npos) return {};

  // Reject paths starting with / or \ (absolute)
  if (relative_file_path.front() == '/' || relative_file_path.front() == '\\') return {};

  // Reject parent traversal
  if (relative_file_path.find("..") != std::string_view::npos) return {};

  std::error_code ec;
  std::filesystem::path combined = base_dir / relative_file_path;
  auto canonical_base = std::filesystem::weakly_canonical(base_dir, ec);
  auto canonical_combined = std::filesystem::weakly_canonical(combined, ec);

  if (ec) return {};

  std::wstring base_str = canonical_base.wstring();
  std::wstring comb_str = canonical_combined.wstring();

  // Normalize separators
  std::replace(base_str.begin(), base_str.end(), L'/', L'\\');
  std::replace(comb_str.begin(), comb_str.end(), L'/', L'\\');

  if (!base_str.ends_with(L'\\')) {
    base_str.push_back(L'\\');
  }

  if (comb_str.starts_with(base_str)) {
    return canonical_combined;
  }

  return {};
}

}  // namespace kr::updater::windows
