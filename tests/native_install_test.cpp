#include "krypton/updater/windows/chromium_install.hpp"
#include "krypton/updater/windows/legacy_uninstall.hpp"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

namespace {
namespace fs = std::filesystem;
using namespace kr::updater::windows;
int failures = 0;

void Check(bool value, std::string_view message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void Write(const fs::path &path) {
  fs::create_directories(path.parent_path());
  std::ofstream(path) << "test data";
}

void TestNativeCommands() {
  Check(IsChromiumVersion("0.20.1.1") && IsChromiumVersion("1.16.0.1"),
        "Native four-part Chromium versions are supported");
  for (const auto version : {"1.2.3", "1.2.3.4.5", "1..2.3", "../2.3.4",
                             "1.2.3.65536", "1.2.3.4 --uninstall"})
    Check(!IsChromiumVersion(version),
          "Unsafe or malformed native versions are rejected");
  const auto command = ChromiumInstallArguments(L"C:/a path/helium.7z",
                                                L"C:/a path/preferences.json");
  Check(command.find(L"--uncompressed-archive=\"C:/a path/helium.7z\"") !=
            std::wstring::npos,
        "Native setup receives a quoted uncompressed archive path");
  Check(command.find(L"--do-not-launch-chrome") != std::wstring::npos &&
            command.find(L"--allow-downgrade") == std::wstring::npos,
        "The frontend controls launch and does not force a downgrade");
  const auto uninstall = ChromiumUninstallArguments();
  Check(uninstall.find(L"--uninstall") != std::wstring::npos &&
            uninstall.find(L"--force-uninstall") != std::wstring::npos &&
            uninstall.find(L"delete-profile") == std::wstring::npos,
        "The shared uninstall UI owns confirmation and preserves profile data");
}

void TestLegacyFileRemoval(const fs::path &workspace) {
  const auto root = workspace / "legacy";
  Write(root / "krypton.exe");
  Write(root / "KryptonUpdater.exe");
  Write(root / "KryptonUninstaller.exe");
  Write(root / "app-0.20.1-dev.1/chrome.exe");
  Write(root / "app-0.20.1-dev.1/assets/settings/image.png");
  Write(root / "User Data/Default/Bookmarks");
  Write(root / "app-0.20.1-dev.1/User Data/Default/Preferences");
  Write(root / "personal-file.txt");
  Write(root / "app-not-a-version/personal-file.txt");
  const auto result = RemoveLegacyProgramFiles(root);
  Check(result.has_value(),
        "Legacy managed program files can be removed in isolation");
  Check(!fs::exists(root / "krypton.exe") &&
            !fs::exists(root / "app-0.20.1-dev.1/chrome.exe"),
        "Legacy executables and managed assets are removed");
  Check(
      fs::exists(root / "User Data/Default/Bookmarks") &&
          fs::exists(root / "app-0.20.1-dev.1/User Data/Default/Preferences"),
      "Profile data is preserved at both root and portable-profile locations");
  Check(fs::exists(root / "personal-file.txt") &&
            fs::exists(root / "app-not-a-version/personal-file.txt"),
        "Unknown files and directories are preserved");
  Check(!RemoveLegacyProgramFiles(fs::path("relative")) &&
            !RemoveLegacyProgramFiles(root.root_path()),
        "Relative paths and filesystem roots cannot be removed");
}

} // namespace

int main() {
  TestNativeCommands();
  const auto workspace =
      fs::temp_directory_path() /
      ("KryptonNativeInstallTest-" + std::to_string(GetCurrentProcessId()));
  fs::create_directories(workspace);
  TestLegacyFileRemoval(workspace);
  if (workspace.parent_path() == fs::temp_directory_path() &&
      workspace.filename().string().starts_with("KryptonNativeInstallTest-")) {
    std::error_code ignored;
    fs::remove_all(workspace, ignored);
  }
  return failures ? 1 : 0;
}
