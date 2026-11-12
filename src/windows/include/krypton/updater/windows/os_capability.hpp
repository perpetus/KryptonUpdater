#pragma once

#include "krypton/updater/compatibility.hpp"
#include <cstdint>

namespace kr::updater::windows {

// XGETBV is only executed after verifying CPU and OS XSAVE support.
[[nodiscard]] SystemCapabilities QuerySystemCapabilities() noexcept;

// Checks whether the OS meets the minimum requirements for Krypton
// (Windows 10 version 1903 build 18362 or later).
[[nodiscard]] bool IsOSSupportedForKrypton() noexcept;

// Checks whether the in-box System XAML Island hosting API
// (Windows.UI.Xaml.Hosting.DesktopWindowXamlSource) is supported.
[[nodiscard]] bool IsSystemXamlSupported() noexcept;

// Returns the exact Windows OS build number (e.g. 19045, 22631, etc.)
[[nodiscard]] uint32_t GetWindowsBuildNumber() noexcept;

} // namespace kr::updater::windows
