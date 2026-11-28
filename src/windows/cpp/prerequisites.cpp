#include "krypton/updater/windows/prerequisites.hpp"

#include "krypton/updater/windows/authenticode_verifier.hpp"

// clang-format off
#include <windows.h>
#include <appmodel.h>
#include <objbase.h>
#include <roapi.h>
#include <shellapi.h>
// clang-format on

#include <charconv>
#include <format>
#include <iterator>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <vector>

namespace kr::updater::windows {
namespace {

using Version = std::array<std::uint32_t, 4>;

struct CloseHandleDeleter {
  void operator()(HANDLE handle) const noexcept { CloseHandle(handle); }
};
using Handle = std::unique_ptr<void, CloseHandleDeleter>;

struct FreeLibraryDeleter {
  void operator()(HMODULE module) const noexcept { FreeLibrary(module); }
};
using Module =
    std::unique_ptr<std::remove_pointer_t<HMODULE>, FreeLibraryDeleter>;

struct RuntimeApartment {
  RuntimeApartment() = default;
  RuntimeApartment(const RuntimeApartment &) = delete;
  RuntimeApartment &operator=(const RuntimeApartment &) = delete;
  RuntimeApartment(RuntimeApartment &&) = delete;
  RuntimeApartment &operator=(RuntimeApartment &&) = delete;
  HRESULT result = RoInitialize(RO_INIT_MULTITHREADED);
  ~RuntimeApartment() {
    if (SUCCEEDED(result))
      RoUninitialize();
  }
};

std::string ProfileValue(const std::filesystem::path &path,
                         const wchar_t *section, const wchar_t *key) {
  wchar_t value[128]{};
  const auto length = GetPrivateProfileStringW(
      section, key, L"", value, static_cast<DWORD>(std::size(value)),
      path.c_str());
  if (length == 0 || length >= std::size(value) - 1)
    return {};
  std::string result;
  result.reserve(length);
  for (const auto ch : std::wstring_view(value, length)) {
    if (ch > 127)
      return {};
    result.push_back(static_cast<char>(ch));
  }
  return result;
}

std::optional<std::uint32_t> ParseNumber(std::string_view text, int base = 10) {
  if (text.empty())
    return std::nullopt;
  std::uint32_t result = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), result, base);
  if (error != std::errc{} || end != text.data() + text.size())
    return std::nullopt;
  return result;
}

std::optional<Version> ParseVersion(std::string_view text) {
  Version result{};
  for (std::size_t index = 0; index < result.size(); ++index) {
    const auto separator = text.find('.');
    const bool last = index + 1 == result.size();
    if (last != (separator == std::string_view::npos))
      return std::nullopt;
    const auto component = ParseNumber(text.substr(0, separator));
    if (!component || *component > 65535)
      return std::nullopt;
    result[index] = *component;
    if (!last)
      text.remove_prefix(separator + 1);
  }
  return result;
}

std::optional<Version> FileVersion(const std::filesystem::path &path) {
  DWORD ignored = 0;
  const auto size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
  if (size == 0)
    return std::nullopt;
  std::vector<std::uint8_t> buffer(size);
  if (!GetFileVersionInfoW(path.c_str(), 0, size, buffer.data()))
    return std::nullopt;
  VS_FIXEDFILEINFO *info = nullptr;
  UINT length = 0;
  if (!VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<void **>(&info),
                      &length) ||
      length < sizeof(VS_FIXEDFILEINFO) || info->dwSignature != 0xFEEF04BD)
    return std::nullopt;
  return Version{HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                 HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS)};
}

bool VisualCppAvailable(const Version &minimum) {
  wchar_t directory[MAX_PATH]{};
  const auto length = GetSystemDirectoryW(directory, MAX_PATH);
  if (length == 0 || length >= MAX_PATH)
    return false;
  for (const auto *name :
       {L"msvcp140.dll", L"vcruntime140.dll", L"vcruntime140_1.dll"}) {
    const auto version = FileVersion(std::filesystem::path(directory) / name);
    if (!version || *version < minimum)
      return false;
  }
  return true;
}

