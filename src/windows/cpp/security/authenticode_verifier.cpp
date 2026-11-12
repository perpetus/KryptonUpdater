#include "krypton/updater/windows/authenticode_verifier.hpp"

// clang-format off
#include <windows.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
// clang-format on

namespace kr::updater::windows {

SignatureStatus
AuthenticodeVerifier::Verify(const std::filesystem::path &binary_path,
                             const AuthenticodeOptions &options) noexcept {
  std::error_code file_error;
  if (!std::filesystem::exists(binary_path, file_error)) {
    return SignatureStatus::kFileNotFound;
  }

  std::wstring path_str = binary_path.wstring();

  WINTRUST_FILE_INFO file_info{};
  file_info.cbStruct = sizeof(WINTRUST_FILE_INFO);
  file_info.pcwszFilePath = path_str.c_str();

  GUID action_guid = WINTRUST_ACTION_GENERIC_VERIFY_V2;

  WINTRUST_DATA trust_data{};
  trust_data.cbStruct = sizeof(WINTRUST_DATA);
  trust_data.dwUIChoice = WTD_UI_NONE;
  trust_data.fdwRevocationChecks = WTD_REVOKE_NONE;
  trust_data.dwUnionChoice = WTD_CHOICE_FILE;
  trust_data.pFile = &file_info;
  trust_data.dwStateAction = WTD_STATEACTION_VERIFY;
  trust_data.dwProvFlags = WTD_SAFER_FLAG;

  LONG status = WinVerifyTrust(nullptr, &action_guid, &trust_data);

  SignatureStatus result = SignatureStatus::kVerificationFailed;

  if (status == ERROR_SUCCESS) {
    result = SignatureStatus::kValidAndTrusted;

    // Check publisher subject name if required
    if (!options.expected_publisher.empty()) {
      result = SignatureStatus::kUntrustedPublisher;
      CRYPT_PROVIDER_DATA *prov_data =
          trust_data.hWVTStateData
              ? WTHelperProvDataFromStateData(trust_data.hWVTStateData)
              : nullptr;
      if (prov_data) {
        CRYPT_PROVIDER_SGNR *sgnr =
            WTHelperGetProvSignerFromChain(prov_data, 0, FALSE, 0);
        if (sgnr) {
          CRYPT_PROVIDER_CERT *prov_cert =
              WTHelperGetProvCertFromChain(sgnr, 0);
          if (prov_cert && prov_cert->pCert) {
            PCCERT_CONTEXT cert = prov_cert->pCert;
            wchar_t subject_name[256]{};
            CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr,
                               subject_name, ARRAYSIZE(subject_name));

            const std::wstring_view publisher(subject_name);
            if (options.require_trusted_signature
                    ? publisher == options.expected_publisher
                    : publisher.find(options.expected_publisher) !=
                          std::wstring_view::npos) {
              result = SignatureStatus::kValidAndTrusted;
            }
          }
        }
      }
    }
  } else if (status == TRUST_E_NOSIGNATURE) {
    result = SignatureStatus::kUnsigned;
  } else if (status == TRUST_E_BAD_DIGEST) {
    result = SignatureStatus::kSignatureCorrupted;
  } else {
    result = SignatureStatus::kVerificationFailed;
  }

  // Close state handle
  trust_data.dwStateAction = WTD_STATEACTION_CLOSE;
  WinVerifyTrust(nullptr, &action_guid, &trust_data);

  // If in dev mode and unsigned, allow it
  if (!options.require_trusted_signature && options.allow_unsigned_dev &&
      (result == SignatureStatus::kUnsigned ||
       result == SignatureStatus::kUntrustedPublisher)) {
    return SignatureStatus::kValidAndTrusted;
  }

#ifdef KR_ALLOW_UNSIGNED
  if (!options.require_trusted_signature &&
      (result == SignatureStatus::kUnsigned ||
       result == SignatureStatus::kUntrustedPublisher)) {
    return SignatureStatus::kValidAndTrusted;
  }
#endif

  return result;
}

} // namespace kr::updater::windows
