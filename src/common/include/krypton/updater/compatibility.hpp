#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace kr::updater {

// Native XAML island hosting requires Windows 10 version 1903 or later.
inline constexpr std::uint32_t kMinimumWindowsMajor = 10;
inline constexpr std::uint32_t kMinimumWindowsBuild = 18362;

struct SystemCapabilities {
  std::uint32_t windows_major = 0;
  std::uint32_t windows_build = 0;
  std::uint32_t cpu_leaf1_ecx = 0;
  std::uint32_t cpu_leaf7_ebx = 0;
  std::uint32_t cpu_extended_leaf1_ecx = 0;
  std::uint64_t enabled_xstate = 0;
};

enum class CompatibilityIssue { kWindowsVersion, kCpuInstruction, kAvxState };

struct CompatibilityFailure {
  CompatibilityIssue issue;
  std::string_view missing_instruction;
};

[[nodiscard]] std::expected<void, CompatibilityFailure>
EvaluateBrowserCompatibility(const SystemCapabilities &system) noexcept;

[[nodiscard]] std::string
CompatibilityErrorMessage(const CompatibilityFailure &failure);

} // namespace kr::updater
