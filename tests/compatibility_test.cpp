#include "krypton/updater/compatibility.hpp"
#include "krypton/updater/windows/os_capability.hpp"

#include <array>
#include <iostream>
#include <string_view>

namespace {
using namespace kr::updater;
int failures = 0;

void Check(bool value, std::string_view message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

SystemCapabilities Supported() {
  SystemCapabilities result;
  result.windows_major = 10;
  result.windows_build = 18362;
  for (const unsigned bit :
       {0u, 1u, 9u, 12u, 19u, 20u, 23u, 25u, 26u, 27u, 28u, 29u})
    result.cpu_leaf1_ecx |= 1u << bit;
  result.cpu_leaf7_ebx = (1u << 3) | (1u << 5) | (1u << 8);
  result.cpu_extended_leaf1_ecx = 1u << 5;
  result.enabled_xstate = 6;
  return result;
}

void TestWindowsVersions() {
  auto system = Supported();
  Check(EvaluateBrowserCompatibility(system).has_value(),
        "The exact minimum Windows build and required CPU are accepted");
  system.windows_build = 18361;
  auto result = EvaluateBrowserCompatibility(system);
  Check(!result && result.error().issue == CompatibilityIssue::kWindowsVersion,
        "A Windows build immediately below the minimum is rejected");
  system.windows_major = 6;
  system.windows_build = 9600;
  Check(!EvaluateBrowserCompatibility(system), "Windows 8.1 is rejected");
  system = Supported();
  system.windows_build = 22631;
  Check(EvaluateBrowserCompatibility(system).has_value(),
        "Windows 11's actual major version 10 is accepted");
  system.windows_major = 11;
  system.windows_build = 1;
  Check(EvaluateBrowserCompatibility(system).has_value(),
        "A future Windows major version is accepted");
  system.windows_major = system.windows_build = 0;
  Check(!EvaluateBrowserCompatibility(system),
        "Failure to determine Windows version fails closed");
}

void TestCpuInstructions() {
  struct Required {
    unsigned bit;
    std::string_view name;
  };
  constexpr std::array leaf1{Required{0, "SSE3"},    Required{1, "PCLMUL"},
                             Required{9, "SSSE3"},   Required{12, "FMA"},
                             Required{19, "SSE4.1"}, Required{20, "SSE4.2"},
                             Required{23, "POPCNT"}, Required{25, "AES"},
                             Required{28, "AVX"},    Required{29, "F16C"}};
  for (const auto &instruction : leaf1) {
    auto system = Supported();
    system.cpu_leaf1_ecx &= ~(1u << instruction.bit);
    const auto result = EvaluateBrowserCompatibility(system);
    Check(!result && result.error().missing_instruction == instruction.name,
          "A missing compiler-required instruction is identified precisely");
  }
  constexpr std::array leaf7{Required{3, "BMI1"}, Required{5, "AVX2"},
                             Required{8, "BMI2"}};
  for (const auto &instruction : leaf7) {
    auto system = Supported();
    system.cpu_leaf7_ebx &= ~(1u << instruction.bit);
    const auto result = EvaluateBrowserCompatibility(system);
    Check(!result && result.error().missing_instruction == instruction.name,
          "AVX2 and both BMI instruction sets are checked separately");
  }
  auto system = Supported();
  system.cpu_extended_leaf1_ecx = 0;
  const auto result = EvaluateBrowserCompatibility(system);
  Check(!result && result.error().missing_instruction == "LZCNT",
        "Missing extended CPUID information cannot imply LZCNT support");
}

void TestOperatingSystemAvxState() {
  for (const unsigned bit : {26u, 27u}) {
    auto system = Supported();
    system.cpu_leaf1_ecx &= ~(1u << bit);
    const auto result = EvaluateBrowserCompatibility(system);
    Check(!result && result.error().issue == CompatibilityIssue::kAvxState,
          "AVX2 hardware alone is insufficient without XSAVE and OSXSAVE");
  }
  for (const std::uint64_t mask : {0u, 2u, 4u}) {
    auto system = Supported();
    system.enabled_xstate = mask;
    const auto result = EvaluateBrowserCompatibility(system);
    Check(!result && result.error().issue == CompatibilityIssue::kAvxState,
          "Windows must enable both XMM and YMM register state");
  }
  auto system = Supported();
  system.enabled_xstate = 0xE7;
  Check(EvaluateBrowserCompatibility(system).has_value(),
        "Additional OS-enabled register state does not prevent AVX2 support");
}

void TestMessages() {
  const auto message =
      CompatibilityErrorMessage({CompatibilityIssue::kCpuInstruction, "AVX2"});
  Check(message.starts_with(
            "Sorry to let you down, but Krypton can't run on your system."),
        "Unsupported systems receive the requested friendly message");
  Check(message.find("requires AVX2 support") != std::string::npos,
        "The message identifies the missing instruction");
  const auto windows =
      CompatibilityErrorMessage({CompatibilityIssue::kWindowsVersion, {}});
  Check(windows.find("1903") != std::string::npos &&
            windows.find("18362") != std::string::npos,
        "The Windows message identifies the actual minimum version and build");
}

} // namespace

int main() {
  TestWindowsVersions();
  TestCpuInstructions();
  TestOperatingSystemAvxState();
  TestMessages();
  const auto native = windows::QuerySystemCapabilities();
  std::cout << "Native compatibility probe: Windows " << native.windows_major
            << " build " << native.windows_build << ", "
            << (EvaluateBrowserCompatibility(native) ? "supported"
                                                     : "unsupported")
            << '\n';
  return failures ? 1 : 0;
}
