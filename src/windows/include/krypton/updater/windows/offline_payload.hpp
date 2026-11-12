#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>

namespace kr::updater::windows {

enum class PayloadType { kNone, kSevenZip, kZip, kMsi, kExe, kUnknown };

[[nodiscard]] constexpr std::string_view
PayloadTypeToString(PayloadType type) noexcept {
  switch (type) {
  case PayloadType::kNone:
    return "None";
  case PayloadType::kSevenZip:
    return "7-Zip Archive (.7z)";
  case PayloadType::kZip:
    return "ZIP Archive (.zip)";
  case PayloadType::kMsi:
    return "MSI Windows Installer (.msi)";
  case PayloadType::kExe:
    return "Executable Installer (.exe)";
  case PayloadType::kUnknown:
    return "Unknown Payload";
  }
  return "Unknown";
}

class OfflinePayload {
public:
  [[nodiscard]] static bool HasEmbeddedPayload() noexcept;
  [[nodiscard]] static std::span<const std::uint8_t> GetEmbeddedData() noexcept;
  [[nodiscard]] static std::string_view GetEmbeddedManifest() noexcept;
  [[nodiscard]] static std::string_view GetEmbeddedManifestSignature() noexcept;
  [[nodiscard]] static PayloadType
  DetectType(std::span<const std::uint8_t> data) noexcept;
  [[nodiscard]] static PayloadType DetectEmbeddedType() noexcept;

  // Extracts embedded data to a temporary file and returns its path
  [[nodiscard]] static std::optional<std::filesystem::path> ExtractToTempFile();
};

} // namespace kr::updater::windows
