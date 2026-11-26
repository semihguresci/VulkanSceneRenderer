#include "Container/renderer/core/RendererFrontend.h"

#include "Container/app/AppConfig.h"
#include "Container/renderer/bim/BimManager.h"
#include "Container/renderer/lighting/DdgiManager.h"
#include "Container/renderer/pathtracing/ReferencePathTracePass.h"
#include "Container/renderer/platform/VulkanContextInitializer.h"
#include "Container/renderer/scene/SceneController.h"
#include "Container/renderer/scene/TextureMipDemand.h"
#include "Container/renderer/temporal/TemporalManager.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/FrameSyncManager.h"
#include "Container/utility/Logger.h"
#include "Container/utility/SceneManager.h"
#include "Container/utility/TextureMipUpload.h"
#include "Container/utility/VulkanDevice.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <ranges>
#include <tuple>
#include <unordered_map>

namespace container::renderer {
namespace {

using gpu::GpuMaterial;
using gpu::TextureMipUploadState;

struct TextureUse {
  uint32_t GpuMaterial::*slot;
  gpu::GpuTextureTransform GpuMaterial::*transform;
};

#define MATERIAL_TEXTURE_USE(name) TextureUse{&GpuMaterial::name##TextureIndex, &GpuMaterial::name##TextureTransform}
constexpr std::array materialTextureUses{
    MATERIAL_TEXTURE_USE(baseColor), MATERIAL_TEXTURE_USE(normal),
    MATERIAL_TEXTURE_USE(occlusion), MATERIAL_TEXTURE_USE(emissive),
    MATERIAL_TEXTURE_USE(metallicRoughness), MATERIAL_TEXTURE_USE(roughness),
    MATERIAL_TEXTURE_USE(metalness), MATERIAL_TEXTURE_USE(specular),
    MATERIAL_TEXTURE_USE(height), MATERIAL_TEXTURE_USE(opacity),
    MATERIAL_TEXTURE_USE(transmission), MATERIAL_TEXTURE_USE(specularColor),
    MATERIAL_TEXTURE_USE(clearcoat), MATERIAL_TEXTURE_USE(clearcoatRoughness),
    MATERIAL_TEXTURE_USE(clearcoatNormal), MATERIAL_TEXTURE_USE(thickness),
    MATERIAL_TEXTURE_USE(sheenColor), MATERIAL_TEXTURE_USE(sheenRoughness),
    MATERIAL_TEXTURE_USE(iridescence), MATERIAL_TEXTURE_USE(iridescenceThickness),
    MATERIAL_TEXTURE_USE(anisotropy), MATERIAL_TEXTURE_USE(diffuseTransmission),
    MATERIAL_TEXTURE_USE(diffuseTransmissionColor),
};
#undef MATERIAL_TEXTURE_USE

constexpr std::array experimentalTransforms{
    &GpuMaterial::retroreflectionTextureTransform,
    &GpuMaterial::scatterStrengthTextureTransform,
    &GpuMaterial::scatterColorTextureTransform,
};

template <typename Visitor>
void visitMaterialTextures(const GpuMaterial& material, Visitor&& visit) {
  for (const auto& use : materialTextureUses)
    visit(material.*use.slot, material.*use.transform);
  for (int ext = 0; ext < static_cast<int>(experimentalTransforms.size()); ++ext)
    visit(material.experimentalTextures[ext], material.*experimentalTransforms[ext]);
}

}  // namespace

struct RendererFrontend::TextureStreaming {
  struct Entry {
    gpu::TextureAllocationIdentity fallback;
    gpu::TextureMipSource source;
    std::unique_ptr<gpu::UploadedTextureMipTail> tail;
    std::vector<VkDeviceSize> estimates;
    uint64_t imageId{};
    uint32_t demand{};
    uint32_t planned{};
    uint32_t nextDemand{};
    float footprint{};
    bool live{};
    bool uploadFailed{};
    std::optional<std::tuple<uint32_t, VkDeviceSize, VkDeviceSize>> deferredRequest;

