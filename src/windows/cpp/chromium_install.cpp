#include "krypton/updater/windows/chromium_install.hpp"

#include "krypton/updater/release_manifest.hpp"

#include "krypton/updater/windows/legacy_uninstall.hpp"
#include "krypton/updater/windows/locked_file.hpp"
#include "krypton/updater/windows/payload_runner.hpp"
#include "krypton/updater/windows/release_security.hpp"

// clang-format off
#include <windows.h>
#include <shlobj.h>
// clang-format on

#include <array>
#include <charconv>
#include <format>
#include <fstream>
#include <memory>
#include <vector>

namespace kr::updater::windows {
namespace {

constexpr wchar_t kUninstallKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Krypton Krypton";

ScopedFileHandle VerifyNativeSetup(const std::filesystem::path &setup,
                                   const std::filesystem::path &metadata) {
  auto lock = LockFileForVerification(setup);
  const auto marker_lock = LockFileForVerification(metadata);
  auto signature = metadata;
  signature += ".sig";
  if (!lock || !marker_lock || !VerifyReleaseFile(metadata, signature))
    return {};

  wchar_t expected[65]{};
  if (GetPrivateProfileStringW(L"Package", L"SetupSha256", L"", expected,
                               std::size(expected), metadata.c_str()) != 64)
    return {};
  const auto actual = Sha256File(setup);
  if (!actual || std::wstring(actual->begin(), actual->end()) != expected)
    return {};
  return lock;
}

std::wstring ReadRegistration(const wchar_t *value) {
  DWORD bytes = 0;
  if (RegGetValueW(HKEY_CURRENT_USER, kUninstallKey, value, RRF_RT_REG_SZ,
                   nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
      bytes < sizeof(wchar_t) || bytes > 65536)
    return {};
  std::wstring result(bytes / sizeof(wchar_t), L'\0');
  if (RegGetValueW(HKEY_CURRENT_USER, kUninstallKey, value, RRF_RT_REG_SZ,
                   nullptr, result.data(), &bytes) != ERROR_SUCCESS)
    return {};
  while (!result.empty() && result.back() == L'\0')
    result.pop_back();
  return result;
}

bool WriteRegistration(const wchar_t *name, const std::wstring &value) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, KEY_SET_VALUE, &key) !=
      ERROR_SUCCESS)
    return false;
  const auto result = RegSetValueExW(
      key, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value.c_str()),
      static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(key);
  return result == ERROR_SUCCESS;
}
void RetryOldVersionCleanup(const ChromiumInstallation &installed) {
  const auto setup =
      installed.application / installed.version / "Installer/setup.exe";
  const auto setup_lock =
      VerifyNativeSetup(setup, setup.parent_path() / "KryptonPackage.manifest");
  if (!setup_lock)
    return;
  // Chromium owns old-version selection, in-use checks, and bounded retries.
  // Run independently so a locked old version cannot delay setup completion.
  auto command = std::format(L"\"{}\" --delete-old-versions --verbose-logging",
                             setup.wstring());
  STARTUPINFOW startup{sizeof(STARTUPINFOW)};
  PROCESS_INFORMATION process{};
  if (CreateProcessW(setup.c_str(), command.data(), nullptr, nullptr, FALSE,
                     CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr,
                     installed.application.c_str(), &startup, &process)) {
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
  }
}

std::string PackageVersion(const std::filesystem::path &package) {
  wchar_t value[64]{};
  const auto metadata = package / "chromium-install.manifest";
  const auto length = GetPrivateProfileStringW(
      L"Chromium", L"BrowserVersion", L"", value, 64, metadata.c_str());
  std::string version;
  for (const wchar_t ch : std::wstring_view(value, length)) {
    if (ch > 127)
      return {};
    version.push_back(static_cast<char>(ch));
  }
  return IsChromiumVersion(version) ? version : std::string{};
}

