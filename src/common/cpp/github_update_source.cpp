#include "krypton/updater/github_update_source.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>

namespace kr::updater {
namespace {

struct JsonReader {
  std::string_view src;
  size_t pos = 0;

  void SkipWs() {
    while (pos < src.size() && (std::isspace(static_cast<unsigned char>(src[pos])) || src[pos] == '\r' || src[pos] == '\n')) {
      ++pos;
    }
  }

  bool Match(char ch) {
    SkipWs();
    if (pos < src.size() && src[pos] == ch) {
      ++pos;
      return true;
    }
    return false;
  }

  std::optional<std::string> ReadString() {
    SkipWs();
    if (pos >= src.size() || src[pos] != '"') return std::nullopt;
    ++pos;
    std::string result;
    while (pos < src.size()) {
      char c = src[pos++];
      if (c == '"') return result;
      if (c == '\\') {
        if (pos >= src.size()) return std::nullopt;
        char esc = src[pos++];
        if (esc == '"' || esc == '\\' || esc == '/') result.push_back(esc);
        else if (esc == 'n') result.push_back('\n');
        else if (esc == 'r') result.push_back('\r');
        else if (esc == 't') result.push_back('\t');
        else result.push_back(esc);
      } else {
        result.push_back(c);
      }
    }
    return std::nullopt;
  }

  std::optional<std::uint64_t> ReadNumber() {
    SkipWs();
    size_t start = pos;
    while (pos < src.size() && std::isdigit(static_cast<unsigned char>(src[pos]))) {
      ++pos;
    }
    if (start == pos) return std::nullopt;
    std::uint64_t val = 0;
    auto [ptr, ec] = std::from_chars(src.data() + start, src.data() + pos, val);
    if (ec != std::errc{}) return std::nullopt;
    return val;
  }

  std::optional<bool> ReadBool() {
    SkipWs();
    if (src.substr(pos, 4) == "true") {
      pos += 4;
      return true;
    }
    if (src.substr(pos, 5) == "false") {
      pos += 5;
      return false;
    }
    return std::nullopt;
  }