    [[nodiscard]] uint32_t first() const noexcept {
      return tail ? tail->firstMip() : source.fallbackFirstMip;
    }
  };

  explicit TextureStreaming(RendererFrontend& owner)
      : frontend(owner),
        upload(owner.svc_.allocationManager, owner.svc_.ctx.deviceWrapper
            ->queueFamilyIndices().graphicsFamily.value()),
    // Async DDGI waits on the graphics submission made after upload completion.
        queueFamilies(owner.svc_.ctx.deviceWrapper->asyncComputeSharingFamilies()) {
    const auto profile = owner.svc_.allocationManager.textureDecodeOptions(true);
    deviceBc3Srgb = std::ranges::find(profile.allowedFormats, VK_FORMAT_BC3_SRGB_BLOCK) !=
                    profile.allowedFormats.end();
  }

  void clearIdleUpload() {
    if (upload.state() == TextureMipUploadState::Idle) {
      uploading = 0;
      uploadResidentBytes = 0;
    }
  }

  void retireUpload() {
    if (upload.state() == TextureMipUploadState::Retiring)
      (void)upload.tick(0);
    clearIdleUpload();
  }

  void cancelUpload() {
    if (upload.state() == TextureMipUploadState::Uploading || upload.ready()) {
      upload.cancel();
      ++cancellations;
    }
    retireUpload();
  }

  void publish(std::span<Entry* const> removed, Entry* changed = nullptr,
               std::unique_ptr<gpu::UploadedTextureMipTail> replacement = {}) {
    auto& scene = *frontend.subs_.sceneManager;
    std::vector<material::TextureMipViewOverride> overrides;
    overrides.reserve(entries.size());
    for (auto& [key, entry] : entries) {
      if (!entry.live || std::ranges::find(removed, &entry) != removed.end())
        continue;
      const auto* image = &entry == changed ? replacement.get() : entry.tail.get();
      if (image)
        overrides.push_back({entry.fallback, image->view()});
    }

    // All current descriptor owners share material images. The graphics
    // fences and DDGI timeline protect both descriptor writes and old views.
    frontend.subs_.frameSyncManager->waitForAllFrames();
    if (frontend.subs_.ddgiManager)
      frontend.subs_.ddgiManager->synchronizeAsync();
    scene.writeMaterialTextureImageDescriptors(overrides);

    for (auto* entry : removed) {
      if (entry->tail)
        ++evictions;
      entry->tail.reset();
      entry->imageId = entry->fallback.incarnation();
    }
    if (changed) {
      if (replacement->firstMip() < changed->first())
        ++promotions;
      else
        ++evictions;
      changed->tail = std::move(replacement);
      // The public ID distinguishes effective allocations without exposing
      // VkImage handles or aliasing the allocator's fallback identities.
      changed->imageId = (uint64_t{1} << 63) | ++publicationSerial;
    }
    if (!removed.empty() || changed) {
      frontend.subs_.temporalManager->reset("texture mip residency changed");
      if (frontend.subs_.referencePathTracePass)
        frontend.subs_.referencePathTracePass->invalidateHistory();
      if (frontend.subs_.ddgiManager)
        frontend.subs_.ddgiManager->resetSceneHistory();
      ++frontend.referenceTimingGeneration_;
      frontend.referenceAdaptiveBatch_.reset();
      frontend.referenceTimedLightingMs_.reset();
    }
  }

