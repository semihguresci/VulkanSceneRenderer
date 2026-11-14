#include "Container/renderer/scene/RenderSetup.h"

#include "Container/utility/AtomicFile.h"
#include "Container/utility/ContentFingerprint.h"
#include "Container/utility/Platform.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace container::renderer {
namespace {

using Json = nlohmann::json;
constexpr size_t kMaxSidecarBytes = 32 * 1024 * 1024;
constexpr size_t kMaxStoredPathBytes = 32768;
constexpr std::array<std::string_view, 7> kToneMappers{
    "aces", "reinhard", "clip", "filmic", "uncharted", "agx", "neutral"};

[[noreturn]] void Invalid(std::string_view reason) {
  throw std::invalid_argument("Render setup: " + std::string(reason));
}

void ValidateSourceFields(const RenderSetupSource& source) {
  if (source.digest.size() != 64 || source.byteSize == 0 ||
      !std::ranges::all_of(source.digest, [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
      })) {
    Invalid("source identity requires a nonempty primary file and a SHA-256 digest");
  }
}

bool SafeText(std::string_view value, size_t limit) {
  return !value.empty() && value.size() <= limit &&
         std::ranges::none_of(value, [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}

void ValidateName(std::string_view name) {
  if (!SafeText(name, kMaxRenderSetupNameBytes) ||
      std::ranges::all_of(name, [](char ch) { return ch == ' '; })) {
    Invalid("name must contain text and fit within 128 UTF-8 bytes");
  }
}

void ValidatePathText(std::string_view path) {
  if (!SafeText(path, kMaxStoredPathBytes)) {
    Invalid("HDR path is empty, too long, or contains control characters");
  }
}

void ValidateHdrPath(const std::filesystem::path& path) {
  ValidatePathText(container::util::pathToUtf8(path));
  if (!path.is_absolute() || path.filename().empty()) {
    Invalid("HDR path must name a file");
  }
  auto extension = container::util::pathToUtf8(path.extension());
  std::ranges::transform(extension, extension.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  if (extension != ".hdr" && extension != ".exr") {
    Invalid("HDR reference must name a .hdr or .exr file");
  }
}

float GradeNumber(double value, float min, float max, std::string_view field) {
  if (!std::isfinite(value) || value < min || value > max) {
    Invalid(std::string("display grade ") + std::string(field) + " is out of range");
  }
  return static_cast<float>(value);
}

float ReadGradeNumber(const Json& value, float min, float max, std::string_view field) {
  if (!value.is_number()) {
    Invalid(std::string("display grade ") + std::string(field) + " must be numeric");
  }
  return GradeNumber(value.get<double>(), min, max, field);
}

std::array<float, 3> ReadGradeTone(const Json& value, std::string_view field) {
  if (!value.is_array() || value.size() != 3) {
    Invalid(std::string("display grade ") + std::string(field) + " requires three channels");
  }
  return {ReadGradeNumber(value[0], 0.0f, 1.0f, field),
          ReadGradeNumber(value[1], 0.0f, 1.0f, field),
          ReadGradeNumber(value[2], 0.0f, 1.0f, field)};
}

DisplayGrade ReadGrade(const Json& value) {
  if (!value.is_object() || value.size() != 8) {
    Invalid("expected all display grade fields");
  }
  const auto& name = value.at("toneMap").get_ref<const std::string&>();
  const auto mode = std::ranges::find(kToneMappers, name);
  if (mode == kToneMappers.end()) {
    Invalid("unknown display grade tone map");
  }
  return {
      .toneMapper = static_cast<ToneMapper>(mode - kToneMappers.begin()),
      .saturation = ReadGradeNumber(value.at("saturation"), 0.0f, 2.0f, "saturation"),
      .contrast = ReadGradeNumber(value.at("contrast"), 0.5f, 2.0f, "contrast"),
      .temperatureKelvin =
          ReadGradeNumber(value.at("temperatureKelvin"), 2000.0f, 15000.0f, "temperatureKelvin"),
      .tintDuv = ReadGradeNumber(value.at("tintDuv"), -0.025f, 0.025f, "tintDuv"),
      .vibrance = ReadGradeNumber(value.at("vibrance"), -1.0f, 1.0f, "vibrance"),
      .shadowTone = ReadGradeTone(value.at("shadowTone"), "shadowTone"),
      .highlightTone = ReadGradeTone(value.at("highlightTone"), "highlightTone"),
  };
}

Json WriteGrade(const DisplayGrade& grade) {
  const auto mode = static_cast<size_t>(grade.toneMapper);
  if (mode >= kToneMappers.size()) {
    Invalid("unknown display grade tone map");
  }
  return {{"toneMap", kToneMappers[mode]}, {"saturation", grade.saturation},
          {"contrast", grade.contrast}, {"temperatureKelvin", grade.temperatureKelvin},
          {"tintDuv", grade.tintDuv}, {"vibrance", grade.vibrance},
          {"shadowTone", grade.shadowTone}, {"highlightTone", grade.highlightTone}};
}

void ValidateEntryFields(const RenderSetupEntry& entry) {
  ValidateName(entry.name);
  if (entry.visibility.size() > kMaxRenderSetupVisibility) {
    Invalid("too many node visibility entries");
  }
  std::unordered_set<uint32_t> seen;
  seen.reserve(entry.visibility.size());
  for (const auto& flag : entry.visibility) {
    if (!seen.insert(flag.sourceNode).second) {
      Invalid("duplicate visibility entry for source node " + std::to_string(flag.sourceNode));
    }
  }
  if (!entry.hdrPath.empty()) {
    ValidateHdrPath(entry.hdrPath);
  }
  (void)ReadGrade(WriteGrade(entry.grade));
}

template <typename T>
T ReadUnsigned(const Json& value, std::string_view field) {
  if (!value.is_number_unsigned()) {
    Invalid(std::string(field) + " must be an unsigned integer");
  }
  const auto number = value.get<uint64_t>();
  if (number > std::numeric_limits<T>::max()) {
    Invalid(std::string(field) + " is out of range");
  }
  return static_cast<T>(number);
}

std::optional<uint32_t> ReadOptionalIndex(const Json& value, std::string_view field) {
  return value.is_null() ? std::nullopt : std::optional{ReadUnsigned<uint32_t>(value, field)};
}

std::filesystem::path SidecarDirectory(const std::filesystem::path& path) {
  if (path.empty() || path.filename().empty()) {
    Invalid("sidecar path must name a file");
  }
  return std::filesystem::absolute(path).lexically_normal().parent_path();
}

std::filesystem::path ReadHdr(const Json& value, const std::filesystem::path& directory) {
  if (value.is_null()) {
    return {};
  }
  const auto& text = value.get_ref<const std::string&>();
  ValidatePathText(text);
  auto path = container::util::pathFromUtf8(text);
  if (!path.is_absolute()) {
    if (path.has_root_name() || path.has_root_directory()) {
      Invalid("HDR path must be fully absolute or relative to the sidecar");
    }
    path = directory / path;
  }
  return path.lexically_normal();
}

Json WriteHdr(const std::filesystem::path& path, const std::filesystem::path& directory) {
  if (path.empty()) {
    return nullptr;
  }
  const auto relative = path.lexically_relative(directory);
  const auto generic = (relative.empty() ? path : relative).generic_u8string();
  const std::string stored(generic.begin(), generic.end());
  ValidatePathText(stored);
  return stored;
}

RenderSetupEntry ReadEntry(const Json& value, const std::filesystem::path& directory,
                           size_t& remainingFlags) {
  if (!value.is_object() || value.size() != 6) {
    Invalid("expected all render setup fields");
  }
  RenderSetupEntry entry{
      .name = value.at("name").get<std::string>(),
      .cameraSourceNode = ReadOptionalIndex(value.at("camera"), "camera source node"),
      .materialVariant = ReadOptionalIndex(value.at("variant"), "material variant"),
      .hdrPath = ReadHdr(value.at("hdr"), directory),
      .grade = ReadGrade(value.at("grade")),
  };
  const auto& visibility = value.at("visibility");
  if (!visibility.is_array() || visibility.size() > remainingFlags) {
    Invalid("sidecar exceeds the node visibility limit");
  }
  remainingFlags -= visibility.size();
  entry.visibility.reserve(visibility.size());
  for (const auto& flag : visibility) {
    if (!flag.is_object() || flag.size() != 2 || !flag.at("visible").is_boolean()) {
      Invalid("visibility entries require a node index and a boolean");
    }
    entry.visibility.push_back({ReadUnsigned<uint32_t>(flag.at("node"), "visibility source node"),
                                flag.at("visible").get<bool>()});
  }
  return entry;
}

Json WriteEntry(const RenderSetupEntry& entry, const std::filesystem::path& directory) {
  auto visibility = Json::array();
  for (const auto& flag : entry.visibility) {
    visibility.push_back({{"node", flag.sourceNode}, {"visible", flag.visible}});
  }
  return {{"name", entry.name}, {"camera", entry.cameraSourceNode ? Json(*entry.cameraSourceNode) : Json(nullptr)},
          {"variant", entry.materialVariant ? Json(*entry.materialVariant) : Json(nullptr)},
          {"visibility", std::move(visibility)}, {"hdr", WriteHdr(entry.hdrPath, directory)},
          {"grade", WriteGrade(entry.grade)}};
}

Json ReadSidecar(const std::filesystem::path& path) {
  std::error_code error;
  const auto byteSize = std::filesystem::file_size(path, error);
  if (error || byteSize == 0 || byteSize > kMaxSidecarBytes) {
    Invalid("sidecar must be a readable file with at most 32 MiB of JSON");
  }
  std::ifstream file(path, std::ios::binary);
  std::string bytes(static_cast<size_t>(byteSize), '\0');
  if (!file.read(bytes.data(), static_cast<std::streamsize>(bytes.size())) ||
      file.peek() != std::char_traits<char>::eof() || file.bad()) {
    Invalid("sidecar changed or could not be read");
  }
  std::array<std::set<std::string>, 7> keys;
  size_t objects = 0;
  size_t events = 0;
  return Json::parse(bytes, [&](int depth, Json::parse_event_t event, Json& value) {
    if (depth < 0 || size_t(depth) >= keys.size() ||
        ++events > 6 * kMaxRenderSetupVisibility + 64 * kMaxRenderSetups + 16) {
      Invalid("unexpected nesting or too many values in sidecar");
    }
    if (event == Json::parse_event_t::object_start) {
      if (++objects > kMaxRenderSetupVisibility + 2 * kMaxRenderSetups + 2 ||
          size_t(depth + 1) >= keys.size()) {
        Invalid("sidecar exceeds the object or nesting limit");
      }
      keys[depth + 1].clear();
    }
    if (event == Json::parse_event_t::key) {
      const auto& key = value.get_ref<const std::string&>();
      if (key.size() > 64 || keys[depth].size() >= 8 || !keys[depth].insert(key).second) {
        Invalid("duplicate or unexpected sidecar field");
      }
    }
    if (event == Json::parse_event_t::value && value.is_string() &&
        value.get_ref<const std::string&>().size() > kMaxStoredPathBytes) {
      Invalid("sidecar string exceeds the size limit");
    }
    return true;
  });
}

} // namespace

RenderSetupSource FingerprintRenderSetupSource(const std::filesystem::path& path,
                                               uint32_t sceneIndex) {
  auto fingerprint = container::util::FingerprintFileContent(path);
  const RenderSetupSource source{std::move(fingerprint.digest), fingerprint.byteSize, sceneIndex};
  ValidateSourceFields(source);
  return source;
}

void ValidateRenderSetupSource(const RenderSetupSource& saved, const RenderSetupSource& active) {
  ValidateSourceFields(saved);
  ValidateSourceFields(active);
  if (saved.digest != active.digest || saved.byteSize != active.byteSize) {
    Invalid("sidecar belongs to different primary glTF file contents");
  }
  if (saved.sceneIndex != active.sceneIndex) {
    Invalid("sidecar belongs to a different glTF scene in the primary file");
  }
}

void ValidateRenderSetupDocument(const RenderSetupDocument& document) {
  ValidateSourceFields(document.source);
  if (document.setups.size() > kMaxRenderSetups) {
    Invalid("sidecar exceeds the number of saved setups");
  }
  std::set<std::string_view> names;
  size_t remainingFlags = kMaxRenderSetupVisibility;
  for (const auto& entry : document.setups) {
    if (entry.visibility.size() > remainingFlags) {
      Invalid("sidecar exceeds the total node visibility limit");
    }
    remainingFlags -= entry.visibility.size();
    ValidateEntryFields(entry);
    if (!names.insert(entry.name).second) {
      Invalid("duplicate setup name " + entry.name);
    }
  }
}

RenderSetupDocument LoadRenderSetupDocument(const std::filesystem::path& path) {
  try {
    const auto directory = SidecarDirectory(path);
    const auto root = ReadSidecar(path);
    if (!root.is_object() || root.size() != 3 || ReadUnsigned<uint32_t>(root.at("version"), "version") != 1) {
      Invalid("unsupported sidecar version or fields");
    }
    const auto& source = root.at("source");
    if (!source.is_object() || source.size() != 3) {
      Invalid("expected all source identity fields");
    }
    const auto& setups = root.at("setups");
    if (!setups.is_array() || setups.size() > kMaxRenderSetups) {
      Invalid("sidecar exceeds the number of saved setups");
    }
    RenderSetupDocument document{
        .source = {.digest = source.at("digest").get<std::string>(),
                   .byteSize = ReadUnsigned<uint64_t>(source.at("byteSize"), "source byte size"),
                   .sceneIndex = ReadUnsigned<uint32_t>(source.at("sceneIndex"), "scene index")},
    };
    size_t remainingFlags = kMaxRenderSetupVisibility;
    document.setups.reserve(setups.size());
    for (const auto& entry : setups) {
      document.setups.push_back(ReadEntry(entry, directory, remainingFlags));
    }
    ValidateRenderSetupDocument(document);
    return document;
  } catch (const Json::exception& error) {
    Invalid(error.what());
  }
}

void SaveRenderSetupDocument(const std::filesystem::path& path,
                             const RenderSetupDocument& document) {
  ValidateRenderSetupDocument(document);
  const auto directory = SidecarDirectory(path);
  auto setups = Json::array();
  for (const auto& entry : document.setups) {
    setups.push_back(WriteEntry(entry, directory));
  }
  const Json root{{"version", 1},
                  {"source", {{"digest", document.source.digest}, {"byteSize", document.source.byteSize},
                              {"sceneIndex", document.source.sceneIndex}}},
                  {"setups", std::move(setups)}};
  try {
    auto encoded = root.dump();
    if (encoded.size() >= kMaxSidecarBytes) {
      Invalid("sidecar exceeds the 32 MiB size limit");
    }
    encoded.push_back('\n');
    container::util::writeFileAtomically(path, encoded);
  } catch (const Json::exception& error) {
    Invalid(error.what());
  }
}

void ValidateRenderSetupEntry(const RenderSetupEntry& entry, const RenderSetupSource& savedSource,
                              const RenderSetupSource& activeSource,
                              std::span<const RenderSetupNode> activeNodes, size_t variantCount) {
  ValidateRenderSetupSource(savedSource, activeSource);
  ValidateEntryFields(entry);
  if (entry.materialVariant && *entry.materialVariant >= variantCount) {
    Invalid("material variant " + std::to_string(*entry.materialVariant) + " is unavailable");
  }
  struct Binding {
    size_t count{};
    bool camera{};
  };
  std::unordered_map<uint32_t, Binding> bindings;
  bindings.reserve(entry.visibility.size() + size_t(entry.cameraSourceNode.has_value()));
  for (const auto& flag : entry.visibility) {
    bindings.try_emplace(flag.sourceNode);
  }
  if (entry.cameraSourceNode) {
    bindings.try_emplace(*entry.cameraSourceNode);
  }
  for (const auto& node : activeNodes) {
    if (auto found = bindings.find(node.sourceNode); found != bindings.end()) {
      ++found->second.count;
      found->second.camera = node.camera;
    }
  }
  if (entry.cameraSourceNode) {
    const auto& camera = bindings.at(*entry.cameraSourceNode);
    if (camera.count != 1 || !camera.camera) {
      Invalid("camera source node " + std::to_string(*entry.cameraSourceNode) + " has no unique active camera");
    }
  }
  for (const auto& flag : entry.visibility) {
    if (bindings.at(flag.sourceNode).count != 1) {
      Invalid("visibility source node " + std::to_string(flag.sourceNode) +
              " has no unique active binding");
    }
  }
  if (!entry.hdrPath.empty()) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(entry.hdrPath, error)) {
      Invalid("HDR resource is missing or unreadable at " + container::util::pathToUtf8(entry.hdrPath));
    }
  }
}

} // namespace container::renderer
