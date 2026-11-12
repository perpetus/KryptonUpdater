#include "krypton/updater/windows/release_security.hpp"
#include "krypton/updater/windows/install_cleanup.hpp"
#include "krypton/updater/windows/release_trust.hpp"
#include "krypton/updater/windows/resource_ids.h"
// clang-format off
#include <windows.h>
#include <bcrypt.h>
#include <msdelta.h>
// clang-format on
#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <fstream>
#include <memory>
#include <vector>

namespace kr::updater::windows {
namespace {
struct Algorithm {
  BCRYPT_ALG_HANDLE handle{};
  ~Algorithm() {
    if (handle)
      BCryptCloseAlgorithmProvider(handle, 0);
  }
};
struct Hash {
  BCRYPT_HASH_HANDLE handle{};
  ~Hash() {
    if (handle)
      BCryptDestroyHash(handle);
  }
};
struct Key {
  BCRYPT_KEY_HANDLE handle{};
  ~Key() {
    if (handle)
      BCryptDestroyKey(handle);
  }
};
constexpr std::uint64_t kMaxDeltaBytes = 2ull * 1024 * 1024 * 1024;
struct FileCloser {
  void operator()(void *value) const {
    if (value && value != INVALID_HANDLE_VALUE)
      CloseHandle(value);
  }
};
struct ViewCloser {
  void operator()(const void *value) const {
    if (value)
      UnmapViewOfFile(value);
  }
};
struct MappedInput {
  std::unique_ptr<void, FileCloser> file;
  std::unique_ptr<void, FileCloser> mapping;
  std::unique_ptr<const void, ViewCloser> view;
  std::size_t size{};
};
std::expected<MappedInput, std::string>
MapDeltaInput(const std::filesystem::path &path) {
  MappedInput result;
  result.file.reset(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                nullptr));
  LARGE_INTEGER size{};
  if (result.file.get() == INVALID_HANDLE_VALUE ||
      !GetFileSizeEx(result.file.get(), &size) || size.QuadPart <= 0 ||
      static_cast<std::uint64_t>(size.QuadPart) > kMaxDeltaBytes)
    return std::unexpected("Invalid delta input file.");
  result.size = static_cast<std::size_t>(size.QuadPart);
  result.mapping.reset(CreateFileMappingW(result.file.get(), nullptr,
                                          PAGE_READONLY, 0, 0, nullptr));
  if (!result.mapping)
    return std::unexpected("Could not map the delta input.");
  result.view.reset(
      MapViewOfFile(result.mapping.get(), FILE_MAP_READ, 0, 0, 0));
  if (!result.view)
    return std::unexpected("Could not read the mapped delta input.");
  return result;
}
std::expected<void, std::string> ApplyZucchini(
    const std::filesystem::path &base, const std::filesystem::path &patch,
    const std::filesystem::path &output, std::uint64_t expected_size) {
  const auto delta = MapDeltaInput(patch);
  if (!delta || delta->size < 24)
    return std::unexpected("Invalid Zucchini patch header.");
  std::array<std::uint32_t, 6> header{};
  std::memcpy(header.data(), delta->view.get(), sizeof(header));
  std::error_code error;
  if (header[0] != 0x6363755a || header[4] != expected_size ||
      header[2] != std::filesystem::file_size(base, error) || error)
    return std::unexpected(
        "The Zucchini patch does not match its signed input/output sizes.");
  const auto resource =
      FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_ZUCCHINI_HELPER), RT_RCDATA);
  const auto loaded = resource ? LoadResource(nullptr, resource) : nullptr;
  const auto bytes = loaded ? LockResource(loaded) : nullptr;
  const auto size = resource ? SizeofResource(nullptr, resource) : 0;
  if (!bytes || !size)
    return std::unexpected(
        "The updater is missing its embedded Zucchini helper.");
  auto workspace = TemporaryInstallDirectory::Create();
  if (!workspace)
    return std::unexpected(workspace.error());
  const auto executable = workspace->path() / "KryptonPatch.exe";
  {
    std::ofstream file(executable, std::ios::binary | std::ios::trunc);
    file.write(static_cast<const char *>(bytes),
               static_cast<std::streamsize>(size));
    file.close();
    if (!file)
      return std::unexpected("Could not extract the Zucchini helper.");
  }
  const auto hash = Sha256File(executable);
  if (!hash || *hash != KR_ZUCCHINI_SHA256)
    return std::unexpected(
        "The extracted Zucchini helper failed its embedded hash check.");
  auto command =
      std::format(L"\"{}\" -apply \"{}\" \"{}\" \"{}\"", executable.wstring(),
                  base.wstring(), patch.wstring(), output.wstring());
  STARTUPINFOW startup{sizeof(STARTUPINFOW)};
  PROCESS_INFORMATION child{};
  if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
                      FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                      nullptr, workspace->path().c_str(), &startup, &child))
    return std::unexpected("Could not start the Zucchini patch helper.");
  std::unique_ptr<void, FileCloser> process(child.hProcess),
      thread(child.hThread);
  DWORD exit_code{};
  if (WaitForSingleObject(process.get(), INFINITE) != WAIT_OBJECT_0 ||
      !GetExitCodeProcess(process.get(), &exit_code) || exit_code != 0 ||
      std::filesystem::file_size(output, error) != expected_size || error)
    return std::unexpected(
        "Zucchini could not reconstruct the signed package.");
  return {};
}
} // namespace

