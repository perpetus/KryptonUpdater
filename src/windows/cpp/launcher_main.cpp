// clang-format off
#include <windows.h>
#include <shellapi.h>
// clang-format on

#include <filesystem>
#include <string>

#include "krypton/updater/windows/authenticode_verifier.hpp"
#include "krypton/updater/windows/os_capability.hpp"
#include "krypton/updater/windows/version_manager.hpp"

int WINAPI wWinMain(HINSTANCE /*hInstance*/, HINSTANCE /*hPrevInstance*/,
                    PWSTR lpCmdLine, int nCmdShow) {
  const auto compatibility = kr::updater::EvaluateBrowserCompatibility(
      kr::updater::windows::QuerySystemCapabilities());
  if (!compatibility) {
    const auto message =
        kr::updater::CompatibilityErrorMessage(compatibility.error());
    const std::wstring wide(message.begin(), message.end());
    MessageBoxW(nullptr, wide.c_str(), L"Krypton - Unsupported System",
                MB_ICONINFORMATION | MB_OK);
    return 1;
  }
  wchar_t module_path[MAX_PATH]{};
  GetModuleFileNameW(nullptr, module_path, MAX_PATH);
  std::filesystem::path root = std::filesystem::path(module_path).parent_path();

  // Security check: Verify that KryptonUpdater.exe has a valid digital
  // signature
  auto updater_exe = root / "KryptonUpdater.exe";
  if (std::filesystem::exists(updater_exe)) {
    auto sig_status =
        kr::updater::windows::AuthenticodeVerifier::Verify(updater_exe);
    if (sig_status != kr::updater::windows::SignatureStatus::kValidAndTrusted) {
      MessageBoxW(nullptr,
                  L"Krypton security check failed: KryptonUpdater.exe "
                  L"signature is untrusted or modified.",
                  L"Security Warning", MB_ICONERROR | MB_OK);
      return 1;
    }
  }

  // Find newest installed version
  auto newest =
      kr::updater::windows::VersionManager::GetNewestInstalledVersion(root);
  if (!newest) {
    MessageBoxW(nullptr,
                L"No installed version of Krypton was found. Please run "
                L"KryptonUpdater.exe to reinstall.",
                L"Krypton Launcher", MB_ICONERROR | MB_OK);
    return 1;
  }

  // Look for browser executable in the version directory
  std::filesystem::path target_exe;
  for (const auto &name : {L"kr_preview.exe", L"krypton.exe", L"chrome.exe"}) {
    auto candidate = newest->second / name;
    if (std::filesystem::exists(candidate)) {
      target_exe = candidate;
      break;
    }
  }

  if (target_exe.empty()) {
    MessageBoxW(
        nullptr,
        L"Could not locate browser executable in the active version directory.",
        L"Krypton Launcher", MB_ICONERROR | MB_OK);
    return 1;
  }

  // Build command line and launch process forwarding all arguments
  std::wstring cmd = std::format(L"\"{}\" {}", target_exe.wstring(),
                                 lpCmdLine ? lpCmdLine : L"");

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = static_cast<WORD>(nCmdShow);
  PROCESS_INFORMATION pi{};

  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                      target_exe.parent_path().wstring().c_str(), &si, &pi)) {
    return 1;
  }

  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return 0;
}
