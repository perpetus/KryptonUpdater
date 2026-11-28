// clang-format off
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
// clang-format on

#include "krypton/updater/updater_engine.hpp"
#include "krypton/updater/windows/chromium_install.hpp"
#include "krypton/updater/windows/headless_view.hpp"
#include "krypton/updater/windows/winui_installer_window.hpp"

#include <cstdio>
#include <filesystem>
#include <format>
#include <memory>
#include <string_view>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  int count = 0;
  const auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
  bool silent = false;
  bool relocated = false;
  std::filesystem::path requested_root;
  for (int index = 1; index < count; ++index) {
    const std::wstring_view argument(arguments[index]);
    silent |= argument == L"--silent";
    relocated |= argument == L"--relocated";
    if (argument == L"--install-dir" && index + 1 < count)
      requested_root = arguments[++index];
  }
  LocalFree(arguments);
  wchar_t executable[32768]{};
  if (!GetModuleFileNameW(nullptr, executable, 32768))
    return 1;
  const std::filesystem::path own_path(executable);
  if (requested_root.empty()) {
    const auto own_parent = own_path.parent_path();
    const auto version = own_parent.parent_path().filename().u8string();
    if (own_parent.filename() == L"Installer" &&
        kr::updater::windows::IsChromiumVersion(std::string(
            reinterpret_cast<const char *>(version.data()), version.size())))
      requested_root = own_parent.parent_path().parent_path();
    else if (std::filesystem::is_regular_file(own_parent / "krypton.exe"))
      requested_root = own_parent;
    else
      requested_root =
          kr::updater::windows::DefaultChromiumApplicationDirectory();
  }

  // Run the UI and native removal backend outside Application so neither this
  // executable nor its working directory prevents removing the installation.
  if (!relocated) {
    GUID identifier{};
    wchar_t guid[40]{};
    if (FAILED(CoCreateGuid(&identifier)) ||
        !StringFromGUID2(identifier, guid, 40))
      return 1;
    const auto directory = std::filesystem::temp_directory_path() /
                           std::format(L"KryptonUninstall-{}", guid);
    std::error_code error;
    if (!std::filesystem::create_directory(directory, error))
      return 1;
    const auto copy = directory / "KryptonUninstaller.exe";
    std::filesystem::copy_file(own_path, copy, error);
    if (error)
      return 1;
    auto command = std::format(L"\"{}\" --relocated --install-dir \"{}\"{}",
                               copy.wstring(), requested_root.wstring(),
                               silent ? L" --silent" : L"");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(copy.c_str(), command.data(), nullptr, nullptr, FALSE,
                        0, nullptr, directory.c_str(), &startup, &child))
      return 1;
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    return 0;
  }

  kr::updater::EngineOptions options;
  options.uninstall_mode = true;
  options.silent = silent;
  options.install_root = requested_root;
  kr::updater::UpdaterEngine engine(std::move(options));
  std::unique_ptr<kr::updater::IUpdaterView> view;
  if (silent) {
    view = std::make_unique<kr::updater::windows::HeadlessView>(true);
  } else {
    view = std::make_unique<kr::updater::windows::WinUIInstallerWindow>();
  }
  view->Initialize(&engine);
  return view->RunModal();
}
