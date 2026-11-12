#include "krypton/updater/windows/shell_registrar.hpp"

// clang-format off
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <propkey.h>
#include <propvarutil.h>
// clang-format on
#include <format>

namespace kr::updater::windows {
namespace {
class ComApartment {
public:
  ComApartment() noexcept
      : status_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
  ~ComApartment() {
    if (SUCCEEDED(status_))
      CoUninitialize();
  }
  ComApartment(const ComApartment &) = delete;
  ComApartment &operator=(const ComApartment &) = delete;
  ComApartment(ComApartment &&) = delete;
  ComApartment &operator=(ComApartment &&) = delete;
  [[nodiscard]] bool usable() const noexcept {
    return SUCCEEDED(status_) || status_ == RPC_E_CHANGED_MODE;
  }

private:
  HRESULT status_;
};

bool SetRegString(HKEY root, const std::wstring &subkey,
                  const std::wstring &value_name, const std::wstring &data) {
  HKEY hKey = nullptr;
  if (RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                      KEY_WRITE, nullptr, &hKey, nullptr) != ERROR_SUCCESS) {
    return false;
  }
  auto status =
      RegSetValueExW(hKey, value_name.empty() ? nullptr : value_name.c_str(), 0,
                     REG_SZ, reinterpret_cast<const BYTE *>(data.c_str()),
                     static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(hKey);
  return status == ERROR_SUCCESS;
}

} // namespace

bool ShellRegistrar::RegisterBrowser(const std::filesystem::path &install_root,
                                     const SemVer & /*version*/) {
  std::wstring launcher = (install_root / "krypton.exe").wstring();
  std::wstring icon = std::format(L"\"{}\",0", launcher);
  std::wstring open_cmd = std::format(L"\"{}\" \"%1\"", launcher);

  // 1. HKCU\Software\Clients\StartMenuInternet\Krypton
  std::wstring client_key = L"Software\\Clients\\StartMenuInternet\\Krypton";
  SetRegString(HKEY_CURRENT_USER, client_key, L"", L"Krypton Browser");
  SetRegString(HKEY_CURRENT_USER, client_key + L"\\DefaultIcon", L"", icon);
  SetRegString(HKEY_CURRENT_USER, client_key + L"\\shell\\open\\command", L"",
               open_cmd);

  // Capabilities
  std::wstring caps_key = client_key + L"\\Capabilities";
  SetRegString(HKEY_CURRENT_USER, caps_key, L"ApplicationDescription",
               L"Fast, modern, native web browser");
  SetRegString(HKEY_CURRENT_USER, caps_key, L"ApplicationName", L"Krypton");
  SetRegString(HKEY_CURRENT_USER, caps_key, L"ApplicationIcon", icon);

  SetRegString(HKEY_CURRENT_USER, caps_key + L"\\StartMenu",
               L"StartMenuInternet", L"Krypton");
  SetRegString(HKEY_CURRENT_USER, caps_key + L"\\URLAssociations", L"http",
               L"KryptonHTML");
  SetRegString(HKEY_CURRENT_USER, caps_key + L"\\URLAssociations", L"https",
               L"KryptonHTML");
  SetRegString(HKEY_CURRENT_USER, caps_key + L"\\FileAssociations", L".htm",
               L"KryptonHTML");
  SetRegString(HKEY_CURRENT_USER, caps_key + L"\\FileAssociations", L".html",
               L"KryptonHTML");
  SetRegString(HKEY_CURRENT_USER, caps_key + L"\\FileAssociations", L".pdf",
               L"KryptonHTML");

  // 2. HKCU\Software\RegisteredApplications
  SetRegString(HKEY_CURRENT_USER, L"Software\\RegisteredApplications",
               L"Krypton", caps_key);

  // 3. HKCU\Software\Classes\KryptonHTML
  std::wstring prog_id = L"Software\\Classes\\KryptonHTML";
  SetRegString(HKEY_CURRENT_USER, prog_id, L"", L"Krypton HTML Document");
  SetRegString(HKEY_CURRENT_USER, prog_id + L"\\DefaultIcon", L"", icon);
  SetRegString(HKEY_CURRENT_USER, prog_id + L"\\shell\\open\\command", L"",
               open_cmd);

  return true;
}

bool ShellRegistrar::CreateStartMenuShortcut(
    const std::filesystem::path &target_exe,
    const std::wstring &app_user_model_id) {
  ComApartment apartment;
  if (!apartment.usable())
    return false;
  PWSTR programs_path = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr,
                                  &programs_path))) {
    return false;
  }

  std::filesystem::path link_path =
      std::filesystem::path(programs_path) / "Krypton.lnk";
  CoTaskMemFree(programs_path);

  IShellLinkW *shell_link = nullptr;
  if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                              IID_IShellLinkW,
                              reinterpret_cast<void **>(&shell_link)))) {
    return false;
  }

  shell_link->SetPath(target_exe.wstring().c_str());
  shell_link->SetWorkingDirectory(target_exe.parent_path().wstring().c_str());
  shell_link->SetDescription(L"Krypton Browser");

  // Set AppUserModelID property for Windows Action Center notification routing
  IPropertyStore *prop_store = nullptr;
  if (SUCCEEDED(shell_link->QueryInterface(
          IID_IPropertyStore, reinterpret_cast<void **>(&prop_store)))) {
    PROPVARIANT pv;
    InitPropVariantFromString(app_user_model_id.c_str(), &pv);
    prop_store->SetValue(PKEY_AppUserModel_ID, pv);
    PropVariantClear(&pv);
    prop_store->Commit();
    prop_store->Release();
  }

  IPersistFile *persist_file = nullptr;
  bool ok = false;
  if (SUCCEEDED(shell_link->QueryInterface(
          IID_IPersistFile, reinterpret_cast<void **>(&persist_file)))) {
    ok = SUCCEEDED(persist_file->Save(link_path.wstring().c_str(), TRUE));
    persist_file->Release();
  }

  shell_link->Release();
  return ok;
}

