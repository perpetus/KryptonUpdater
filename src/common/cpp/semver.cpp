#include "krypton/updater/semver.hpp"

#include <charconv>
#include <format>

namespace kr::updater {

std::optional<SemVer> SemVer::Parse(std::string_view text) noexcept {
  // Strip leading 'v' or 'V' if present
  if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) {
    text.remove_prefix(1);
  }

  if (text.empty()) {
    return std::nullopt;
  }

  // Parse major
  auto dot1 = text.find('.');
  if (dot1 == std::string_view::npos) {
    return std::nullopt;
  }
  std::uint32_t major_val = 0;
  auto [ptr1, ec1] = std::from_chars(text.data(), text.data() + dot1, major_val);
  if (ec1 != std::errc{} || ptr1 != text.data() + dot1) {
    return std::nullopt;
  }

  // Parse minor
  auto rest = text.substr(dot1 + 1);
  auto dot2 = rest.find('.');
  if (dot2 == std::string_view::npos) {
    // Check if there is a '-' or '+' or just minor
    auto dash = rest.find('-');
    auto plus = rest.find('+');
    auto end_minor = std::min(dash, plus);
    std::uint32_t minor_val = 0;
    auto [ptr2, ec2] = std::from_chars(rest.data(), rest.data() + (end_minor == std::string_view::npos ? rest.size() : end_minor), minor_val);
    if (ec2 != std::errc{}) return std::nullopt;
    return SemVer{major_val, minor_val, 0, ""};
  }

  std::uint32_t minor_val = 0;
  auto [ptr2, ec2] = std::from_chars(rest.data(), rest.data() + dot2, minor_val);
  if (ec2 != std::errc{} || ptr2 != rest.data() + dot2) {
    return std::nullopt;
  }

  // Parse patch and prerelease
  auto patch_rest = rest.substr(dot2 + 1);
  auto dash = patch_rest.find('-');
  auto plus = patch_rest.find('+');
  auto patch_end = std::min(dash, plus);

  std::uint32_t patch_val = 0;
  size_t patch_len = (patch_end == std::string_view::npos) ? patch_rest.size() : patch_end;
  auto [ptr3, ec3] = std::from_chars(patch_rest.data(), patch_rest.data() + patch_len, patch_val);
  if (ec3 != std::errc{} || ptr3 != patch_rest.data() + patch_len) {
    return std::nullopt;
  }

  std::string prerelease_str;
  if (dash != std::string_view::npos) {
    size_t pre_start = dash + 1;
    size_t pre_len = (plus != std::string_view::npos && plus > dash) ? (plus - pre_start) : std::string_view::npos;
    prerelease_str = std::string(patch_rest.substr(pre_start, pre_len));
  }

  return SemVer{major_val, minor_val, patch_val, std::move(prerelease_str)};
}

std::string SemVer::ToString() const {
  if (prerelease.empty()) {
    return std::format("{}.{}.{}", major, minor, patch);
  }
  return std::format("{}.{}.{}-{}", major, minor, patch, prerelease);
}

}  // namespace kr::updater
