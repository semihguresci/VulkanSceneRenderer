#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace container::geometry {

enum class ImportCompleteness { Unreported, Complete, Partial, Failed };

inline std::string_view importCompletenessName(ImportCompleteness value) {
  switch (value) {
  case ImportCompleteness::Complete:
    return "Complete";
  case ImportCompleteness::Partial:
    return "Partial";
  case ImportCompleteness::Failed:
    return "Failed";
  default:
    return "Unreported";
  }
}

struct ImportDiagnostic {
  std::string representationType{};
  uint32_t entityId{0};
  uint32_t productId{0};
  std::string productGuid{};
  std::string reason{};
};

struct ImportReport {
  ImportCompleteness completeness{ImportCompleteness::Unreported};
  size_t sourceProductCount{0};
  size_t importedProductCount{0};
  size_t skippedProductCount{0};
  size_t partialProductCount{0};
  std::vector<ImportDiagnostic> diagnostics{};
  std::map<std::string, size_t> representationWarnings{};
  // A prepared IFCX model may be displayed after the STEP import fails.
  std::string fallbackSource{};
  // Other importers share this report without inheriting IFC terminology.
  std::string sourceFormat{"IFC"};
  std::string sourceItemName{"products"};

  [[nodiscard]] std::string summary() const {
    if (completeness == ImportCompleteness::Unreported)
      return {};
    std::string result =
        std::string(importCompletenessName(completeness)) +
        " " + sourceFormat + " import: " + std::to_string(importedProductCount) + "/" +
        std::to_string(sourceProductCount) + " " + sourceItemName + " imported, " +
        std::to_string(skippedProductCount) + " skipped, " +
        std::to_string(partialProductCount) + " incomplete; " +
        std::to_string(diagnostics.size()) + " representation warnings";
    if (!fallbackSource.empty())
      result += "; displaying IFCX fallback: " + fallbackSource;
    return result;
  }
};

} // namespace container::geometry
