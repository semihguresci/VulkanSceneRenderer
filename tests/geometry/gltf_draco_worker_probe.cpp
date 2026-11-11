// Test-only subprocess for deterministic launcher failure/timeout/memory cases.
// Never installed or used by the renderer's decoder discovery.
#include <Container/geometry/GltfDracoProtocol.h>

#include <cstring>
#include <fstream>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>

namespace {
bool redirectToJunction(const std::filesystem::path& directory, const std::filesystem::path& target) {
  struct MountPoint {
    DWORD tag;
    WORD length, reserved;
    WORD substituteOffset, substituteLength, printOffset, printLength;
    wchar_t path[1];
  };
  const std::wstring substitute = L"\\??\\" + target.native();
  const auto print = target.native();
  const size_t bytes = 16 + (substitute.size() + print.size() + 2) * sizeof(wchar_t);
  if (bytes > MAXIMUM_REPARSE_DATA_BUFFER_SIZE) return false;
  std::vector<std::byte> storage(bytes);
  auto* mount = reinterpret_cast<MountPoint*>(storage.data());
  mount->tag = IO_REPARSE_TAG_MOUNT_POINT;
  mount->length = static_cast<WORD>(bytes - 8);
  mount->substituteLength = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
  mount->printOffset = mount->substituteLength + sizeof(wchar_t);
  mount->printLength = static_cast<WORD>(print.size() * sizeof(wchar_t));
  std::memcpy(mount->path, substitute.c_str(), (substitute.size() + 1) * sizeof(wchar_t));
  std::memcpy(reinterpret_cast<std::byte*>(mount->path) + mount->printOffset,
              print.c_str(), (print.size() + 1) * sizeof(wchar_t));
  const HANDLE handle = CreateFileW(directory.c_str(), GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  DWORD returned{};
  const bool result = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, mount,
      static_cast<DWORD>(bytes), nullptr, 0, &returned, nullptr) != FALSE;
  CloseHandle(handle);
  return result;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
  using namespace container::geometry::gltf;
  if (argc != 3) return 2;
  BOOL inJob{};
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION job{};
  if (!IsProcessInJob(GetCurrentProcess(), nullptr, &inJob) || !inJob ||
      !QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &job, sizeof(job), nullptr)) return 3;
  try {
    const auto request = draco_worker::ReadRequest(argv[1]);
    const auto action = std::to_integer<unsigned char>(request.compressed.front());
    if (action == 1) { Sleep(10'000); return 0; }
    if (action == 7) {
      // Let the parent cancel only after this process starts decoding.
      std::ofstream(argv[2], std::ios::binary).put('r');
      Sleep(10'000);
      return 0;
    }
    if (action == 2) return 17;
    if (action == 3) {
      std::ofstream output(argv[2], std::ios::binary);
      for (uint64_t i = 0; i <= request.limits.maxOutputBytes; ++i) output.put('\0');
      return 0;
    }
    if (action == 6) {
      const auto directory = std::filesystem::path(argv[1]).parent_path();
      // A retained no-delete-sharing directory handle must deny this rename.
      const auto moved = std::filesystem::path(directory.native() + L"-moved");
      if (MoveFileW(directory.c_str(), moved.c_str())) return 17;
      if (DeleteFileW(argv[1]) && redirectToJunction(directory, directory.parent_path() / "victim"))
        return 0; // Parent sees the victim response, then cleans only the link.
      // Failed attack emits a valid response, making EXPECT_THROW fail below.
    }
    DracoDecodedMesh mesh;
    mesh.pointCount = 3; mesh.faceIndices = {0, 1, 2};
    DracoDecodedAttribute attribute;
    attribute.uniqueId = request.attributes[0].uniqueId;
    attribute.scalarType = DracoScalarType::UInt32; attribute.componentCount = 1;
    uint32_t values[3]{16'777'217u, 0xffffffffu, 7u};
    if (action == 5) {
      void* memory = VirtualAlloc(nullptr, 256ull * 1024 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
      values[0] = memory == nullptr ? 1u : 0u;
      if (memory) VirtualFree(memory, 0, MEM_RELEASE);
      values[1] = job.ProcessMemoryLimit == request.limits.processMemoryBytes ? 1u : 0u;
      const DWORD required = JOB_OBJECT_LIMIT_PROCESS_TIME | JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      values[2] = (job.BasicLimitInformation.LimitFlags & required) == required &&
          job.BasicLimitInformation.ActiveProcessLimit == 1 && GetConsoleWindow() == nullptr ? 1u : 0u;
    }
    attribute.values.resize(sizeof(values));
    std::memcpy(attribute.values.data(), values, sizeof(values));
    mesh.attributes.push_back(std::move(attribute));
    draco_worker::WriteResponse(argv[2], mesh, request.limits);
    if (action == 4) {
      std::fstream output(argv[2], std::ios::binary | std::ios::in | std::ios::out);
      output.put('\0');
    }
    return 0;
  } catch (...) { return 4; }
}
#else
int main() { return 3; }
#endif
