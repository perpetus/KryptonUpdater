#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "krypton/updater/update_source.hpp"

namespace kr::updater {

using HttpGetFunc = std::function<std::expected<std::string, int>(std::string_view url)>;

class GitHubUpdateSource : public IUpdateSource {
 public:
  GitHubUpdateSource(std::string owner, std::string repo, HttpGetFunc http_get = nullptr);

  void SetHttpGet(HttpGetFunc http_get) {
    http_get_ = std::move(http_get);
  }

  [[nodiscard]] std::string EndpointUrl() const;

  [[nodiscard]] std::expected<UpdateRelease, UpdateSourceError> CheckForUpdate(
      const UpdateCheckOptions& options) override;

  // Pure parser for testing JSON response handling without network
  [[nodiscard]] static std::expected<UpdateRelease, UpdateSourceError> ParseReleaseJson(
      std::string_view json_text);

  // Asset filtering helper: locates preferred Windows asset (.zip, .msi, or .exe)
  [[nodiscard]] static const ReleaseAsset* FindBestAsset(
      const std::vector<ReleaseAsset>& assets,
      std::string_view preferred_ext = ".zip");

 private:
  std::string owner_;
  std::string repo_;
  HttpGetFunc http_get_;
};

}  // namespace kr::updater