bool WindowsRuntimeAvailable(const std::filesystem::path &payload,
                             std::uint32_t release, const Version &minimum) {
  const RuntimeApartment apartment;
  if (FAILED(apartment.result) && apartment.result != RPC_E_CHANGED_MODE)
    return false;
  const auto bootstrap = payload / L"Microsoft.WindowsAppRuntime.Bootstrap.dll";
  Module module(LoadLibraryExW(bootstrap.c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                   LOAD_LIBRARY_SEARCH_DEFAULT_DIRS));
  if (!module)
    return false;
  using Initialize = HRESULT(WINAPI *)(UINT32, PCWSTR, PACKAGE_VERSION, UINT32);
  using Shutdown = void(WINAPI *)();
  const auto initialize = reinterpret_cast<Initialize>(
      GetProcAddress(module.get(), "MddBootstrapInitialize2"));
  const auto shutdown = reinterpret_cast<Shutdown>(
      GetProcAddress(module.get(), "MddBootstrapShutdown"));
  PACKAGE_VERSION version{};
  version.Major = static_cast<UINT16>(minimum[0]);
  version.Minor = static_cast<UINT16>(minimum[1]);
  version.Build = static_cast<UINT16>(minimum[2]);
  version.Revision = static_cast<UINT16>(minimum[3]);
  if (!initialize || !shutdown ||
      FAILED(initialize(release, nullptr, version, 0)))
    return false;
  shutdown();
  return true;
}

bool NativeAvailable(void *, const PrerequisiteRequirement &requirement,
                     const std::filesystem::path &payload) {
  return requirement.kind == PrerequisiteKind::kVisualCpp
             ? VisualCppAvailable(requirement.minimum_version)
             : WindowsRuntimeAvailable(payload, requirement.runtime_major_minor,
                                       requirement.minimum_version);
}

std::string_view Name(PrerequisiteKind kind) {
  return kind == PrerequisiteKind::kVisualCpp ? "Microsoft Visual C++ x64"
                                              : "Windows App Runtime";
}

std::expected<std::uint32_t, std::string>
InstallPrerequisite(const PrerequisiteRequirement &requirement, bool silent,
                    const PrerequisiteProgress &progress) {
  // Deny writes and replacement between signature verification and execution,
  // including while an elevated Microsoft installer is running.
  Handle source(CreateFileW(requirement.installer.c_str(), GENERIC_READ,
                            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr));
  if (source.get() == INVALID_HANDLE_VALUE) {
    static_cast<void>(source.release());
    return std::unexpected(std::format("Could not open the {} installer ({}).",
                                       Name(requirement.kind), GetLastError()));
  }
  const auto signature = AuthenticodeVerifier::Verify(
      requirement.installer, {.expected_publisher = L"Microsoft Corporation",
                              .allow_unsigned_dev = false,
                              .require_trusted_signature = true});
  if (signature != SignatureStatus::kValidAndTrusted)
    return std::unexpected(std::format(
        "The {} installer does not have a trusted Microsoft signature: {}.",
        Name(requirement.kind), SignatureStatusToString(signature)));

  const std::wstring arguments =
      requirement.kind == PrerequisiteKind::kVisualCpp
          ? L"/install /quiet /norestart"
          : L"--quiet";
  auto command =
      std::format(L"\"{}\" {}", requirement.installer.wstring(), arguments);
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  HANDLE process_handle = nullptr;
  if (CreateProcessW(requirement.installer.c_str(), command.data(), nullptr,
                     nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                     &startup, &process)) {
    Handle thread(process.hThread);
    process_handle = process.hProcess;
  } else {
    const auto error = GetLastError();
    if (error != ERROR_ELEVATION_REQUIRED || silent)
      return std::unexpected(std::format(
          "Could not start {} ({}). Run this installer interactively if "
          "administrator approval is required.",
          Name(requirement.kind), error));

    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SHELLEXECUTEINFOW execute{};
    execute.cbSize = sizeof(execute);
    execute.fMask =
        SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    execute.lpVerb = L"runas";
    execute.lpFile = requirement.installer.c_str();
    execute.lpParameters = arguments.c_str();
    execute.nShow = SW_HIDE;
    const bool started = ShellExecuteExW(&execute) != FALSE;
    const auto launch_error = started ? ERROR_SUCCESS : GetLastError();
    if (SUCCEEDED(initialized))
      CoUninitialize();
    if (!started)
      return std::unexpected(std::format(
          "Administrator approval for {} was canceled or failed ({}).",
          Name(requirement.kind), launch_error));
    process_handle = execute.hProcess;
  }

  Handle child(process_handle);
  constexpr DWORD kProgressIntervalMs = 15'000;
  DWORD wait = WAIT_FAILED;
  if (child) {
    unsigned elapsed_seconds = 0;
    while ((wait = WaitForSingleObject(child.get(), kProgressIntervalMs)) ==
           WAIT_TIMEOUT) {
      elapsed_seconds += kProgressIntervalMs / 1000;
      if (progress)
        progress(std::format("Waiting for {} setup ({} seconds).",
                             Name(requirement.kind), elapsed_seconds));
    }
  }
  DWORD exit_code = 0;
  if (wait != WAIT_OBJECT_0 || !GetExitCodeProcess(child.get(), &exit_code))
    return std::unexpected(std::format("Could not wait for {} ({}).",
                                       Name(requirement.kind), GetLastError()));
  return exit_code;
}

} // namespace

