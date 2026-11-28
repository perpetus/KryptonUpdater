#pragma once
#include "krypton/updater/release_manifest.hpp"
#include <expected>
#include <filesystem>
#include <stop_token>
#include <string>
namespace kr::updater::windows {
struct BackgroundUpdateOptions {
  bool check_only{};
  bool download_only{};
  bool apply_pending{};
  bool worker{};
  bool browser_start{};
  bool require_delta{};
  std::filesystem::path release_file;
};
[[nodiscard]] std::expected<std::string, std::string>
RunSignedUpdate(const BackgroundUpdateOptions &options);

[[nodiscard]] std::expected<SignedRelease, std::string>
FetchInstallRelease(std::stop_token stop = {});
[[nodiscard]] std::expected<void, std::string>
WriteInstallManifest(const std::filesystem::path &archive,
                     const SignedRelease &release);
// Complete a pending update already applied by standalone setup.
void ReconcileInstalledUpdateStatus(
    const std::filesystem::path &application) noexcept;
} // namespace kr::updater::windows
