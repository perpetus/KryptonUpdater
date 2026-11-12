#include "krypton/updater/windows/http_downloader.hpp"
#include "krypton/updater/windows/release_security.hpp"
// clang-format off
#include <windows.h>
#include <winhttp.h>
// clang-format on
#include <array>
#include <chrono>
#include <fstream>
#include <memory>

namespace kr::updater::windows {
namespace {
struct InternetCloser {
  void operator()(void *handle) const {
    if (handle)
      WinHttpCloseHandle(handle);
  }
};
using Internet = std::unique_ptr<void, InternetCloser>;
struct Request {
  Internet connection;
  Internet handle;
};
std::expected<Request, HttpError> Open(void *session, std::string_view url) {
  if (!session || url.empty() || url.size() > 2048)
    return std::unexpected(HttpError::kInvalidUrl);
  const auto length =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, url.data(),
                          static_cast<int>(url.size()), nullptr, 0);
  if (!length)
    return std::unexpected(HttpError::kInvalidUrl);
  std::wstring wide(static_cast<std::size_t>(length), '\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, url.data(),
                      static_cast<int>(url.size()), wide.data(), length);
  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  parts.dwSchemeLength = parts.dwHostNameLength = parts.dwUrlPathLength =
      parts.dwExtraInfoLength = parts.dwUserNameLength =
          parts.dwPasswordLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0,
                       &parts) ||
      parts.nScheme != INTERNET_SCHEME_HTTPS ||
      parts.nPort != INTERNET_DEFAULT_HTTPS_PORT || parts.dwUserNameLength ||
      parts.dwPasswordLength)
    return std::unexpected(HttpError::kInvalidUrl);
  const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
  std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
  if (parts.dwExtraInfoLength)
    path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
  Request request;
  request.connection.reset(
      WinHttpConnect(session, host.c_str(), parts.nPort, 0));
  if (!request.connection)
    return std::unexpected(HttpError::kConnectionFailed);
  request.handle.reset(WinHttpOpenRequest(
      request.connection.get(), L"GET", path.c_str(), nullptr,
      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
  if (!request.handle)
    return std::unexpected(HttpError::kRequestFailed);
  DWORD autologon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
  DWORD disabled = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
  if (!WinHttpSetOption(request.handle.get(), WINHTTP_OPTION_AUTOLOGON_POLICY,
                        &autologon, sizeof(autologon)) ||
      !WinHttpSetOption(request.handle.get(), WINHTTP_OPTION_DISABLE_FEATURE,
                        &disabled, sizeof(disabled)))
    return std::unexpected(HttpError::kRequestFailed);
  const auto headers = host == L"api.github.com"
                           ? L"Accept: application/vnd.github+json\r\n"
                             L"X-GitHub-Api-Version: 2022-11-28\r\n"
                           : L"Accept: application/octet-stream\r\n";
  if (!WinHttpSendRequest(request.handle.get(), headers, static_cast<DWORD>(-1),
                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request.handle.get(), nullptr))
    return std::unexpected(HttpError::kRequestFailed);
  DWORD status{}, size = sizeof(status);
  if (!WinHttpQueryHeaders(request.handle.get(),
                           WINHTTP_QUERY_STATUS_CODE |
                               WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                           WINHTTP_NO_HEADER_INDEX))
    return std::unexpected(HttpError::kRequestFailed);
  if (status == 404)
    return std::unexpected(HttpError::kNotFound);
  if (status != 200)
    return std::unexpected(HttpError::kHttpError);
  return request;
}
} // namespace
HttpDownloader::HttpDownloader() {
  session_handle_ =
      WinHttpOpen(L"KryptonUpdater/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session_handle_)
    return;
  DWORD tls = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
  DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
  if (!WinHttpSetOption(session_handle_, WINHTTP_OPTION_SECURE_PROTOCOLS, &tls,
                        sizeof(tls)) ||
      !WinHttpSetOption(session_handle_, WINHTTP_OPTION_REDIRECT_POLICY,
                        &redirects, sizeof(redirects)) ||
      !WinHttpSetTimeouts(session_handle_, 10000, 15000, 30000, 30000)) {
    WinHttpCloseHandle(session_handle_);
    session_handle_ = nullptr;
  }
}
HttpDownloader::~HttpDownloader() {
  if (session_handle_)
    WinHttpCloseHandle(session_handle_);
}
std::expected<std::string, HttpError>
HttpDownloader::GetString(std::string_view url, std::stop_token stop) {
  auto request = Open(session_handle_, url);
  if (!request)
    return std::unexpected(request.error());
  std::string result;
  std::array<char, 16384> buffer{};
  for (;;) {
    if (stop.stop_requested())
      return std::unexpected(HttpError::kCancelled);
    DWORD count{};
    if (!WinHttpReadData(request->handle.get(), buffer.data(),
                         static_cast<DWORD>(buffer.size()), &count))
      return std::unexpected(HttpError::kRequestFailed);
    if (!count)
      break;
    if (result.size() + count > 16 * 1024 * 1024)
      return std::unexpected(HttpError::kHttpError);
    result.append(buffer.data(), count);
  }
  return result;
}
std::expected<DownloadResult, HttpError>
HttpDownloader::DownloadToFile(std::string_view url,
                               const std::filesystem::path &destination,
                               DownloadProgressCallback progress,
                               std::stop_token stop, std::uint64_t max_bytes) {
  auto request = Open(session_handle_, url);
  if (!request)
    return std::unexpected(request.error());
  std::ofstream output(destination, std::ios::binary | std::ios::trunc);
  if (!output)
    return std::unexpected(HttpError::kFileWriteFailed);
  std::array<char, 65536> buffer{};
  std::uint64_t received{};
  const auto started = std::chrono::steady_clock::now();
  for (;;) {
    if (stop.stop_requested())
      return std::unexpected(HttpError::kCancelled);
    DWORD count{};
    if (!WinHttpReadData(request->handle.get(), buffer.data(),
                         static_cast<DWORD>(buffer.size()), &count))
      return std::unexpected(HttpError::kRequestFailed);
    if (!count)
      break;
    if (count > max_bytes - received)
      return std::unexpected(HttpError::kHttpError);
    if (!output.write(buffer.data(), count))
      return std::unexpected(HttpError::kFileWriteFailed);
    received += count;
    if (progress) {
      const auto elapsed = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - started)
                               .count();
      progress({received, max_bytes,
                elapsed > 0 ? static_cast<double>(received) / elapsed : 0});
    }
  }
  output.close();
  if (!output)
    return std::unexpected(HttpError::kFileWriteFailed);
  const auto hash = Sha256File(destination);
  if (!hash)
    return std::unexpected(HttpError::kFileWriteFailed);
  return DownloadResult{received, *hash};
}
} // namespace kr::updater::windows
