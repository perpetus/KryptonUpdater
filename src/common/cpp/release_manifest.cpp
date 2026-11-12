#include "krypton/updater/release_manifest.hpp"
#include "krypton/updater/manifest_verifier.hpp"
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <charconv>
#include <format>
#include <sstream>
#include <stdexcept>

namespace kr::updater {
namespace {
bool SafeName(std::string_view value) {
  if (value.empty() || value.size() > 180 || value.find("..") != value.npos)
    return false;
  for (const char ch : value)
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
          (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.'))
      return false;
  return true;
}
bool Digest(std::string_view value) {
  if (value.size() != 64)
    return false;
  for (const char ch : value)
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
      return false;
  return true;
}
SignedAsset Asset(const Poco::JSON::Object::Ptr &object,
                  std::string_view extension) {
  if (!object)
    throw std::runtime_error("Missing release asset.");
  const auto size = object->getValue<Poco::Int64>("size");
  SignedAsset asset{object->getValue<std::string>("name"),
                    object->getValue<std::string>("sha256"),
                    size > 0 ? static_cast<std::uint64_t>(size) : 0};
  if (!SafeName(asset.name) || !asset.name.ends_with(extension) ||
      !Digest(asset.sha256) || !asset.size ||
      asset.size > 2ull * 1024 * 1024 * 1024)
    throw std::runtime_error("Invalid release asset.");
  return asset;
}
} // namespace
std::expected<BrowserVersion, std::string>
ParseBrowserVersion(std::string_view text) {
  BrowserVersion version{};
  for (std::size_t index = 0; index < version.size(); ++index) {
    const auto dot = text.find('.');
    if ((index < 3) != (dot != text.npos))
      return std::unexpected("Expected a four-part browser version.");
    const auto part = text.substr(0, dot);
    const auto [end, error] =
        std::from_chars(part.data(), part.data() + part.size(), version[index]);
    if (part.empty() || error != std::errc{} ||
        end != part.data() + part.size() || version[index] > 65535 ||
        (part.size() > 1 && part.front() == '0'))
      return std::unexpected("Invalid browser version.");
    if (index < 3)
      text.remove_prefix(dot + 1);
  }
  return version;
}
std::expected<SignedRelease, std::string>
ParseSignedRelease(std::string_view json, std::string_view repository,
                   std::uint64_t now, bool allow_expired) {
  try {
    if (json.empty() || json.size() > 16 * 1024 * 1024)
      return std::unexpected("Invalid release metadata size.");
    Poco::JSON::Parser parser;
    parser.setDepth(24);
    const auto root =
        parser.parse(std::string(json)).extract<Poco::JSON::Object::Ptr>();
    if (root->getValue<int>("schema") != 1 ||
        root->getValue<std::string>("repository") != repository ||
        root->getValue<std::string>("channel") != "stable")
      return std::unexpected("Release channel or repository mismatch.");
    SignedRelease release;
    release.version = root->getValue<std::string>("version");
    release.tag = root->getValue<std::string>("tag");
    const auto version = ParseBrowserVersion(release.version);
    if (!version || release.tag != "v" + release.version)
      return std::unexpected("Invalid release version or tag.");
    const auto issued = root->getValue<Poco::Int64>("issued");
    const auto expires = root->getValue<Poco::Int64>("expires");
    const auto sequence = root->getValue<Poco::Int64>("sequence");
    if (issued <= 0 || expires <= issued || sequence <= 0 ||
        static_cast<std::uint64_t>(issued) > now + 300 ||
        (!allow_expired && static_cast<std::uint64_t>(expires) <= now) ||
        expires - issued > 90 * 86400)
      return std::unexpected(
          "The signed release is expired or has an invalid timestamp.");
    release.expires = static_cast<std::uint64_t>(expires);
    release.sequence = static_cast<std::uint64_t>(sequence);
    release.full = Asset(root->getObject("full"), ".7z");
    const auto deltas = root->getArray("deltas");
    if (!deltas || deltas->size() > 4)
      return std::unexpected("Invalid delta list.");
    for (std::size_t index = 0; index < deltas->size(); ++index) {
      const auto object = deltas->getObject(static_cast<unsigned>(index));
      ReleaseDelta delta{Asset(object, ".delta"),
                         object->getValue<std::string>("base_version"),
                         object->getValue<std::string>("base_sha256"),
                         object->optValue<std::string>("algorithm", "msdelta")};
      const auto base = ParseBrowserVersion(delta.base_version);
      if (!base || *base >= *version || !Digest(delta.base_sha256) ||
          delta.asset.size >= release.full.size)
        return std::unexpected("Invalid delta base or size.");
      release.deltas.push_back(std::move(delta));
    }
    release.package_manifest = root->getValue<std::string>("package_manifest");
    release.package_signature =
        root->getValue<std::string>("package_signature");
    if (release.package_signature.size() != 128 ||
        !Digest(release.package_signature.substr(0, 64)) ||
        !Digest(release.package_signature.substr(64)))
      return std::unexpected("Invalid package signature.");
    const auto parsed = ManifestVerifier::ParseJson(release.package_manifest);
    if (!parsed.first)
      return std::unexpected("Invalid package file manifest.");
    const auto native =
        std::format("{}.{}.{}-dev.{}", (*version)[0], (*version)[1],
                    (*version)[2], (*version)[3]);
    if (parsed.first->version.ToString() != native)
      return std::unexpected("Package version mismatch.");
    return release;
  } catch (const std::exception &) {
    return std::unexpected("Malformed signed release metadata.");
  }
}
} // namespace kr::updater
