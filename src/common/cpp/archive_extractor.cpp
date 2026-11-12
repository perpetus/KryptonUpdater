#include "krypton/updater/archive_extractor.hpp"

#include <Poco/SevenZip/Archive.h>
#include <Poco/SevenZip/ArchiveEntry.h>

#ifdef _WIN32
#include <windows.h>

#include <bcrypt.h>
#endif

#include <algorithm>
#include <atomic>
#include <format>
#include <fstream>
#include <mutex>
#include <vector>

namespace kr::updater {
namespace {

std::string ComputeFileSha256Hex(const std::filesystem::path &file_path) {
#ifdef _WIN32
  BCRYPT_ALG_HANDLE hAlg = nullptr;
  BCRYPT_HASH_HANDLE hHash = nullptr;
  std::string hex;
  if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >=
      0) {
    DWORD cbHash = 32;
    if (BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0) >= 0) {
      std::ifstream f(file_path, std::ios::binary);
      if (f) {
        std::vector<char> buf(65536);
        while (f.read(buf.data(), buf.size()) || f.gcount() > 0) {
          BCryptHashData(hHash, reinterpret_cast<PUCHAR>(buf.data()),
                         static_cast<ULONG>(f.gcount()), 0);
        }
        std::vector<UCHAR> hash_buf(cbHash);
        if (BCryptFinishHash(hHash, hash_buf.data(), cbHash, 0) >= 0) {
          hex.reserve(cbHash * 2);
          for (UCHAR b : hash_buf) {
            hex += std::format("{:02x}", b);
          }
        }
      }
      BCryptDestroyHash(hHash);
    }
    BCryptCloseAlgorithmProvider(hAlg, 0);
  }
  return hex;
#else
  (void)file_path;
  return "";
#endif
}

} // namespace

ArchiveExtractor::ArchiveExtractor(ThreadPool *thread_pool)
    : thread_pool_(thread_pool) {}

std::pair<std::vector<ArchiveEntryInfo>, ExtractionError>
ArchiveExtractor::InspectArchive(const std::filesystem::path &archive_path) {
  if (!std::filesystem::exists(archive_path)) {
    return {{}, ExtractionError::kArchiveNotFound};
  }

  try {
    Poco::SevenZip::Archive archive(archive_path.string());
    std::vector<ArchiveEntryInfo> entries;
    entries.reserve(archive.size());

    for (const auto &entry : archive) {
      ArchiveEntryInfo info;
      info.file_path = entry.path();
      info.uncompressed_size = entry.size();
      info.is_directory = entry.isDirectory();
      entries.push_back(std::move(info));
    }

    return {std::move(entries), ExtractionError::kNone};
  } catch (...) {
    return {{}, ExtractionError::kCorruptedArchive};
  }
}

ExtractionError
ArchiveExtractor::ExtractArchive(const std::filesystem::path &archive_path,
                                 const std::filesystem::path &target_dir,
                                 const ManifestVerifier *manifest_verifier,
                                 ExtractionProgressCallback progress_cb) {
  if (!std::filesystem::exists(archive_path)) {
    return ExtractionError::kArchiveNotFound;
  }

  try {
    Poco::SevenZip::Archive archive(archive_path.string());

    // Security step 1: Validate against Zip-Slip path traversal for every entry
    for (const auto &entry : archive) {
      const auto &path = entry.path();
      const bool unsafe_directory =
          entry.isDirectory() &&
          (path.empty() || path.front() == '/' || path.front() == '\\' ||
           path.find(':') != std::string::npos ||
           path.find("..") != std::string::npos);
      if (unsafe_directory ||
          (!entry.isDirectory() && !ManifestVerifier::IsPathSafe(path))) {
        return ExtractionError::kInvalidPathTraversal;
      }
    }

    // Security step 2: If manifest_verifier is active, ensure all files match
    // manifest
    if (manifest_verifier) {
      std::vector<std::string> archive_files;
      for (const auto &entry : archive) {
        if (entry.isFile())
          archive_files.push_back(entry.path());
      }
      if (!manifest_verifier->Manifest() ||
          archive_files.size() != manifest_verifier->Manifest()->files.size() ||
          !manifest_verifier->ValidateArchiveEntries(archive_files)) {
        return ExtractionError::kManifestMismatch;
      }
      for (const auto &entry : archive) {
        if (entry.isDirectory())
          continue;
        const auto *expected = manifest_verifier->GetEntry(entry.path());
        if (!expected || expected->size_bytes != entry.size())
          return ExtractionError::kManifestMismatch;
      }
    }

    std::uint64_t total_bytes = 0;
    for (const auto &entry : archive) {
      if (entry.isFile())
        total_bytes += entry.size();
    }

    std::filesystem::create_directories(target_dir);

    // Multi-threaded parallel extraction of entries across thread pool
    if (thread_pool_ && thread_pool_->ThreadCount() > 1) {
      std::vector<std::future<void>> futures;
      std::atomic<bool> failed{false};
      std::atomic<std::uint64_t> bytes_extracted{0};
      std::mutex err_mutex;
      ExtractionError error_code = ExtractionError::kNone;

      for (const auto &entry : archive) {
        if (entry.isDirectory()) {
          std::filesystem::create_directories(target_dir / entry.path());
          continue;
        }

        futures.push_back(thread_pool_->Enqueue([&, entry]() {
          if (failed.load())
            return;
          try {
            auto out_path = target_dir / entry.path();
            std::filesystem::create_directories(out_path.parent_path());

            // Each decoder owns its stream; sharing an archive races seek
            // state.
            Poco::SevenZip::Archive reader(archive_path.string());
            const auto item = std::find_if(
                reader.begin(), reader.end(), [&](const auto &candidate) {
                  return candidate.path() == entry.path();
                });
            if (item == reader.end()) {
              std::lock_guard lock(err_mutex);
              failed = true;
              error_code = ExtractionError::kDecompressionFailed;
              return;
            }
            reader.extract(*item, target_dir.string());

            // Security step 3: Verify SHA-256 against manifest
            if (manifest_verifier) {
              std::string actual_hash = ComputeFileSha256Hex(out_path);
              if (!manifest_verifier->VerifyHash(entry.path(), actual_hash)) {
                std::lock_guard lock(err_mutex);
                failed = true;
                error_code = ExtractionError::kHashMismatch;
                return;
              }
            }

            bytes_extracted += entry.size();
            if (progress_cb) {
              progress_cb(bytes_extracted.load(), total_bytes, entry.path());
            }
          } catch (...) {
            std::lock_guard lock(err_mutex);
            failed = true;
            error_code = ExtractionError::kDecompressionFailed;
          }
        }));
      }

      for (auto &fut : futures) {
        fut.get();
      }

      if (failed.load()) {
        return error_code;
      }
    } else {
      // Sequential extraction
      std::uint64_t extracted_bytes = 0;
      for (const auto &entry : archive) {
        if (entry.isDirectory())
          continue;
        archive.extract(entry, target_dir.string());
        if (manifest_verifier &&
            !manifest_verifier->VerifyHash(
                entry.path(), ComputeFileSha256Hex(target_dir / entry.path())))
          return ExtractionError::kHashMismatch;
        extracted_bytes += entry.size();
        if (progress_cb)
          progress_cb(extracted_bytes, total_bytes, entry.path());
      }
    }

    return ExtractionError::kNone;
  } catch (...) {
    return ExtractionError::kDecompressionFailed;
  }
}

} // namespace kr::updater
