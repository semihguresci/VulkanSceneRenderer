#pragma once

#include "Container/utility/GuiManager.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace container::ui::bcf {

struct BcfTopic {
  std::string guid{};
  std::string title{};
  std::string status{};
  std::string priority{};
  std::vector<std::string> labels{};
  std::vector<std::string> tags{};
};

struct BcfComment {
  std::string guid{};
  std::string date{};
  std::string author{};
  std::string text{};
  std::string viewpointGuid{};
};

struct BcfPin {
  std::string guid{};
  std::string label{};
  bool hasLocation{false};
  glm::vec3 location{0.0f};
  std::string ifcGuid{};
  std::string sourceId{};
};

struct BcfMarkupLine {
  std::string guid{};
  glm::vec3 start{0.0f};
  glm::vec3 end{0.0f};
  glm::vec3 color{1.0f};
  std::string label{};
};

struct BcfViewpointMarkup {
  std::string guid{};
  std::string viewpointFile{};
  std::string snapshotFile{};
  std::vector<BcfPin> pins{};
  std::vector<BcfMarkupLine> lines{};
};

struct BcfVisualizationInfo {
  ViewpointSnapshotState snapshot{};
  BcfViewpointMarkup markup{};
};

struct BcfMarkup {
  BcfTopic topic{};
  std::vector<BcfComment> comments{};
  std::vector<BcfViewpointMarkup> viewpoints{};
};

struct BcfTopicViewpoint {
  BcfViewpointMarkup markup{};
  std::optional<ViewpointSnapshotState> snapshot{};
};

struct BcfTopicFolder {
  BcfMarkup markup{};
  std::vector<BcfTopicViewpoint> viewpoints{};
};

[[nodiscard]] std::string StablePinGuid(std::string_view ifcGuid,
                                        std::string_view sourceId,
                                        std::string_view topicGuid = {});

[[nodiscard]] std::string ExportVisualizationInfo(
    const ViewpointSnapshotState& snapshot);

[[nodiscard]] std::string ExportVisualizationInfo(
    const ViewpointSnapshotState& snapshot, const BcfViewpointMarkup& markup);

[[nodiscard]] std::optional<ViewpointSnapshotState> ImportVisualizationInfo(
    std::string_view xml);

[[nodiscard]] std::optional<BcfViewpointMarkup> ImportVisualizationInfoMarkup(
    std::string_view xml);

[[nodiscard]] std::optional<BcfViewpointMarkup> ImportVisualizationInfoMarkup(
    std::string_view xml, std::string_view topicGuid);

bool SaveVisualizationInfo(const ViewpointSnapshotState& snapshot,
                           const std::filesystem::path& path);
bool SaveVisualizationInfo(const ViewpointSnapshotState& snapshot,
                           const BcfViewpointMarkup& markup,
                           const std::filesystem::path& path);

[[nodiscard]] std::optional<ViewpointSnapshotState> LoadVisualizationInfo(
    const std::filesystem::path& path);
[[nodiscard]] std::optional<BcfVisualizationInfo>
LoadVisualizationInfoWithMarkup(const std::filesystem::path& path);

[[nodiscard]] std::string ExportMarkup(const BcfMarkup& markup);

[[nodiscard]] std::optional<BcfMarkup> ImportMarkup(std::string_view xml);

bool SaveMarkup(const BcfMarkup& markup, const std::filesystem::path& path);

[[nodiscard]] std::optional<BcfMarkup> LoadMarkup(
    const std::filesystem::path& path);

bool SaveTopicFolder(const BcfTopicFolder& topic,
                     const std::filesystem::path& folder);

[[nodiscard]] std::optional<BcfTopicFolder> LoadTopicFolder(
    const std::filesystem::path& folder);

bool SaveBcfArchive(const BcfTopicFolder& topic,
                    const std::filesystem::path& path);

[[nodiscard]] std::optional<BcfTopicFolder> LoadBcfArchive(
    const std::filesystem::path& path);

}  // namespace container::ui::bcf
