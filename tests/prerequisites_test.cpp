#include "krypton/updater/windows/prerequisites.hpp"

#include "krypton/updater/windows/authenticode_verifier.hpp"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <vector>

namespace {
using namespace kr::updater::windows;
namespace fs = std::filesystem;
int failures = 0;

void Check(bool value, std::string_view message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void Write(const fs::path &path, std::string_view contents) {
  fs::create_directories(path.parent_path());
  std::ofstream file(path, std::ios::binary);
  file << contents;
}

fs::path Payload(const fs::path &root, std::string_view name,
                 std::string_view vc_version = "14.51.36247.0") {
  const auto path = root / name;
  Write(path / "krypton_ui.dll", "test UI");
  Write(path / "prerequisites/vc_redist.x64.exe", "test VC installer");
  Write(path / "prerequisites/WindowsAppRuntimeInstall-x64.exe",
        "test Windows installer");
  Write(path / "prerequisites/requirements.manifest",
        std::string("[WindowsAppRuntime]\nMajorMinor=131077\n"
                    "MinimumVersion=2.5.1.0\n[VisualCpp]\nMinimumVersion=") +
            std::string(vc_version) + "\n");
  return path;
}

struct FakePlatform {
  bool vc_ready = false;
  bool windows_ready = false;
  bool succeeds = true;
  bool becomes_ready = true;
  bool received_silent = false;
  std::uint32_t exit_code = 0;
  std::vector<PrerequisiteKind> installs;

  static bool Available(void *context,
                        const PrerequisiteRequirement &requirement,
                        const fs::path &) {
    auto &self = *static_cast<FakePlatform *>(context);
    if (requirement.kind == PrerequisiteKind::kVisualCpp) {
      Check(requirement.minimum_version ==
                std::array<std::uint32_t, 4>{14, 51, 36247, 0},
            "VC check uses the browser's minimum runtime version");
      return self.vc_ready;
    }
    Check(
        requirement.runtime_major_minor == 0x00020005 &&
            requirement.minimum_version ==
                std::array<std::uint32_t, 4>{2, 5, 1, 0},
        "Windows runtime check uses the matching release and package version");
    return self.windows_ready;
  }

  static std::expected<std::uint32_t, std::string>
  Install(void *context, const PrerequisiteRequirement &requirement,
          bool silent) {
    auto &self = *static_cast<FakePlatform *>(context);
    self.installs.push_back(requirement.kind);
    self.received_silent = silent;
    if (!self.succeeds)
      return std::unexpected("Microsoft prerequisite installation failed");
    if (self.becomes_ready) {
      if (requirement.kind == PrerequisiteKind::kVisualCpp)
        self.vc_ready = true;
      else
        self.windows_ready = true;
    }
    return self.exit_code;
  }

  PrerequisiteOperations Operations() { return {this, &Available, &Install}; }
};

void TestRequirements(const fs::path &root) {
  FakePlatform platform;
  const auto operations = platform.Operations();
  Check(EnsureBrowserPrerequisites(root / "legacy-fixture", true, &operations)
            .has_value(),
        "A minimal non-WinUI fixture has no framework prerequisite");
  Write(root / "missing/krypton_ui.dll", "test UI");
  Check(!EnsureBrowserPrerequisites(root / "missing", true, &operations),
        "WinUI packages cannot omit their prerequisite requirements");
  Check(!EnsureBrowserPrerequisites(Payload(root, "invalid-version", "14.51"),
                                    true, &operations),
        "An invalid required version fails before executing a prerequisite");
  Check(!EnsureBrowserPrerequisites(
            Payload(root, "overflow-version", "14.51.999999999999.0"), true,
            &operations),
        "Overflowing version components are rejected");
  const auto missing_installer = Payload(root, "missing-installer");
  fs::remove(missing_installer / "prerequisites/vc_redist.x64.exe");
  Check(!EnsureBrowserPrerequisites(missing_installer, true, &operations),
        "Incomplete prerequisite payloads fail closed");
  Check(platform.installs.empty(),
        "Invalid prerequisite metadata never starts a process");
}

void TestInstallation(const fs::path &root) {
  {
    FakePlatform platform;
    platform.vc_ready = platform.windows_ready = true;
    const auto operations = platform.Operations();
    Check(EnsureBrowserPrerequisites(Payload(root, "already-installed"), false,
                                     &operations)
              .has_value(),
          "Already usable runtimes are accepted");
    Check(platform.installs.empty(), "Usable runtimes are not reinstalled");
  }
  {
    FakePlatform platform;
    const auto operations = platform.Operations();
    Check(EnsureBrowserPrerequisites(Payload(root, "missing-both"), true,
                                     &operations)
              .has_value(),
          "Missing runtimes are installed and rechecked");
    Check(platform.installs ==
              std::vector{PrerequisiteKind::kVisualCpp,
                          PrerequisiteKind::kWindowsAppRuntime},
          "VC++ is prepared before bootstrapping Windows App Runtime");
    Check(platform.received_silent,
          "Background updates preserve the no-elevation-prompt policy");
  }
  {
    FakePlatform platform;
    platform.vc_ready = true;
    const auto operations = platform.Operations();
    Check(EnsureBrowserPrerequisites(Payload(root, "missing-windows"), false,
                                     &operations)
              .has_value(),
          "Only the missing Windows runtime is installed");
    Check(platform.installs ==
              std::vector{PrerequisiteKind::kWindowsAppRuntime},
          "The existing VC runtime is preserved");
  }
  {
    FakePlatform platform;
    platform.succeeds = false;
    const auto operations = platform.Operations();
    Check(!EnsureBrowserPrerequisites(Payload(root, "installer-failure"), false,
                                      &operations),
          "A failed prerequisite blocks browser activation");
    Check(platform.installs.size() == 1,
          "Failure stops before starting further installers");
  }
  {
    FakePlatform platform;
    platform.becomes_ready = false;
    const auto operations = platform.Operations();
    Check(!EnsureBrowserPrerequisites(Payload(root, "false-success"), false,
                                      &operations),
          "Exit code zero is insufficient if the runtime is still missing");
  }
  {
    FakePlatform platform;
    platform.exit_code = ERROR_SUCCESS_REBOOT_REQUIRED;
    const auto operations = platform.Operations();
    const auto result = EnsureBrowserPrerequisites(
        Payload(root, "reboot-required"), false, &operations);
    Check(!result &&
              result.error().find("Restart Windows") != std::string::npos,
          "A pending prerequisite reboot prevents a premature browser launch");
  }
  {
    FakePlatform platform;
    platform.vc_ready = true;
    platform.exit_code = 0x80073D06;
    const auto operations = platform.Operations();
    Check(EnsureBrowserPrerequisites(Payload(root, "newer-package"), false,
                                     &operations)
              .has_value(),
          "An existing newer Windows package is accepted after a real recheck");
  }
}

void TestStrictSignatures(const fs::path &root) {
  const auto unsigned_file = root / "unsigned.exe";
  Write(unsigned_file, "unsigned executable");
  const auto signature = AuthenticodeVerifier::Verify(
      unsigned_file, {.expected_publisher = L"Microsoft Corporation",
                      .allow_unsigned_dev = true,
                      .require_trusted_signature = true});
  Check(signature != SignatureStatus::kValidAndTrusted,
        "Microsoft prerequisites cannot inherit either development bypass");
}

} // namespace

int wmain(int argc, wchar_t **argv) {
  if (argc == 2) {
    const fs::path payload(argv[1]);
    for (const auto *name :
         {"vc_redist.x64.exe", "WindowsAppRuntimeInstall-x64.exe"}) {
      const auto status = AuthenticodeVerifier::Verify(
          payload / "prerequisites" / name,
          {.expected_publisher = L"Microsoft Corporation",
           .allow_unsigned_dev = false,
           .require_trusted_signature = true});
      Check(status == SignatureStatus::kValidAndTrusted,
            "The bundled vendor installer passes strict native trust checks");
    }
    const auto result = CheckBrowserPrerequisites(payload);
    if (result)
      std::cout << "Both browser prerequisites are already usable; no system "
                   "changes made\n";
    else
      std::cout << result.error() << " No system changes made.\n";
    return failures ? 1 : 0;
  }
  const auto root =
      fs::temp_directory_path() /
      ("KryptonPrerequisiteTest-" + std::to_string(GetCurrentProcessId()));
  fs::create_directories(root);
  TestRequirements(root);
  TestInstallation(root);
  TestStrictSignatures(root);
  if (root.parent_path() == fs::temp_directory_path() &&
      root.filename().string().starts_with("KryptonPrerequisiteTest-")) {
    std::error_code ignored;
    fs::remove_all(root, ignored);
  }
  if (failures == 0)
    std::cout
        << "Prerequisite detection, installation and signature checks passed\n";
  return failures ? 1 : 0;
}
