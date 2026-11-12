#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace kr::updater::windows {

struct ChromiumInstallation {
  std::filesystem::path application;
  std::string version;
};

[[nodiscard]] std::optional<ChromiumInstallation> FindChromiumInstallation();
[[nodiscard]] std::filesystem::path DefaultChromiumApplicationDirectory();

struct ChromiumInstallOptions {
  bool desktop_shortcut = true;
  bool start_menu_shortcut = true;
};

// Use Krypton's own Chromium setup for layout, upgrades, registration and
// uninstall. Prerequisites and the outer archive are verified by the caller.
[[nodiscard]] std::expected<std::filesystem::path, std::string>
InstallChromiumBrowser(const std::filesystem::path &package,
                       const ChromiumInstallOptions &options);

[[nodiscard]] std::expected<void, std::string>
UninstallChromiumBrowser(const std::filesystem::path &requested_root);

// Pure validation/command construction is shared with the isolated tests.
[[nodiscard]] bool IsChromiumVersion(std::string_view version) noexcept;
[[nodiscard]] std::wstring
ChromiumInstallArguments(const std::filesystem::path &archive,
                         const std::filesystem::path &preferences);
[[nodiscard]] std::wstring ChromiumUninstallArguments();

} // namespace kr::updater::windows
