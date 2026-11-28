#include "krypton/updater/windows/version_manager.hpp"

#include <algorithm>
#include <format>
#include <memory>
#include <shlobj.h>
#include <windows.h>

namespace kr::updater::windows {
namespace {
struct FileCloser {
  void operator()(void *handle) const noexcept {
    if (handle && handle != INVALID_HANDLE_VALUE)
      CloseHandle(handle);
  }
};
using File = std::unique_ptr<void, FileCloser>;
File LockForCleanup(const std::filesystem::path &path) {
  const auto attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES ||
      (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
    return {};
  File file(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
                        FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                        FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (file.get() == INVALID_HANDLE_VALUE)
    file.reset();
  return file;
}
bool DirectChild(const std::filesystem::path &root,
                 const std::filesystem::path &child) {
  std::error_code error;
  return root.is_absolute() && child.is_absolute() &&
         std::filesystem::equivalent(child.lexically_normal().parent_path(),
                                     root, error) &&
         !error && VersionManager::IsPlainDirectory(root);
}
} // namespace

bool VersionManager::IsPlainDirectory(
    const std::filesystem::path &path) noexcept {
  try {
    if (!path.is_absolute())
      return false;
    for (auto parent = path.lexically_normal();;
         parent = parent.parent_path()) {
      const auto attributes = GetFileAttributesW(parent.c_str());
      if (attributes == INVALID_FILE_ATTRIBUTES ||
          (attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
          !(attributes & FILE_ATTRIBUTE_DIRECTORY))
        return false;
      if (parent == parent.root_path())
        return true;
    }
  } catch (...) {
    return false;
  }
}
bool VersionManager::RemoveOwnedFile(
    const std::filesystem::path &root,
    const std::filesystem::path &file) noexcept {
  try {
    if (!DirectChild(root, file))
      return false;
    const auto lock = LockForCleanup(file);
    if (!lock)
      return false;
    std::error_code error;
    return std::filesystem::remove(file, error) && !error;
  } catch (...) {
    return false;
  }
}
bool VersionManager::RemoveOwnedDirectory(
    const std::filesystem::path &root,
    const std::filesystem::path &directory) noexcept {
  try {
    if (!DirectChild(root, directory) || !IsPlainDirectory(directory))
      return false;
    // Lock every file before deleting any, leaving active work complete.
    std::vector<File> locks;
    std::error_code error;
    for (auto entry =
             std::filesystem::recursive_directory_iterator(directory, error);
         !error && entry != std::filesystem::recursive_directory_iterator{};
         entry.increment(error)) {
      const auto attributes = GetFileAttributesW(entry->path().c_str());
      if (attributes == INVALID_FILE_ATTRIBUTES ||
          (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return false;
      if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        auto lock = LockForCleanup(entry->path());
        if (!lock)
          return false;
        locks.push_back(std::move(lock));
      }
    }
    if (error || !IsPlainDirectory(directory))
      return false;
    std::filesystem::remove_all(directory, error);
    return !error;
  } catch (...) {
    return false;
  }
}

std::filesystem::path VersionManager::GetDefaultInstallRoot() {
  PWSTR directory = nullptr;
  if (FAILED(
          SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &directory)))
    return {};
  const auto result =
      std::filesystem::path(directory) / "Krypton/Krypton/Application";
  CoTaskMemFree(directory);
  return result;
}

std::filesystem::path
VersionManager::GetVersionDirectory(const std::filesystem::path &root,
                                    const SemVer &version) {
  return root / std::format("app-{}", version.ToString());
}

std::filesystem::path
VersionManager::GetStagingDirectory(const std::filesystem::path &root,
                                    const SemVer &version) {
  return root / std::format("app-{}.staging", version.ToString());
}

std::vector<std::pair<SemVer, std::filesystem::path>>
VersionManager::GetInstalledVersions(const std::filesystem::path &root) {
  std::vector<std::pair<SemVer, std::filesystem::path>> result;
  std::error_code ec;

  if (!std::filesystem::exists(root, ec))
    return result;

  for (const auto &entry : std::filesystem::directory_iterator(root, ec)) {
    if (!entry.is_directory())
      continue;
    const auto name = entry.path().filename().u8string();
    if (name.starts_with(u8"app-") && !name.ends_with(u8".staging")) {
      std::string_view ver_part(reinterpret_cast<const char *>(name.data() + 4),
                                name.size() - 4);
      auto ver = SemVer::Parse(ver_part);
      if (ver) {
        result.emplace_back(*ver, entry.path());
      }
    }
  }

  std::sort(result.begin(), result.end(),
            [](const auto &a, const auto &b) { return a.first < b.first; });

  return result;
}

std::optional<std::pair<SemVer, std::filesystem::path>>
VersionManager::GetNewestInstalledVersion(const std::filesystem::path &root) {
  auto versions = GetInstalledVersions(root);
  if (versions.empty())
    return std::nullopt;
  return versions.back();
}

bool VersionManager::CommitStagedVersion(const std::filesystem::path &root,
                                         const SemVer &version) {
  auto staging = GetStagingDirectory(root, version);
  auto target = GetVersionDirectory(root, version);

  std::error_code ec;
  if (!std::filesystem::exists(staging, ec))
    return false;

  if (std::filesystem::exists(target, ec)) {
    if (!RemoveOwnedDirectory(root, target))
      return false;
  }

  std::filesystem::rename(staging, target, ec);
  return !ec;
}

void VersionManager::CleanupOldVersions(const std::filesystem::path &root,
                                        const SemVer &current_version) {
  auto versions = GetInstalledVersions(root);
  for (const auto &[ver, path] : versions) {
    if (ver < current_version) {
      (void)RemoveOwnedDirectory(root, path);
    }
  }
}

} // namespace kr::updater::windows
