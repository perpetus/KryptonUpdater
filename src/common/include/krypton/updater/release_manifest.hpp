#pragma once
#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace kr::updater {
using BrowserVersion = std::array<std::uint32_t, 4>;
struct SignedAsset {
  std::string name;
  std::string sha256;
  std::uint64_t size{};
};
struct ReleaseDelta {
  SignedAsset asset;
  std::string base_version;
  std::string base_sha256;
  std::string algorithm = "msdelta";
};
struct SignedRelease {
  std::string version;
  std::uint64_t sequence{};
  std::uint64_t expires{};
  SignedAsset full;
  std::vector<ReleaseDelta> deltas;
  std::string package_manifest;
  std::string package_signature;
  std::string tag;
};
[[nodiscard]] std::expected<BrowserVersion, std::string>
ParseBrowserVersion(std::string_view text);
// Only parse bytes after the platform verifies their detached signature.
[[nodiscard]] std::expected<SignedRelease, std::string>
ParseSignedRelease(std::string_view json, std::string_view repository,
                   std::uint64_t now, bool allow_expired = false);
} // namespace kr::updater
