// The only production translation unit that includes/links the Draco decoder.
// Container's loader invokes this executable only through the bounded client.
#include <Container/geometry/GltfDracoProtocol.h>

#include <draco/compression/decode.h>

#include <bit>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {
using namespace container::geometry::gltf;

DracoScalarType scalarType(draco::DataType type) {
  switch (type) {
  case draco::DT_INT8: return DracoScalarType::Int8;
  case draco::DT_UINT8: return DracoScalarType::UInt8;
  case draco::DT_INT16: return DracoScalarType::Int16;
  case draco::DT_UINT16: return DracoScalarType::UInt16;
  case draco::DT_INT32: return DracoScalarType::Int32;
  case draco::DT_UINT32: return DracoScalarType::UInt32;
  case draco::DT_INT64: return DracoScalarType::Int64;
  case draco::DT_UINT64: return DracoScalarType::UInt64;
  case draco::DT_FLOAT32: return DracoScalarType::Float32;
  case draco::DT_FLOAT64: return DracoScalarType::Float64;
  case draco::DT_BOOL: return DracoScalarType::Bool;
  default: throw std::runtime_error("decoded attribute has an unsupported scalar type");
  }
}

uint64_t checkedAdd(uint64_t a, uint64_t b) {
  if (b > std::numeric_limits<uint64_t>::max() - a) throw std::runtime_error("decoded byte count overflows");
  return a + b;
}
uint64_t checkedMultiply(uint64_t a, uint64_t b) {
  if (a && b > std::numeric_limits<uint64_t>::max() / a) throw std::runtime_error("decoded byte count overflows");
  return a * b;
}

DracoDecodedMesh decode(const draco_worker::Request& request) {
  if constexpr (std::endian::native != std::endian::little)
    throw std::runtime_error("bounded Draco worker requires a little-endian host");
  draco::DecoderBuffer buffer;
  buffer.Init(reinterpret_cast<const char*>(request.compressed.data()), request.compressed.size());
  draco::Decoder decoder;
  auto decoded = decoder.DecodeMeshFromBuffer(&buffer);
  if (!decoded.ok()) throw std::runtime_error(decoded.status().error_msg_string());
  const auto mesh = std::move(decoded).value();
  if (!mesh || !mesh->num_points() || mesh->num_points() > request.limits.maxPoints ||
      !mesh->num_faces() || mesh->num_faces() > request.limits.maxFaces)
    throw std::runtime_error("decoded point/face count exceeds request limits");

  DracoDecodedMesh output;
  output.pointCount = mesh->num_points();
  uint64_t bytes = checkedAdd(draco_worker::kResponseHeaderBytes,
                              checkedMultiply(mesh->num_faces(), 12));
  output.attributes.reserve(request.attributes.size());
  // Inspect/count the complete response before allocating dense output arrays.
  for (const auto& requested : request.attributes) {
    const auto* source = mesh->GetAttributeByUniqueId(requested.uniqueId);
    if (!source || source->num_components() != requested.componentCount)
      throw std::runtime_error("missing unique attribute ID or mismatched component count");
    DracoDecodedAttribute attribute;
    attribute.uniqueId = requested.uniqueId;
    attribute.scalarType = scalarType(source->data_type());
    attribute.componentCount = requested.componentCount;
    attribute.normalized = source->normalized();
    const uint64_t element = checkedMultiply(attribute.componentCount, DracoScalarByteWidth(attribute.scalarType));
    // GetMappedValue copies byte_stride(), not merely num_components(). Reject
    // unexpected padding before it can overrun a tightly packed output element.
    if (source->byte_stride() != static_cast<int64_t>(element) || source->byte_offset() < 0 ||
        (!source->is_mapping_identity() && source->indices_map_size() < output.pointCount))
      throw std::runtime_error("decoded attribute has an invalid raw value layout");
    bytes = checkedAdd(bytes, checkedAdd(draco_worker::kAttributeHeaderBytes,
                                        checkedMultiply(output.pointCount, element)));
    output.attributes.push_back(std::move(attribute));
  }
  if (bytes > request.limits.maxOutputBytes || bytes > std::numeric_limits<size_t>::max())
    throw std::runtime_error("decoded output exceeds byte budget");
  output.faceIndices.resize(static_cast<size_t>(mesh->num_faces()) * 3);
  for (uint32_t face = 0; face < mesh->num_faces(); ++face)
    for (uint32_t corner = 0; corner < 3; ++corner) {
      const auto index = mesh->face(draco::FaceIndex(face))[corner].value();
      if (index >= output.pointCount) throw std::runtime_error("decoded face index is out of range");
      output.faceIndices[static_cast<size_t>(face) * 3 + corner] = index;
    }
  for (auto& attribute : output.attributes) {
    const auto* source = mesh->GetAttributeByUniqueId(attribute.uniqueId);
    const uint64_t element = uint64_t(attribute.componentCount) * DracoScalarByteWidth(attribute.scalarType);
    attribute.values.resize(static_cast<size_t>(output.pointCount * element));
    if (!source->buffer()) throw std::runtime_error("decoded attribute has no raw buffer");
    const uint64_t available = source->buffer()->data_size();
    const uint64_t offset = static_cast<uint64_t>(source->byte_offset());
    for (uint32_t point = 0; point < output.pointCount; ++point) {
      const auto mapped = source->mapped_index(draco::PointIndex(point));
      const uint64_t position = checkedAdd(offset, checkedMultiply(mapped.value(), element));
      if (mapped.value() >= source->size() || position > available || element > available - position)
        throw std::runtime_error("decoded point-to-attribute mapping is out of bounds");
      // Preserve normalized raw integers exactly; ConvertValue would normalize
      // or round them. The parent applies the declared glTF accessor semantics.
      source->GetMappedValue(draco::PointIndex(point),
          attribute.values.data() + static_cast<size_t>(point * element));
    }
  }
  return output;
}

