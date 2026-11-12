#include "krypton/updater/updater_engine.hpp"
#include "krypton/updater/windows/version_manager.hpp"

#include <algorithm>
#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {
using namespace kr::updater;
namespace fs = std::filesystem;
int failures = 0;

bool Check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
  return condition;
}

std::string Read(const fs::path &path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), {}};
}

struct Observer final : IUpdaterObserver {
  std::vector<UpdateState> states;
  std::string error;
  void OnStateChanged(UpdateState state) override { states.push_back(state); }
  void OnProgress(const ProgressReport &) override {}
  void OnSecurityAlert(const SecurityAlert &) override {}
  void OnError(std::string_view message) override { error = message; }
};

EngineOptions Options(const fs::path &archive, const fs::path &root) {
  EngineOptions options;
  options.force_offline = true;
  options.custom_package_path = archive;
  options.install_root = root;
  options.skip_shell_registration = true;
  options.skip_prerequisites_for_testing = true;
  options.verify_only_for_testing = true;
  return options;
}

void VerifyInstall(const fs::path &archive, const fs::path &root) {
  Observer observer;
  UpdaterEngine engine(Options(archive, root), &observer);
  if (!Check(engine.Execute() == 0, observer.error))
    return;
  Check(engine.CurrentState() == UpdateState::kCompleted,
        "Offline install reaches completion");
  const std::vector<UpdateState> expected = {
      UpdateState::kVerifying, UpdateState::kExtracting,
      UpdateState::kInstalling, UpdateState::kCompleted};
  Check(observer.states == expected,
        "Offline verification/extraction/install state order");
  auto sidecar = archive;
  sidecar += ".manifest.json";
  const auto parsed = ManifestVerifier::ParseJson(Read(sidecar));
  if (!Check(parsed.first.has_value(), "Installed package has valid metadata"))
    return;
  const auto version = parsed.first->version;
  const bool chromium =
      std::ranges::any_of(parsed.first->files, [](const auto &entry) {
        return entry.relative_path == "chromium-install.manifest";
      });
  if (chromium) {
    const auto staged =
        windows::VersionManager::GetStagingDirectory(root, version);
    Check(fs::exists(staged / "setup.exe"), "Verified native setup is staged");
    Check(fs::exists(staged / "helium.7z"),
          "Verified native browser archive is staged");
    Check(!fs::exists(root / "chrome.exe"),
          "Verification never invokes native setup");
    std::cout << "Verified native Chromium package: " << version.ToString()
              << ", no machine registration changes\n";
    return;
  }
  const auto target =
      windows::VersionManager::GetVersionDirectory(root, version);
  Check(fs::exists(target / "chrome.exe"),
        "Browser is in its actual version directory");
  Check(fs::exists(root / "krypton.exe"), "Root launcher is installed");
  Check(fs::exists(root / "KryptonUpdater.exe"), "Online updater is installed");
  Check(Read(root / "krypton.exe") == Read(target / "krypton-launcher.exe"),
        "Launcher bytes match verified payload");
  Check(Read(root / "KryptonUpdater.exe") ==
            Read(target / "krypton-updater.exe"),
        "Updater bytes match verified payload");
  std::cout << "Verified offline install: " << version.ToString() << ", "
            << parsed.first->files.size() << " runtime files\n";
}

void TestRejections(const fs::path &fixture, const fs::path &workspace) {
  {
    auto options = Options({}, workspace / "missing");
    Observer observer;
    UpdaterEngine engine(options, &observer);
    Check(engine.Execute() != 0,
          "Forced offline without a package fails closed");
    Check(!fs::exists(options.install_root),
          "Missing payload does not create an install");
  }
  {
    auto options = Options(fixture, workspace / "canceled");
    UpdaterEngine engine(options);
    engine.Cancel();
    Check(engine.Execute() != 0, "Cancellation before extraction is honored");
    Check(!fs::exists(options.install_root),
          "Cancellation does not commit files");
  }
  auto sidecar = fixture;
  sidecar += ".manifest.json";
  const auto parsed = ManifestVerifier::ParseJson(Read(sidecar));
  if (!Check(parsed.first.has_value(), "Fixture manifest parses"))
    return;
  for (const std::size_t workers : {0u, 2u}) {
    auto manifest = *parsed.first;
    manifest.files.front().sha256.assign(64, '0');
    ManifestVerifier verifier;
    Check(verifier.LoadManifest(std::move(manifest)),
          "Hash rejection manifest loads");
    ThreadPool pool(workers ? workers : 1);
    ArchiveExtractor extractor(workers ? &pool : nullptr);
    const auto result = extractor.ExtractArchive(
        fixture,
        workspace / (workers ? "parallel-bad-hash" : "sequential-bad-hash"),
        &verifier);
    Check(result == ExtractionError::kHashMismatch,
          "Every decoder path checks SHA-256");
  }
  {
    auto manifest = *parsed.first;
    manifest.files.pop_back();
    ManifestVerifier verifier;
    Check(verifier.LoadManifest(std::move(manifest)),
          "Incomplete manifest loads for rejection");
    ArchiveExtractor extractor;
    Check(extractor.ExtractArchive(fixture, workspace / "unexpected-file",
                                   &verifier) ==
              ExtractionError::kManifestMismatch,
          "Unlisted archive entries are rejected");
  }
  {
    const auto archive = workspace / "missing-manifest.7z";
    fs::copy_file(fixture, archive);
    Observer observer;
    UpdaterEngine engine(
        Options(archive, workspace / "missing-manifest-install"), &observer);
    Check(engine.Execute() != 0, "An archive without a manifest is rejected");
    Check(engine.CurrentState() == UpdateState::kError,
          "Missing manifest is an installation error");
  }
  Check(!ManifestVerifier::IsPathSafe("../escape.exe"),
        "Traversal is rejected");
  Check(!ManifestVerifier::IsPathSafe("setup.ps1"),
        "Script files remain prohibited");
  Check(ManifestVerifier::IsPathSafe(
            "IwaKeyDistribution/iwa-key-distribution.pb"),
        "Browser protobuf data is permitted");
}
} // namespace

int wmain(int argc, wchar_t **argv) {
  if (argc == 3) {
    VerifyInstall(fs::path(argv[1]), fs::path(argv[2]));
    return failures ? 1 : 0;
  }
  const auto workspace =
      fs::temp_directory_path() /
      ("KryptonOfflineTest-" + std::to_string(GetCurrentProcessId()) + "-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(workspace);
  const fs::path fixture = fs::path(KR_OFFLINE_FIXTURE_DIR) / "offline.7z";
  VerifyInstall(fixture, workspace / "valid");
  TestRejections(fixture, workspace);
  // This directory is generated above, under the system temporary directory.
  if (workspace.parent_path() == fs::temp_directory_path() &&
      workspace.filename().string().starts_with("KryptonOfflineTest-")) {
    std::error_code ignored;
    fs::remove_all(workspace, ignored);
  }
  return failures ? 1 : 0;
}
