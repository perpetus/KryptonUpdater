#include "krypton/updater/compatibility.hpp"

#include <array>
#include <format>

namespace kr::updater {
namespace {

constexpr std::uint32_t Bit(unsigned index) noexcept { return 1u << index; }

struct Instruction {
  std::uint32_t mask;
  std::string_view name;
};

// Match the compiler switches in Krypton's x64 SIMD build configuration.
constexpr std::array kLeaf1Instructions{
    Instruction{Bit(0), "SSE3"},    Instruction{Bit(9), "SSSE3"},
    Instruction{Bit(19), "SSE4.1"}, Instruction{Bit(20), "SSE4.2"},
    Instruction{Bit(23), "POPCNT"}, Instruction{Bit(1), "PCLMUL"},
    Instruction{Bit(25), "AES"},    Instruction{Bit(12), "FMA"},
    Instruction{Bit(29), "F16C"}};
constexpr std::array kLeaf7Instructions{Instruction{Bit(3), "BMI1"},
                                        Instruction{Bit(8), "BMI2"}};

} // namespace

std::expected<void, CompatibilityFailure>
EvaluateBrowserCompatibility(const SystemCapabilities &system) noexcept {
  if (system.windows_major < kMinimumWindowsMajor ||
      (system.windows_major == kMinimumWindowsMajor &&
       system.windows_build < kMinimumWindowsBuild))
    return std::unexpected(
        CompatibilityFailure{CompatibilityIssue::kWindowsVersion, {}});
  if ((system.cpu_leaf1_ecx & Bit(28)) == 0)
    return std::unexpected(
        CompatibilityFailure{CompatibilityIssue::kCpuInstruction, "AVX"});
  if ((system.cpu_leaf7_ebx & Bit(5)) == 0)
    return std::unexpected(
        CompatibilityFailure{CompatibilityIssue::kCpuInstruction, "AVX2"});
  constexpr auto kAvxStateFlags = Bit(26) | Bit(27);
  if ((system.cpu_leaf1_ecx & kAvxStateFlags) != kAvxStateFlags ||
      (system.enabled_xstate & 0x6u) != 0x6u)
    return std::unexpected(
        CompatibilityFailure{CompatibilityIssue::kAvxState, {}});
  for (const auto &instruction : kLeaf1Instructions)
    if ((system.cpu_leaf1_ecx & instruction.mask) == 0)
      return std::unexpected(CompatibilityFailure{
          CompatibilityIssue::kCpuInstruction, instruction.name});
  for (const auto &instruction : kLeaf7Instructions)
    if ((system.cpu_leaf7_ebx & instruction.mask) == 0)
      return std::unexpected(CompatibilityFailure{
          CompatibilityIssue::kCpuInstruction, instruction.name});
  if ((system.cpu_extended_leaf1_ecx & Bit(5)) == 0)
    return std::unexpected(
        CompatibilityFailure{CompatibilityIssue::kCpuInstruction, "LZCNT"});
  return {};
}

std::string CompatibilityErrorMessage(const CompatibilityFailure &failure) {
  constexpr std::string_view introduction =
      "Sorry to let you down, but Krypton can't run on your system.\n\n";
  switch (failure.issue) {
  case CompatibilityIssue::kWindowsVersion:
    return std::format(
        "{}Krypton requires Windows 10 version 1903 (build {}) or later, "
        "including Windows 11. Please update Windows to continue.",
        introduction, kMinimumWindowsBuild);
  case CompatibilityIssue::kCpuInstruction:
    return std::format(
        "{}This AVX2 build requires {} support, which this processor does "
        "not provide. Please use a supported system.",
        introduction, failure.missing_instruction);
  case CompatibilityIssue::kAvxState:
    return std::format(
        "{}Windows is not providing the AVX register support required by "
        "this AVX2 build. Your system or virtual machine must support "
        "saving both XMM and YMM registers.",
        introduction);
  }
  return std::string(introduction);
}

} // namespace kr::updater
