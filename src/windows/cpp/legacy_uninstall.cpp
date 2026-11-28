#include "krypton/updater/windows/legacy_uninstall.hpp"

#include "krypton/updater/semver.hpp"

// clang-format off
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <taskschd.h>
// clang-format on
#include <winrt/base.h>

#include <array>
#include <format>
#include <string_view>

namespace kr::updater::windows {
namespace {
constexpr wchar_t kLegacyUninstall[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Krypton";

std::wstring ReadString(const wchar_t *key, const wchar_t *name) {
  DWORD bytes = 0;
  if (RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_SZ, nullptr,
                   nullptr, &bytes) != ERROR_SUCCESS ||
      bytes > 65536)
    return {};
  std::wstring value(bytes / sizeof(wchar_t), L'\0');
  if (RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_SZ, nullptr,
                   value.data(), &bytes) != ERROR_SUCCESS)
    return {};
  while (!value.empty() && value.back() == L'\0')
    value.pop_back();
  return value;
}

bool SamePath(const std::filesystem::path &left,
              const std::filesystem::path &right) {
  return _wcsicmp(left.lexically_normal().c_str(),
                  right.lexically_normal().c_str()) == 0;
}

bool ProtectedName(std::wstring_view name) {
  return _wcsicmp(std::wstring(name).c_str(), L"User Data") == 0 ||
         _wcsicmp(std::wstring(name).c_str(), L"UserData") == 0;
}

bool RemoveDirectory(const std::filesystem::path &path,
                     std::error_code &error) {
  const auto attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES ||
      (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
    return true;
  for (std::filesystem::directory_iterator entry(path, error), end;
       !error && entry != end; entry.increment(error)) {
    const auto child = entry->path();
    if (ProtectedName(child.filename().wstring()))
      continue;
    const auto flags = GetFileAttributesW(child.c_str());
    if (flags == INVALID_FILE_ATTRIBUTES ||
        (flags & FILE_ATTRIBUTE_REPARSE_POINT))
      continue;
    if (flags & FILE_ATTRIBUTE_DIRECTORY) {
      if (!RemoveDirectory(child, error))
        return false;
    } else {
      std::filesystem::remove(child, error);
      if (error)
        return false;
    }
  }
  if (error)
    return false;
  if (std::filesystem::is_empty(path, error))
    std::filesystem::remove(path, error);
  return !error;
}

bool BrowserRunning(const std::filesystem::path &root) {
  const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE)
    return true;
  PROCESSENTRY32W process{};
  process.dwSize = sizeof(process);
  bool running = false;
  for (BOOL present = Process32FirstW(snapshot, &process); present;
       present = Process32NextW(snapshot, &process)) {
    if (_wcsicmp(process.szExeFile, L"chrome.exe") != 0 &&
        _wcsicmp(process.szExeFile, L"krypton.exe") != 0)
      continue;
    const auto handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                    process.th32ProcessID);
    if (!handle)
      continue;
    std::array<wchar_t, 32768> image{};
    DWORD length = static_cast<DWORD>(image.size());
    if (QueryFullProcessImageNameW(handle, 0, image.data(), &length)) {
      auto prefix = root.lexically_normal().wstring();
      prefix += L'\\';
      running = length > prefix.size() &&
                _wcsnicmp(image.data(), prefix.c_str(), prefix.size()) == 0;
    }
    CloseHandle(handle);
    if (running)
      break;
  }
  CloseHandle(snapshot);
  return running;
}

struct Apartment {
  HRESULT status = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  Apartment() = default;
  Apartment(const Apartment &) = delete;
  Apartment &operator=(const Apartment &) = delete;
  Apartment(Apartment &&) = delete;
  Apartment &operator=(Apartment &&) = delete;
  ~Apartment() {
    if (SUCCEEDED(status))
      CoUninitialize();
  }
};

void RemoveShortcut(const std::filesystem::path &shortcut,
                    const std::filesystem::path &target) {
  bool owned = false;
  {
    winrt::com_ptr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(link.put()))))
      return;
    const auto file = link.as<IPersistFile>();
    std::array<wchar_t, 32768> path{};
    if (SUCCEEDED(file->Load(shortcut.c_str(), STGM_READ)) &&
        SUCCEEDED(link->GetPath(path.data(), static_cast<int>(path.size()),
                                nullptr, SLGP_RAWPATH)))
      owned = SamePath(path.data(), target);
  }
  if (owned) {
    std::error_code ignored;
    std::filesystem::remove(shortcut, ignored);
  }
}

