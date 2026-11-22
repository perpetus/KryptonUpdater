#include "krypton/updater/updater_engine.hpp"

#include <array>
#include <fstream>
#include <iterator>
#include <tuple>

#ifdef _WIN32
#include "krypton/updater/windows/authenticode_verifier.hpp"
#include "krypton/updater/windows/chromium_install.hpp"
#include "krypton/updater/windows/http_downloader.hpp"
#include "krypton/updater/windows/install_cleanup.hpp"
#include "krypton/updater/windows/offline_payload.hpp"
#include "krypton/updater/windows/os_capability.hpp"
#include "krypton/updater/windows/payload_runner.hpp"
#include "krypton/updater/windows/prerequisites.hpp"
#include "krypton/updater/windows/release_security.hpp"
#include "krypton/updater/windows/release_trust.hpp"
#include "krypton/updater/windows/shell_registrar.hpp"
#include "krypton/updater/windows/signed_update_service.hpp"
#include "krypton/updater/windows/version_manager.hpp"
#include <windows.h>
#endif

#include <chrono>
#include <format>
#include <iostream>

namespace kr::updater {
namespace {
template <typename Callback> class OnExit {
public:
  explicit OnExit(Callback callback) : callback_(std::move(callback)) {}
  ~OnExit() noexcept { callback_(); }
  OnExit(const OnExit &) = delete;
  OnExit &operator=(const OnExit &) = delete;

private:
  Callback callback_;
};
} // namespace

UpdaterEngine::UpdaterEngine(EngineOptions options, IUpdaterObserver *observer)
    : options_(std::move(options)), state_machine_(observer) {
#ifdef _WIN32
  if (options_.install_root.empty()) {
    options_.install_root = windows::VersionManager::GetDefaultInstallRoot();
  }
#endif
}

UpdaterEngine::~UpdaterEngine() { Cancel(); }

void UpdaterEngine::SetObserver(IUpdaterObserver *observer) {
  state_machine_.SetObserver(observer);
}

UpdateState UpdaterEngine::CurrentState() const noexcept {
  return state_machine_.CurrentState();
}

void UpdaterEngine::Cancel() {
  cancelled_ = true;
  stop_source_.request_stop();
}

int UpdaterEngine::Execute() {
#ifdef _WIN32
#if !defined(KR_UPDATER_TEST_SEAMS)
  if (options_.verify_only_for_testing ||
      options_.skip_prerequisites_for_testing) {
    state_machine_.ReportError("Test-only installer options are unavailable.");
    return 1;
  }
#endif
  if (options_.preview_mode) {
    state_machine_.TransitionTo(UpdateState::kInstalling);
    return 0;
  }
  if (options_.uninstall_mode)
    return ExecuteUninstall();
  const auto compatibility =
      EvaluateBrowserCompatibility(windows::QuerySystemCapabilities());
  if (!compatibility) {
    state_machine_.ReportError(
        CompatibilityErrorMessage(compatibility.error()));
    return 1;
  }
  if (options_.force_offline || windows::OfflinePayload::HasEmbeddedPayload() ||
      !options_.custom_package_path.empty()) {
    if (!options_.custom_package_path.empty())
      return ExecuteOffline(options_.custom_package_path, {});
    if (!windows::OfflinePayload::HasEmbeddedPayload()) {
      state_machine_.ReportError("No offline package was provided.");
      return 1;
    }
    auto workspace = windows::TemporaryInstallDirectory::Create();
    if (!workspace) {
      state_machine_.ReportError(workspace.error());
      return 1;
    }
    const auto package =
        windows::OfflinePayload::ExtractToFile(workspace->path());
    if (!package) {
      state_machine_.ReportError(
          "Could not extract the embedded offline package.");
      return 1;
    }
    return ExecuteOffline(*package,
                          windows::OfflinePayload::GetEmbeddedManifest());
  }
  return ExecuteOnline();
#else
  return 0;
#endif
}

int UpdaterEngine::ExecuteOffline(const std::filesystem::path &payload_path,
                                  std::string_view embedded_manifest) {
#ifdef _WIN32
  state_machine_.TransitionTo(UpdateState::kVerifying);
  std::ifstream payload(payload_path, std::ios::binary);
  std::array<std::uint8_t, 8> header{};
  payload.read(reinterpret_cast<char *>(header.data()), header.size());
  const auto count = static_cast<std::size_t>(payload.gcount());
  payload.close();
  const auto type = windows::OfflinePayload::DetectType(
      std::span<const std::uint8_t>(header.data(), count));
  if (type == windows::PayloadType::kUnknown ||
      type == windows::PayloadType::kNone) {
    state_machine_.ReportError(
        "The offline package is missing or unsupported.");
    return 1;
  }
  if (cancelled_) {
    state_machine_.ReportError("Installation canceled.");
    return 1;
  }

  if (type == windows::PayloadType::kMsi ||
      type == windows::PayloadType::kExe) {
    std::error_code error;
    const auto absolute = std::filesystem::absolute(payload_path, error);
    auto lock = error ? windows::ScopedFileHandle{}
                      : windows::LockFileForVerification(absolute);
    const auto signature = lock
                               ? windows::AuthenticodeVerifier::Verify(absolute)
                               : windows::SignatureStatus::kVerificationFailed;
    if (signature != windows::SignatureStatus::kValidAndTrusted) {
      state_machine_.ReportSecurityAlert(SecurityAlert{
          std::format("Payload signature verification: {}",
                      windows::SignatureStatusToString(signature)),
          payload_path.string(), true});
      state_machine_.ReportError(
          "The installer payload has an untrusted signature.");
      return 1;
    }
    state_machine_.TransitionTo(UpdateState::kInstalling);
    std::uint32_t exit_code = 0;
    const bool success =
        type == windows::PayloadType::kMsi
            ? windows::PayloadRunner::RunMsi(absolute, true, &exit_code)
            : windows::PayloadRunner::RunExe(absolute, L"/SILENT", &exit_code,
                                             std::move(lock));
    if (!success) {
      state_machine_.ReportError(
          std::format("Installer execution failed with code {}", exit_code));
      return 1;
    }
    state_machine_.TransitionTo(UpdateState::kCompleted);
    return 0;
  }

  std::string sidecar;
  if (embedded_manifest.empty()) {
    auto manifest_path = payload_path;
    manifest_path += ".manifest.json";
    std::ifstream metadata(manifest_path, std::ios::binary);
    sidecar.assign(std::istreambuf_iterator<char>(metadata), {});
    embedded_manifest = sidecar;
  }
  if (!options_.verify_only_for_testing) {
    std::string signature;
    if (!options_.custom_package_path.empty()) {
      auto proof_path = payload_path;
      proof_path += ".manifest.json.sig";
      const auto proof = windows::ReadBoundedFile(proof_path, 64);
      if (proof)
        signature = *proof;
    } else {
      signature = windows::OfflinePayload::GetEmbeddedManifestSignature();
    }
    if (!windows::VerifyReleaseSignature(embedded_manifest, signature)) {
      state_machine_.ReportError(
          "The package does not have a valid Krypton release signature.");
      return 1;
    }
  }
  auto [manifest, parse_error] = ManifestVerifier::ParseJson(embedded_manifest);
  if (!manifest) {
    state_machine_.ReportError(std::format("Invalid offline manifest: {}",
                                           ManifestErrorToString(parse_error)));
    return 1;
  }
  const auto version = manifest->version;
  const auto version_text = version.ToString();
  if (version_text.find_first_not_of(
          "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.-") !=
          std::string::npos ||
      version_text.find("..") != std::string::npos) {
    state_machine_.ReportError(
        "The package version is not a safe directory name.");
    return 1;
  }
  ManifestVerifier verifier;
  const bool loaded = verifier.LoadManifest(std::move(*manifest));
  const bool chromium = loaded && verifier.HasFile("chromium-install.manifest");
  const bool complete =
      chromium
          ? verifier.HasFile("setup.exe") && verifier.HasFile("helium.7z") &&
                verifier.HasFile("prerequisites/requirements.manifest")
          : verifier.HasFile("chrome.exe") &&
                verifier.HasFile("krypton-launcher.exe") &&
                verifier.HasFile("krypton-updater.exe");
  if (!loaded || !complete) {
    state_machine_.ReportError(
        "The offline manifest is incomplete or contains duplicate paths.");
    return 1;
  }

  std::error_code ec;
  const auto root =
      std::filesystem::absolute(options_.install_root, ec).lexically_normal();
  if (ec) {
    state_machine_.ReportError(
        "The installation directory could not be resolved.");
    return 1;
  }
  const auto staging =
      windows::VersionManager::GetStagingDirectory(root, version);
  if (staging.parent_path() != root) {
    state_machine_.ReportError(
        "The staging directory is outside the installation directory.");
    return 1;
  }
  std::filesystem::create_directories(root, ec);
  if (ec) {
    state_machine_.ReportError(
        "The installation directory could not be created.");
    return 1;
  }
  if (std::filesystem::exists(staging, ec) &&
      !windows::VersionManager::RemoveOwnedDirectory(root, staging)) {
    state_machine_.ReportError(
        "An existing staging directory could not be cleared.");
    return 1;
  }
  std::filesystem::create_directories(staging, ec);
  if (ec) {
    state_machine_.ReportError("The staging directory could not be created.");
    return 1;
  }
  const OnExit clean_staging([&] {
    if (!options_.verify_only_for_testing)
      (void)windows::VersionManager::RemoveOwnedDirectory(root, staging);
  });

  state_machine_.TransitionTo(UpdateState::kExtracting);
  ArchiveExtractor extractor(&thread_pool_);
  const auto extraction = extractor.ExtractArchive(
      payload_path, staging, &verifier,
      [this](std::uint64_t bytes, std::uint64_t total, std::string_view file) {
        if (cancelled_)
          return;
        const float fraction =
            total ? static_cast<float>(bytes) / static_cast<float>(total)
                  : 0.0f;
        state_machine_.ReportProgress(
            {bytes, total, 0.0, fraction, std::format("Extracting: {}", file)});
      });
  if (extraction != ExtractionError::kNone || cancelled_) {
    (void)windows::VersionManager::RemoveOwnedDirectory(root, staging);
    state_machine_.ReportError(
        cancelled_ ? "Installation canceled."
                   : std::format("Extraction error: {}",
                                 ExtractionErrorToString(extraction)));
    return 1;
  }

  state_machine_.TransitionTo(UpdateState::kInstalling);
  if (!options_.skip_prerequisites_for_testing) {
    state_machine_.ReportProgress(
        {0, 0, 0.0, 0.0f, "Checking Windows runtime prerequisites..."});
    const auto prerequisites =
        windows::EnsureBrowserPrerequisites(staging, options_.silent);
    if (!prerequisites) {
      (void)windows::VersionManager::RemoveOwnedDirectory(root, staging);
      state_machine_.ReportError(prerequisites.error());
      return 1;
    }
    if (cancelled_) {
      (void)windows::VersionManager::RemoveOwnedDirectory(root, staging);
      state_machine_.ReportError("Installation canceled.");
      return 1;
    }
  }
  if (chromium) {
    if (options_.verify_only_for_testing) {
      state_machine_.TransitionTo(UpdateState::kCompleted);
      return 0;
    }
    const auto installed = windows::InstallChromiumBrowser(
        staging, {options_.create_desktop_shortcut,
                  options_.create_start_menu_shortcut});
    if (!installed) {
      state_machine_.ReportError(installed.error());
      (void)windows::VersionManager::RemoveOwnedDirectory(root, staging);
      return 1;
    }
    installed_executable_ = *installed;
    options_.install_root = installed_executable_.parent_path();
    // Retain one full package as the authenticated base for the next delta.
    const auto package_hash = windows::Sha256File(payload_path);
    if (package_hash) {
      const auto cache =
          options_.install_root.parent_path() / "Update/packages";
      std::filesystem::create_directories(cache, ec);
      if (!ec)
        std::filesystem::copy_file(
            payload_path, cache / (*package_hash + ".7z"),
            std::filesystem::copy_options::skip_existing, ec);
    }
    (void)windows::VersionManager::RemoveOwnedDirectory(root, staging);
    windows::CleanupInstallArtifacts(
        options_.install_root,
        package_hash ? std::string_view(*package_hash) : std::string_view{});
    state_machine_.TransitionTo(UpdateState::kCompleted);
    return 0;
  }
  for (const auto &[source, destination] :
       {std::pair{"krypton-launcher.exe", "krypton.exe"},
        std::pair{"krypton-updater.exe", "KryptonUpdater.exe"}}) {
    std::filesystem::copy_file(
        staging / source, root / destination,
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
      state_machine_.ReportError(
          std::format("Could not install {}: {}", destination, ec.message()));
      return 1;
    }
  }
  if (!windows::VersionManager::CommitStagedVersion(root, version)) {
    state_machine_.ReportError(
        "Failed to commit the verified browser version.");
    return 1;
  }
  windows::VersionManager::CleanupOldVersions(root, version);
  installed_executable_ = root / "krypton.exe";

  if (!options_.skip_shell_registration) {
    std::ignore = windows::ShellRegistrar::RegisterBrowser(root, version);
    if (options_.create_start_menu_shortcut)
      std::ignore = windows::ShellRegistrar::CreateStartMenuShortcut(
          root / "krypton.exe");
    if (options_.create_desktop_shortcut)
      std::ignore =
          windows::ShellRegistrar::CreateDesktopShortcut(root / "krypton.exe");
    std::ignore = windows::ShellRegistrar::RegisterUninstaller(root, version);
    std::ignore = windows::ShellRegistrar::RegisterUpdateTask(
        root / "KryptonUpdater.exe");
  }
  state_machine_.TransitionTo(UpdateState::kCompleted);
  return 0;
#else
  (void)payload_path;
  (void)embedded_manifest;
  return 0;
#endif
}

int UpdaterEngine::ExecuteOnline() {
#ifdef _WIN32
  if (options_.repo_owner + "/" + options_.repo_name !=
      windows::kReleaseRepository) {
    state_machine_.ReportError(
        "Web setup requires Krypton's official release repository.");
    return 1;
  }
  state_machine_.TransitionTo(UpdateState::kChecking);
  const auto release = windows::FetchInstallRelease(stop_source_.get_token());
  if (!release || cancelled_) {
    state_machine_.ReportError(cancelled_ ? "Installation canceled."
                                          : release.error());
    return 1;
  }
  if (options_.check_only) {
    const auto installed = windows::FindChromiumInstallation();
    const auto current =
        installed
            ? ParseBrowserVersion(installed->version)
            : std::expected<BrowserVersion, std::string>(BrowserVersion{});
    const auto next = ParseBrowserVersion(release->version);
    if (!current || !next) {
      state_machine_.ReportError(
          "Could not compare installed and published browser versions.");
      return 1;
    }
    const bool available = *next > *current;
    state_machine_.TransitionTo(available ? UpdateState::kUpdateAvailable
                                          : UpdateState::kUpToDate);
    return 0;
  }
  auto workspace = windows::TemporaryInstallDirectory::Create();
  if (!workspace) {
    state_machine_.ReportError(workspace.error());
    return 1;
  }
  state_machine_.TransitionTo(UpdateState::kUpdateAvailable);
  state_machine_.TransitionTo(UpdateState::kDownloading);
  const auto archive = workspace->path() / "browser.7z";
  const auto url = std::format("https://github.com/{}/releases/download/{}/{}",
                               windows::kReleaseRepository, release->tag,
                               release->full.name);
  windows::HttpDownloader http;
  const auto download = http.DownloadToFile(
      url, archive,
      [this](const windows::DownloadProgress &progress) {
        const auto fraction =
            progress.total_bytes
                ? static_cast<float>(
                      static_cast<double>(progress.bytes_received) /
                      static_cast<double>(progress.total_bytes))
                : 0.0f;
        state_machine_.ReportProgress(
            {progress.bytes_received, progress.total_bytes,
             progress.bytes_per_second, fraction, "Downloading Krypton..."});
      },
      stop_source_.get_token(), release->full.size);
  if (cancelled_ || !download ||
      download->bytes_downloaded != release->full.size ||
      download->sha256_hex != release->full.sha256) {
    state_machine_.ReportError(
        cancelled_
            ? "Installation canceled."
            : "The browser download failed its signed size or SHA-256 check.");
    return 1;
  }
  if (const auto saved = windows::WriteInstallManifest(archive, *release);
      !saved) {
    state_machine_.ReportError(saved.error());
    return 1;
  }
  // Reuse the same signed file verification, prerequisites, and native install
  // path as offline setup. The workspace cleans up on every exit.
  auto previous_package = std::move(options_.custom_package_path);
  const OnExit restore_package(
      [&] { options_.custom_package_path = std::move(previous_package); });
  options_.custom_package_path = archive;
  return ExecuteOffline(archive, release->package_manifest);
#else
  return 1;
#endif
}

int UpdaterEngine::ExecuteUninstall() {
#ifdef _WIN32
  state_machine_.TransitionTo(UpdateState::kInstalling);
  const auto result = windows::UninstallChromiumBrowser(options_.install_root);
  if (!result) {
    state_machine_.ReportError(result.error());
    return 1;
  }
  state_machine_.TransitionTo(UpdateState::kCompleted);
  return 0;
#else
  return 1;
#endif
}

} // namespace kr::updater
