#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace kr::updater::windows {

enum class SignatureStatus {
  kValidAndTrusted,
  kUntrustedPublisher,
  kSignatureCorrupted,
  kUnsigned,
  kFileNotFound,
  kVerificationFailed
};

[[nodiscard]] constexpr std::string_view
SignatureStatusToString(SignatureStatus status) noexcept {
  switch (status) {
  case SignatureStatus::kValidAndTrusted:
    return "Valid Authenticode signature from trusted publisher";
  case SignatureStatus::kUntrustedPublisher:
    return "Signature valid, but signer does not match expected publisher";
  case SignatureStatus::kSignatureCorrupted:
    return "Signature is corrupted or file has been tampered with";
  case SignatureStatus::kUnsigned:
    return "File has no digital signature";
  case SignatureStatus::kFileNotFound:
    return "Target binary not found";
  case SignatureStatus::kVerificationFailed:
    return "Verification failed";
  }
  return "Unknown status";
}

struct AuthenticodeOptions {
  std::wstring expected_publisher = L"Krypton";
  bool allow_unsigned_dev = false;
  // Vendor prerequisites must never inherit the development signing bypass.
  bool require_trusted_signature = false;
};

class AuthenticodeVerifier {
public:
  [[nodiscard]] static SignatureStatus
  Verify(const std::filesystem::path &binary_path,
         const AuthenticodeOptions &options = {}) noexcept;
};

} // namespace kr::updater::windows
