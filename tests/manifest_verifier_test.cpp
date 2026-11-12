#include <cassert>
#include <iostream>

#include "krypton/updater/manifest_verifier.hpp"

using namespace kr::updater;

void TestValidManifest() {
  std::string_view json = R"({
    "version": "1.0.0",
    "files": [
      { "path": "krypton.exe", "sha256": "abcdef123456", "size": 1024 },
      { "path": "locales/en-US.pak", "sha256": "9876543210fe", "size": 2048 }
    ]
  })";

  auto [manifest, err] = ManifestVerifier::ParseJson(json);
  assert(err == ManifestValidationError::kNone);
  assert(manifest.has_value());
  assert(manifest->version == *SemVer::Parse("1.0.0"));
  assert(manifest->files.size() == 2);

  ManifestVerifier verifier;
  assert(verifier.LoadManifest(std::move(*manifest)));
  assert(verifier.HasFile("krypton.exe"));
  assert(verifier.HasFile("locales/en-US.pak"));
  assert(verifier.VerifyHash("krypton.exe", "abcdef123456"));
  assert(!verifier.VerifyHash("krypton.exe", "wronghash"));
}

void TestZipSlipRejection() {
  std::string_view bad_json = R"({
    "version": "1.0.0",
    "files": [
      { "path": "../../Windows/System32/evil.dll", "sha256": "1234", "size": 100 }
    ]
  })";

  auto [manifest, err] = ManifestVerifier::ParseJson(bad_json);
  assert(err == ManifestValidationError::kInvalidPath);
}

void TestProhibitedExtensionRejection() {
  std::string_view bad_ext_json = R"({
    "version": "1.0.0",
    "files": [
      { "path": "malicious_script.bat", "sha256": "1234", "size": 100 }
    ]
  })";

  auto [manifest, err] = ManifestVerifier::ParseJson(bad_ext_json);
  assert(err == ManifestValidationError::kInvalidPath);
}

void TestUnmanifestedArchiveFiles() {
  std::string_view json = R"({
    "version": "1.0.0",
    "files": [
      { "path": "krypton.exe", "sha256": "abcdef123456", "size": 1024 }
    ]
  })";

  auto [manifest, err] = ManifestVerifier::ParseJson(json);
  ManifestVerifier verifier;
  verifier.LoadManifest(std::move(*manifest));

  std::vector<std::string> clean_archive = {"krypton.exe"};
  assert(verifier.ValidateArchiveEntries(clean_archive));

  std::string unexpected;
  std::vector<std::string> tainted_archive = {"krypton.exe", "rogue_file.dll"};
  assert(!verifier.ValidateArchiveEntries(tainted_archive, &unexpected));
  assert(unexpected == "rogue_file.dll");
}

int main() {
  TestValidManifest();
  TestZipSlipRejection();
  TestProhibitedExtensionRejection();
  TestUnmanifestedArchiveFiles();
  std::cout << "[PASS] manifest_verifier_test\n";
  return 0;
}