  void refreshCatalog() {
    auto& scene = *frontend.subs_.sceneManager;
    const auto revisions = std::tuple{scene.generationEpoch(), scene.materialResourceRevision(),
                                      scene.materialTextureDescriptorRevision()};
    if (catalogRevisions == revisions)
      return;
    for (auto& [key, entry] : entries)
      entry.live = false;
    auto rows = scene.materialTextureSamplingRows();
    slotEntries.assign(rows.size(), nullptr);
    for (size_t slot = 0; slot < rows.size(); ++slot) {
      const auto& row = rows[slot];
      if (!row.active || !row.allocationIdentity)
        continue;
      const auto id = row.allocationIdentity.incarnation();
      auto found = entries.find(id);
      if (found == entries.end()) {
        auto source = frontend.svc_.allocationManager.textureMipSource(row.allocationIdentity);
        if (!source)
          continue;
        const auto firstMip = source->fallbackFirstMip;
        found = entries.try_emplace(id, Entry{
            .fallback = row.allocationIdentity, .source = std::move(*source),
            .estimates = std::vector<VkDeviceSize>(firstMip), .imageId = id,
            .demand = firstMip, .planned = firstMip, .nextDemand = firstMip,
        }).first;
      }
      found->second.live = true;
      slotEntries[slot] = &found->second;
    }

    std::vector<Entry*> removed;
    bool retained = false;
    for (auto& [key, entry] : entries) {
      if (entry.tail && !entry.live)
        removed.push_back(&entry);
      retained |= entry.live && entry.tail != nullptr;
    }
    if (uploading && (!entries.contains(uploading) || !entries.at(uploading).live))
      cancelUpload();
    const bool swapped = catalogRevisions && std::get<0>(*catalogRevisions) != std::get<0>(revisions);
    if (retained || !removed.empty() || swapped)
      publish(removed);
    std::erase_if(entries, [](const auto& row) { return !row.second.live; });
    catalogRevisions = revisions;
  }

  void collectDemand(const TextureMipDemandView& view) {
    const auto materials = frontend.subs_.sceneManager->gpuMaterialData();
    materialPixels.assign(materials.size(), 0.0f);
    for (auto& [key, entry] : entries) {
      entry.footprint = 0.0f;
      entry.nextDemand = entry.source.fallbackFirstMip;
    }

    const auto collect = [&](const gpu::ObjectData& object) {
      const auto index = object.objectInfo.x;
      if (index < materialPixels.size())
        materialPixels[index] = std::max(materialPixels[index], view.projectedPixels(object.boundingSphere));
    };
    for (const auto& object : frontend.subs_.sceneController->objectData())
      collect(object);
    if (const auto* bim = frontend.subs_.bimManager.get(); bim && bim->hasScene()) {
      const auto filter = frontend.currentBimDrawFilter();
      const auto& objects = bim->objectData();
      for (uint32_t i = 0; i < objects.size(); ++i) {
        if (bim->objectMatchesFilter(i, filter))
          collect(objects[i]);
      }
    }

    const auto accumulate = [&](uint32_t slot, const gpu::GpuTextureTransform& transform, float pixels) {
      if (slot >= slotEntries.size() || !slotEntries[slot])
        return;
      auto& entry = *slotEntries[slot];
      const auto& base = entry.source.image->mips.front();
      const glm::vec2 repeat{glm::length(glm::vec2(transform.row0)),
                             glm::length(glm::vec2(transform.row1))};
      const auto mip = textureMipForFootprint(pixels, {base.width, base.height}, repeat,
                                             entry.source.fallbackFirstMip, entry.demand);
      entry.nextDemand = std::min(entry.nextDemand, mip);
      entry.footprint = std::max(entry.footprint, pixels);
    };
    for (size_t i = 0; i < materials.size(); ++i) {
      const auto pixels = materialPixels[i];
      if (!(pixels > 0.0f))
        continue;
      visitMaterialTextures(materials[i], [&](uint32_t slot, const gpu::GpuTextureTransform& transform) {
        accumulate(slot, transform, pixels);
      });
    }
    for (auto& [key, entry] : entries)
      entry.demand = entry.nextDemand;
  }