bool IsLegacyVersionMigration(const std::filesystem::path &package,
                              std::string_view version) {
  const auto installed = FindChromiumInstallation();
  const auto target = ParseBrowserVersion(version);
  // The earlier Krypton release used 1.16.0.1 for Chromium 152. Current
  // Helium-based builds use 0.x product versions. Permit only this known
  // numbering transition, and only when the signed engine version increases.
  if (!installed || installed->version != "1.16.0.1" || !target ||
      (*target)[0] != 0 || (*target)[1] < 20)
    return false;
  const auto marker = package / "KryptonPackage.manifest";
  auto signature = marker;
  signature += ".sig";
  if (!VerifyReleaseFile(marker, signature))
    return false;
  wchar_t engine[64]{};
  GetPrivateProfileStringW(L"Package", L"EngineVersion", L"", engine, 64,
                           marker.c_str());
  std::string engine_text;
  for (const wchar_t ch : std::wstring_view(engine)) {
    if (ch > 127)
      return false;
    engine_text.push_back(static_cast<char>(ch));
  }
  const auto target_engine = ParseBrowserVersion(engine_text);
  if (!target_engine)
    return false;

  const auto executable = installed->application / "chrome.exe";
  DWORD ignored{};
  const auto bytes = GetFileVersionInfoSizeW(executable.c_str(), &ignored);
  if (!bytes || bytes > 1024 * 1024)
    return false;
  std::vector<std::byte> information(bytes);
  if (!GetFileVersionInfoW(executable.c_str(), 0, bytes, information.data()))
    return false;
  void *value{};
  UINT length{};
  if (!VerQueryValueW(information.data(), L"\\", &value, &length) || !value ||
      length < sizeof(VS_FIXEDFILEINFO))
    return false;
  const auto &fixed = *static_cast<const VS_FIXEDFILEINFO *>(value);
  if (fixed.dwSignature != 0xFEEF04BD)
    return false;
  const BrowserVersion installed_file{
      HIWORD(fixed.dwFileVersionMS), LOWORD(fixed.dwFileVersionMS),
      HIWORD(fixed.dwFileVersionLS), LOWORD(fixed.dwFileVersionLS)};
  const BrowserVersion installed_engine{
      HIWORD(fixed.dwProductVersionMS), LOWORD(fixed.dwProductVersionMS),
      HIWORD(fixed.dwProductVersionLS), LOWORD(fixed.dwProductVersionLS)};
  const auto registered = ParseBrowserVersion(installed->version);
  return registered && installed_file == *registered &&
         installed_engine[0] == 152 && *target_engine > installed_engine;
}

} // namespace

bool IsChromiumVersion(std::string_view version) noexcept {
  for (unsigned index = 0; index < 4; ++index) {
    const auto dot = version.find('.');
    if ((index == 3) != (dot == std::string_view::npos))
      return false;
    const auto part = version.substr(0, dot);
    if (part.empty())
      return false;
    std::uint32_t number = 0;
    const auto [end, error] =
        std::from_chars(part.data(), part.data() + part.size(), number);
    if (error != std::errc{} || end != part.data() + part.size() ||
        number > 65535)
      return false;
    if (index < 3)
      version.remove_prefix(dot + 1);
  }
  return true;
}

std::optional<ChromiumInstallation> FindChromiumInstallation() {
  const auto location = ReadRegistration(L"InstallLocation");
  const auto registered_version = ReadRegistration(L"DisplayVersion");
  std::string version;
  for (const auto ch : registered_version) {
    if (ch > 127)
      return std::nullopt;
    version.push_back(static_cast<char>(ch));
  }
  const std::filesystem::path application(location);
  std::error_code error;
  if (!application.is_absolute() || application.filename() != L"Application" ||
      !IsChromiumVersion(version) ||
      !std::filesystem::is_regular_file(application / "chrome.exe", error))
    return std::nullopt;
  return ChromiumInstallation{application, version};
}

std::filesystem::path DefaultChromiumApplicationDirectory() {
  if (const auto installed = FindChromiumInstallation())
    return installed->application;
  PWSTR directory = nullptr;
  if (FAILED(
          SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &directory)))
    return {};
  const auto result =
      std::filesystem::path(directory) / "Krypton/Krypton/Application";
  CoTaskMemFree(directory);
  return result;
}

std::wstring
ChromiumInstallArguments(const std::filesystem::path &archive,
                         const std::filesystem::path &preferences) {
  return std::format(L"--uncompressed-archive=\"{}\" --installerdata=\"{}\" "
                     L"--do-not-launch-chrome --verbose-logging",
                     archive.wstring(), preferences.wstring());
}

std::wstring ChromiumUninstallArguments() {
  // Keep User Data. Deleting profiles requires a separate explicit choice.
  return L"--uninstall --force-uninstall --verbose-logging";
}

