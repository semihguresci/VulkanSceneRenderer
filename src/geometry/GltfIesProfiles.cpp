#include <Container/geometry/GltfIesProfiles.h>

#include <tiny_gltf.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace container::geometry::gltf {
namespace {

constexpr std::string_view kDataPrefix = "data:application/x-ies-lm-63;base64,";
constexpr size_t kMaxAngles = 1024;
constexpr size_t kMaxMeasurements = 256 * 1024;
constexpr size_t kMaxCombinedProfileBytes = 16 * 1024 * 1024;
constexpr std::string_view kWhitespace = " \t\r\n";
using Value = tinygltf::Value;

[[noreturn]] void fail(std::string_view message) { throw std::invalid_argument(std::string(message)); }

std::string encodeBase64(std::string_view bytes) {
  constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string result;
  result.reserve((bytes.size() + 2) / 3 * 4);
  for (size_t i = 0; i < bytes.size(); i += 3) {
    const auto a = static_cast<unsigned char>(bytes[i]);
    const auto b = i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) : 0;
    const auto c = i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i + 2]) : 0;
    result.push_back(alphabet[a >> 2]);
    result.push_back(alphabet[((a & 3) << 4) | (b >> 4)]);
    result.push_back(i + 1 < bytes.size() ? alphabet[((b & 15) << 2) | (c >> 6)] : '=');
    result.push_back(i + 2 < bytes.size() ? alphabet[c & 63] : '=');
  }
  return result;
}

std::string_view trim(std::string_view value) {
  const auto first = value.find_first_not_of(kWhitespace);
  return first == std::string_view::npos ? std::string_view{} :
      value.substr(first, value.find_last_not_of(kWhitespace) - first + 1);
}

class Numbers {
 public:
  explicit Numbers(std::string_view text) : text_(text) {}

  template <typename T>
  T read(std::string_view field) {
    skip();
    const auto end = text_.find_first_of(" ,\t\r\n");
    auto token = text_.substr(0, end);
    text_.remove_prefix(token.size());
    if (token.starts_with('+')) token.remove_prefix(1);
    T number{};
    if (token.empty()) fail("Missing LM-63 " + std::string(field));
    const auto result = std::from_chars(token.data(), token.data() + token.size(), number);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size() || !std::isfinite(number))
      fail("Invalid LM-63 " + std::string(field));
    return number;
  }

  bool empty() { skip(); return text_.empty(); }

 private:
  void skip() {
    const auto first = text_.find_first_not_of(" ,\t\r\n");
    text_.remove_prefix(first == std::string_view::npos ? text_.size() : first);
  }
  std::string_view text_;
};

std::vector<float> angles(Numbers& input, int count, float limit, std::string_view field) {
  std::vector<float> result;
  result.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    const float angle = input.read<float>(field);
    if (angle < 0 || angle > limit || (!result.empty() && angle <= result.back()))
      fail("LM-63 " + std::string(field) + " values must be increasing and in range");
    result.push_back(angle);
  }
  return result;
}

IesHorizontalSymmetry symmetryFor(const std::vector<float>& horizontal, bool lm95) {
  if (horizontal.front() == 0.0f) {
    if (horizontal.size() == 1) return IesHorizontalSymmetry::Axial;
    if (horizontal.back() == 90.0f) return IesHorizontalSymmetry::Quadrant;
    if (horizontal.back() == 180.0f) return IesHorizontalSymmetry::Half;
    if (horizontal.back() == 360.0f) return IesHorizontalSymmetry::Full;
  }
  if (lm95 && horizontal.front() == 90.0f && horizontal.back() == 270.0f)
    return IesHorizontalSymmetry::OppositeHalf;
  fail("Unsupported Type C horizontal coverage. Expected axial, 0–90, 0–180, 0–360 or LM-63-95 90–270 degrees");
}

struct Interval { size_t low, high; float weight; };

