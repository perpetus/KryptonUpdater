#include "krypton/updater/manifest_verifier.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace kr::updater {
namespace {

// Allowed extensions for files in the browser distribution
constexpr std::string_view kAllowedExtensions[] = {
    ".7z",  ".zip", ".exe", ".dll", ".pak", ".bin",      ".dat", ".json",
    ".ico", ".png", ".jpg", ".svg", ".pfx", ".manifest", ".msi", ".xml",
    ".pb",  ".txt", ".md",  ".mo",  ".po",  ".bmp",      ".cur", ".sig"};

// Forbidden extensions that must NEVER be written into the directory
constexpr std::string_view kProhibitedExtensions[] = {
    ".bat", ".cmd", ".ps1", ".vbs", ".js", ".scr", ".com", ".cpl", ".reg"};

std::string NormalizePath(std::string_view path) {
  std::string result;
  result.reserve(path.size());
  for (char ch : path) {
    if (ch == '\\') {
      result.push_back('/');
    } else {
      result.push_back(ch);
    }
  }
  return result;
}

struct JsonScanner {
  std::string_view source;
  size_t cursor = 0;

  void SkipWhitespace() {
    while (cursor < source.size() &&
           (std::isspace(static_cast<unsigned char>(source[cursor])) ||
            source[cursor] == '\r' || source[cursor] == '\n')) {
      ++cursor;
    }
  }

  bool Match(char expected) {
    SkipWhitespace();
    if (cursor < source.size() && source[cursor] == expected) {
      ++cursor;
      return true;
    }
    return false;
  }

  std::optional<std::string> ParseString() {
    SkipWhitespace();
    if (cursor >= source.size() || source[cursor] != '"')
      return std::nullopt;
    ++cursor;
    std::string val;
    while (cursor < source.size()) {
      char c = source[cursor++];
      if (c == '"')
        return val;
      if (c == '\\') {
        if (cursor >= source.size())
          return std::nullopt;
        char esc = source[cursor++];
        if (esc == '"' || esc == '\\' || esc == '/')
          val.push_back(esc);
        else if (esc == 'n')
          val.push_back('\n');
        else if (esc == 'r')
          val.push_back('\r');
        else if (esc == 't')
          val.push_back('\t');
        else
          val.push_back(esc);
      } else {
        val.push_back(c);
      }
    }
    return std::nullopt;
  }

  std::optional<std::uint64_t> ParseNumber() {
    SkipWhitespace();
    size_t start = cursor;
    while (cursor < source.size() &&
           std::isdigit(static_cast<unsigned char>(source[cursor]))) {
      ++cursor;
    }
    if (start == cursor)
      return std::nullopt;
    std::uint64_t val = 0;
    auto [ptr, ec] =
        std::from_chars(source.data() + start, source.data() + cursor, val);
    if (ec != std::errc{})
      return std::nullopt;
    return val;
  }
};

} // namespace

bool ManifestVerifier::IsExtensionPermitted(std::string_view path) noexcept {
  auto dot = path.rfind('.');
  if (dot == std::string_view::npos)
    return false;
  std::string ext;
  for (size_t i = dot; i < path.size(); ++i) {
    ext.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(path[i]))));
  }

  for (auto prohibited : kProhibitedExtensions) {
    if (ext == prohibited)
      return false;
  }

  for (auto allowed : kAllowedExtensions) {
    if (ext == allowed)
      return true;
  }
  return false;
}

bool ManifestVerifier::IsPathSafe(std::string_view path) noexcept {
  if (path.empty())
    return false;
  // Disallow absolute paths or Windows drive letters
  if (path.front() == '/' || path.front() == '\\')
    return false;
  if (path.find(':') != std::string_view::npos)
    return false;

  // Disallow directory traversal (Zip-Slip)
  if (path.find("..") != std::string_view::npos)
    return false;

  return IsExtensionPermitted(path);
}

