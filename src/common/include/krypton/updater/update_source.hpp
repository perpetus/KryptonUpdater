#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include "krypton/updater/semver.hpp"

namespace kr::updater {

struct ReleaseAsset {
  std::string name;
  std::string download_url;
  std::uint64_t size_bytes = 0;
  std::string sha256;
  std::string content_type;
};

struct UpdateRelease {
  SemVer version;
  std::string tag_name;
  std::string release_name;
  std::string release_notes;
  std::vector<ReleaseAsset> assets;
  bool is_prerelease = false;
};

struct UpdateCheckOptions {
  std::string current_version = "0.0.0";
  std::string channel = "stable";
  bool include_prereleases = false;
};

enum class UpdateSourceError {
  kNetworkError,
  kInvalidResponse,
  kNoReleasesFound,
  kRateLimited,
  kHttpError
};

[[nodiscard]] constexpr std::string_view UpdateSourceErrorToString(UpdateSourceError err) noexcept {
  switch (err) {
    case UpdateSourceError::kNetworkError: return "Network connection failure";
    case UpdateSourceError::kInvalidResponse: return "Malformed API response from update endpoint";
    case UpdateSourceError::kNoReleasesFound: return "No compatible releases found";
    case UpdateSourceError::kRateLimited: return "API rate limit reached";
    case UpdateSourceError::kHttpError: return "HTTP error returned by endpoint";
  }
  return "Unknown update source error";
}

class IUpdateSource {
 public:
  virtual ~IUpdateSource() = default;

  [[nodiscard]] virtual std::expected<UpdateRelease, UpdateSourceError> CheckForUpdate(
      const UpdateCheckOptions& options) = 0;
};

}  // namespace kr::updater