Interval interval(const std::vector<float>& samples, float angle) {
  if (angle <= samples.front()) return {0, 0, 0};
  if (angle >= samples.back()) return {samples.size() - 1, samples.size() - 1, 0};
  const auto high = static_cast<size_t>(std::ranges::upper_bound(samples, angle) - samples.begin());
  return {high - 1, high, (angle - samples[high - 1]) / (samples[high] - samples[high - 1])};
}

const Value* field(const Value& value, const char* key) {
  if (!value.IsObject()) return nullptr;
  const auto& fields = value.Get<Value::Object>();
  const auto item = fields.find(key);
  return item == fields.end() ? nullptr : &item->second;
}

size_t index(const Value* value, size_t count, std::string_view label) {
  if (!value || !value->IsInt() || value->Get<int>() < 0 || static_cast<size_t>(value->Get<int>()) >= count)
    fail(std::string(label) + " index is missing or out of range");
  return static_cast<size_t>(value->Get<int>());
}

double number(const Value& value, double limit, std::string_view label) {
  if (!value.IsNumber()) fail(std::string(label) + " must be numeric");
  const double result = value.GetNumberAsDouble();
  if (!std::isfinite(result) || result < 0 || result > limit)
    fail(std::string(label) + " must be finite and in range");
  return result;
}

std::string readExternalProfile(std::string_view uri, const std::filesystem::path& directory,
    bool allowExternal, uintmax_t& remainingInputBytes) {
  if (!allowExternal) fail("External IES files are disabled for embedded-only import");
  if (uri.empty() || uri.size() > 4096 || uri.contains('\0') || uri.find_first_of(":\\?#") != std::string_view::npos)
    fail("IES URI must name a relative local file");
  std::string decoded;
  decoded.reserve(uri.size());
  for (size_t i = 0; i < uri.size(); ++i) {
    const auto c = uri[i];
    if (c != '%') { decoded.push_back(c); continue; }
    unsigned byte{};
    if (uri.size() - i < 3) fail("Invalid percent escape in IES URI");
    const auto end = uri.data() + i + 3;
    const auto parsed = std::from_chars(uri.data() + i + 1, end, byte, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != end || byte == 0 || byte == ':' || byte == '\\' || byte == '/')
      fail("Invalid percent escape in IES URI");
    decoded.push_back(static_cast<char>(byte));
    i += 2;
  }
  const std::filesystem::path relative = std::filesystem::u8path(decoded);
  if (relative.has_root_path() || std::ranges::find(relative, std::filesystem::path("..")) != relative.end())
    fail("IES URI must remain inside the model directory");
  const auto root = std::filesystem::canonical(directory.empty() ? std::filesystem::path(".") : directory);
  const auto path = std::filesystem::canonical(root / relative);
  if (std::ranges::mismatch(root, path).in1 != root.end() || !std::filesystem::is_regular_file(path))
    fail("IES URI must resolve to a local file inside the model directory");
  const auto size = std::filesystem::file_size(path);
  if (size == 0 || size > kIesMaximumProfileBytes) fail("IES file exceeds the supported byte range");
  if (size > remainingInputBytes) fail("IES file exceeds the remaining glTF input byte budget");
  remainingInputBytes -= size;
  std::ifstream stream(path, std::ios::binary);
  std::string text(static_cast<size_t>(size), '\0');
  if (!stream.read(text.data(), static_cast<std::streamsize>(text.size())) || stream.peek() != EOF || stream.bad())
    fail("Could not read the complete IES file");
  return text;
}

