#include "Container/geometry/OpenVdbFileAuthoringSpec.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <nlohmann/json.hpp>
#include <set>
#endif

namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable() {
  throw std::runtime_error("OpenVDB file authoring specifications are "
                           "unavailable; configure with -DENABLE_OPENVDB=ON.");
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
constexpr uintmax_t Fixed = 1024 * 1024;
void add(uintmax_t &value, uintmax_t extra, uintmax_t cap) {
  if (value > cap || extra > cap - value)
    throw std::invalid_argument(
        "author-file parser/model workspace exceeds maxInputBytes");
  value += extra;
}
uintmax_t reserve(uintmax_t encoded, uintmax_t decoded, uintmax_t cap) {
  uintmax_t total = 0;
  add(total, Fixed, cap);
  for (unsigned n = 0; n < 3; ++n)
    add(total, encoded, cap);
  for (unsigned n = 0; n < 4; ++n)
    add(total, decoded, cap);
  return total;
}
void validate(const FileAuthoringSpecOptions &o, const ProcessingOptions &p) {
  if (!o.maxSpecBytes || o.maxSpecBytes > 64ull * 1024 * 1024 || !o.maxDepth ||
      o.maxDepth > 64 || !o.maxGrids || o.maxGrids > 4096 ||
      !o.maxMetadataBytes || !o.maxEdits || o.maxEdits > 65536 ||
      !o.maxPoints || !o.maxPointAttributes || o.maxPointAttributes > 1024 ||
      !o.maxPointGroups || o.maxPointGroups > 4096 || !p.maxInputBytes ||
      !p.maxVoxelCount)
    throw std::invalid_argument(
        "invalid author-file spec/parser/native limits");
}
void lexical(const std::string &input, size_t maxDepth) {
  size_t depth = 0, token = 0;
  bool quoted = false, escaped = false;
  for (const unsigned char c : input) {
    if (quoted) {
      if (escaped)
        escaped = false;
      else if (c == '\\')
        escaped = true;
      else if (c == '"')
        quoted = false;
      continue;
    }
    if (c == '"') {
      quoted = true;
      token = 0;
    } else if (c == '{' || c == '[') {
      if (++depth > maxDepth)
        throw std::invalid_argument("author-file JSON exceeds maxDepth");
      token = 0;
    } else if (c == '}' || c == ']') {
      if (depth)
        --depth;
      token = 0;
    } else if (c == ':' || c == ',' || c == ' ' || c == '\r' || c == '\n' ||
               c == '\t')
      token = 0;
    else if (++token > 128)
      throw std::invalid_argument(
          "author-file raw numeric/literal token exceeds 128 bytes");
  }
}
using Number = std::variant<int64_t, uint64_t, double>;
using Atom = std::variant<std::monostate, bool, Number, std::string,
                          std::array<Number, 3>>;
enum class Kind {
  Root,
  Inputs,
  Input,
  Select,
  Remove,
  GridEdits,
  GridEdit,
  FileMetadata,
  GridMetadata,
  Metadata,
  Vector
};
struct Frame {
  Kind kind;
  std::string key;
  std::set<std::string> keys;
};
struct MetadataBuilder {
  std::string name, operation, type;
  Atom value;
  std::vector<Number> vector;
};
class Parser final : public nlohmann::json_sax<nlohmann::json> {
public:
  Parser(const std::filesystem::path &base,
         const FileAuthoringSpecOptions &limits, uintmax_t encoded,
         uintmax_t cap)
      : base_(base), limits_(limits), encoded_(encoded), cap_(cap) {
    authoring.maxGrids = limits.maxGrids;
    authoring.maxMetadataBytes = limits.maxMetadataBytes;
    authoring.maxEdits = limits.maxEdits;
    authoring.maxPoints = limits.maxPoints;
    authoring.maxPointAttributes = limits.maxPointAttributes;
    authoring.maxPointGroups = limits.maxPointGroups;
  }
  uintmax_t retainedReserve() const {
    return reserve(encoded_, decoded_, cap_);
  }
  FileAuthoringOptions authoring;
  std::string error;
  bool null() override {
    return fail("null is not permitted by the author-file schema");
  }
  bool boolean(bool value) override {
    if (is(Kind::Metadata) && current().key == "value") {
      metadata_.value = value;
      return true;
    }
    if (is(Kind::Root)) {
      if (current().key == "activeMaskCompression") {
        authoring.activeMaskCompression = value;
        return true;
      }
      if (current().key == "saveFloatAsHalf") {
        authoring.saveFloatAsHalf = value;
        return true;
      }
    }
    return fail("unexpected boolean in author-file specification");
  }
  bool number_integer(number_integer_t value) override {
    return number(int64_t(value));
  }
  bool number_unsigned(number_unsigned_t value) override {
    return number(uint64_t(value));
  }
  bool number_float(number_float_t value, const string_t &token) override {
    bool nonzero = false;
    for (const char c : token) {
      if (c == 'e' || c == 'E')
        break;
      if (c >= '1' && c <= '9')
        nonzero = true;
    }
    if (!std::isfinite(value) || (value == 0 && nonzero))
      return fail("numeric metadata overflow/nonzero underflow is unsupported");
    return number(double(value));
  }
  bool string(string_t &value) override {
    account(value.size() + 128);
    if (frames_.empty())
      return fail("author-file root must be an object");
    if (is(Kind::Select) || is(Kind::Remove)) {
      operation();
      auto &list = is(Kind::Select) ? input().select : input().remove;
      list.push_back(std::move(value));
      return true;
    }
    const auto &key = current().key;
    if (is(Kind::Input) && key == "path") {
      if (value.empty() || value.size() > 4096 ||
          value.find('\0') != std::string::npos)
        return fail("input path must contain 1..4096 UTF-8 bytes without NUL");
      const auto path = std::filesystem::u8path(value);
      input().path =
          path.is_absolute() ? path : (base_ / path).lexically_normal();
      return true;
    }
    if (is(Kind::GridEdit)) {
      if (key == "selector") {
        gridEdit().selector = std::move(value);
        return true;
      }
      if (key == "newName") {
        gridEdit().newName = std::move(value);
        return true;
      }
    }
    if (is(Kind::Metadata)) {
      if (key == "name")
        metadata_.name = std::move(value);
      else if (key == "operation")
        metadata_.operation = std::move(value);
      else if (key == "type")
        metadata_.type = std::move(value);
      else if (key == "value")
        metadata_.value = std::move(value);
      else
        return fail("unexpected metadata string");
      return true;
    }
    if (is(Kind::Root)) {
      if (key == "metadataConflict") {
        if (value == "reject")
          authoring.metadataConflict = FileMetadataConflict::Reject;
        else if (value == "preferFirst")
          authoring.metadataConflict = FileMetadataConflict::PreferFirst;
        else if (value == "preferLast")
          authoring.metadataConflict = FileMetadataConflict::PreferLast;
        else
          return fail("invalid metadataConflict enum");
        return true;
      }
      if (key == "compression") {
        if (value == "zip")
          authoring.compression = FileCompression::Zip;
        else if (value == "blosc")
          authoring.compression = FileCompression::Blosc;
        else if (value == "none")
          authoring.compression = FileCompression::None;
        else
          return fail("invalid compression enum");
        return true;
      }
    }
    return fail("unexpected string in author-file specification");
  }
  bool binary(binary_t &) override {
    return fail("binary JSON is not supported");
  }
  bool start_object(std::size_t) override {
    account(256);
    if (frames_.empty()) {
      frames_.push_back({Kind::Root});
      return true;
    }
    if (is(Kind::Inputs)) {
      if (authoring.inputs.size() == 16)
        return fail("author-file inputs exceeds16");
      authoring.inputs.emplace_back();
      frames_.push_back({Kind::Input});
      return true;
    }
    if (is(Kind::GridEdits)) {
      operation();
      input().edits.emplace_back();
      frames_.push_back({Kind::GridEdit});
      return true;
    }
    if (is(Kind::FileMetadata) || is(Kind::GridMetadata)) {
      operation();
      metadata_ = {};
      frames_.push_back({Kind::Metadata});
      return true;
    }
    return fail("unexpected object in author-file specification");
  }
  bool key(string_t &key) override {
    account(key.size() + 128);
    if (frames_.empty())
      return fail("unexpected JSON key");
    auto &frame = current();
    if (!frame.keys.insert(key).second)
      return fail("duplicate author-file JSON key: " + key);
    const auto allowed = [&](std::initializer_list<const char *> names) {
      return std::any_of(names.begin(), names.end(),
                         [&](const char *name) { return key == name; });
    };
    bool valid = false;
    if (is(Kind::Root))
      valid =
          allowed({"version", "inputs", "metadata", "metadataConflict",
                   "compression", "activeMaskCompression", "saveFloatAsHalf"});
    if (is(Kind::Input))
      valid = allowed({"path", "select", "remove", "edits"});
    if (is(Kind::GridEdit))
      valid = allowed({"selector", "newName", "metadata"});
    if (is(Kind::Metadata))
      valid = allowed({"name", "operation", "type", "value"});
    if (!valid)
      return fail("unknown author-file JSON key: " + key);
    frame.key = std::move(key);
    return true;
  }
  bool end_object() override {
    if (frames_.empty())
      return fail("unexpected object end");
    const auto &keys = current().keys;
    if (is(Kind::Root) &&
        (!keys.contains("version") || !keys.contains("inputs") ||
         authoring.inputs.empty()))
      return fail("author-file requires version1 and nonempty inputs");
    if (is(Kind::Input) && !keys.contains("path"))
      return fail("author-file input requires path");
    if (is(Kind::GridEdit) && !keys.contains("selector"))
      return fail("author-file grid edit requires selector");
    if (is(Kind::Metadata) && !finishMetadata())
      return false;
    frames_.pop_back();
    return true;
  }
  bool start_array(std::size_t) override {
    account(128);
    if (frames_.empty())
      return fail("author-file root must be an object");
    const auto key = current().key;
    std::optional<Kind> kind;
    if (is(Kind::Root) && key == "inputs")
      kind = Kind::Inputs;
    if (is(Kind::Root) && key == "metadata")
      kind = Kind::FileMetadata;
    if (is(Kind::Input) && key == "select")
      kind = Kind::Select;
    if (is(Kind::Input) && key == "remove")
      kind = Kind::Remove;
    if (is(Kind::Input) && key == "edits")
      kind = Kind::GridEdits;
    if (is(Kind::GridEdit) && key == "metadata")
      kind = Kind::GridMetadata;
    if (is(Kind::Metadata) && key == "value")
      kind = Kind::Vector;
    if (!kind)
      return fail("unexpected array in author-file specification");
    frames_.push_back({*kind});
    return true;
  }
  bool end_array() override {
    if (is(Kind::Vector)) {
      if (metadata_.vector.size() != 3)
        return fail("vector metadata requires exactly3 numeric components");
      metadata_.value = std::array<Number, 3>{
          metadata_.vector[0], metadata_.vector[1], metadata_.vector[2]};
    }
    if (frames_.empty())
      return fail("unexpected array end");
    frames_.pop_back();
    return true;
  }
  bool parse_error(std::size_t, const std::string &,
                   const nlohmann::detail::exception &exception) override {
    return fail(std::string("invalid author-file JSON: ") + exception.what());
  }

private:
  Frame &current() { return frames_.back(); }
  bool is(Kind kind) const {
    return !frames_.empty() && frames_.back().kind == kind;
  }
  FileAuthoringInput &input() { return authoring.inputs.back(); }
  FileGridEdit &gridEdit() { return input().edits.back(); }
  bool fail(std::string reason) {
    if (error.empty())
      error = std::move(reason);
    return false;
  }
  void account(uintmax_t bytes) {
    add(decoded_, bytes, cap_);
    (void)retainedReserve();
  }
  void operation() {
    if (operations_ == limits_.maxEdits)
      throw std::invalid_argument("author-file exceeds external maxEdits");
    ++operations_;
  }
  bool number(Number value) {
    if (is(Kind::Root) && current().key == "version") {
      const bool version = std::visit(
          [](const auto &v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, double>)
              return false;
            else
              return v == 1;
          },
          value);
      return version || fail("author-file version must be integer1");
    }
    if (is(Kind::Metadata) && current().key == "value") {
      metadata_.value = value;
      return true;
    }
    if (is(Kind::Vector)) {
      if (metadata_.vector.size() == 3)
        return fail("vector metadata has more than3 components");
      metadata_.vector.push_back(value);
      return true;
    }
    return fail("unexpected number in author-file specification");
  }
  template <class T> T real(const Number &number) {
    // Keep integer tokens exact until their requested destination conversion.
    // An intermediate Double can round an integer onto a Float midpoint.
    return std::visit(
        [](auto value) {
          const T converted = static_cast<T>(value);
          if (!std::isfinite(converted) || (value != 0 && converted == 0))
            throw std::invalid_argument(
                "typed metadata overflow/nonzero underflow");
          return converted;
        },
        number);
  }
  int64_t integer(const Number &value) {
    if (const auto *signedValue = std::get_if<int64_t>(&value))
      return *signedValue;
    if (const auto *unsignedValue = std::get_if<uint64_t>(&value)) {
      if (*unsignedValue <= uint64_t(INT64_MAX))
        return int64_t(*unsignedValue);
    }
    throw std::invalid_argument(
        "int32/int64 metadata requires an exact in-range integer token");
  }
  bool finishMetadata() {
    const auto &keys = current().keys;
    if (!keys.contains("name") || !keys.contains("operation"))
      return fail("metadata requires name and operation");
    FileMetadataEdit edit;
    edit.name = std::move(metadata_.name);
    if (metadata_.operation == "remove") {
      if (keys.contains("type") || keys.contains("value"))
        return fail("metadata Remove must not contain type/value");
      edit.operation = FileMetadataOperation::Remove;
    } else if (metadata_.operation == "set") {
      if (!keys.contains("type") || !keys.contains("value"))
        return fail("metadata Set requires type and value");
      const auto *numeric = std::get_if<Number>(&metadata_.value);
      if (metadata_.type == "bool") {
        const auto *v = std::get_if<bool>(&metadata_.value);
        if (!v)
          return fail("bool metadata requires boolean value");
        edit.value = *v;
      } else if (metadata_.type == "string") {
        auto *v = std::get_if<std::string>(&metadata_.value);
        if (!v)
          return fail("string metadata requires string value");
        edit.value = std::move(*v);
      } else if (metadata_.type == "int32" || metadata_.type == "int64") {
        if (!numeric)
          return fail("integer metadata requires integer value");
        const auto v = integer(*numeric);
        if (metadata_.type == "int32") {
          if (v < INT32_MIN || v > INT32_MAX)
            return fail("int32 metadata is out of range");
          edit.value = int32_t(v);
        } else
          edit.value = v;
      } else if (metadata_.type == "float" || metadata_.type == "double") {
        if (!numeric)
          return fail("real metadata requires numeric value");
        if (metadata_.type == "float")
          edit.value = real<float>(*numeric);
        else
          edit.value = real<double>(*numeric);
      } else if (metadata_.type == "vec3s" || metadata_.type == "vec3d") {
        const auto *v = std::get_if<std::array<Number, 3>>(&metadata_.value);
        if (!v)
          return fail("vector metadata requires a three-number array");
        if (metadata_.type == "vec3s")
          edit.value = std::array<float, 3>{
              real<float>((*v)[0]), real<float>((*v)[1]), real<float>((*v)[2])};
        else
          edit.value = std::array<double, 3>{real<double>((*v)[0]),
                                             real<double>((*v)[1]),
                                             real<double>((*v)[2])};
      } else
        return fail("unknown typed metadata name");
    } else
      return fail("metadata operation must be set or remove");
    if (frames_.size() < 2)
      return fail("invalid metadata nesting");
    const auto parent = frames_[frames_.size() - 2].kind;
    if (parent == Kind::FileMetadata)
      authoring.metadata.push_back(std::move(edit));
    else if (parent == Kind::GridMetadata)
      gridEdit().metadata.push_back(std::move(edit));
    else
      return fail("invalid metadata parent");
    return true;
  }
  std::filesystem::path base_;
  const FileAuthoringSpecOptions &limits_;
  uintmax_t encoded_{0}, cap_{0}, decoded_{0};
  size_t operations_{0};
  std::vector<Frame> frames_;
  MetadataBuilder metadata_;
};
#endif
} // namespace

