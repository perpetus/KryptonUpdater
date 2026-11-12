#include "krypton/updater/windows/os_capability.hpp"

// clang-format off
#include <windows.h>
#include <winternl.h>
// clang-format on
#include <intrin.h>

#include <string>

namespace kr::updater::windows {

namespace {

using RtlGetVersionFn = NTSTATUS(NTAPI *)(PRTL_OSVERSIONINFOW);

RTL_OSVERSIONINFOW QueryRealOSVersion() noexcept {
  RTL_OSVERSIONINFOW osvi{sizeof(RTL_OSVERSIONINFOW)};
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (ntdll) {
    auto fn = reinterpret_cast<RtlGetVersionFn>(
        GetProcAddress(ntdll, "RtlGetVersion"));
    if (fn) {
      fn(&osvi);
      return osvi;
    }
  }
#pragma warning(push)
#pragma warning(disable : 4996)
  GetVersionExW(reinterpret_cast<OSVERSIONINFOW *>(&osvi));
#pragma warning(pop)
  return osvi;
}

} // namespace

uint32_t GetWindowsBuildNumber() noexcept {
  static const uint32_t build = []() -> uint32_t {
    auto osvi = QueryRealOSVersion();
    return osvi.dwBuildNumber;
  }();
  return build;
}

SystemCapabilities QuerySystemCapabilities() noexcept {
  const auto version = QueryRealOSVersion();
  SystemCapabilities result;
  result.windows_major = version.dwMajorVersion;
  result.windows_build = version.dwBuildNumber;
#if defined(_M_X64) || defined(_M_IX86)
  int cpu[4]{};
  __cpuid(cpu, 0);
  const auto maximum = static_cast<std::uint32_t>(cpu[0]);
  if (maximum >= 1) {
    __cpuidex(cpu, 1, 0);
    result.cpu_leaf1_ecx = static_cast<std::uint32_t>(cpu[2]);
    constexpr auto kXsave = 1u << 26;
    constexpr auto kOsXsave = 1u << 27;
    if ((result.cpu_leaf1_ecx & (kXsave | kOsXsave)) == (kXsave | kOsXsave))
      result.enabled_xstate = _xgetbv(0);
  }
  if (maximum >= 7) {
    __cpuidex(cpu, 7, 0);
    result.cpu_leaf7_ebx = static_cast<std::uint32_t>(cpu[1]);
  }
  __cpuid(cpu, static_cast<int>(0x80000000u));
  if (static_cast<std::uint32_t>(cpu[0]) >= 0x80000001u) {
    __cpuid(cpu, static_cast<int>(0x80000001u));
    result.cpu_extended_leaf1_ecx = static_cast<std::uint32_t>(cpu[2]);
  }
#endif
  return result;
}

bool IsOSSupportedForKrypton() noexcept {
  // Krypton requires Windows 10 build 18362 (version 1903) or later
  auto osvi = QueryRealOSVersion();
  if (osvi.dwMajorVersion > kMinimumWindowsMajor)
    return true;
  if (osvi.dwMajorVersion == kMinimumWindowsMajor &&
      osvi.dwBuildNumber >= kMinimumWindowsBuild)
    return true;
  return false;
}

bool IsSystemXamlSupported() noexcept {
  // DesktopWindowXamlSource requires Windows 10 build 18362+
  if (!IsOSSupportedForKrypton())
    return false;

  // Verify that Windows.UI.Xaml.dll exists in System32
  WCHAR sys_path[MAX_PATH];
  if (GetSystemDirectoryW(sys_path, MAX_PATH)) {
    std::wstring xaml_path = std::wstring(sys_path) + L"\\Windows.UI.Xaml.dll";
    DWORD attrs = GetFileAttributesW(xaml_path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES ||
        (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
      return false;
    }
  }
  return true;
}

} // namespace kr::updater::windows
