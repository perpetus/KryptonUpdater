#include "krypton/updater/windows/signed_update_service.hpp"
#include "krypton/updater/release_manifest.hpp"
#include "krypton/updater/updater_engine.hpp"
#include "krypton/updater/windows/chromium_install.hpp"
#include "krypton/updater/windows/http_downloader.hpp"
#include "krypton/updater/windows/install_cleanup.hpp"
#include "krypton/updater/windows/release_security.hpp"
#include "krypton/updater/windows/release_trust.hpp"
#include "krypton/updater/windows/version_manager.hpp"
// clang-format off
#include <windows.h>
#include <tlhelp32.h>
#include <RestartManager.h>
#include <netlistmgr.h>
#include <Poco/JSON/Object.h>
// clang-format on
#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <fstream>
#include <memory>
#include <sstream>
#include <vector>

namespace kr::updater::windows {
namespace {
namespace fs = std::filesystem;

bool IsMeteredConnection() {
  const auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(apartment) && apartment != RPC_E_CHANGED_MODE)
    return false;
  const bool metered = [] {
    INetworkCostManager *cost_manager = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(NetworkListManager), nullptr,
                                CLSCTX_ALL, IID_PPV_ARGS(&cost_manager))))
      return false;
    const auto release = [](INetworkCostManager *manager) {
      manager->Release();
    };
    const std::unique_ptr<INetworkCostManager, decltype(release)> manager(
        cost_manager, release);
    DWORD cost = NLM_CONNECTION_COST_UNKNOWN;
    return SUCCEEDED(manager->GetCost(&cost, nullptr)) &&
           cost != NLM_CONNECTION_COST_UNKNOWN &&
           (cost & NLM_CONNECTION_COST_UNRESTRICTED) == 0;
  }();
  if (SUCCEEDED(apartment))
    CoUninitialize();
  return metered;
}

class ScopedDownloadEcoQoS {
public:
  explicit ScopedDownloadEcoQoS(bool enable) noexcept {
    if (!enable)
      return;
    PROCESS_POWER_THROTTLING_STATE state{
        PROCESS_POWER_THROTTLING_CURRENT_VERSION,
        PROCESS_POWER_THROTTLING_EXECUTION_SPEED,
        PROCESS_POWER_THROTTLING_EXECUTION_SPEED};
    enabled_ =
        SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling,
                              &state, sizeof(state)) != FALSE;
  }

  ScopedDownloadEcoQoS(const ScopedDownloadEcoQoS &) = delete;
  ScopedDownloadEcoQoS &operator=(const ScopedDownloadEcoQoS &) = delete;

  ~ScopedDownloadEcoQoS() {
    if (!enabled_)
      return;
    PROCESS_POWER_THROTTLING_STATE state{
        PROCESS_POWER_THROTTLING_CURRENT_VERSION, 0, 0};
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state,
                          sizeof(state));
  }

private:
  bool enabled_ = false;
};