  void planResidency() {
    priority.clear();
    VkDeviceSize currentTails = uploadResidentBytes;
    for (auto& [key, entry] : entries) {
      if (entry.tail)
        currentTails += entry.tail->residentBytes();
      if (entry.live)
        priority.push_back(&entry);
    }
    std::ranges::sort(priority, [](const Entry* a, const Entry* b) {
      if (a->footprint != b->footprint)
        return a->footprint > b->footprint;
      return a->fallback.incarnation() < b->fallback.incarnation();
    });
    const auto budget = frontend.svc_.allocationManager.sceneAssetBudget();
    const auto otherResident = budget.used.residentBytes - currentTails;
    auto remaining = budget.limits.residentBytes ? budget.limits.residentBytes - otherResident
                                                 : VkDeviceSize{VK_WHOLE_SIZE};
    for (auto* entry : priority) {
      const auto fallback = entry->source.fallbackFirstMip;
      entry->planned = fallback;
      if (entry->uploadFailed)
        continue;
      for (uint32_t mip = entry->demand; mip < fallback; ++mip) {
        auto& bytes = entry->estimates[mip];
        if (bytes == 0)
          bytes = upload.estimateResidentBytes(*entry->source.image, mip, queueFamilies);
        if (bytes <= remaining) {
          entry->planned = mip;
          remaining -= bytes;
          break;
        }
      }
    }
  }

  void advance() {
    std::vector<Entry*> removed;
    for (auto* entry : priority) {
      if (entry->tail && entry->planned == entry->source.fallbackFirstMip)
        removed.push_back(entry);
    }
    if (!removed.empty())
      publish(removed);

    if (uploading && (!entries.contains(uploading) || entries.at(uploading).planned > uploadMip))
      cancelUpload();
    if (upload.state() == TextureMipUploadState::Retiring)
      return;
    if (upload.state() == TextureMipUploadState::Idle) {
      const auto before = frontend.svc_.allocationManager.sceneAssetBudget();
      const auto requestedChunk = VkDeviceSize(frontend.svc_.config.textureUploadChunkKiB) * 1024u;
      for (auto* entry : priority) {
        if (entry->planned == entry->first())
          continue;
        const auto key = entry->fallback.incarnation();
        const auto request = std::tuple{entry->planned, before.used.residentBytes, before.used.uploadBytes};
        if (entry->deferredRequest == request)
          continue;
        try {
          upload.begin(entry->source.image, entry->planned, requestedChunk, queueFamilies);
        } catch (const gpu::SceneAssetBudgetExceeded&) {
          ++budgetDeferrals;
          entry->deferredRequest = request;
          const auto headroom = before.limits.residentBytes - before.used.residentBytes;
          if (before.limits.residentBytes && entry->estimates[entry->planned] > headroom) {
            // A replacement needs both allocations until the old view is retired.
            // Reclaim only tails that exceed the budget plan, or this slot's old tail.
            removed.clear();
            for (auto* current : priority) {
              if (current->tail && (current == entry || current->first() < current->planned))
                removed.push_back(current);
            }
            if (!removed.empty()) {
              publish(removed);
              return;
            }
          }
          continue;
        } catch (const std::runtime_error& error) {
          entry->uploadFailed = true;
          ++uploadFailures;
          container::log::ContainerLogger::instance().renderer()->warn(
              "Texture mip upload could not begin, keeping a valid resident image: {}", error.what());
          continue;
        }
        uploading = key;
        uploadMip = entry->planned;
        uploadResidentBytes =
            frontend.svc_.allocationManager.sceneAssetBudget().used.residentBytes - before.used.residentBytes;
        entry->deferredRequest.reset();
        break;
      }
    }
    VkDeviceSize submitted = 0;
    try {
      submitted = upload.tick();
    } catch (const std::runtime_error& error) {
      if (const auto target = entries.find(uploading); target != entries.end())
        target->second.uploadFailed = true;
      ++uploadFailures;
      upload.cancel();
      clearIdleUpload();
      container::log::ContainerLogger::instance().renderer()->warn(
          "Texture mip upload could not finish, keeping a valid resident image: {}", error.what());
      return;
    }
    const auto progress = upload.progress();
    maxBytesPerTick = std::max(maxBytesPerTick, submitted);
    peakStagingBytes = std::max(peakStagingBytes, progress.stagingBytes);
    submittedChunks += submitted != 0;
    if (upload.ready()) {
      auto& entry = entries.at(uploading);
      auto replacement = upload.takeReady();
      entry.estimates[replacement->firstMip()] = replacement->residentBytes();
      uploading = 0;
      uploadResidentBytes = 0;
      publish({}, &entry, std::move(replacement));
    }
  }