std::expected<std::string, std::string>
ReadBoundedFile(const std::filesystem::path &file, std::uint64_t limit) {
  std::error_code error;
  const auto size = std::filesystem::file_size(file, error);
  if (error || size > limit)
    return std::unexpected("File is missing or exceeds its size limit.");
  std::string result(static_cast<std::size_t>(size), '\0');
  std::ifstream input(file, std::ios::binary);
  if (!input.read(result.data(), static_cast<std::streamsize>(result.size())))
    return std::unexpected("Could not read the complete file.");
  return result;
}

std::expected<std::string, std::string>
Sha256File(const std::filesystem::path &file) {
  Algorithm algorithm;
  Hash hash;
  if (BCryptOpenAlgorithmProvider(&algorithm.handle, BCRYPT_SHA256_ALGORITHM,
                                  nullptr, 0) < 0 ||
      BCryptCreateHash(algorithm.handle, &hash.handle, nullptr, 0, nullptr, 0,
                       0) < 0)
    return std::unexpected("SHA-256 initialization failed.");
  std::ifstream input(file, std::ios::binary);
  if (!input)
    return std::unexpected("Could not open the file for hashing.");
  std::array<char, 65536> buffer{};
  while (input.read(buffer.data(), buffer.size()) || input.gcount()) {
    if (BCryptHashData(hash.handle, reinterpret_cast<PUCHAR>(buffer.data()),
                       static_cast<ULONG>(input.gcount()), 0) < 0)
      return std::unexpected("SHA-256 failed.");
  }
  std::array<unsigned char, 32> digest{};
  if (!input.eof() ||
      BCryptFinishHash(hash.handle, digest.data(),
                       static_cast<ULONG>(digest.size()), 0) < 0)
    return std::unexpected("Could not hash the complete file.");
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(64);
  for (const auto byte : digest) {
    result.push_back(digits[byte >> 4]);
    result.push_back(digits[byte & 15]);
  }
  return result;
}