std::expected<std::filesystem::path, std::string>
InstallChromiumBrowser(const std::filesystem::path &package,
                       const ChromiumInstallOptions &options) {
  const auto version = PackageVersion(package);
  const auto setup = package / "setup.exe";
  const auto archive = package / "helium.7z";
  if (version.empty() || !std::filesystem::is_regular_file(setup) ||
      !std::filesystem::is_regular_file(archive))
    return std::unexpected(
        "The native Chromium installation package is incomplete.");
  auto setup_lock =
      VerifyNativeSetup(setup, package / "KryptonPackage.manifest");
  if (!setup_lock)
    return std::unexpected(
        "Krypton's native setup signature could not be verified.");
  const auto preferences = package / "initial_preferences.json";
  const bool legacy_version_migration =
      IsLegacyVersionMigration(package, version);
  {
    std::ofstream file(preferences, std::ios::binary);
    file << std::format(
        R"({{"distribution":{{"do_not_launch_chrome":true,"do_not_create_desktop_shortcut":{},"do_not_create_any_shortcuts":{},"allow_downgrade":{}}}}})",
        !options.desktop_shortcut,
        !options.start_menu_shortcut && !options.desktop_shortcut,
        legacy_version_migration);
    if (!file)
      return std::unexpected(
          "Could not write the browser installation preferences.");
  }
  std::uint32_t exit_code = 0;
  if (!PayloadRunner::RunExe(setup,
                             ChromiumInstallArguments(archive, preferences),
                             &exit_code, std::move(setup_lock))) {
    switch (exit_code) {
    case 4:
      return std::unexpected("A newer version of Krypton is already installed. "
                             "The same version can be reinstalled for repair.");
    case 31:
      return std::unexpected("Close all Krypton windows, then run setup again "
                             "to repair this version.");
    case 12:
    case 13:
    case 57:
      return std::unexpected(std::format(
          "The native installer could not unpack "
          "the browser package (code {}). Download the latest Krypton setup.",
          exit_code));
    case 60:
      return std::unexpected("Another Krypton installation is running. Close "
                             "the other setup window and try again.");
    default:
      return std::unexpected(std::format("Krypton's native installer could not "
                                         "finish (code {}).",
                                         exit_code));
    }
  }
  const auto installed = FindChromiumInstallation();
  if (!installed || installed->version != version)
    return std::unexpected(
        "The native installer did not register the expected browser version.");
  const auto uninstaller =
      installed->application / version / "Installer/KryptonUninstaller.exe";
  if (!std::filesystem::is_regular_file(uninstaller))
    return std::unexpected(
        "The native installation is missing KryptonUninstaller.exe.");
  if (!WriteRegistration(
          L"UninstallString",
          std::format(L"\"{}\" --uninstall", uninstaller.wstring())) ||
      !WriteRegistration(
          L"QuietUninstallString",
          std::format(L"\"{}\" --uninstall --silent", uninstaller.wstring())))
    return std::unexpected("Could not register Krypton's uninstaller.");
  RetryOldVersionCleanup(*installed);
  return installed->application / "chrome.exe";
}

std::expected<void, std::string>
UninstallChromiumBrowser(const std::filesystem::path &requested_root) {
  if (IsRegisteredLegacyInstallation(requested_root))
    return UninstallLegacyBrowser(requested_root);
  const auto installed = FindChromiumInstallation();
  if (!installed || _wcsicmp(installed->application.lexically_normal().c_str(),
                             requested_root.lexically_normal().c_str()) != 0)
    return std::unexpected(
        "No standard Krypton installation was found for this user.");
  const auto setup =
      installed->application / installed->version / "Installer/setup.exe";
  auto setup_lock =
      VerifyNativeSetup(setup, setup.parent_path() / "KryptonPackage.manifest");
  if (!setup_lock)
    return std::unexpected(
        "Krypton's native uninstaller signature could not be verified.");
  std::uint32_t exit_code = 0;
  if (!PayloadRunner::RunExe(setup, ChromiumUninstallArguments(), &exit_code,
                             std::move(setup_lock)))
    return std::unexpected(
        std::format("Krypton could not be removed (code {}). Close its browser "
                    "windows and try again.",
                    exit_code));
  return {};
}

} // namespace kr::updater::windows