bool ShellRegistrar::CreateDesktopShortcut(
    const std::filesystem::path &target_exe,
    const std::wstring &app_user_model_id) {
  ComApartment apartment;
  if (!apartment.usable())
    return false;
  PWSTR desktop_path = nullptr;
  if (FAILED(
          SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop_path))) {
    return false;
  }

  std::filesystem::path link_path =
      std::filesystem::path(desktop_path) / "Krypton.lnk";
  CoTaskMemFree(desktop_path);

  IShellLinkW *shell_link = nullptr;
  if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                              IID_IShellLinkW,
                              reinterpret_cast<void **>(&shell_link)))) {
    return false;
  }

  shell_link->SetPath(target_exe.wstring().c_str());
  shell_link->SetWorkingDirectory(target_exe.parent_path().wstring().c_str());
  shell_link->SetDescription(L"Krypton Browser");

  IPropertyStore *prop_store = nullptr;
  if (SUCCEEDED(shell_link->QueryInterface(
          IID_IPropertyStore, reinterpret_cast<void **>(&prop_store)))) {
    PROPVARIANT pv;
    InitPropVariantFromString(app_user_model_id.c_str(), &pv);
    prop_store->SetValue(PKEY_AppUserModel_ID, pv);
    PropVariantClear(&pv);
    prop_store->Commit();
    prop_store->Release();
  }

  IPersistFile *persist_file = nullptr;
  bool ok = false;
  if (SUCCEEDED(shell_link->QueryInterface(
          IID_IPersistFile, reinterpret_cast<void **>(&persist_file)))) {
    ok = SUCCEEDED(persist_file->Save(link_path.wstring().c_str(), TRUE));
    persist_file->Release();
  }

  shell_link->Release();
  return ok;
}

bool ShellRegistrar::RegisterUninstaller(
    const std::filesystem::path &install_root, const SemVer &version) {
  std::wstring key =
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Krypton";
  std::wstring launcher = (install_root / "krypton.exe").wstring();
  std::wstring updater = (install_root / "KryptonUpdater.exe").wstring();
  std::wstring icon = std::format(L"\"{}\",0", launcher);
  std::wstring uninstall_cmd = std::format(L"\"{}\" --uninstall", updater);
  std::wstring quiet_cmd = std::format(L"\"{}\" --uninstall --silent", updater);

  SetRegString(HKEY_CURRENT_USER, key, L"DisplayName", L"Krypton");
  const auto version_text = version.ToString();
  std::wstring ver_str(version_text.begin(), version_text.end());
  SetRegString(HKEY_CURRENT_USER, key, L"DisplayVersion", ver_str);
  SetRegString(HKEY_CURRENT_USER, key, L"Publisher", L"Krypton");
  SetRegString(HKEY_CURRENT_USER, key, L"DisplayIcon", icon);
  SetRegString(HKEY_CURRENT_USER, key, L"InstallLocation",
               install_root.wstring());
  SetRegString(HKEY_CURRENT_USER, key, L"UninstallString", uninstall_cmd);
  SetRegString(HKEY_CURRENT_USER, key, L"QuietUninstallString", quiet_cmd);

  return true;
}

bool ShellRegistrar::RegisterUpdateTask(
    const std::filesystem::path &updater_exe) {
  // Uses standard Windows schtasks command to create a daily background task
  std::wstring cmd =
      std::format(L"schtasks.exe /create /tn \"KryptonUpdateTask\" /tr "
                  L"\"\\\"{}\\\" --update-silent\" /sc daily /f",
                  updater_exe.wstring());

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi{};

  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    return false;
  }

  WaitForSingleObject(pi.hProcess, 5000);
  DWORD exit_code = 0;
  GetExitCodeProcess(pi.hProcess, &exit_code);

  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);

  return exit_code == 0;
}

void ShellRegistrar::UnregisterAll(
    const std::filesystem::path & /*install_root*/) {
  RegDeleteTreeW(HKEY_CURRENT_USER,
                 L"Software\\Clients\\StartMenuInternet\\Krypton");
  RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\KryptonHTML");
  RegDeleteTreeW(
      HKEY_CURRENT_USER,
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Krypton");

  HKEY hRegApps = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", 0,
                    KEY_WRITE, &hRegApps) == ERROR_SUCCESS) {
    RegDeleteValueW(hRegApps, L"Krypton");
    RegCloseKey(hRegApps);
  }

  // Delete scheduled task
  std::wstring cmd = L"schtasks.exe /delete /tn \"KryptonUpdateTask\" /f";
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi{};
  if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                     CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
  }
}

} // namespace kr::updater::windows
