#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "krypton/updater/semver.hpp"

namespace kr::updater {

struct ManifestEntry {
  std::string relative_path;
  std::string sha256;
  std::uint64_t size_bytes = 0;
};

struct UpdateManifest {
  SemVer version;
  std::vector<ManifestEntry> files;
};

enum class ManifestValidationError {
  kNone,
  kInvalidJson,
  kMissingVersion,
  kInvalidVersion,
  kEmptyFileList,
  kInvalidPath,
  kDisallowedExtension,
  kDuplicatePath
};

[[nodiscard]] constexpr std::string_view ManifestErrorToString(ManifestValidationError err) noexcept {
  switch (err) {
    case ManifestValidationError::kNone: return "No error";
    case ManifestValidationError::kInvalidJson: return "Invalid JSON payload";
    case ManifestValidationError::kMissingVersion: return "Missing 'version' field in manifest";
    case ManifestValidationError::kInvalidVersion: return "Invalid SemVer format for version";
    case ManifestValidationError::kEmptyFileList: return "Manifest has no files listed";
    case ManifestValidationError::kInvalidPath: return "Path contains directory traversal or invalid characters";
    case ManifestValidationError::kDisallowedExtension: return "File extension is prohibited";
    case ManifestValidationError::kDuplicatePath: return "Duplicate file path in manifest";
  }
  return "Unknown error";
}

class ManifestVerifier {
 public:
  ManifestVerifier() = default;

  [[nodiscard]] static bool IsExtensionPermitted(std::string_view path) noexcept;
  [[nodiscard]] static bool IsPathSafe(std::string_view path) noexcept;

  [[nodiscard]] static std::pair<std::optional<UpdateManifest>, ManifestValidationError> ParseJson(
      std::string_view json_text);

  bool LoadManifest(UpdateManifest manifest);

  [[nodiscard]] const UpdateManifest* Manifest() const noexcept {
    return manifest_ ? &manifest_.value() : nullptr;
  }

  [[nodiscard]] bool HasFile(std::string_view relative_path) const noexcept;
  [[nodiscard]] const ManifestEntry* GetEntry(std::string_view relative_path) const noexcept;

  // Verifies that a file's computed sha256 matches the manifest
  [[nodiscard]] bool VerifyHash(std::string_view relative_path, std::string_view actual_sha256) const noexcept;

  // Checks that all files in the archive are in the manifest, and optionally that all manifest files are present
  [[nodiscard]] bool ValidateArchiveEntries(
      const std::vector<std::string>& files_in_archive,
      std::string* out_unexpected_file = nullptr) const;

 private:
  std::optional<UpdateManifest> manifest_;
  std::unordered_map<std::string, ManifestEntry> entries_by_path_;
};

}  // namespace kr::updater
