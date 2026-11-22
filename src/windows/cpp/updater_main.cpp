#include "krypton/updater/windows/release_security.hpp"
#include "krypton/updater/windows/signed_update_service.hpp"
// clang-format off
#include <windows.h>
#include <shellapi.h>
// clang-format on
#include <filesystem>
#include <format>
#include <string_view>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  using namespace kr::updater::windows;
  try {
    int count{};
    auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments)
      return 1;
    BackgroundUpdateOptions options;
    bool silent{}, uninstall{};
    for (int index = 1; index < count; ++index) {
      const std::wstring_view arg(arguments[index]);
      if (arg == L"--worker")
        options.worker = true;
      else if (arg == L"--browser-start")
        options.browser_start = true;
      else if (arg == L"--check")
        options.check_only = true;
      else if (arg == L"--download-only")
        options.download_only = true;
      else if (arg == L"--apply-pending")
        options.apply_pending = true;
      else if (arg == L"--require-delta")
        options.require_delta = true;
      else if (arg == L"--release-file" && index + 1 < count)
        options.release_file = std::filesystem::absolute(arguments[++index]);
      else if (arg == L"--uninstall")
        uninstall = true;
      else if (arg != L"--silent" && arg != L"--update-silent") {
        LocalFree(arguments);
        return ERROR_INVALID_PARAMETER;
      }
      if (arg == L"--silent" || arg == L"--browser-start" ||
          arg == L"--update-silent" || arg == L"--worker")
        silent = true;
    }
    LocalFree(arguments);
    if (uninstall) {
      wchar_t own[32768]{};
      if (!GetModuleFileNameW(nullptr, own, 32768))
        return 1;
      const auto executable =
          std::filesystem::path(own).parent_path() / L"KryptonUninstaller.exe";
      auto signature = executable;
      signature += L".sig";
      if (!VerifyReleaseFile(executable, signature))
        return ERROR_INVALID_IMAGE_HASH;
      auto command = std::format(L"\"{}\" --uninstall{}", executable.wstring(),
                                 silent ? L" --silent" : L"");
      STARTUPINFOW startup{sizeof(STARTUPINFOW)};
      PROCESS_INFORMATION child{};
      if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
                          FALSE, 0, nullptr, nullptr, &startup, &child))
        return 1;
      CloseHandle(child.hThread);
      CloseHandle(child.hProcess);
      return 0;
    }
    const auto result = RunSignedUpdate(options);
    if (!silent) {
      const auto &text = result ? *result : result.error();
      const auto length = MultiByteToWideChar(
          CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
      std::wstring message(static_cast<std::size_t>(length), L'\0');
      MultiByteToWideChar(CP_UTF8, 0, text.data(),
                          static_cast<int>(text.size()), message.data(),
                          length);
      MessageBoxW(nullptr, message.c_str(), L"Krypton Updater",
                  MB_OK | (result ? MB_ICONINFORMATION : MB_ICONWARNING));
    }
    return result ? 0 : 1;
  } catch (...) {
    return 1;
  }
}