void RemoveUpdateTask(const std::filesystem::path &root) {
  winrt::com_ptr<ITaskService> service;
  if (FAILED(CoCreateInstance(CLSID_TaskScheduler, nullptr,
                              CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(service.put()))))
    return;
  VARIANT empty{};
  if (FAILED(service->Connect(empty, empty, empty, empty)))
    return;
  BSTR folder_name = SysAllocString(L"\\");
  BSTR task_name = SysAllocString(L"KryptonUpdateTask");
  winrt::com_ptr<ITaskFolder> folder;
  winrt::com_ptr<IRegisteredTask> task;
  if (folder_name && task_name &&
      SUCCEEDED(service->GetFolder(folder_name, folder.put())) &&
      SUCCEEDED(folder->GetTask(task_name, task.put()))) {
    winrt::com_ptr<ITaskDefinition> definition;
    winrt::com_ptr<IActionCollection> actions;
    winrt::com_ptr<IAction> action;
    if (SUCCEEDED(task->get_Definition(definition.put())) &&
        SUCCEEDED(definition->get_Actions(actions.put())) &&
        SUCCEEDED(actions->get_Item(1, action.put()))) {
      const auto executable = action.try_as<IExecAction>();
      BSTR path = nullptr;
      if (executable && SUCCEEDED(executable->get_Path(&path))) {
        if (path && SamePath(path, root / "KryptonUpdater.exe"))
          static_cast<void>(folder->DeleteTask(task_name, 0));
        SysFreeString(path);
      }
    }
  }
  SysFreeString(task_name);
  SysFreeString(folder_name);
}

} // namespace

bool IsRegisteredLegacyInstallation(const std::filesystem::path &root) {
  std::error_code error;
  return root.is_absolute() && root != root.root_path() &&
         SamePath(root, ReadString(kLegacyUninstall, L"InstallLocation")) &&
         std::filesystem::is_regular_file(root / "krypton.exe", error) &&
         !(GetFileAttributesW(root.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT);
}

std::expected<void, std::string>
RemoveLegacyProgramFiles(const std::filesystem::path &root) {
  if (!root.is_absolute() || root == root.root_path())
    return std::unexpected("The installation directory is invalid.");
  std::error_code error;
  for (std::filesystem::directory_iterator entry(root, error), end;
       !error && entry != end; entry.increment(error)) {
    const auto filename = entry->path().filename().u8string();
    if (!filename.starts_with(u8"app-"))
      continue;
    std::string_view version(reinterpret_cast<const char *>(filename.data()),
                             filename.size());
    version.remove_prefix(4);
    if (version.ends_with(".staging"))
      version.remove_suffix(8);
    if (!SemVer::Parse(version))
      continue;
    if (!RemoveDirectory(entry->path(), error))
      break;
  }
  if (!error) {
    for (const auto *name :
         {"krypton.exe", "KryptonUpdater.exe", "KryptonUninstaller.exe"}) {
      std::filesystem::remove(root / name, error);
      if (error)
        break;
    }
  }
  if (error)
    return std::unexpected("Some Krypton files are in use. Close its browser "
                           "windows and try again.");
  return {};
}

std::expected<void, std::string>
UninstallLegacyBrowser(const std::filesystem::path &root) {
  if (!IsRegisteredLegacyInstallation(root))
    return std::unexpected(
        "The selected directory is not a registered Krypton installation.");
  if (BrowserRunning(root))
    return std::unexpected(
        "Close Krypton's browser windows and try uninstalling again.");
  const Apartment apartment;
  RemoveUpdateTask(root);
  const auto removed = RemoveLegacyProgramFiles(root);
  if (!removed)
    return removed;
  for (const auto &folder : {FOLDERID_Programs, FOLDERID_Desktop}) {
    PWSTR directory = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(folder, 0, nullptr, &directory))) {
      RemoveShortcut(std::filesystem::path(directory) / "Krypton.lnk",
                     root / "krypton.exe");
      CoTaskMemFree(directory);
    }
  }
  const auto command = std::format(L"\"{}\"", (root / "krypton.exe").wstring());
  constexpr wchar_t client[] = L"Software\\Clients\\StartMenuInternet\\Krypton";
  constexpr wchar_t html[] = L"Software\\Classes\\KryptonHTML";
  if (ReadString(L"Software\\Clients\\StartMenuInternet\\Krypton\\shell\\open\\"
                 L"command",
                 L"")
          .starts_with(command)) {
    RegDeleteTreeW(HKEY_CURRENT_USER, client);
    if (ReadString(L"Software\\RegisteredApplications", L"Krypton") ==
        L"Software\\Clients\\StartMenuInternet\\Krypton\\Capabilities") {
      HKEY key = nullptr;
      if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\RegisteredApplications",
                        0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, L"Krypton");
        RegCloseKey(key);
      }
    }
  }
  if (ReadString(L"Software\\Classes\\KryptonHTML\\shell\\open\\command", L"")
          .starts_with(command))
    RegDeleteTreeW(HKEY_CURRENT_USER, html);
  RegDeleteTreeW(HKEY_CURRENT_USER, kLegacyUninstall);
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  std::error_code ignored;
  if (std::filesystem::is_empty(root, ignored))
    std::filesystem::remove(root, ignored);
  return {};
}

} // namespace kr::updater::windows