  void SkipValue() {
    SkipWs();
    if (pos >= src.size()) return;
    char c = src[pos];
    if (c == '"') {
      ReadString();
    } else if (c == '{') {
      Match('{');
      int depth = 1;
      while (pos < src.size() && depth > 0) {
        if (src[pos] == '{') ++depth;
        else if (src[pos] == '}') --depth;
        else if (src[pos] == '"') { ReadString(); continue; }
        ++pos;
      }
    } else if (c == '[') {
      Match('[');
      int depth = 1;
      while (pos < src.size() && depth > 0) {
        if (src[pos] == '[') ++depth;
        else if (src[pos] == ']') --depth;
        else if (src[pos] == '"') { ReadString(); continue; }
        ++pos;
      }
    } else {
      while (pos < src.size() && src[pos] != ',' && src[pos] != '}' && src[pos] != ']') {
        ++pos;
      }
    }
  }
};

}  // namespace

GitHubUpdateSource::GitHubUpdateSource(std::string owner, std::string repo, HttpGetFunc http_get)
    : owner_(std::move(owner)), repo_(std::move(repo)), http_get_(std::move(http_get)) {}

std::string GitHubUpdateSource::EndpointUrl() const {
  return std::format("https://api.github.com/repos/{}/{}/releases/latest", owner_, repo_);
}

std::expected<UpdateRelease, UpdateSourceError> GitHubUpdateSource::ParseReleaseJson(
    std::string_view json_text) {
  JsonReader reader{json_text, 0};
  if (!reader.Match('{')) {
    return std::unexpected(UpdateSourceError::kInvalidResponse);
  }

  UpdateRelease release;
  bool has_tag = false;

  while (true) {
    reader.SkipWs();
    if (reader.Match('}')) break;

    auto key = reader.ReadString();
    if (!key || !reader.Match(':')) {
      return std::unexpected(UpdateSourceError::kInvalidResponse);
    }

    if (*key == "tag_name") {
      auto tag = reader.ReadString();
      if (!tag) return std::unexpected(UpdateSourceError::kInvalidResponse);
      auto ver = SemVer::Parse(*tag);
      if (!ver) return std::unexpected(UpdateSourceError::kInvalidResponse);
      release.version = *ver;
      release.tag_name = *tag;
      has_tag = true;
    } else if (*key == "name") {
      auto name = reader.ReadString();
      if (name) release.release_name = *name;
    } else if (*key == "body") {
      auto body = reader.ReadString();
      if (body) release.release_notes = *body;
    } else if (*key == "prerelease") {
      auto pre = reader.ReadBool();
      if (pre) release.is_prerelease = *pre;
    } else if (*key == "assets") {
      if (!reader.Match('[')) {
        return std::unexpected(UpdateSourceError::kInvalidResponse);
      }
      while (true) {
        reader.SkipWs();
        if (reader.Match(']')) break;
        if (!reader.Match('{')) {
          return std::unexpected(UpdateSourceError::kInvalidResponse);
        }

        ReleaseAsset asset;
        while (true) {
          reader.SkipWs();
          if (reader.Match('}')) break;

          auto akey = reader.ReadString();
          if (!akey || !reader.Match(':')) {
            return std::unexpected(UpdateSourceError::kInvalidResponse);
          }

          if (*akey == "name") {
            auto aname = reader.ReadString();
            if (aname) asset.name = *aname;
          } else if (*akey == "browser_download_url") {
            auto aurl = reader.ReadString();
            if (aurl) asset.download_url = *aurl;
          } else if (*akey == "size") {
            auto asize = reader.ReadNumber();
            if (asize) asset.size_bytes = *asize;
          } else if (*akey == "content_type") {
            auto actype = reader.ReadString();
            if (actype) asset.content_type = *actype;
          } else {
            reader.SkipValue();
          }
          reader.Match(',');
        }

        if (!asset.name.empty() && !asset.download_url.empty()) {
          release.assets.push_back(std::move(asset));
        }
        reader.Match(',');
      }
    } else {
      reader.SkipValue();
    }
    reader.Match(',');
  }

  if (!has_tag) {
    return std::unexpected(UpdateSourceError::kNoReleasesFound);
  }

  return release;
}

const ReleaseAsset* GitHubUpdateSource::FindBestAsset(
    const std::vector<ReleaseAsset>& assets,
    std::string_view preferred_ext) {
  // First look for matching extension with win-x64 or windows
  for (const auto& asset : assets) {
    std::string lower_name;
    lower_name.reserve(asset.name.size());
    for (char c : asset.name) lower_name.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    if (lower_name.ends_with(preferred_ext)) {
      if (lower_name.find("win") != std::string::npos || lower_name.find("x64") != std::string::npos) {
        return &asset;
      }
    }
  }

  // Fallback: any asset ending with the preferred extension
  for (const auto& asset : assets) {
    std::string lower_name;
    lower_name.reserve(asset.name.size());
    for (char c : asset.name) lower_name.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    if (lower_name.ends_with(preferred_ext)) {
      return &asset;
    }
  }

  // Fallback: search for any .zip, .msi, or .exe
  for (const auto& asset : assets) {
    if (asset.name.ends_with(".zip") || asset.name.ends_with(".msi") || asset.name.ends_with(".exe")) {
      return &asset;
    }
  }

  return nullptr;
}

std::expected<UpdateRelease, UpdateSourceError> GitHubUpdateSource::CheckForUpdate(
    const UpdateCheckOptions& options) {
  if (!http_get_) {
    return std::unexpected(UpdateSourceError::kNetworkError);
  }

  auto response = http_get_(EndpointUrl());
  if (!response) {
    return std::unexpected(UpdateSourceError::kNetworkError);
  }

  auto parsed = ParseReleaseJson(*response);
  if (!parsed) {
    return parsed;
  }

  if (parsed->is_prerelease && !options.include_prereleases) {
    return std::unexpected(UpdateSourceError::kNoReleasesFound);
  }

  auto cur_ver = SemVer::Parse(options.current_version);
  if (cur_ver && parsed->version <= *cur_ver) {
    return std::unexpected(UpdateSourceError::kNoReleasesFound); // Up to date
  }

  return parsed;
}

}  // namespace kr::updater