std::pair<std::optional<UpdateManifest>, ManifestValidationError>
ManifestVerifier::ParseJson(std::string_view json_text) {
  JsonScanner scanner{json_text, 0};
  if (!scanner.Match('{')) {
    return {std::nullopt, ManifestValidationError::kInvalidJson};
  }

  UpdateManifest manifest;
  bool has_version = false;

  while (true) {
    scanner.SkipWhitespace();
    if (scanner.Match('}'))
      break;

    auto key = scanner.ParseString();
    if (!key || !scanner.Match(':')) {
      return {std::nullopt, ManifestValidationError::kInvalidJson};
    }

    if (*key == "version") {
      auto ver_str = scanner.ParseString();
      if (!ver_str) {
        return {std::nullopt, ManifestValidationError::kInvalidVersion};
      }
      auto parsed_ver = SemVer::Parse(*ver_str);
      if (!parsed_ver) {
        return {std::nullopt, ManifestValidationError::kInvalidVersion};
      }
      manifest.version = *parsed_ver;
      has_version = true;
    } else if (*key == "files") {
      if (!scanner.Match('[')) {
        return {std::nullopt, ManifestValidationError::kInvalidJson};
      }
      while (true) {
        scanner.SkipWhitespace();
        if (scanner.Match(']'))
          break;
        if (!scanner.Match('{')) {
          return {std::nullopt, ManifestValidationError::kInvalidJson};
        }

        ManifestEntry entry;
        bool has_path = false;
        bool has_hash = false;

        while (true) {
          scanner.SkipWhitespace();
          if (scanner.Match('}'))
            break;
          auto fkey = scanner.ParseString();
          if (!fkey || !scanner.Match(':')) {
            return {std::nullopt, ManifestValidationError::kInvalidJson};
          }
          if (*fkey == "path") {
            auto path_val = scanner.ParseString();
            if (!path_val)
              return {std::nullopt, ManifestValidationError::kInvalidJson};
            entry.relative_path = NormalizePath(*path_val);
            has_path = true;
          } else if (*fkey == "sha256") {
            auto hash_val = scanner.ParseString();
            if (!hash_val)
              return {std::nullopt, ManifestValidationError::kInvalidJson};
            entry.sha256 = *hash_val;
            has_hash = true;
          } else if (*fkey == "size") {
            auto size_val = scanner.ParseNumber();
            if (size_val)
              entry.size_bytes = *size_val;
          } else {
            scanner.ParseString();
          }
          scanner.Match(',');
        }

        if (!has_path || !has_hash) {
          return {std::nullopt, ManifestValidationError::kInvalidJson};
        }
        if (!IsPathSafe(entry.relative_path)) {
          return {std::nullopt, ManifestValidationError::kInvalidPath};
        }

        manifest.files.push_back(std::move(entry));
        scanner.Match(',');
      }
    } else {
      scanner.ParseString();
    }
    scanner.Match(',');
  }

  if (!has_version) {
    return {std::nullopt, ManifestValidationError::kMissingVersion};
  }
  if (manifest.files.empty()) {
    return {std::nullopt, ManifestValidationError::kEmptyFileList};
  }

  return {std::move(manifest), ManifestValidationError::kNone};
}

bool ManifestVerifier::LoadManifest(UpdateManifest manifest) {
  entries_by_path_.clear();
  entries_by_path_.reserve(manifest.files.size());
  for (auto &entry : manifest.files) {
    std::string norm = NormalizePath(entry.relative_path);
    if (entries_by_path_.contains(norm)) {
      return false; // Duplicate path
    }
    entries_by_path_[norm] = entry;
  }
  manifest_ = std::move(manifest);
  return true;
}

bool ManifestVerifier::HasFile(std::string_view relative_path) const noexcept {
  std::string norm = NormalizePath(relative_path);
  return entries_by_path_.contains(norm);
}

const ManifestEntry *
ManifestVerifier::GetEntry(std::string_view relative_path) const noexcept {
  std::string norm = NormalizePath(relative_path);
  auto it = entries_by_path_.find(norm);
  if (it != entries_by_path_.end()) {
    return &it->second;
  }
  return nullptr;
}

bool ManifestVerifier::VerifyHash(
    std::string_view relative_path,
    std::string_view actual_sha256) const noexcept {
  const auto *entry = GetEntry(relative_path);
  if (!entry)
    return false;
  if (entry->sha256.size() != actual_sha256.size())
    return false;

  for (size_t i = 0; i < entry->sha256.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(entry->sha256[i])) !=
        std::tolower(static_cast<unsigned char>(actual_sha256[i]))) {
      return false;
    }
  }
  return true;
}

bool ManifestVerifier::ValidateArchiveEntries(
    const std::vector<std::string> &files_in_archive,
    std::string *out_unexpected_file) const {
  for (const auto &file : files_in_archive) {
    std::string norm = NormalizePath(file);
    if (!entries_by_path_.contains(norm)) {
      if (out_unexpected_file) {
        *out_unexpected_file = file;
      }
      return false;
    }
  }
  return true;
}

} // namespace kr::updater
