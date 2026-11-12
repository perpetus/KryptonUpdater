// clang-format off
#include <windows.h>
#include <shellapi.h>
// clang-format on

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "krypton/updater/updater_engine.hpp"
#include "krypton/updater/windows/headless_view.hpp"
#include "krypton/updater/windows/winui_installer_window.hpp"

namespace {

std::string WideToUtf8(std::wstring_view wstr) {
  if (wstr.empty())
    return {};
  int size = WideCharToMultiByte(CP_UTF8, 0, wstr.data(),
                                 static_cast<int>(wstr.size()), nullptr, 0,
                                 nullptr, nullptr);
  std::string result(size, '\0');
  WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()),
                      result.data(), size, nullptr, nullptr);
  return result;
}

} // namespace

int WINAPI wWinMain(HINSTANCE /*hInstance*/, HINSTANCE /*hPrevInstance*/,
                    PWSTR /*lpCmdLine*/, int /*nCmdShow*/) {
  int argc = 0;
  LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);

  kr::updater::EngineOptions options;
#ifndef KR_WEB_INSTALLER
  options.force_offline = true;
#endif
  bool use_gui = true;
  bool verbose = false;

  for (int i = 1; i < argc; ++i) {
    std::wstring arg = argv[i];
    if (arg == L"--silent" || arg == L"-s" || arg == L"--update-silent") {
      options.silent = true;
      use_gui = false;
    } else if (arg == L"--check") {
      options.check_only = true;
      use_gui = false;
      verbose = true;
    } else if (arg == L"--verbose" || arg == L"-v") {
      verbose = true;
    } else if (arg == L"--offline") {
      options.force_offline = true;
    } else if (arg == L"--repo" && i + 1 < argc) {
      std::wstring repo_arg = argv[++i];
      auto slash = repo_arg.find(L'/');
      if (slash != std::wstring::npos) {
        options.repo_owner = WideToUtf8(repo_arg.substr(0, slash));
        options.repo_name = WideToUtf8(repo_arg.substr(slash + 1));
      }
    } else if (arg == L"--package" && i + 1 < argc) {
      options.custom_package_path = argv[++i];
    } else if (arg == L"--install-dir" && i + 1 < argc) {
      options.install_root = argv[++i];
    }
  }

  LocalFree(argv);

  kr::updater::UpdaterEngine engine(options);
  std::unique_ptr<kr::updater::IUpdaterView> view;

  if (use_gui) {
    view = std::make_unique<kr::updater::windows::WinUIInstallerWindow>();
  } else {
    // Attach console if needed for output
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
      FILE *fp = nullptr;
      freopen_s(&fp, "CONOUT$", "w", stdout);
      freopen_s(&fp, "CONOUT$", "w", stderr);
    }
    view = std::make_unique<kr::updater::windows::HeadlessView>(verbose);
  }

  view->Initialize(&engine);
  return view->RunModal();
}
