#include "krypton/updater/windows/offline_payload.hpp"
#include "krypton/updater/windows/resource_ids.h"

#include <fstream>
#include <random>
#include <windows.h>

namespace kr::updater::windows {

bool OfflinePayload::HasEmbeddedPayload() noexcept {
  HRSRC res =
      FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_OFFLINE_PAYLOAD), RT_RCDATA);
  return res != nullptr;
}

std::span<const std::uint8_t> OfflinePayload::GetEmbeddedData() noexcept {
  HRSRC res =
      FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_OFFLINE_PAYLOAD), RT_RCDATA);
  if (!res)
    return {};

  HGLOBAL handle = LoadResource(nullptr, res);
  if (!handle)
    return {};

  const void *ptr = LockResource(handle);
  DWORD size = SizeofResource(nullptr, res);
  if (!ptr || size == 0)
    return {};

  return std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t *>(ptr), size);
}

std::string_view OfflinePayload::GetEmbeddedManifest() noexcept {
  const auto resource =
      FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_OFFLINE_MANIFEST), RT_RCDATA);
  if (!resource)
    return {};
  const auto loaded = LoadResource(nullptr, resource);
  const auto bytes = static_cast<const char *>(LockResource(loaded));
  const auto size = SizeofResource(nullptr, resource);
  return bytes ? std::string_view(bytes, size) : std::string_view{};
}

std::string_view OfflinePayload::GetEmbeddedManifestSignature() noexcept {
  const auto resource = FindResourceW(
      nullptr, MAKEINTRESOURCEW(IDR_OFFLINE_SIGNATURE), RT_RCDATA);
  if (!resource)
    return {};
  const auto loaded = LoadResource(nullptr, resource);
  const auto bytes = static_cast<const char *>(LockResource(loaded));
  const auto size = SizeofResource(nullptr, resource);
  return bytes ? std::string_view(bytes, size) : std::string_view{};
}

PayloadType
OfflinePayload::DetectType(std::span<const std::uint8_t> data) noexcept {
  // 7-Zip signature: 7z \xBC \xAF \x27 \x1C
  if (data.size() >= 6 && data[0] == '7' && data[1] == 'z' && data[2] == 0xBC &&
      data[3] == 0xAF && data[4] == 0x27 && data[5] == 0x1C) {
    return PayloadType::kSevenZip;
  }

  // ZIP header: "PK\x03\x04"
  if (data.size() >= 4 && data[0] == 'P' && data[1] == 'K' && data[2] == 0x03 &&
      data[3] == 0x04) {
    return PayloadType::kZip;
  }

  // MSI Compound File: 0xD0, 0xCF, 0x11, 0xE0
  if (data.size() >= 4 && data[0] == 0xD0 && data[1] == 0xCF &&
      data[2] == 0x11 && data[3] == 0xE0) {
    return PayloadType::kMsi;
  }

  // Windows PE EXE: "MZ"
  if (data.size() >= 2 && data[0] == 'M' && data[1] == 'Z') {
    return PayloadType::kExe;
  }

  return PayloadType::kUnknown;
}

PayloadType OfflinePayload::DetectEmbeddedType() noexcept {
  return DetectType(GetEmbeddedData());
}

std::optional<std::filesystem::path> OfflinePayload::ExtractToTempFile() {
  auto data = GetEmbeddedData();
  if (data.empty())
    return std::nullopt;

  auto type = DetectType(data);
  const char *ext = ".bin";
  if (type == PayloadType::kSevenZip)
    ext = ".7z";
  else if (type == PayloadType::kZip)
    ext = ".zip";
  else if (type == PayloadType::kMsi)
    ext = ".msi";
  else if (type == PayloadType::kExe)
    ext = ".exe";

  std::error_code ec;
  auto temp_dir = std::filesystem::temp_directory_path(ec) / "KryptonInstall";
  std::filesystem::create_directories(temp_dir, ec);

  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<uint32_t> dis(100000, 999999);

  auto temp_path = temp_dir / std::format("payload_{}{}", dis(gen), ext);
  std::ofstream out(temp_path, std::ios::binary);
  if (!out)
    return std::nullopt;

  out.write(reinterpret_cast<const char *>(data.data()), data.size());
  out.close();

  return temp_path;
}

} // namespace kr::updater::windows
