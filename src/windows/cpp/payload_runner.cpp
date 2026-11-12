#include "krypton/updater/windows/payload_runner.hpp"

#include <windows.h>
#include <format>

namespace kr::updater::windows {

bool PayloadRunner::RunMsi(
    const std::filesystem::path& msi_path,
    bool per_user,
    std::uint32_t* out_exit_code) {
  std::wstring cmd = std::format(
      L"msiexec.exe /i \"{}\" /qn /norestart {}",
      msi_path.wstring(),
      per_user ? L"ALLUSERS=2 MSIINSTALLPERUSER=1" : L"ALLUSERS=1");

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};

  if (!CreateProcessW(
          nullptr,
          cmd.data(),
          nullptr,
          nullptr,
          FALSE,
          0,
          nullptr,
          nullptr,
          &si,
          &pi)) {
    return false;
  }

  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD exit_code = 0;
  GetExitCodeProcess(pi.hProcess, &exit_code);

  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);

  if (out_exit_code) *out_exit_code = exit_code;
  return exit_code == ERROR_SUCCESS || exit_code == ERROR_SUCCESS_REBOOT_INITIATED || exit_code == ERROR_SUCCESS_REBOOT_REQUIRED;
}

bool PayloadRunner::RunExe(
    const std::filesystem::path& exe_path,
    const std::wstring& arguments,
    std::uint32_t* out_exit_code) {
  std::wstring cmd = std::format(L"\"{}\" {}", exe_path.wstring(), arguments);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};

  if (!CreateProcessW(
          nullptr,
          cmd.data(),
          nullptr,
          nullptr,
          FALSE,
          0,
          nullptr,
          nullptr,
          &si,
          &pi)) {
    return false;
  }

  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD exit_code = 0;
  GetExitCodeProcess(pi.hProcess, &exit_code);

  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);

  if (out_exit_code) *out_exit_code = exit_code;
  return exit_code == 0;
}

}  // namespace kr::updater::windows