fs::path Suffixed(fs::path path, const wchar_t *suffix) {
  path += suffix;
  return path;
}
struct HandleCloser {
  void operator()(void *value) const {
    if (value && value != INVALID_HANDLE_VALUE)
      CloseHandle(value);
  }
};
using Handle = std::unique_ptr<void, HandleCloser>;
Handle g_relaunch_event;
std::uint64_t Now() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}
std::expected<void, std::string> Write(const fs::path &file,
                                       std::string_view value) {
  auto temporary = file;
  temporary += ".tmp";
  std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
  stream.write(value.data(), static_cast<std::streamsize>(value.size()));
  stream.close();
  if (!stream ||
      !MoveFileExW(temporary.c_str(), file.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    return std::unexpected("Could not persist verified update metadata.");
  return {};
}
void Log(const fs::path &cache, std::string_view message) {
  const auto path = cache / "update.log";
  std::error_code error;
  const auto size = fs::file_size(path, error);
  std::ofstream stream(path, std::ios::binary | ((!error && size < 1024 * 1024)
                                                     ? std::ios::app
                                                     : std::ios::trunc));
  stream << Now() << ' ' << message << '\n';
}
void Status(const fs::path &cache, std::string_view state, int progress = 0,
            std::string_view version = {}, std::string_view message = {}) {
  Poco::JSON::Object value;
  value.set("state", std::string(state));
  value.set("progress", progress);
  value.set("version", std::string(version));
  value.set("message", std::string(message));
  value.set("updated", Now());
  std::ostringstream stream;
  value.stringify(stream);
  const auto saved = Write(cache / "status.json", stream.str());
  if (!saved)
    Log(cache, saved.error());
}
void FinishRequestedRelaunch(const fs::path &cache,
                             const fs::path &application) {
  if (!g_relaunch_event ||
      WaitForSingleObject(g_relaunch_event.get(), 0) != WAIT_OBJECT_0)
    return;
  const auto executable = application / "chrome.exe";
  auto command =
      std::format(L"\"{}\" --restore-last-session", executable.wstring());
  STARTUPINFOW startup{sizeof(STARTUPINFOW)};
  PROCESS_INFORMATION child{};
  if (CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
                     FALSE, 0, nullptr, application.c_str(), &startup,
                     &child)) {
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    ResetEvent(g_relaunch_event.get());
  } else {
    Log(cache, "Could not reopen Krypton after the requested update relaunch.");
  }
}
std::string DecodeSignature(std::string_view hex) {
  std::string value;
  value.reserve(64);
  const auto nibble = [](char ch) {
    return ch <= '9' ? ch - '0' : ch - 'a' + 10;
  };
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
    value.push_back(
        static_cast<char>((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
  return value;
}
std::expected<SignedRelease, std::string> LoadRelease(const fs::path &metadata,
                                                      bool expired = false) {
  const auto envelope = ReadBoundedFile(metadata, 16 * 1024 * 1024 + 64);
  if (!envelope || envelope->size() <= 64)
    return std::unexpected("Signed release metadata is missing.");
  const std::string_view proof(envelope->data(), 64);
  const std::string_view bytes(envelope->data() + 64, envelope->size() - 64);
  if (!VerifyReleaseSignature(bytes, proof))
    return std::unexpected("Release signature verification failed.");
  return ParseSignedRelease(bytes, kReleaseRepository, Now(), expired);
}
std::expected<void, std::string>
RelaunchWorker(const fs::path &cache, const BackgroundUpdateOptions &options) {
  std::array<wchar_t, 32768> name{};
  if (!GetModuleFileNameW(nullptr, name.data(),
                          static_cast<DWORD>(name.size())))
    return std::unexpected("Could not locate the updater.");
  const fs::path current(name.data());
  auto signature = current;
  signature += ".sig";
  if (!VerifyReleaseFile(current, signature))
    return std::unexpected(
        "The updater executable's release signature is invalid.");
  const auto hash = Sha256File(current);
  if (!hash)
    return std::unexpected(hash.error());
  const auto directory = cache / "workers" / *hash;
  std::error_code error;
  fs::create_directories(directory, error);
  if (error)
    return std::unexpected("Could not create the updater worker directory.");
  const auto executable = directory / "KryptonUpdater.exe";
  auto proof = executable;
  proof += ".sig";
  if (!VerifyReleaseFile(executable, proof)) {
    fs::copy_file(current, executable, fs::copy_options::overwrite_existing,
                  error);
    if (error)
      return std::unexpected("Could not stage the updater worker.");
    fs::copy_file(signature, proof, fs::copy_options::overwrite_existing,
                  error);
    if (error || !VerifyReleaseFile(executable, proof))
      return std::unexpected("Could not verify the staged updater worker.");
  }
  auto command =
      std::format(L"\"{}\" --worker --silent{}{}{}", executable.wstring(),
                  options.check_only ? L" --check" : L"",
                  options.download_only ? L" --download-only" : L"",
                  options.apply_pending ? L" --apply-pending" : L"");
  if (options.browser_start)
    command += L" --browser-start";
  if (options.require_delta)
    command += L" --require-delta";
  if (!options.release_file.empty())
    command +=
        std::format(L" --release-file \"{}\"", options.release_file.wstring());
  STARTUPINFOW startup{sizeof(STARTUPINFOW)};
  PROCESS_INFORMATION child{};
  if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
                      FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                      nullptr, directory.c_str(), &startup, &child))
    return std::unexpected("Could not start the background update worker.");
  Handle thread(child.hThread), process(child.hProcess);
  return {};
}
std::expected<std::vector<Handle>, std::string>
BrowserProcesses(const fs::path &application) {
  std::vector<Handle> processes;
  Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
  if (snapshot.get() == INVALID_HANDLE_VALUE)
    return std::unexpected("Could not enumerate browser processes.");
  PROCESSENTRY32W entry{sizeof(PROCESSENTRY32W)};
  if (!Process32FirstW(snapshot.get(), &entry))
    return std::unexpected("Could not read browser processes.");
  auto prefix = application.lexically_normal().wstring();
  if (!prefix.ends_with(L'\\'))
    prefix += L'\\';
  do {
    if (_wcsicmp(entry.szExeFile, L"chrome.exe") != 0 &&
        _wcsicmp(entry.szExeFile, L"chrome_crashpad_handler.exe") != 0)
      continue;
    Handle process(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                               FALSE, entry.th32ProcessID));
    if (!process) {
      if (GetLastError() == ERROR_ACCESS_DENIED)
        return std::unexpected("A browser process could not be inspected. The "
                               "update remains staged.");
      continue;
    }
    std::array<wchar_t, 32768> image{};
    DWORD length = static_cast<DWORD>(image.size());
    if (!QueryFullProcessImageNameW(process.get(), 0, image.data(), &length)) {
      if (WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0)
        continue;
      return std::unexpected("A browser process path could not be inspected.");
    }
    if (length >= prefix.size() &&
        _wcsnicmp(image.data(), prefix.c_str(), prefix.size()) == 0)
      processes.push_back(std::move(process));
  } while (Process32NextW(snapshot.get(), &entry));
  if (GetLastError() != ERROR_NO_MORE_FILES)
    return std::unexpected("Browser enumeration did not finish.");
  return processes;
}
std::expected<void, std::string> WaitForBrowserExit(const fs::path &application,
                                                    const fs::path &cache) {
  for (;;) {
    auto processes = BrowserProcesses(application);
    if (!processes)
      return std::unexpected(processes.error());
    if (processes->empty())
      return {};
    Log(cache, "Update verified; waiting for browser processes to exit.");
    for (const auto &process : *processes)
      if (WaitForSingleObject(process.get(), INFINITE) != WAIT_OBJECT_0)
        return std::unexpected("Could not wait for the browser to close.");
    // Re-snapshot only after process exit events, including processes created
    // while the download or a previous process was still running.
  }
}
bool ExecutablesUnlocked(const ChromiumInstallation &installation) {
  std::vector<Handle> files;
  for (const auto &file :
       {installation.application / "chrome.exe",
        installation.application / installation.version / "chrome.dll"}) {
    Handle handle(CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE)
      return false;
    files.push_back(std::move(handle));
  }
  return true;
}
std::expected<void, std::string>
WaitForExecutableLocks(const ChromiumInstallation &installed,
                       const fs::path &cache) {
  for (;;) {
    if (const auto exited = WaitForBrowserExit(installed.application, cache);
        !exited)
      return std::unexpected(exited.error());
    if (ExecutablesUnlocked(installed))
      return {};
    DWORD session{};
    wchar_t key[CCH_RM_SESSION_KEY + 1]{};
    if (RmStartSession(&session, 0, key) != ERROR_SUCCESS)
      return std::unexpected("Could not identify executable lock owners. The "
                             "update remains staged.");
    struct Session {
      DWORD value;
      ~Session() { RmEndSession(value); }
    } owner{session};
    const auto executable = installed.application / "chrome.exe";
    const auto library =
        installed.application / installed.version / "chrome.dll";
    std::array<LPCWSTR, 2> paths{executable.c_str(), library.c_str()};
    if (RmRegisterResources(session, static_cast<UINT>(paths.size()),
                            paths.data(), 0, nullptr, 0,
                            nullptr) != ERROR_SUCCESS)
      return std::unexpected("Could not inspect browser executable locks.");
    UINT needed{}, count{};
    DWORD reboot{};
    const auto query = RmGetList(session, &needed, &count, nullptr, &reboot);
    if (query != ERROR_MORE_DATA || !needed || needed > 1024)
      return std::unexpected("Browser files remain locked. The update is "
                             "retained for the next startup.");
    std::vector<RM_PROCESS_INFO> owners(needed);
    count = needed;
    if (RmGetList(session, &needed, &count, owners.data(), &reboot) !=
        ERROR_SUCCESS)
      return std::unexpected(
          "Executable lock ownership changed. The update remains staged.");
    std::vector<Handle> processes;
    for (UINT index = 0; index < count; ++index) {
      const auto &identity = owners[index].Process;
      Handle process(
          OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                      identity.dwProcessId));
      if (!process)
        continue;
      FILETIME created{}, exited{}, kernel{}, user{};
      if (GetProcessTimes(process.get(), &created, &exited, &kernel, &user) &&
          CompareFileTime(&created, &identity.ProcessStartTime) == 0)
        processes.push_back(std::move(process));
    }
    if (processes.empty())
      return std::unexpected("The file lock owner cannot be waited on. The "
                             "update remains staged.");
    Log(cache, "Waiting for executable lock-owner process exit notifications.");
    for (const auto &process : processes)
      if (WaitForSingleObject(process.get(), INFINITE) != WAIT_OBJECT_0)
        return std::unexpected(
            "Could not wait for the executable lock owner to exit.");
  }
}
struct UpdateObserver final : IUpdaterObserver {
  std::string error;
  void OnStateChanged(UpdateState) override {}
  void OnProgress(const ProgressReport &) override {}
  void OnSecurityAlert(const SecurityAlert &) override {}
  void OnError(std::string_view message) override { error = message; }
};
std::expected<void, std::string>
Download(HttpDownloader &http, const std::string &url, const SignedAsset &asset,
         const fs::path &destination, const fs::path &cache,
         const fs::path &local_source = {}) {
  if (FileMatches(destination, asset.size, asset.sha256))
    return {};
  auto partial = destination;
  partial += ".part";
  if (!local_source.empty()) {
    if (!FileMatches(local_source, asset.size, asset.sha256))
      return std::unexpected(
          "The local asset does not match the signed release.");
    std::error_code error;
    fs::copy_file(local_source, partial, fs::copy_options::overwrite_existing,
                  error);
    if (error || !FileMatches(partial, asset.size, asset.sha256) ||
        !MoveFileExW(partial.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
      return std::unexpected("Could not stage the signed local update asset.");
    Log(cache, std::format("Staged signed local asset: {} ({} bytes).",
                           asset.name, asset.size));
    return {};
  }
  int last_percent = -1;
  const auto downloaded = http.DownloadToFile(
      url, partial,
      [&](const DownloadProgress &progress) {
        const auto percent =
            static_cast<int>(progress.bytes_received * 100 / asset.size);
        if (percent != last_percent) {
          last_percent = percent;
          Status(cache, "downloading", percent);
        }
      },
      {}, asset.size);
  if (!downloaded || downloaded->bytes_downloaded != asset.size ||
      downloaded->sha256_hex != asset.sha256) {
    std::error_code ignored;
    fs::remove(partial, ignored);
    return std::unexpected(
        "The update download failed its signed size or SHA-256 check.");
  }
  if (!MoveFileExW(partial.c_str(), destination.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    return std::unexpected("Could not commit the verified download.");
  return {};
}
std::expected<std::string, std::string>
RunWorker(const BackgroundUpdateOptions &options,
          ChromiumInstallation installed, const fs::path &cache) {
  Handle lock(CreateFileW((cache / "worker.lock").c_str(),
                          GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr));
  if (lock.get() == INVALID_HANDLE_VALUE)
    return "An update worker is already running.";
  const auto pending_path = cache / "pending.signed";
  std::error_code pending_error;
  if (fs::exists(pending_path, pending_error) && !pending_error) {
    const auto staged = LoadRelease(pending_path, true);
    const auto current = ParseBrowserVersion(installed.version);
    if (staged && current) {
      const auto next = ParseBrowserVersion(staged->version);
      if (next && *next <= *current)
        fs::remove(pending_path, pending_error);
    }
  }
  CleanupInstallArtifacts(installed.application);
  g_relaunch_event.reset(CreateEventW(nullptr, TRUE, FALSE,
                                      L"Local\\KryptonSignedUpdateRelaunch"));
  if (!g_relaunch_event)
    return std::unexpected("Could not create the update relaunch event.");
  ResetEvent(g_relaunch_event.get());
  Status(cache, "checking");
  const auto packages = cache / "packages";
  std::error_code error;
  fs::create_directories(packages, error);
  if (error)
    return std::unexpected("Could not create the update package cache.");
  HttpDownloader http;
  auto pending = cache / "pending.signed";
  if (!options.apply_pending) {
    const bool automatic_download =
        options.browser_start && options.release_file.empty();
    if (automatic_download && IsMeteredConnection()) {
      constexpr auto message =
          "Automatic update deferred on a metered connection.";
      Status(cache, "deferred", 0, {}, message);
      return message;
    }
    const ScopedDownloadEcoQoS eco_qos(automatic_download);
    const auto endpoint = std::format("https://github.com/{}/releases/latest/"
                                      "download/KryptonUpdate-win-x64.json",
                                      kReleaseRepository);
    auto bytes = std::expected<std::string, HttpError>(
        std::unexpected(HttpError::kNotFound));
    auto proof = std::expected<std::string, HttpError>(
        std::unexpected(HttpError::kNotFound));
    if (options.release_file.empty()) {
      bytes = http.GetString(endpoint);
      proof = http.GetString(endpoint + ".sig");
    } else {
      const auto metadata =
          ReadBoundedFile(options.release_file, 16 * 1024 * 1024);
      const auto signature =
          ReadBoundedFile(Suffixed(options.release_file, L".sig"), 64);
      if (!metadata || !signature)
        return std::unexpected(
            "The signed local release metadata is missing or oversized.");
      bytes = *metadata;
      proof = *signature;
    }
    if (!bytes || !proof) {
      Log(cache, std::format("Release metadata fetch: {}; signature fetch: {}",
                             bytes ? "OK" : HttpErrorToString(bytes.error()),
                             proof ? "OK" : HttpErrorToString(proof.error())));
      if (!options.check_only && fs::exists(pending)) {
        Log(cache, "GitHub is unavailable; resuming the previously signed "
                   "pending update.");
        const auto envelope = ReadBoundedFile(pending, 16 * 1024 * 1024 + 64);
        if (!envelope || envelope->size() <= 64)
          return std::unexpected("The pending update metadata is invalid.");
        bytes = envelope->substr(64);
        proof = envelope->substr(0, 64);
      } else if (!bytes && !proof && bytes.error() == HttpError::kNotFound &&
                 proof.error() == HttpError::kNotFound) {
        const auto repository_api =
            std::format("https://api.github.com/repos/{}", kReleaseRepository);
        const auto latest = http.GetString(repository_api + "/releases/latest");
        if (!latest)
          Log(cache, std::format("Latest stable release lookup: {}",
                                 HttpErrorToString(latest.error())));
        // A missing stable release is normal for a prerelease-only repository.
        // Confirm the repository exists; missing assets in an existing stable
        // release, authorization failures, and network failures remain errors.
        if (!latest && latest.error() == HttpError::kNotFound &&
            http.GetString(repository_api).has_value()) {
          Status(cache, "up_to_date", 100, installed.version);
          return "No stable Krypton update is available. Testing prereleases "
                 "can be installed manually.";
        }
        return std::unexpected("Could not fetch signed GitHub release "
                               "metadata. No changes were made.");
      } else {
        return std::unexpected("Could not fetch signed GitHub release "
                               "metadata. No changes were made.");
      }
    }
    if (!VerifyReleaseSignature(*bytes, *proof))
      return std::unexpected("GitHub release signature verification failed.");
    const auto release = ParseSignedRelease(*bytes, kReleaseRepository, Now());
    if (!release)
      return std::unexpected(release.error());
    const auto current = ParseBrowserVersion(installed.version);
    const auto next = ParseBrowserVersion(release->version);
    if (!current || !next)
      return std::unexpected(
          "Could not compare installed and published browser versions.");
    const auto accepted = cache / "accepted.signed";
    if (fs::exists(accepted)) {
      const auto highest = LoadRelease(accepted, true);
      const auto previous = ReadBoundedFile(accepted, 16 * 1024 * 1024 + 64);
      if (!highest || !previous)
        return std::unexpected(
            "The stored release checkpoint could not be verified.");
      const auto high_version = ParseBrowserVersion(highest->version);
      if (!high_version || release->sequence < highest->sequence ||
          *next < *high_version ||
          (release->sequence == highest->sequence &&
           *bytes != std::string_view(*previous).substr(64)))
        return std::unexpected(
            "Release rollback or conflicting metadata was rejected.");
    }
    if (*next <= *current) {
      Status(cache, "up_to_date", 100, installed.version);
      return "Krypton is up to date; no downgrade was attempted.";
    }
    if (options.check_only) {
      Status(cache, "available", 0, release->version);
      return "A verified Krypton update is available: " + release->version;
    }
    const auto archive = packages / (release->full.sha256 + ".7z");
    const auto asset_url = [&](const SignedAsset &asset) {
      return std::format("https://github.com/{}/releases/download/{}/{}",
                         kReleaseRepository, release->tag, asset.name);
    };
    const auto local_asset = [&](const SignedAsset &asset) {
      return options.release_file.empty()
                 ? fs::path{}
                 : options.release_file.parent_path() / asset.name;
    };
    bool ready = !options.require_delta &&
                 FileMatches(archive, release->full.size, release->full.sha256);
    if (!ready) {
      for (const auto &delta : release->deltas) {
        if (delta.base_version != installed.version)
          continue;
        const auto base = packages / (delta.base_sha256 + ".7z");
        const auto base_hash = Sha256File(base);
        if (!base_hash || *base_hash != delta.base_sha256)
          continue;
        const auto patch = packages / (delta.asset.sha256 + ".delta");
        const auto downloaded =
            Download(http, asset_url(delta.asset), delta.asset, patch, cache,
                     local_asset(delta.asset));
        if (!downloaded) {
          Log(cache, downloaded.error());
          continue;
        }
        auto reconstructed = archive;
        reconstructed += ".part";
        const auto applied = ApplyReleaseDelta(
            base, patch, reconstructed, release->full.size, delta.algorithm);
        if (applied &&
            FileMatches(reconstructed, release->full.size,
                        release->full.sha256) &&
            MoveFileExW(reconstructed.c_str(), archive.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
          ready = true;
          Log(cache, std::format("Verified {} delta reconstruction completed: "
                                 "{} bytes; full package {} bytes.",
                                 delta.algorithm, delta.asset.size,
                                 release->full.size));
          break;
        }
        fs::remove(reconstructed, error);
        Log(cache, "Delta could not be used; falling back to the full signed "
                   "package.");
      }
    }
    if (!ready) {
      if (options.require_delta)
        return std::unexpected("No matching signed delta could be applied; "
                               "full-package fallback is disabled.");
      const auto downloaded =
          Download(http, asset_url(release->full), release->full, archive,
                   cache, local_asset(release->full));
      if (!downloaded)
        return std::unexpected(downloaded.error());
      Log(cache, "Full signed update package downloaded and verified.");
    }
    // Publish checkpoint and pending files only after authenticating the entire
    // package. Interrupted writes fail closed on the next startup.
    const auto envelope = *proof + *bytes;
    for (const auto &path : {accepted, pending}) {
      if (const auto saved = Write(path, envelope); !saved)
        return std::unexpected(saved.error());
    }
  }
  auto release = LoadRelease(pending);
  if (!release)
    return std::unexpected(release.error());
  if (options.download_only) {
    // Standalone setup can finish while the worker is downloading a release.
    const auto latest_install = FindChromiumInstallation();
    if (!latest_install || latest_install->application != installed.application)
      return std::unexpected(
          "The installation changed; the update remains staged.");
    const auto latest_version = ParseBrowserVersion(latest_install->version);
    const auto staged_version = ParseBrowserVersion(release->version);
    if (latest_version && staged_version &&
        *staged_version <= *latest_version &&
        !fs::exists(latest_install->application / "new_chrome.exe", error) &&
        !error) {
      fs::remove(pending, error);
      Status(cache, "up_to_date", 100, latest_install->version);
      return "The staged update is already installed.";
    }
  }
  Status(cache, "ready", 100, release->version,
         "The verified update will install when Krypton closes.");
  if (options.download_only)
    return "The signed update is downloaded and staged: " + release->version;
  if (const auto waited = WaitForExecutableLocks(installed, cache); !waited)
    return std::unexpected(waited.error());
  // Registration may have changed while waiting for a different installer.
  const auto current_install = FindChromiumInstallation();
  if (!current_install || current_install->application != installed.application)
    return std::unexpected(
        "The installation changed; the update remains staged.");
  const auto current = ParseBrowserVersion(current_install->version);
  const auto next = ParseBrowserVersion(release->version);
  if (!current || !next)
    return std::unexpected(
        "Could not compare the staged and installed versions.");
  if (*next <= *current) {
    fs::remove(pending, error);
    CleanupInstallArtifacts(current_install->application);
    Status(cache, "up_to_date", 100, current_install->version);
    FinishRequestedRelaunch(cache, current_install->application);
    return "The staged update is no longer newer than the installed browser.";
  }
  // Revalidate freshness and identity after waiting for the browser to close.
  const auto refreshed = LoadRelease(pending);
  if (!refreshed)
    return std::unexpected(refreshed.error());
  if (refreshed->version != release->version ||
      refreshed->sequence != release->sequence ||
      refreshed->full.sha256 != release->full.sha256)
    return std::unexpected(
        "The pending release changed while waiting to apply it.");
  release = refreshed;
  if (!ExecutablesUnlocked(*current_install))
    return std::unexpected("Browser files are still locked. The update remains "
                           "staged for the next startup.");
  const auto archive = packages / (release->full.sha256 + ".7z");
  if (!FileMatches(archive, release->full.size, release->full.sha256))
    return std::unexpected(
        "The staged update no longer matches its signed hash.");
  if (const auto saved = WriteInstallManifest(archive, *release); !saved)
    return std::unexpected(saved.error());
  EngineOptions engine_options;
  engine_options.custom_package_path = archive;
  engine_options.install_root = current_install->application;
  engine_options.silent = true;
  UpdateObserver observer;
  UpdaterEngine engine(engine_options, &observer);
  Status(cache, "installing", 100, release->version);
  Log(cache, "Browser processes exited and executable locks cleared; applying "
             "verified update.");
  if (engine.Execute() != 0) {
    FinishRequestedRelaunch(cache, current_install->application);
    return std::unexpected("Update installation failed: " + observer.error);
  }
  const auto verified = FindChromiumInstallation();
  if (!verified || verified->version != release->version)
    return std::unexpected("Installed version verification failed.");
  fs::remove(pending, error);
  CleanupInstallArtifacts(verified->application, release->full.sha256);
  Status(cache, "updated", 100, release->version);
  FinishRequestedRelaunch(cache, current_install->application);
  return "Krypton was updated to " + release->version + ".";
}
} // namespace
std::expected<SignedRelease, std::string>
FetchInstallRelease(std::stop_token stop) {
  HttpDownloader http;
  const auto endpoint = std::format("https://github.com/{}/releases/latest/"
                                    "download/KryptonUpdate-win-x64.json",
                                    kReleaseRepository);
  const auto bytes = http.GetString(endpoint, stop);
  if (!bytes)
    return std::unexpected(
        std::format("Could not fetch the stable release: {}.",
                    HttpErrorToString(bytes.error())));
  const auto proof = http.GetString(endpoint + ".sig", stop);
  if (!proof || !VerifyReleaseSignature(*bytes, *proof))
    return std::unexpected(
        "The stable release signature could not be verified.");
  auto release = ParseSignedRelease(*bytes, kReleaseRepository, Now());
  if (!release)
    return std::unexpected(release.error());
  if (const auto installed = FindChromiumInstallation()) {
    const auto checkpoint =
        installed->application.parent_path() / "Update/accepted.signed";
    std::error_code error;
    if (fs::exists(checkpoint, error)) {
      const auto highest = LoadRelease(checkpoint, true);
      const auto previous = ReadBoundedFile(checkpoint, 16 * 1024 * 1024 + 64);
      if (!highest || !previous)
        return std::unexpected(
            "The stored release checkpoint could not be verified.");
      const auto next_version = ParseBrowserVersion(release->version);
      const auto high_version = ParseBrowserVersion(highest->version);
      if (!next_version || !high_version ||
          release->sequence < highest->sequence ||
          *next_version < *high_version ||
          (release->sequence == highest->sequence &&
           *bytes != std::string_view(*previous).substr(64)))
        return std::unexpected(
            "Release rollback or conflicting metadata was rejected.");
    } else if (error) {
      return std::unexpected(
          "The stored release checkpoint could not be inspected.");
    }
  }
  return release;
}
std::expected<void, std::string>
WriteInstallManifest(const fs::path &archive, const SignedRelease &release) {
  const auto signature = DecodeSignature(release.package_signature);
  if (!VerifyReleaseSignature(release.package_manifest, signature))
    return std::unexpected("The package file manifest signature is invalid.");
  if (const auto saved =
          Write(Suffixed(archive, L".manifest.json"), release.package_manifest);
      !saved)
    return std::unexpected(saved.error());
  return Write(Suffixed(archive, L".manifest.json.sig"), signature);
}
std::expected<std::string, std::string>
RunSignedUpdate(const BackgroundUpdateOptions &options) {
  const auto installed = FindChromiumInstallation();
  if (!installed)
    return std::unexpected(
        "A standard per-user Krypton installation was not found.");
  const auto cache = installed->application.parent_path() / "Update";
  std::error_code error;
  fs::create_directories(cache, error);
  if (error)
    return std::unexpected("Could not create the per-user update directory.");
  if (!options.worker) {
    const auto started = RelaunchWorker(cache, options);
    if (!started)
      return std::unexpected(started.error());
    return "Krypton is checking for a signed update in the background.";
  }
  std::array<wchar_t, 32768> own{};
  if (!GetModuleFileNameW(nullptr, own.data(), static_cast<DWORD>(own.size())))
    return std::unexpected("Could not locate the update worker.");
  const fs::path executable(own.data());
  auto signature = executable;
  signature += ".sig";
  if (!VerifyReleaseFile(executable, signature))
    return std::unexpected("The update worker release signature is invalid.");
  auto result = RunWorker(options, *installed, cache);
  if (!result)
    Status(cache, "error", 0, {}, result.error());
  Log(cache, result ? *result : result.error());
  return result;
}
void ReconcileInstalledUpdateStatus(const fs::path &application) noexcept {
  try {
    const auto cache = application.parent_path() / "Update";
    if (!VersionManager::IsPlainDirectory(cache))
      return;
    Handle lock(CreateFileW((cache / "worker.lock").c_str(),
                            GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (lock.get() == INVALID_HANDLE_VALUE)
      return;
    const auto installed = FindChromiumInstallation();
    if (!installed || installed->application != application)
      return;
    std::error_code error;
    if (fs::exists(application / "new_chrome.exe", error) || error)
      return;
    const auto pending = cache / "pending.signed";
    const bool has_pending = fs::exists(pending, error);
    if (error)
      return;
    if (has_pending) {
      const auto release = LoadRelease(pending, true);
      if (!release)
        return;
      const auto next = ParseBrowserVersion(release->version);
      const auto current = ParseBrowserVersion(installed->version);
      if (!next || !current || *next > *current || !fs::remove(pending, error))
        return;
    }
    Status(cache, "updated", 100, installed->version);
  } catch (...) {
    // Update status is advisory after setup has verified the installation.
  }
}
} // namespace kr::updater::windows