std::string readGltfProfile(const tinygltf::Model& model, const Value& entry,
    const std::filesystem::path& directory, bool allowExternal, uintmax_t& remainingInputBytes) {
  const auto* uri = field(entry, "uri");
  const auto* view = field(entry, "bufferView");
  if (bool(uri) == bool(view)) fail("IES profile requires exactly one URI or buffer view");
  if (uri) {
    if (!uri->IsString()) fail("IES URI must be a string");
    const auto& path = uri->Get<std::string>();
    return path.starts_with("data:") ? DecodeGltfIesProfileUri(path) :
        readExternalProfile(path, directory, allowExternal, remainingInputBytes);
  }
  const auto* mime = field(entry, "mimeType");
  if (!mime || !mime->IsString() || mime->Get<std::string>() != kIesMimeType)
    fail("IES buffer view requires application/x-ies-lm-63 MIME type");
  const auto& range = model.bufferViews[index(view, model.bufferViews.size(), "IES buffer view")];
  if (range.buffer < 0 || static_cast<size_t>(range.buffer) >= model.buffers.size() ||
      range.byteLength == 0 || range.byteLength > kIesMaximumProfileBytes || range.byteStride != 0)
    fail("Invalid IES buffer range");
  const auto& bytes = model.buffers[static_cast<size_t>(range.buffer)].data;
  if (range.byteOffset > bytes.size() || range.byteLength > bytes.size() - range.byteOffset)
    fail("IES buffer range exceeds the loaded buffer");
  return {reinterpret_cast<const char*>(bytes.data() + range.byteOffset), range.byteLength};
}

} // namespace

IesProfile ParseIesProfile(std::string_view text) {
  if (text.empty() || text.size() > kIesMaximumProfileBytes || text.contains('\0'))
    fail("IES file exceeds the supported text range");
  if (text.starts_with("\xef\xbb\xbf")) text.remove_prefix(3);
  const auto newline = text.find_first_of("\r\n");
  if (newline == std::string_view::npos) fail("Missing LM-63 header");
  const auto header = trim(text.substr(0, newline));
  const bool lm95 = header == "IESNA:LM-63-1995";
  if (!lm95 && header != "IESNA:LM-63-2002" && header != "IES:LM-63-2019")
    fail("Unsupported IES header. Expected LM-63-1995, LM-63-2002 or LM-63-2019");
  text.remove_prefix(newline + 1);
  bool foundTilt = false;
  while (!text.empty()) {
    const auto end = text.find_first_of("\r\n");
    if (end == std::string_view::npos) break;
    const auto line = trim(text.substr(0, end));
    text.remove_prefix(end + 1);
    if (!line.starts_with("TILT=")) continue;
    if (trim(line.substr(5)) != "NONE") fail("Only TILT=NONE is supported for LM-63 profiles");
    foundTilt = true;
    break;
  }
  if (!foundTilt) fail("Missing LM-63 TILT record");
  Numbers input(text);
  input.read<int>("lamp count");
  input.read<double>("lumens per lamp");
  const auto multiplier = input.read<double>("candela multiplier");
  const auto vertical = input.read<int>("vertical angle count");
  const auto horizontal = input.read<int>("horizontal angle count");
  if (input.read<int>("photometry type") != 1) fail("Only Type C photometry is supported");
  const auto units = input.read<int>("unit type");
  if (units != 1 && units != 2) fail("Invalid LM-63 unit type");
  input.read<double>("luminous width");
  input.read<double>("luminous length");
  input.read<double>("luminous height");
  const auto ballast = input.read<double>("ballast factor");
  const auto legacyFactor = input.read<double>("file generation or ballast lamp factor");
  input.read<double>("input watts");
  if (vertical < 2 || horizontal < 1 || static_cast<size_t>(vertical) > kMaxAngles ||
      static_cast<size_t>(horizontal) > kMaxAngles || static_cast<size_t>(vertical) * horizontal > kMaxMeasurements)
    fail("LM-63 angular table exceeds the supported measurement range");
  const double scale = multiplier * ballast * (lm95 ? legacyFactor : 1.0);
  if (!(multiplier > 0) || !(ballast > 0) || !(scale > 0) || !std::isfinite(scale))
    fail("Invalid LM-63 candela or ballast scale");

  IesProfile profile;
  profile.verticalDegrees = angles(input, vertical, 180.0f, "vertical angle");
  profile.horizontalDegrees = angles(input, horizontal, 360.0f, "horizontal angle");
  profile.symmetry = symmetryFor(profile.horizontalDegrees, lm95);
  const size_t count = static_cast<size_t>(vertical) * horizontal;
  profile.candela.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    const double measured = input.read<double>("candela measurement");
    const double candela = measured * scale;
    if (measured < 0 || !std::isfinite(candela) || candela > std::numeric_limits<float>::max())
      fail("Invalid or unrepresentable LM-63 candela measurement");
    profile.candela.push_back(static_cast<float>(candela));
  }
  if (!input.empty()) fail("Unexpected data after the LM-63 candela table");
  return profile;
}

