#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

namespace kr::updater::windows {

enum class PrerequisiteKind { kVisualCpp, kWindowsAppRuntime };

struct PrerequisiteRequirement {
  PrerequisiteKind kind;
  std::filesystem::path installer;
  std::array<std::uint32_t, 4> minimum_version{};
  std::uint32_t runtime_major_minor = 0;
};

// The seam lets tests exercise missing runtimes without modifying Windows.
struct PrerequisiteOperations {
  void *context = nullptr;
  bool (*is_available)(void *, const PrerequisiteRequirement &,
                       const std::filesystem::path &) = nullptr;
  std::expected<std::uint32_t, std::string> (*install)(
      void *, const PrerequisiteRequirement &, bool silent) = nullptr;
};

// Called on the installer worker after extraction and SHA-256 verification,
// before committing a browser version or offering to launch it.
[[nodiscard]] std::expected<void, std::string>
EnsureBrowserPrerequisites(const std::filesystem::path &payload, bool silent,
                           const PrerequisiteOperations *operations = nullptr);

// Read-only capability check used by packaging diagnostics.
[[nodiscard]] std::expected<void, std::string>
CheckBrowserPrerequisites(const std::filesystem::path &payload);

} // namespace kr::updater::windows
