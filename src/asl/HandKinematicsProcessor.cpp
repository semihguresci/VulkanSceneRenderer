#include "Container/asl/PoseReplay.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {
namespace fs = std::filesystem;
using namespace container::asl;
std::string utf8(const fs::path& path) {
    auto value = path.u8string();
    return std::string(value.begin(), value.end());
}
double number(const fs::path& argument, double low, double high) {
    const auto value = argument.wstring();
    std::size_t consumed{};
    const double result = std::stod(value, &consumed);
    if (consumed != value.size() || !std::isfinite(result) || result < low || result > high)
        throw std::invalid_argument("Numeric option is outside its supported range.");
    return result;
}
struct TemporaryFile {
    fs::path path;
    ~TemporaryFile() { if (!path.empty()) { std::error_code error; fs::remove(path, error); } }
};
int process(const std::vector<fs::path>& args) {
    fs::path input, output;
    double aspect = 1.0;
    auto options = replayHandFitOptions();
    bool aspectSet = false, velocitySet = false, iterationsSet = false, gapSet = false;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const auto key = args[i].wstring();
        if (key == L"--help" && args.size() == 2) {
            std::cout << "ASLHandKinematicsProcessor --input replay.json --output fit.json\n"
                         "  [--image-aspect-ratio 1] [--max-angular-velocity 12]\n"
                         "  [--max-iterations 64] [--max-temporal-gap-ms 250]\n"
                         "Output is a derived fit payload, never a replacement for raw landmarks.\n";
            return 0;
        }
        if (++i >= args.size()) throw std::invalid_argument("A command-line option is missing its value.");
        if (key == L"--input" && input.empty()) input = args[i];
        else if (key == L"--output" && output.empty()) output = args[i];
        else if (key == L"--image-aspect-ratio" && !aspectSet) {
            aspect = number(args[i], 0.125, 8.0); aspectSet = true;
        } else if (key == L"--max-angular-velocity" && !velocitySet) {
            options.maxAngularVelocityRadPerSecond = number(args[i], 0.0, 100.0); velocitySet = true;
        } else if (key == L"--max-iterations" && !iterationsSet) {
            const auto count = number(args[i], 1, 128);
            if (std::floor(count) != count) throw std::invalid_argument("Iteration count must be an integer.");
            options.maxIterations = static_cast<std::size_t>(count); iterationsSet = true;
        } else if (key == L"--max-temporal-gap-ms" && !gapSet) {
            const auto count = number(args[i], 1, 10000);
            if (std::floor(count) != count) throw std::invalid_argument("Temporal gap must be an integer.");
            options.maxTemporalGapMs = static_cast<std::int64_t>(count); gapSet = true;
        } else throw std::invalid_argument("Unknown or repeated command-line option.");
    }
    if (input.empty() || output.empty()) throw std::invalid_argument("Both --input and --output are required.");
    input = fs::absolute(input).lexically_normal();
    output = fs::absolute(output).lexically_normal();
    if (output.extension() != ".json") throw std::invalid_argument("Output must have a .json extension.");
    if (fs::exists(output)) throw std::invalid_argument("Output already exists; choose a fresh path.");
    const auto replay = PoseReplay::load(input);
    const auto begin = std::chrono::steady_clock::now();
    const HandPoseReplay fitted(replay, aspect, options);
    const auto document = fitted.fitDocument();
    const auto encoded = document.dump(-1, ' ', false, nlohmann::json::error_handler_t::strict) + "\n";
    if (encoded.size() > kMaxReplayBytes) throw std::invalid_argument("Fit payload exceeds 64 MiB.");
    fs::create_directories(output.parent_path());
    TemporaryFile temporary{output.parent_path() / (output.filename().wstring() + L"." +
        std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()) + L".tmp")};
    if (fs::exists(temporary.path)) throw std::runtime_error("Temporary output collision.");
    {
        std::ofstream stream(temporary.path, std::ios::binary | std::ios::out);
        if (!stream) throw std::runtime_error("Cannot create temporary fit payload.");
        stream.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
        stream.flush();
        if (!stream) throw std::runtime_error("Cannot finish fit payload.");
    }
#ifdef _WIN32
    // No REPLACE_EXISTING: publication must fail if another writer won the race.
    if (!MoveFileExW(temporary.path.c_str(), output.c_str(), MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot publish fit payload without overwriting an existing file.");
#else
    fs::create_hard_link(temporary.path, output);
    fs::remove(temporary.path);
#endif
    temporary.path.clear();
    std::cout << nlohmann::json{{"status", "completed"}, {"output", utf8(output)},
        {"frames", replay.frames().size()}, {"bytes", encoded.size()},
        {"seconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count()},
        {"metric_depth", false}}.dump() << '\n';
    return 0;
}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    try {
        std::vector<std::filesystem::path> args;
        for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]);
        return process(args);
    } catch (const std::exception& error) {
        std::cerr << nlohmann::json{{"status", "failed"}, {"error", error.what()}}.dump() << '\n';
        return 1;
    }
}
