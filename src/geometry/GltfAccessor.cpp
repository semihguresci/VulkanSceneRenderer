#include <Container/geometry/GltfAccessor.h>
#include <Container/utility/ModelImportCancellation.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace container::geometry::gltf {
namespace {
uint64_t unsignedValue(const unsigned char* bytes, size_t width) {
  uint64_t value = 0;
  for (size_t i = 0; i < width; ++i) value |= uint64_t(bytes[i]) << (i*8);
  return value;
}
}

bool HasRequiredExtension(const tinygltf::Model& source, std::string_view extension) {
  return std::find(source.extensionsRequired.begin(),source.extensionsRequired.end(),extension) != source.extensionsRequired.end();
}
[[noreturn]] void CheckedAccessor::fail(std::string_view message) const {
  throw std::runtime_error("glTF " + label_ + ": " + std::string(message));
}
CheckedAccessor::CheckedAccessor(const tinygltf::Model& source, int index,
                                 std::string_view label, AccessorReadOptions options,
                                 const std::stop_token& stop)
  : label_(label), options_(options) {
  if (index < 0 || static_cast<size_t>(index) >= source.accessors.size()) fail("accessor index is out of range");
  accessor_ = &source.accessors[static_cast<size_t>(index)];
  const auto& a = *accessor_;
  if (!a.count || a.bufferView < -1) fail("accessor count or buffer view is invalid");
  switch (a.componentType) {
    case TINYGLTF_COMPONENT_TYPE_BYTE: case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: componentWidth_ = 1; break;
    case TINYGLTF_COMPONENT_TYPE_SHORT: case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: componentWidth_ = 2; break;
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: case TINYGLTF_COMPONENT_TYPE_FLOAT: componentWidth_ = 4; break;
    case TINYGLTF_COMPONENT_TYPE_DOUBLE:
      if (!options.allowFloat64 || !HasRequiredExtension(source,"KHR_accessor_float64"))
        fail("DOUBLE requires explicit experimental KHR_accessor_float64 opt-in in extensionsRequired");
      componentWidth_ = 8; break;
    default: fail("unsupported accessor component type");
  }
  if (a.normalized && a.componentType != TINYGLTF_COMPONENT_TYPE_BYTE &&
      a.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE && a.componentType != TINYGLTF_COMPONENT_TYPE_SHORT &&
      a.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) fail("only 8/16-bit integer components can be normalized");
  size_t rows = 1, columns = 1;
  switch (a.type) {
    case TINYGLTF_TYPE_SCALAR: break;
    case TINYGLTF_TYPE_VEC2: rows = 2; break;
    case TINYGLTF_TYPE_VEC3: rows = 3; break;
    case TINYGLTF_TYPE_VEC4: rows = 4; break;
    case TINYGLTF_TYPE_MAT2: rows = columns = 2; break;
    case TINYGLTF_TYPE_MAT3: rows = columns = 3; break;
    case TINYGLTF_TYPE_MAT4: rows = columns = 4; break;
    default: fail("unsupported accessor shape");
  }
  components_ = rows*columns;
  const size_t columnBytes = rows*componentWidth_;
  const size_t columnStride = columns > 1 ? (columnBytes+3)/4*4 : columnBytes;
  encodedStride_ = columns*columnStride;
  // Final column's padding may be absent at the end of a view.
  const size_t elementExtent = (columns-1)*columnStride+columnBytes;
  for (size_t column = 0; column < columns; ++column)
    for (size_t row = 0; row < rows; ++row)
      componentOffsets_[column*rows+row] = column*columnStride+row*componentWidth_;
  const auto bytes = [&](int viewIndex, size_t offset, size_t count, size_t extent,
                          size_t packedStride, size_t alignment, bool sparse,
                          bool matrixElements, size_t& stride) {
    if (viewIndex < 0 || static_cast<size_t>(viewIndex) >= source.bufferViews.size()) fail("buffer view index is out of range");
    const auto& view = source.bufferViews[static_cast<size_t>(viewIndex)];
    if (sparse && view.target != 0) fail("sparse buffer views must omit target and byte stride");
    if (view.buffer < 0 || static_cast<size_t>(view.buffer) >= source.buffers.size()) fail("buffer index is out of range");
    const auto& data = source.buffers[static_cast<size_t>(view.buffer)].data;
    if (view.byteOffset > data.size() || view.byteLength > data.size()-view.byteOffset) fail("buffer view exceeds its buffer");
    if (offset > view.byteLength || offset % alignment || view.byteOffset % alignment) fail("accessor offset is out of range or misaligned");
    stride = view.byteStride ? view.byteStride : packedStride;
    if (view.byteStride && (sparse || options.usage == AccessorUsage::Indices ||
        view.byteStride < 4 || view.byteStride > 252 || view.byteStride % 4 ||
        view.byteStride % alignment || view.byteStride < packedStride)) fail("invalid accessor byte stride");
    if (!sparse && options.usage == AccessorUsage::VertexAttribute && (offset % 4 || stride % 4))
      fail("vertex attribute elements must be aligned to four bytes");
    if (matrixElements && columns > 1 && (view.byteOffset+offset) % 4)
      fail("matrix columns must be aligned to four bytes");
    const size_t available = view.byteLength-offset;
    if (!count || extent > available || count-1 > (available-extent)/stride) fail("accessor range exceeds its buffer view");
    return data.data()+view.byteOffset+offset;
  };
  if (a.bufferView >= 0)
    dense_ = bytes(a.bufferView,a.byteOffset,a.count,elementExtent,encodedStride_,componentWidth_,false,true,denseStride_);
  else if (a.byteOffset) fail("bufferless accessor must have zero byte offset");
  if (a.sparse.isSparse) {
    if (a.sparse.count <= 0 || static_cast<size_t>(a.sparse.count) > a.count) fail("sparse count is out of range");
    sparseCount_ = static_cast<size_t>(a.sparse.count);
    switch (a.sparse.indices.componentType) {
      case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: sparseIndexWidth_ = 1; break;
      case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: sparseIndexWidth_ = 2; break;
      case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: sparseIndexWidth_ = 4; break;
      default: fail("invalid sparse index component type");
    }
    size_t stride = 0;
    sparseIndices_ = bytes(a.sparse.indices.bufferView,a.sparse.indices.byteOffset,sparseCount_,sparseIndexWidth_,sparseIndexWidth_,sparseIndexWidth_,true,false,stride);
    sparseValues_ = bytes(a.sparse.values.bufferView,a.sparse.values.byteOffset,sparseCount_,elementExtent,encodedStride_,componentWidth_,true,true,stride);
    uint64_t previous = 0;
    for (size_t i = 0; i < sparseCount_; ++i) {
      if (i % 1024 == 0) container::util::checkModelImportCancellation(stop);
      const auto value = unsignedValue(sparseIndices_+i*sparseIndexWidth_,sparseIndexWidth_);
      if (value >= a.count || (i && value <= previous)) fail("sparse indices must be strictly increasing and within accessor count");
      previous = value;
    }
  }
}
const unsigned char* CheckedAccessor::elementBytes(size_t element) const {
  if (element >= count()) fail("element index is out of range");
  size_t low = 0, high = sparseCount_;
  while (low < high) {
    const size_t middle = low+(high-low)/2;
    const auto index = unsignedValue(sparseIndices_+middle*sparseIndexWidth_,sparseIndexWidth_);
    if (index < element) low = middle+1; else high = middle;
  }
  if (low < sparseCount_ && unsignedValue(sparseIndices_+low*sparseIndexWidth_,sparseIndexWidth_) == element)
    return sparseValues_+low*encodedStride_;
  return dense_ ? dense_+element*denseStride_ : nullptr;
}
double CheckedAccessor::component(const unsigned char* bytes, size_t componentIndex) const {
  if (!bytes) return 0;
  const auto raw = unsignedValue(bytes+componentOffsets_[componentIndex],componentWidth_);
  switch (accessor_->componentType) {
    case TINYGLTF_COMPONENT_TYPE_FLOAT: return std::bit_cast<float>(static_cast<uint32_t>(raw));
    case TINYGLTF_COMPONENT_TYPE_DOUBLE: return std::bit_cast<double>(raw);
    case TINYGLTF_COMPONENT_TYPE_BYTE: {
      const auto value = std::bit_cast<int8_t>(static_cast<uint8_t>(raw));
      return accessor_->normalized ? std::max(-1.0,value/127.0) : value;
    }
    case TINYGLTF_COMPONENT_TYPE_SHORT: {
      const auto value = std::bit_cast<int16_t>(static_cast<uint16_t>(raw));
      return accessor_->normalized ? std::max(-1.0,value/32767.0) : value;
    }
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: return accessor_->normalized ? raw/255.0 : static_cast<double>(raw);
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: return accessor_->normalized ? raw/65535.0 : static_cast<double>(raw);
    default: return static_cast<double>(raw);
  }
}
void CheckedAccessor::readDoubles(size_t element, std::span<double> destination) const {
  if (destination.size() != components_) fail("destination component count mismatch");
  const auto* bytes = elementBytes(element);
  for (size_t c = 0; c < components_; ++c) {
    const double value = component(bytes,c);
    if (!std::isfinite(value) && !(options_.allowNonFiniteFloatFrames && accessor_->componentType == TINYGLTF_COMPONENT_TYPE_FLOAT))
      fail("non-finite value at element " + std::to_string(element) + " component " + std::to_string(c));
    destination[c] = value;
  }
}
void CheckedAccessor::readFloats(size_t element, std::span<float> destination) const {
  if (destination.size() != components_) fail("destination component count mismatch");
  std::array<double,16> values{};
  readDoubles(element,std::span(values).first(components_));
  for (size_t c = 0; c < components_; ++c) {
    const double value = values[c];
    if (std::isfinite(value) && std::abs(value) > std::numeric_limits<float>::max())
      fail("value outside runtime float range at element " + std::to_string(element) + " component " + std::to_string(c));
    const float converted = static_cast<float>(value);
    if (std::isfinite(value) && value != 0 && converted == 0)
      fail("value outside runtime float range at element " + std::to_string(element) + " component " + std::to_string(c));
    destination[c] = converted;
  }
}
void CheckedAccessor::readEncodedElement(size_t element, std::span<unsigned char> destination) const {
  if (destination.size() != encodedStride_) fail("encoded destination has an invalid byte count");
  const auto* bytes=elementBytes(element);
  std::fill(destination.begin(),destination.end(),0);
  if (bytes) for(size_t component=0;component<components_;++component)
    std::memcpy(destination.data()+componentOffsets_[component],bytes+componentOffsets_[component],componentWidth_);
}
void CheckedAccessor::readUnsigned(size_t element, std::span<uint32_t> destination) const {
  if (accessor_->normalized || (accessor_->componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE &&
      accessor_->componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT && accessor_->componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT))
    fail("exact unsigned reads require unsigned unnormalized components");
  if (destination.size() != components_) fail("destination component count mismatch");
  const auto* bytes = elementBytes(element);
  for (size_t c = 0; c < components_; ++c)
    destination[c] = bytes ? static_cast<uint32_t>(unsignedValue(bytes+componentOffsets_[c],componentWidth_)) : 0;
}
} // namespace container::geometry::gltf
