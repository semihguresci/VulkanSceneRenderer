#include <filesystem>
#include <fstream>
#include <iostream>
#if defined(CONTAINER_HAS_NANOVDB_IO) && CONTAINER_HAS_NANOVDB_IO
#include "Container/geometry/detail/OpenVdbNanoCompatibility.h"
#include <nanovdb/tools/CreatePrimitives.h>
#endif

int generate(const std::filesystem::path &output) {
  try {
#if defined(CONTAINER_HAS_NANOVDB_IO) && CONTAINER_HAS_NANOVDB_IO
    const auto handle=nanovdb::tools::createLevelSetSphere<float>(
        3.0,nanovdb::Vec3d(0),1.0,2.0,nanovdb::Vec3d(0),"fixture",
        nanovdb::tools::StatsMode::All,nanovdb::CheckMode::Full);
    std::ofstream stream(output,std::ios::binary|std::ios::out|std::ios::noreplace);
    stream.exceptions(std::ios::badbit|std::ios::failbit);
    stream.write(static_cast<const char *>(handle.data()),std::streamsize(handle.size()));
    stream.close();
    return 0;
#else
    (void)output;
    std::cerr << "Native NanoVDB fixture support is disabled\n";
    return 1;
#endif
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n'; return 1;
  }
}
#ifdef _WIN32
int wmain(int argc,wchar_t **argv) { return argc==2 ? generate(argv[1]) : 1; }
#else
int main(int argc,char **argv) { return argc==2 ? generate(argv[1]) : 1; }
#endif
