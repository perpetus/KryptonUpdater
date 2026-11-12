#include "krypton/updater/windows/install_cleanup.hpp"

#include "krypton/updater/release_manifest.hpp"
#include "krypton/updater/windows/chromium_install.hpp"
#include "krypton/updater/windows/release_security.hpp"
#include "krypton/updater/windows/release_trust.hpp"
#include "krypton/updater/windows/version_manager.hpp"

// clang-format off
#include <windows.h>
#include <objbase.h>
// clang-format on

#include <algorithm>
#include <array>
#include <chrono>
#include <utility>
#include <vector>

namespace kr::updater::windows {
namespace {
namespace fs = std::filesystem;
bool Digest(std::wstring_view name) {
  return name.size() == 64 && std::ranges::all_of(name, [](wchar_t ch) {
           return (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f');
         });
}
bool Stale(const fs::directory_entry &entry) {
  std::error_code error;
  const auto written = entry.last_write_time(error);
  return !error &&
         fs::file_time_type::clock::now() - written > std::chrono::hours(24);
}
template <typename Callback>
void Children(const fs::path &root, Callback &&callback) {
  if (!VersionManager::IsPlainDirectory(root))
    return;
  std::error_code error;
  for (auto entry = fs::directory_iterator(root, error);
       !error && entry != fs::directory_iterator{}; entry.increment(error))
    callback(*entry);
}
bool SamePath(const fs::path &left, const fs::path &right) {
  return _wcsicmp(left.lexically_normal().c_str(),
                  right.lexically_normal().c_str()) == 0;
}
std::string CurrentPackage(const fs::path &cache, std::string_view version) {
  const auto bytes =
      ReadBoundedFile(cache / "accepted.signed", 16 * 1024 * 1024 + 64);
  if (!bytes || bytes->size() <= 64)
    return {};
  const std::string_view signature(bytes->data(), 64);
  const std::string_view metadata(bytes->data() + 64, bytes->size() - 64);
  if (!VerifyReleaseSignature(metadata, signature))
    return {};
  const auto now = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  const auto release =
      ParseSignedRelease(metadata, kReleaseRepository, now, true);
  return release && release->version == version ? release->full.sha256
                                                : std::string{};
}
void CleanupPackages(const fs::path &cache,
                     std::string_view installed_package) {
  std::error_code error;
  if (installed_package.size() != 64 ||
      fs::exists(cache / "pending.signed", error) || error)
    return;
  const std::wstring current(installed_package.begin(),
                             installed_package.end());
  if (!Digest(current))
    return;
  const auto packages = cache / "packages";
  std::vector<std::pair<fs::file_time_type, std::wstring>> previous;
  Children(packages, [&](const fs::directory_entry &entry) {
    const auto name = entry.path().filename().wstring();
    if (name.size() != 67 || !name.ends_with(L".7z") ||
        !Digest(std::wstring_view(name).substr(0, 64)) ||
        name.substr(0, 64) == current)
      return;
    const auto written = entry.last_write_time(error);
    if (!error)
      previous.emplace_back(written, name.substr(0, 64));
  });
  std::ranges::sort(
      previous, [](const auto &a, const auto &b) { return a.first > b.first; });
  const auto base = previous.empty() ? std::wstring{} : previous.front().second;
  Children(packages, [&](const fs::directory_entry &entry) {
    const auto name = entry.path().filename().wstring();
    if (name.size() <= 64 || !Digest(std::wstring_view(name).substr(0, 64)))
      return;
    const auto hash = name.substr(0, 64);
    const auto suffix = std::wstring_view(name).substr(64);
    const bool package = suffix == L".7z" || suffix == L".7z.manifest.json" ||
                         suffix == L".7z.manifest.json.sig";
    const bool partial = suffix == L".7z.part" || suffix == L".delta.part";
    if ((package && hash != current && hash != base) || suffix == L".delta" ||
        (partial && Stale(entry)))
      (void)VersionManager::RemoveOwnedFile(packages, entry.path());
  });
}
void CleanupWorkers(const fs::path &cache,
                    const ChromiumInstallation &installed) {
  std::array<wchar_t, 32768> own{};
  const auto length =
      GetModuleFileNameW(nullptr, own.data(), static_cast<DWORD>(own.size()));
  if (!length || length >= own.size())
    return;
  const auto running = fs::path(own.data()).parent_path();
  const auto helper = installed.application / installed.version /
                      "Installer/KryptonUpdater.exe";
  const auto installed_hash = Sha256File(helper);
  if (!installed_hash)
    return;
  const std::wstring keep(installed_hash->begin(), installed_hash->end());
  const auto workers = cache / "workers";
  Children(workers, [&](const fs::directory_entry &entry) {
    const auto name = entry.path().filename().wstring();
    if (!Digest(name) || name == keep || SamePath(entry.path(), running) ||
        !VersionManager::IsPlainDirectory(entry.path()))
      return;
    bool owned = true;
    Children(entry.path(), [&](const fs::directory_entry &child) {
      const auto file = child.path().filename().native();
      owned &=
          file == L"KryptonUpdater.exe" || file == L"KryptonUpdater.exe.sig";
    });
    if (owned)
      (void)VersionManager::RemoveOwnedDirectory(workers, entry.path());
  });
}
void CleanupStaging(const fs::path &application) {
  Children(application, [&](const fs::directory_entry &entry) {
    const auto name = entry.path().filename().string();
    if (name.starts_with("app-") && name.ends_with(".staging") &&
        SemVer::Parse(std::string_view(name).substr(4, name.size() - 12)) &&
        Stale(entry))
      (void)VersionManager::RemoveOwnedDirectory(application, entry.path());
  });
}
} // namespace

TemporaryInstallDirectory::TemporaryInstallDirectory(fs::path path)
    : path_(std::move(path)) {}
TemporaryInstallDirectory::TemporaryInstallDirectory(
    TemporaryInstallDirectory &&other) noexcept
    : path_(std::exchange(other.path_, {})) {}
TemporaryInstallDirectory::~TemporaryInstallDirectory() {
  try {
    if (!path_.empty())
      (void)VersionManager::RemoveOwnedDirectory(path_.parent_path(), path_);
  } catch (...) {
  }
}
std::expected<TemporaryInstallDirectory, std::string>
TemporaryInstallDirectory::Create() {
  std::array<wchar_t, 32768> temporary{};
  const auto length =
      GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data());
  if (!length || length >= temporary.size())
    return std::unexpected("Could not find the installer temporary directory.");
  const fs::path parent = fs::path(temporary.data()).lexically_normal();
  if (!VersionManager::IsPlainDirectory(parent))
    return std::unexpected(
        "The installer temporary directory is not a regular directory.");
  Children(parent, [&](const fs::directory_entry &entry) {
    const auto name = entry.path().filename().wstring();
    GUID identifier{};
    if (name.starts_with(L"KryptonSetup-") && name.size() == 51 &&
        SUCCEEDED(CLSIDFromString(name.c_str() + 13, &identifier)) &&
        Stale(entry))
      (void)VersionManager::RemoveOwnedDirectory(parent, entry.path());
  });
  GUID identifier{};
  std::array<wchar_t, 39> name{};
  if (FAILED(CoCreateGuid(&identifier)) ||
      !StringFromGUID2(identifier, name.data(), static_cast<int>(name.size())))
    return std::unexpected(
        "Could not create an installer workspace identifier.");
  const auto directory =
      parent / (std::wstring(L"KryptonSetup-") + name.data());
  if (!CreateDirectoryW(directory.c_str(), nullptr))
    return std::unexpected("Could not create the installer workspace.");
  return TemporaryInstallDirectory(directory);
}
void CleanupInstallArtifacts(const fs::path &application,
                             std::string_view installed_package) noexcept {
  try {
    const auto installed = FindChromiumInstallation();
    if (!installed || !SamePath(installed->application, application) ||
        !VersionManager::IsPlainDirectory(application))
      return;
    CleanupStaging(application);
    const auto cache = application.parent_path() / "Update";
    if (!VersionManager::IsPlainDirectory(cache))
      return;
    const auto current = installed_package.empty()
                             ? CurrentPackage(cache, installed->version)
                             : std::string(installed_package);
    CleanupPackages(cache, current);
    CleanupWorkers(cache, *installed);
  } catch (...) {
    // Best-effort cleanup must not turn a confirmed install into a failure.
  }
}
} // namespace kr::updater::windows
