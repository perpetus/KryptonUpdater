#pragma once

#include <windows.h>

#include <filesystem>
#include <memory>

#include "krypton/updater/windows/version_manager.hpp"

namespace kr::updater::windows {
struct CloseFileHandle {
  void operator()(HANDLE handle) const noexcept {
    if (handle && handle != INVALID_HANDLE_VALUE)
      CloseHandle(handle);
  }
};

using ScopedFileHandle = std::unique_ptr<void, CloseFileHandle>;

// Protect an exact, plain file against replacement and writes through launch.
[[nodiscard]] inline ScopedFileHandle
LockFileForVerification(const std::filesystem::path &path) {
  if (!VersionManager::IsPlainDirectory(path.parent_path()))
    return {};
  ScopedFileHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  FILE_ATTRIBUTE_TAG_INFO info{};
  if (file.get() == INVALID_HANDLE_VALUE ||
      !GetFileInformationByHandleEx(file.get(), FileAttributeTagInfo, &info,
                                    sizeof(info)) ||
      (info.FileAttributes &
       (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
    return {};
  return file;
}
} // namespace kr::updater::windows