std::expected<void, std::string>
EnsureBrowserPrerequisites(const std::filesystem::path &payload_directory,
                           bool silent,
                           const PrerequisiteOperations *operations,
                           const PrerequisiteProgress &progress) {
  std::error_code error;
  const auto payload =
      std::filesystem::absolute(payload_directory, error).lexically_normal();
  if (error)
    return std::unexpected("The prerequisite directory could not be resolved.");
  const auto metadata = payload / "prerequisites/requirements.manifest";
  if (!std::filesystem::is_regular_file(metadata, error)) {
    // Retain support for the engine's minimal, non-WinUI archive fixtures.
    if (!std::filesystem::exists(payload / "krypton_ui.dll", error))
      return {};
    return std::unexpected(
        "The package is missing its Windows prerequisite requirements.");
  }
  const auto minimum =
      ParseVersion(ProfileValue(metadata, L"VisualCpp", L"MinimumVersion"));
  const auto release =
      ParseNumber(ProfileValue(metadata, L"WindowsAppRuntime", L"MajorMinor"));
  const auto runtime_minimum = ParseVersion(
      ProfileValue(metadata, L"WindowsAppRuntime", L"MinimumVersion"));
  if (!minimum || !release || *release == 0 || !runtime_minimum)
    return std::unexpected("The package has invalid Windows prerequisites.");

  const std::array requirements{
      PrerequisiteRequirement{PrerequisiteKind::kVisualCpp,
                              payload / "prerequisites/vc_redist.x64.exe",
                              *minimum, 0},
      PrerequisiteRequirement{
          PrerequisiteKind::kWindowsAppRuntime,
          payload / "prerequisites/WindowsAppRuntimeInstall-x64.exe",
          *runtime_minimum, *release}};
  if (operations && (!operations->is_available || !operations->install))
    return std::unexpected("The prerequisite installer is unavailable.");
  const auto available = [&](const PrerequisiteRequirement &requirement) {
    return operations ? operations->is_available(operations->context,
                                                 requirement, payload)
                      : NativeAvailable(nullptr, requirement, payload);
  };
  for (const auto &requirement : requirements) {
    if (!std::filesystem::is_regular_file(requirement.installer, error))
      return std::unexpected(
          std::format("The package is missing {}.", Name(requirement.kind)));
    if (available(requirement))
      continue;
    if (progress)
      progress(std::format("Installing {}. Approve Windows setup if prompted.",
                           Name(requirement.kind)));
    const auto result =
        operations
            ? operations->install(operations->context, requirement, silent)
            : InstallPrerequisite(requirement, silent, progress);
    if (!result)
      return std::unexpected(result.error());

    const auto is_code = [&](std::uint32_t code) {
      return *result == code ||
             *result == static_cast<std::uint32_t>(HRESULT_FROM_WIN32(code));
    };
    // The installer may request a restart for other programs even though
    // the browser's required runtime is already usable.
    const bool restart_initiated = is_code(ERROR_SUCCESS_REBOOT_INITIATED);
    if (!restart_initiated && available(requirement))
      continue;
    if (restart_initiated || is_code(ERROR_SUCCESS_REBOOT_REQUIRED))
      return std::unexpected(std::format(
          "Restart Windows to finish installing {}, then reopen Krypton Setup.",
          Name(requirement.kind)));
    return std::unexpected(std::format(
        "{} is still unavailable after setup (code 0x{:08X}). The browser "
        "has not been activated.",
        Name(requirement.kind), *result));
  }
  return {};
}

std::expected<void, std::string>
CheckBrowserPrerequisites(const std::filesystem::path &payload) {
  const PrerequisiteOperations probe{
      nullptr, &NativeAvailable,
      [](void *, const PrerequisiteRequirement &requirement,
         bool) -> std::expected<std::uint32_t, std::string> {
        return std::unexpected(
            std::format("{} needs installation.", Name(requirement.kind)));
      }};
  return EnsureBrowserPrerequisites(payload, true, &probe);
}

} // namespace kr::updater::windows
