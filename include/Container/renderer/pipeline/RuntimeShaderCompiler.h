#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <stop_token>

namespace container::renderer {

class ShaderReloadCandidate {
public:
  explicit ShaderReloadCandidate(std::filesystem::path directory);
  ~ShaderReloadCandidate();
  ShaderReloadCandidate(const ShaderReloadCandidate&) = delete;
  ShaderReloadCandidate& operator=(const ShaderReloadCandidate&) = delete;

  const std::filesystem::path directory;
  std::size_t compiledStages{};
};

[[nodiscard]] std::shared_ptr<ShaderReloadCandidate>
compileShaderCandidate(const std::filesystem::path& executableDirectory,
                       std::stop_token stop = {},
                       std::chrono::milliseconds timeout = std::chrono::minutes(2));

} // namespace container::renderer