float IesProfile::sample(float vertical, float horizontal) const {
  if (!std::isfinite(vertical) || !std::isfinite(horizontal) || verticalDegrees.empty() ||
      vertical < verticalDegrees.front() || vertical > verticalDegrees.back()) return 0;
  horizontal = std::fmod(horizontal, 360.0f);
  if (horizontal < 0.0f) horizontal += 360.0f;
  switch (symmetry) {
    case IesHorizontalSymmetry::Axial: horizontal = 0; break;
    case IesHorizontalSymmetry::Quadrant:
      horizontal = std::fmod(horizontal, 180.0f);
      horizontal = std::min(horizontal, 180.0f - horizontal);
      break;
    case IesHorizontalSymmetry::Half: horizontal = std::min(horizontal, 360.0f - horizontal); break;
    case IesHorizontalSymmetry::OppositeHalf:
      if (horizontal < 90.0f) horizontal = 180.0f - horizontal;
      if (horizontal > 270.0f) horizontal = 540.0f - horizontal;
      break;
    case IesHorizontalSymmetry::Full: break;
  }
  const auto v = interval(verticalDegrees, vertical);
  const auto h = interval(horizontalDegrees, horizontal);
  const auto read = [&](size_t row, size_t column) { return candela[row * verticalDegrees.size() + column]; };
  return std::lerp(std::lerp(read(h.low, v.low), read(h.low, v.high), v.weight),
                   std::lerp(read(h.high, v.low), read(h.high, v.high), v.weight), h.weight);
}

std::vector<float> IesProfile::packedForGpu() const {
  std::vector<float> packed;
  packed.reserve(4 + verticalDegrees.size() + horizontalDegrees.size() + candela.size());
  packed.insert(packed.end(), {static_cast<float>(verticalDegrees.size()), static_cast<float>(horizontalDegrees.size()),
      static_cast<float>(symmetry), 0.0f});
  packed.insert(packed.end(), verticalDegrees.begin(), verticalDegrees.end());
  packed.insert(packed.end(), horizontalDegrees.begin(), horizontalDegrees.end());
  packed.insert(packed.end(), candela.begin(), candela.end());
  return packed;
}

GltfIesBinding ParseGltfIesBinding(const Value& value, size_t profileCount) {
  GltfIesBinding binding;
  binding.profile = index(field(value, "light"), profileCount, "IES light profile");
  if (const auto* scale = field(value, "multiplier"))
    binding.multiplier = static_cast<float>(number(*scale, std::numeric_limits<float>::max(), "IES multiplier"));
  if (const auto* tint = field(value, "color")) {
    if (!tint->IsArray() || tint->ArrayLen() != 3) fail("IES color requires three linear RGB channels");
    for (int c = 0; c < 3; ++c) binding.color[c] = static_cast<float>(number(tint->Get(c), 1.0, "IES color"));
  }
  return binding;
}

std::string DecodeGltfIesProfileUri(std::string_view uri) {
  if (!uri.starts_with(kDataPrefix) || uri.size() > kDataPrefix.size() + (kIesMaximumProfileBytes + 2) / 3 * 4)
    fail("Embedded IES requires a bounded application/x-ies-lm-63 base64 URI");
  uri.remove_prefix(kDataPrefix.size());
  std::vector<unsigned char> decoded;
  std::string mime;
  if (!tinygltf::DecodeDataURI(&decoded, mime, "data:application/octet-stream;base64," + std::string(uri), 0, false) ||
      decoded.empty() || decoded.size() > kIesMaximumProfileBytes)
    fail("Malformed base64 IES profile URI");
  std::string result(decoded.begin(), decoded.end());
  if (encodeBase64(result) != uri) fail("Malformed base64 IES profile URI");
  return result;
}