#ifdef _WIN32
JOBOBJECT_EXTENDED_LIMIT_INFORMATION requireBoundedJob() {
  BOOL inJob = FALSE;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  if (!IsProcessInJob(GetCurrentProcess(), nullptr, &inJob) || !inJob ||
      !QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &limits, sizeof(limits), nullptr))
    throw std::runtime_error("worker refuses to decode outside a bounded Windows Job Object");
  constexpr DWORD required = JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_PROCESS_TIME |
      JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if ((limits.BasicLimitInformation.LimitFlags & required) != required ||
      !limits.ProcessMemoryLimit || limits.BasicLimitInformation.ActiveProcessLimit != 1 ||
      limits.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart <= 0)
    throw std::runtime_error("worker requires enforced process memory/CPU/count limits");
  return limits;
}

int workerMain(const std::filesystem::path& requestPath, const std::filesystem::path& responsePath) {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  if (!requestPath.is_absolute() || !responsePath.is_absolute() ||
      requestPath.parent_path() != responsePath.parent_path() ||
      requestPath.filename() != L"request.bin" || responsePath.filename() != L"response.bin") return 2;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION enforced{};
  try { enforced = requireBoundedJob(); }
  catch (...) { return 3; } // No untrusted input is read before this check.
  uint64_t errorBudget = draco_worker::kResponseHeaderBytes;
  try {
    const auto request = draco_worker::ReadRequest(requestPath);
    errorBudget = request.limits.maxOutputBytes;
    if (enforced.ProcessMemoryLimit > request.limits.processMemoryBytes ||
        enforced.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart >
            static_cast<LONGLONG>(request.limits.timeoutMs) * 10'000)
      throw std::runtime_error("applied worker limits exceed the request budget");
    const auto output = decode(request);
    draco_worker::WriteResponse(responsePath, output, request.limits);
    return 0;
  } catch (const std::exception& error) {
    try { draco_worker::WriteFailure(responsePath, error.what(), errorBudget); return 0; }
    catch (...) { return 4; }
  } catch (...) { return 5; }
}
#endif
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
  return argc == 3 ? workerMain(argv[1], argv[2]) : 2;
}
#else
// No unbounded standalone decode on platforms without an enforced launcher.
int main() { return 3; }
#endif
