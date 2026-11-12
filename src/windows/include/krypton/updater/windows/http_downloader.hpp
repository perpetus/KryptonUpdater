#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>

namespace kr::updater::windows {

struct DownloadProgress {
  std::uint64_t bytes_received = 0;
  std::uint64_t total_bytes = 0;
  double bytes_per_second = 0.0;
};

struct DownloadResult {
  std::uint64_t bytes_downloaded = 0;
  std::string sha256_hex;
};

enum class HttpError {
  kSuccess,
  kInvalidUrl,
  kConnectionFailed,
  kRequestFailed,
  kHttpError,
  kNotFound,
  kFileWriteFailed,
  kCancelled
};

[[nodiscard]] constexpr std::string_view
HttpErrorToString(HttpError err) noexcept {
  switch (err) {
  case HttpError::kSuccess:
    return "Success";
  case HttpError::kInvalidUrl:
    return "Invalid URL provided";
  case HttpError::kConnectionFailed:
    return "Could not establish network connection";
  case HttpError::kRequestFailed:
    return "HTTP request transmission failed";
  case HttpError::kHttpError:
    return "Server returned an HTTP error response";
  case HttpError::kNotFound:
    return "The requested resource was not found (HTTP 404)";
  case HttpError::kFileWriteFailed:
    return "Could not write downloaded data to destination file";
  case HttpError::kCancelled:
    return "Download was cancelled by user";
  }
  return "Unknown HTTP error";
}

using DownloadProgressCallback =
    std::function<void(const DownloadProgress &progress)>;

class HttpDownloader {
public:
  HttpDownloader();
  ~HttpDownloader();

  // Downloads directly to disk with streaming SHA-256 calculation
  [[nodiscard]] std::expected<DownloadResult, HttpError>
  DownloadToFile(std::string_view url,
                 const std::filesystem::path &destination_path,
                 DownloadProgressCallback progress_cb = nullptr,
                 std::stop_token stop_token = {},
                 std::uint64_t max_bytes = 2ull * 1024 * 1024 * 1024);

  // Fetches small text responses (e.g. GitHub Releases JSON)
  [[nodiscard]] std::expected<std::string, HttpError>
  GetString(std::string_view url, std::stop_token stop_token = {});

private:
  void *session_handle_{nullptr};
};

} // namespace kr::updater::windows
