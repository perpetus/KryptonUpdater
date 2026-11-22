#include "krypton/updater/windows/payload_runner.hpp"

#include <windows.h>

#include <array>
#include <format>
#include <utility>

namespace kr::updater::windows {
namespace {
bool Run(const std::filesystem::path &executable,
         const std::filesystem::path &working_directory, std::wstring command,
         std::uint32_t *out_exit_code,
         ScopedFileHandle verification_lock = {}) {
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
                      FALSE, 0, nullptr, working_directory.c_str(), &startup,
                      &process))
    return false;
  // The executable image is now mapped. Setup can move itself during
  // uninstall while the caller waits for it.
  verification_lock.reset();

  DWORD exit_code = 0;
  const bool finished =
      WaitForSingleObject(process.hProcess, INFINITE) == WAIT_OBJECT_0 &&
      GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  if (finished && out_exit_code)
    *out_exit_code = exit_code;
  return finished;
}
} // namespace

bool PayloadRunner::RunMsi(const std::filesystem::path &msi_path, bool per_user,
                           std::uint32_t *out_exit_code) {
  std::error_code error;
  const auto package = std::filesystem::absolute(msi_path, error);
  std::array<wchar_t, MAX_PATH> path{};
  const auto length =
      GetSystemDirectoryW(path.data(), static_cast<UINT>(path.size()));
  if (error || !length || length >= path.size())
    return false;
  const std::filesystem::path system_directory(path.data());
  const auto executable = system_directory / L"msiexec.exe";
  const auto command =
      std::format(L"\"{}\" /i \"{}\" /qn /norestart {}", executable.wstring(),
                  package.wstring(),
                  per_user ? L"ALLUSERS=2 MSIINSTALLPERUSER=1" : L"ALLUSERS=1");
  std::uint32_t exit_code = 0;
  if (!Run(executable, system_directory, command, &exit_code))
    return false;
  if (out_exit_code)
    *out_exit_code = exit_code;
  return exit_code == ERROR_SUCCESS ||
         exit_code == ERROR_SUCCESS_REBOOT_INITIATED ||
         exit_code == ERROR_SUCCESS_REBOOT_REQUIRED;
}

bool PayloadRunner::RunExe(const std::filesystem::path &exe_path,
                           const std::wstring &arguments,
                           std::uint32_t *out_exit_code,
                           ScopedFileHandle verification_lock) {
  std::error_code error;
  const auto executable = std::filesystem::absolute(exe_path, error);
  if (error)
    return false;
  const auto command =
      std::format(L"\"{}\" {}", executable.wstring(), arguments);
  std::uint32_t exit_code = 0;
  if (!Run(executable, executable.parent_path(), command, &exit_code,
           std::move(verification_lock)))
    return false;
  if (out_exit_code)
    *out_exit_code = exit_code;
  return exit_code == 0;
}
} // namespace kr::updater::windows
