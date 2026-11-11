#pragma once

#include <exception>
#include <stop_token>

namespace container::util {

class ModelImportCancelled final : public std::exception {
public:
  [[nodiscard]] const char *what() const noexcept override {
    return "Model import cancelled";
  }
};

inline void checkModelImportCancellation(const std::stop_token &stop) {
  if (stop.stop_requested())
    throw ModelImportCancelled{};
}

} // namespace container::util
