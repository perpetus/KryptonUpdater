#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace kr::updater {

struct SemVer {
  std::uint32_t major = 0;
  std::uint32_t minor = 0;
  std::uint32_t patch = 0;
  std::string prerelease;

  [[nodiscard]] static std::optional<SemVer> Parse(std::string_view text) noexcept;
  [[nodiscard]] std::string ToString() const;

  [[nodiscard]] bool IsPrerelease() const noexcept {
    return !prerelease.empty();
  }

  auto operator<=>(const SemVer& other) const noexcept {
    if (auto cmp = major <=> other.major; cmp != 0) return cmp;
    if (auto cmp = minor <=> other.minor; cmp != 0) return cmp;
    if (auto cmp = patch <=> other.patch; cmp != 0) return cmp;

    // Normal version has higher precedence than any prerelease
    if (prerelease.empty() && !other.prerelease.empty()) return std::strong_ordering::greater;
    if (!prerelease.empty() && other.prerelease.empty()) return std::strong_ordering::less;

    return prerelease <=> other.prerelease;
  }

  bool operator==(const SemVer& other) const noexcept {
    return (*this <=> other) == 0;
  }
};

}  // namespace kr::updater
