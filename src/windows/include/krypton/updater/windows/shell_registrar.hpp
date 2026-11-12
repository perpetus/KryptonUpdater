#pragma once

#include <filesystem>
#include <string>

#include "krypton/updater/semver.hpp"

namespace kr::updater::windows {

class ShellRegistrar {
 public:
  // Registers Krypton as a valid web browser in HKCU (no UAC needed)
  [[nodiscard]] static bool RegisterBrowser(
      const std::filesystem::path& install_root,
      const SemVer& version);

  // Creates the Start Menu shortcut with AppUserModelID
  [[nodiscard]] static bool CreateStartMenuShortcut(
      const std::filesystem::path& target_exe,
      const std::wstring& app_user_model_id = L"Krypton.Browser");

  // Creates the Desktop shortcut with AppUserModelID
  [[nodiscard]] static bool CreateDesktopShortcut(
      const std::filesystem::path& target_exe,
      const std::wstring& app_user_model_id = L"Krypton.Browser");

  // Registers uninstaller in Windows Add/Remove Programs
  [[nodiscard]] static bool RegisterUninstaller(
      const std::filesystem::path& install_root,
      const SemVer& version);

  // Registers the zero-overhead scheduled task for daily background updates
  [[nodiscard]] static bool RegisterUpdateTask(
      const std::filesystem::path& updater_exe);

  // Unregisters everything on uninstall
  static void UnregisterAll(const std::filesystem::path& install_root);
};

}  // namespace kr::updater::windows
