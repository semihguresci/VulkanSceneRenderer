#include "Container/geometry/OpenVdbTransportRenderJob.h"
#include "Container/utility/Platform.h"
#include <charconv>
#include <iomanip>
#include <iostream>
#include <locale>
#include <nlohmann/json.hpp>
#include <set>

namespace {
uint64_t unsignedArgument(const std::string &s) {
  uint64_t v{};
  const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  if (r.ec != std::errc{} || r.ptr != s.data() + s.size())
    throw std::invalid_argument("CLI value must be an unsigned integer");
  return v;
}
} // namespace
int runOpenVdbTransportRender(int argc, char **argv) {
  namespace vdb = container::geometry::vdb;
  try {
    vdb::TransportRenderJobOptions job;
#ifdef CONTAINER_OPENVDB_TRANSPORT_REFERENCE_SPIRV
    job.spirv = CONTAINER_OPENVDB_TRANSPORT_REFERENCE_SPIRV;
#else
    job.spirv =
        container::util::executableDirectory() / "spv_shaders/openvdb_transport_reference.comp.spv";
#endif
    std::string backend = "cpu";
    std::set<std::string> arguments;
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (!arguments.insert(arg).second)
        throw std::invalid_argument("duplicate CLI argument: " + arg);
      if (arg == "--help") {
        std::cout
            << "OpenVdbTransportRender --scene FILE.json --output IMAGE.exr [--backend cpu|gpu] "
               "[--shader FILE.spv] [--samples N] [--seed N] [--order N] [--work N] [--max-bytes "
               "N] [--max-spec-bytes N] [--max-density-values N] [--validation]\n";
        return 0;
      }
      if (arg == "--validation") {
        job.validation = true;
        continue;
      }
      if (i + 1 >= argc)
        throw std::invalid_argument("missing CLI argument for " + arg);
      const std::string value = argv[++i];
      if (arg == "--scene")
        job.sceneSpec = std::filesystem::u8path(value);
      else if (arg == "--output")
        job.output = std::filesystem::u8path(value);
      else if (arg == "--shader")
        job.spirv = std::filesystem::u8path(value);
      else if (arg == "--backend")
        backend = value;
      else if (arg == "--samples") {
        const auto n = unsignedArgument(value);
        if (n < 1 || n > 8192)
          throw std::invalid_argument("samples must be within1..8192");
        job.image.transport.samplesPerRay = uint32_t(n);
      } else if (arg == "--seed")
        job.image.transport.seed = unsignedArgument(value);
      else if (arg == "--order") {
        const auto n = unsignedArgument(value);
        if (n > 16)
          throw std::invalid_argument("order must be within0..16");
        job.image.transport.maxScatteringEvents = uint32_t(n);
      } else if (arg == "--work")
        job.image.transport.maxWork = unsignedArgument(value);
      else if (arg == "--max-bytes")
        job.image.maxImageBytes = job.maxSceneBytes = job.image.transport.maxInputBytes =
            unsignedArgument(value);
      else if (arg == "--max-spec-bytes")
        job.maxSpecBytes = unsignedArgument(value);
      else if (arg == "--max-density-values")
        job.maxDensityValues = unsignedArgument(value);
      else
        throw std::invalid_argument("unknown CLI argument: " + arg);
    }
    if (backend == "cpu")
      job.backend = vdb::TransportRenderBackend::Cpu;
    else if (backend == "gpu")
      job.backend = vdb::TransportRenderBackend::Gpu;
    else
      throw std::invalid_argument("backend must be cpu or gpu");
    if (!job.sceneSpec.empty())
      job.sceneSpec = std::filesystem::absolute(job.sceneSpec);
    if (!job.output.empty())
      job.output = std::filesystem::absolute(job.output);
    if (!job.spirv.empty())
      job.spirv = std::filesystem::absolute(job.spirv);
    // Validate/escape Unicode before any artifact publication. Reporting uses
    // this already-safe JSON token and finite numeric POD fields only.
    const auto u8 = job.output.u8string();
    const std::string outputJson = nlohmann::json(std::string(u8.begin(), u8.end())).dump();
    std::cout.imbue(std::locale::classic());
    std::cout << std::setprecision(17) << std::boolalpha;
    const auto result = vdb::RunTransportRenderJob(job);
    const auto &i = result.image;
    const auto &e = result.exported;
    std::cout << "{\"output\":" << outputJson << ",\"backend\":\"" << backend
              << "\",\"width\":" << i.width << ",\"height\":" << i.height
              << ",\"samplesPerPixel\":" << i.samplesPerPixel
              << ",\"paths\":" << i.diagnostics.paths
              << ",\"preparationWork\":" << i.diagnostics.preparationWork
              << ",\"work\":" << i.diagnostics.work
              << ",\"truncatedPaths\":" << i.diagnostics.truncatedPaths
              << ",\"standardErrorAvailable\":" << (i.samplesPerPixel >= 2)
              << ",\"roundedFloatValues\":" << e.roundedValues
              << ",\"maxAbsoluteConversionError\":" << e.maxAbsoluteError
              << ",\"maxRelativeConversionError\":" << e.maxRelativeError
              << ",\"sourceCount\":" << result.sources.size()
              << ",\"encodedBytes\":" << e.encodedBytes << "}\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "OpenVdbTransportRender: " << e.what() << '\n';
    return 1;
  }
}
#ifdef _WIN32
int wmain(int argc, wchar_t **wargv) {
  try {
    std::vector<std::string> arguments;
    arguments.reserve(size_t(argc));
    for (int i = 0; i < argc; ++i) {
      const auto utf8 = std::filesystem::path(wargv[i]).u8string();
      arguments.emplace_back(utf8.begin(), utf8.end());
    }
    std::vector<char *> argv;
    argv.reserve(arguments.size()+1);
    for (auto &s : arguments)
      argv.push_back(s.data());
    argv.push_back(nullptr);
    return runOpenVdbTransportRender(argc, argv.data());
  } catch (const std::exception &e) {
    std::cerr << "OpenVdbTransportRender: invalid Unicode arguments: " << e.what() << '\n';
    return 1;
  }
}
#else
int main(int argc, char **argv) { return runOpenVdbTransportRender(argc, argv); }
#endif