bool VerifyReleaseSignature(std::string_view data, std::string_view signature) {
  if (signature.size() != 64 || data.size() > 16 * 1024 * 1024)
    return false;
  Algorithm ecdsa, sha;
  Hash hash;
  Key key;
  if (BCryptOpenAlgorithmProvider(&ecdsa.handle, BCRYPT_ECDSA_P256_ALGORITHM,
                                  nullptr, 0) < 0 ||
      BCryptOpenAlgorithmProvider(&sha.handle, BCRYPT_SHA256_ALGORITHM, nullptr,
                                  0) < 0 ||
      BCryptCreateHash(sha.handle, &hash.handle, nullptr, 0, nullptr, 0, 0) < 0)
    return false;
  struct Blob {
    BCRYPT_ECCKEY_BLOB header;
    std::array<std::uint8_t, 64> point;
  } blob{};
  blob.header = {BCRYPT_ECDSA_PUBLIC_P256_MAGIC, 32};
  std::copy(kReleasePublicKey.begin() + 1, kReleasePublicKey.end(),
            blob.point.begin());
  std::array<unsigned char, 32> digest{};
  return BCryptImportKeyPair(ecdsa.handle, nullptr, BCRYPT_ECCPUBLIC_BLOB,
                             &key.handle, reinterpret_cast<PUCHAR>(&blob),
                             sizeof(blob), 0) >= 0 &&
         BCryptHashData(
             hash.handle,
             reinterpret_cast<PUCHAR>(const_cast<char *>(data.data())),
             static_cast<ULONG>(data.size()), 0) >= 0 &&
         BCryptFinishHash(hash.handle, digest.data(),
                          static_cast<ULONG>(digest.size()), 0) >= 0 &&
         BCryptVerifySignature(
             key.handle, nullptr, digest.data(),
             static_cast<ULONG>(digest.size()),
             reinterpret_cast<PUCHAR>(const_cast<char *>(signature.data())),
             static_cast<ULONG>(signature.size()), 0) >= 0;
}

bool VerifyReleaseFile(const std::filesystem::path &file,
                       const std::filesystem::path &signature) {
  const auto bytes = ReadBoundedFile(file, 16 * 1024 * 1024);
  const auto proof = ReadBoundedFile(signature, 64);
  return bytes && proof && VerifyReleaseSignature(*bytes, *proof);
}

bool FileMatches(const std::filesystem::path &file, std::uint64_t size,
                 std::string_view sha256) {
  std::error_code error;
  if (std::filesystem::file_size(file, error) != size || error)
    return false;
  const auto hash = Sha256File(file);
  return hash && *hash == sha256;
}

std::expected<void, std::string>
ApplyReleaseDelta(const std::filesystem::path &base,
                  const std::filesystem::path &patch,
                  const std::filesystem::path &output,
                  std::uint64_t expected_size, std::string_view algorithm) {
  if (!expected_size || expected_size > kMaxDeltaBytes)
    return std::unexpected("Invalid delta output size.");
  if (algorithm == "zucchini")
    return ApplyZucchini(base, patch, output, expected_size);
  if (algorithm != "msdelta")
    return std::unexpected(
        "The signed delta uses an unsupported patch algorithm.");
  auto source = MapDeltaInput(base);
  auto delta = MapDeltaInput(patch);
  if (!source || !delta)
    return std::unexpected("The delta base or patch could not be read.");
  DELTA_INPUT source_input{}, delta_input{};
  source_input.lpcStart = source->view.get();
  source_input.uSize = source->size;
  delta_input.lpcStart = delta->view.get();
  delta_input.uSize = delta->size;
  DELTA_HEADER_INFO info{};
  if (!GetDeltaInfoB(delta_input, &info) || info.TargetSize != expected_size)
    return std::unexpected("Delta target size is invalid.");
  DELTA_OUTPUT target{};
  if (!ApplyDeltaB(DELTA_FLAG_NONE, source_input, delta_input, &target))
    return std::unexpected("Delta reconstruction failed.");
  const auto free_delta = [](void *memory) { DeltaFree(memory); };
  std::unique_ptr<void, decltype(free_delta)> owner(target.lpStart, free_delta);
  if (target.uSize != expected_size)
    return std::unexpected("Delta output length mismatch.");
  std::ofstream file(output, std::ios::binary | std::ios::trunc);
  if (!file.write(static_cast<const char *>(target.lpStart),
                  static_cast<std::streamsize>(target.uSize)))
    return std::unexpected("Could not write the reconstructed update.");
  return {};
}
} // namespace kr::updater::windows