FileAuthoringResult
WriteAuthoredVdbSpec(const std::filesystem::path &specification,
                     const std::filesystem::path &output,
                     const FileAuthoringSpecOptions &o,
                     const ProcessingOptions &p) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validate(o, p);
  const auto absoluteSpec = std::filesystem::absolute(specification);
  if (!std::filesystem::is_regular_file(absoluteSpec))
    throw std::invalid_argument(
        "author-file specification must be a regular file");
  const auto bytes = std::filesystem::file_size(absoluteSpec);
  if (!bytes || bytes > o.maxSpecBytes)
    throw std::invalid_argument(
        "author-file specification exceeds maxSpecBytes or is empty");
  (void)reserve(bytes, 0, p.maxInputBytes);
  std::string snapshot(static_cast<size_t>(bytes), '\0');
  {
    std::ifstream input(absoluteSpec, std::ios::binary);
    if (!input)
      throw std::runtime_error("could not open author-file specification");
    input.read(snapshot.data(), std::streamsize(snapshot.size()));
    if (input.gcount() != std::streamsize(snapshot.size()) || input.bad() ||
        input.peek() != std::char_traits<char>::eof())
      throw std::runtime_error(
          "author-file specification changed size during snapshot");
  }
  lexical(snapshot, o.maxDepth);
  Parser parser(absoluteSpec.parent_path(), o, bytes, p.maxInputBytes);
  if (!nlohmann::json::sax_parse(snapshot, &parser))
    throw std::invalid_argument(parser.error.empty()
                                    ? "invalid author-file specification"
                                    : parser.error);
  const auto retained = parser.retainedReserve();
  if (retained >= p.maxInputBytes)
    throw std::invalid_argument(
        "author-file parser leaves no native processing budget");
  auto limits = p;
  limits.maxInputBytes -= retained;
  return WriteAuthoredVdb(output, parser.authoring, limits);
#else
  (void)specification;
  (void)output;
  (void)o;
  (void)p;
  unavailable();
#endif
}
} // namespace container::geometry::vdb
