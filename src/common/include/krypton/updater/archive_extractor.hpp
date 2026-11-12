#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "krypton/updater/manifest_verifier.hpp"
#include "krypton/updater/thread_pool.hpp"

namespace kr::updater {

struct ArchiveEntryInfo {
  std::string file_path;
  std::uint64_t uncompressed_size = 0;
  bool is_directory = false;
};

enum class ExtractionError {
  kNone,
  kArchiveNotFound,
  kCorruptedArchive,
  kInvalidPathTraversal,
  kManifestMismatch,
  kHashMismatch,
  kDecompressionFailed,
  kFileWriteFailed
};

[[nodiscard]] constexpr std::string_view ExtractionErrorToString(ExtractionError err) noexcept {
  switch (err) {
    case ExtractionError::kNone: return "Success";
    case ExtractionError::kArchiveNotFound: return "7-Zip archive file not found";
    case ExtractionError::kCorruptedArchive: return "Corrupted or invalid 7-Zip archive";
    case ExtractionError::kInvalidPathTraversal: return "Path traversal (Zip-Slip) detected in 7-Zip archive";
    case ExtractionError::kManifestMismatch: return "7-Zip archive contains files not listed in manifest";
    case ExtractionError::kHashMismatch: return "Extracted file hash does not match expected manifest hash";
    case ExtractionError::kDecompressionFailed: return "7-Zip decompression error during extraction";
    case ExtractionError::kFileWriteFailed: return "Failed to write extracted file to disk";
  }
  return "Unknown extraction error";
}

using ExtractionProgressCallback = std::function<void(std::uint64_t bytes_extracted, std::uint64_t total_bytes, std::string_view current_file)>;

class ArchiveExtractor {
 public:
  explicit ArchiveExtractor(ThreadPool* thread_pool = nullptr);

  // Inspects 7-Zip archive and lists all file entries
  [[nodiscard]] static std::pair<std::vector<ArchiveEntryInfo>, ExtractionError> InspectArchive(
      const std::filesystem::path& archive_path);

  // High-performance extraction of 7-Zip archive
  [[nodiscard]] ExtractionError ExtractArchive(
      const std::filesystem::path& archive_path,
      const std::filesystem::path& target_dir,
      const ManifestVerifier* manifest_verifier = nullptr,
      ExtractionProgressCallback progress_cb = nullptr);

 private:
  ThreadPool* thread_pool_;
};

}  // namespace kr::updater