  bool retireForModelLoad() {
    cancelUpload();
    return upload.state() == TextureMipUploadState::Idle;
  }

  VkDeviceSize prepareForModelAdmission() {
    // The pending image has no sampled descriptor. Retire its reservations
    // before checking the incoming model, without changing any live views.
    if (!retireForModelLoad()) {
      upload.drain();
      clearIdleUpload();
    }
    if (!frontend.svc_.allocationManager.sceneAssetBudget().limits.residentBytes)
      return 0;
    VkDeviceSize residentBytes = 0;
    for (const auto& [key, entry] : entries) {
      if (entry.tail)
        residentBytes += entry.tail->residentBytes();
    }
    return residentBytes;
  }

  bool prepareForModelCommit() {
    if (!prepareForModelAdmission())
      return false;
    // Clear sampled overrides and retire their readers before releasing the
    // tail allocations credited by admission.
    std::vector<Entry*> removed;
    for (auto& [key, entry] : entries) {
      if (entry.tail)
        removed.push_back(&entry);
    }
    if (!removed.empty())
      publish(removed);
    return !removed.empty();
  }

  RendererFrontend& frontend;
  gpu::TextureMipUpload upload;
  std::span<const uint32_t> queueFamilies;
  std::optional<std::tuple<uint64_t, uint64_t, uint64_t>> catalogRevisions;
  std::unordered_map<uint64_t, Entry> entries;
  std::vector<Entry*> slotEntries;
  std::vector<Entry*> priority;
  std::vector<float> materialPixels;
  uint64_t uploading{};
  uint32_t uploadMip{};
  VkDeviceSize uploadResidentBytes{};
  uint64_t publicationSerial{};
  bool deviceBc3Srgb{};
  uint64_t promotions{}, evictions{}, cancellations{}, submittedChunks{}, budgetDeferrals{}, uploadFailures{};
  VkDeviceSize maxBytesPerTick{}, peakStagingBytes{};
};

void RendererFrontend::TextureStreamingDeleter::operator()(TextureStreaming* state) const noexcept {
  delete state;
}

void RendererFrontend::updateTextureStreaming(const glm::mat4& view, const glm::mat4& projection,
                                              VkExtent2D extent) {
  if (!extent.width || !extent.height)
    return;
  if (!textureStreaming_)
    textureStreaming_.reset(new TextureStreaming(*this));
  auto& stream = *textureStreaming_;
  stream.retireUpload();
  stream.refreshCatalog();
  if (modelLoadPending()) {
    stream.cancelUpload();
    return;
  }
  if (stream.entries.empty())
    return;
  stream.collectDemand(TextureMipDemandView(view, projection, {extent.width, extent.height}));
  stream.planResidency();
  stream.advance();
}

bool RendererFrontend::retireTextureStreamingForModelLoad() {
  return !textureStreaming_ || textureStreaming_->retireForModelLoad();
}

VkDeviceSize RendererFrontend::prepareTextureStreamingForModelAdmission() {
  return textureStreaming_ ? textureStreaming_->prepareForModelAdmission() : 0;
}

bool RendererFrontend::prepareTextureStreamingForModelCommit() {
  return textureStreaming_ && textureStreaming_->prepareForModelCommit();
}

void RendererFrontend::refreshTextureStreamingDescriptors() {
  if (textureStreaming_)
    textureStreaming_->refreshCatalog();
}

void RendererFrontend::addTextureStreamingDiagnostics(AssetStreamingSummary& summary) const {
  const auto* stream = textureStreaming_.get();
  if (!stream)
    return;
  for (const auto& [key, entry] : stream->entries) {
    if (!entry.live)
      continue;
    ++summary.textureImages;
    summary.textureFallbackImages += !entry.tail;
    summary.textureWaitingImages += entry.first() > entry.demand;
  }
  const auto progress = stream->upload.progress();
  summary.texturePendingBytes = progress.totalBytes - progress.completedBytes;
  summary.texturePeakCopyBytes = stream->maxBytesPerTick;
  summary.texturePromotions = stream->promotions;
  summary.textureEvictions = stream->evictions;
}

std::vector<std::optional<AssetStreamingMarker>> RendererFrontend::textureMaterialMarkers() const {
  const auto* stream = textureStreaming_.get();
  if (!stream || stream->entries.empty() || !subs_.sceneManager)
    return {};
  const auto materials = subs_.sceneManager->gpuMaterialData();
  std::vector<std::optional<AssetStreamingMarker>> markers(materials.size());
  for (size_t i = 0; i < materials.size(); ++i) {
    auto& marker = markers[i];
    visitMaterialTextures(materials[i], [&](uint32_t slot, const gpu::GpuTextureTransform&) {
      if (slot >= stream->slotEntries.size())
        return;
      if (const auto* entry = stream->slotEntries[slot]) {
        if (!marker)
          marker.emplace();
        marker->level = std::max(marker->level, entry->first());
        marker->waitingForDetail |= entry->first() > entry->demand;
      }
    });
  }
  return markers;
}

void RendererFrontend::writeTextureStreamingTelemetry(nlohmann::json& json) const {
  auto rows = nlohmann::json::array();
  const auto* stream = textureStreaming_.get();
  if (stream) {
    for (size_t slot = 0; slot < stream->slotEntries.size(); ++slot) {
      const auto* entry = stream->slotEntries[slot];
      if (!entry)
        continue;
      const auto& image = *entry->source.image;
      const auto first = entry->first();
      const auto& extent = image.mips[first];
      const auto* resource = subs_.sceneManager->materialTextureResource(static_cast<uint32_t>(slot));
      const auto format = vk::to_string(static_cast<vk::Format>(image.format));
      const auto block = material::TextureBlockShape(image.format);
      rows.push_back({{"slot", slot}, {"name", resource ? resource->name : std::string{}},
          {"format", static_cast<int32_t>(image.format)}, {"formatName", format},
          {"srgb", format.find("Srgb") != std::string::npos},
          {"compressed", block.width > 1 || block.height > 1},
          {"sourceExtent", {image.mips.front().width, image.mips.front().height}},
          {"sourceMipCount", image.mips.size()}, {"fallbackFirstMip", entry->source.fallbackFirstMip},
          {"demandFirstMip", entry->demand}, {"plannedFirstMip", entry->planned},
          {"residentFirstMip", first}, {"residentMipCount", image.mips.size() - first},
          {"residentExtent", {extent.width, extent.height}}, {"imageId", entry->imageId},
          {"residentBytes", entry->tail ? entry->tail->residentBytes() : entry->source.residentBytes}});
    }
  }
  json["textureStreaming"] = {
      {"enabled", svc_.config.textureStreamingFallbackExtent != 0},
      {"deviceBc3Srgb", stream && stream->deviceBc3Srgb},
      {"rows", std::move(rows)}, {"promotions", stream ? stream->promotions : 0u},
      {"evictions", stream ? stream->evictions : 0u},
      {"cancellations", stream ? stream->cancellations : 0u},
      {"submittedChunks", stream ? stream->submittedChunks : 0u},
      {"maxBytesPerTick", stream ? stream->maxBytesPerTick : 0u},
      {"peakStagingBytes", stream ? stream->peakStagingBytes : 0u},
      {"budgetDeferrals", stream ? stream->budgetDeferrals : 0u},
      {"uploadFailures", stream ? stream->uploadFailures : 0u},
      {"pending", stream && stream->uploading != 0u},
  };
}

}  // namespace container::renderer