const std::string* GltfIesEmbeddedProfileUri(const tinygltf::Model& model, size_t index) {
  const auto root = model.extensions.find(kGltfIesExtension);
  if (root == model.extensions.end()) return nullptr;
  const auto* lights = field(root->second, "lights");
  if (!lights || !lights->IsArray() || index >= lights->ArrayLen()) return nullptr;
  const auto* uri = field(lights->Get(static_cast<int>(index)), "uri");
  if (!uri || !uri->IsString() || !uri->Get<std::string>().starts_with(kDataPrefix)) return nullptr;
  return &uri->Get<std::string>();
}

std::vector<std::string> PrepareGltfIesProfiles(tinygltf::Model& model,
    const std::filesystem::path& directory, bool allowExternal, uintmax_t& remainingInputBytes) {
  std::vector<std::string> diagnostics;
  const auto root = model.extensions.find(kGltfIesExtension);
  const auto* lights = root == model.extensions.end() ? nullptr : field(root->second, "lights");
  const bool validTable = lights && lights->IsArray() && lights->ArrayLen() > 0 && lights->ArrayLen() <= kIesMaximumProfiles;
  if (!validTable && (root != model.extensions.end() ||
      std::ranges::find(model.extensionsRequired, kGltfIesExtension) != model.extensionsRequired.end()))
    diagnostics.emplace_back("Invalid EXT_lights_ies profile table. Expected 1–64 entries");
  const auto count = validTable ? lights->ArrayLen() : 0;
  std::vector<int> profileMap(count, -1);
  Value::Array profiles;
  size_t total = 0;
  for (size_t i = 0; i < count; ++i) {
    auto entry = lights->Get(static_cast<int>(i));
    try {
      const auto text = readGltfProfile(model, entry, directory, allowExternal, remainingInputBytes);
      if (text.size() > kMaxCombinedProfileBytes - total) fail("IES profile table exceeds the combined byte budget");
      (void)ParseIesProfile(text);
      total += text.size();
      auto& fields = entry.Get<Value::Object>();
      fields.erase("bufferView");
      fields.erase("mimeType");
      fields.insert_or_assign("uri", Value(std::string(kDataPrefix) + encodeBase64(text)));
      profileMap[i] = static_cast<int>(profiles.size());
      profiles.push_back(std::move(entry));
    } catch (const std::exception& error) {
      diagnostics.push_back("EXT_lights_ies profile " + std::to_string(i) + ": " + error.what());
    }
  }
  for (size_t i = 0; i < model.nodes.size(); ++i) {
    auto& node = model.nodes[i];
    const auto found = node.extensions.find(kGltfIesExtension);
    if (found == node.extensions.end()) continue;
    try {
      const auto binding = ParseGltfIesBinding(found->second, count);
      if (profileMap[binding.profile] < 0) fail("IES profile is unavailable");
      found->second.Get<Value::Object>().at("light") = Value(profileMap[binding.profile]);
    } catch (const std::exception& error) {
      diagnostics.push_back("EXT_lights_ies node " + std::to_string(i) + ": " + error.what());
      node.extensions.erase(found);
    }
    node.extensions_json_string.clear();
  }
  if (profiles.empty()) {
    if (model.extensions.erase(kGltfIesExtension)) model.extensions_json_string.clear();
    std::erase(model.extensionsUsed, kGltfIesExtension);
    std::erase(model.extensionsRequired, kGltfIesExtension);
  } else {
    root->second.Get<Value::Object>().at("lights") = Value(std::move(profiles));
    model.extensions_json_string.clear();
  }
  return diagnostics;
}

} // namespace container::geometry::gltf
