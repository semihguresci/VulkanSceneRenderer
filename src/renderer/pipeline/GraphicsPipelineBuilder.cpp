#include "Container/renderer/pipeline/GraphicsPipelineBuilder.h"
#include "Container/geometry/Model.h"
#include "Container/renderer/debug/DebugOverlayRenderer.h"
#include "Container/renderer/lighting/LightPushConstants.h"
#include "Container/utility/FileLoader.h"
#include "Container/utility/PipelineManager.h"
#include "Container/utility/SceneData.h"
#include "Container/utility/ShaderModule.h"
#include "Container/utility/VulkanDevice.h"

#include <array>
#include <stdexcept>
#include <vector>

namespace container::renderer {

using container::gpu::BindlessPushConstants;
using container::gpu::PostProcessPushConstants;
using container::gpu::ShadowPushConstants;
using container::gpu::TiledLightingPushConstants;

GraphicsPipelineBuilder::GraphicsPipelineBuilder(
    std::shared_ptr<container::gpu::VulkanDevice> device,
    container::gpu::PipelineManager &pipelineManager)
    : device_(std::move(device)), pipelineManager_(pipelineManager) {}

PipelineBuildResult GraphicsPipelineBuilder::build(
    const std::filesystem::path &shaderDir,
    const PipelineDescriptorLayouts &descriptorLayouts,
    const PipelineRenderPasses &renderPasses,
    VkSampleCountFlagBits sceneSampleCount) const {

  // ---- shader loading helpers -----------------------------------------------
  struct ShaderModuleStore {
    VkDevice device{VK_NULL_HANDLE};
    std::vector<VkShaderModule> modules{};

    ~ShaderModuleStore() {
      for (VkShaderModule module : modules) {
        if (module != VK_NULL_HANDLE) {
          destroyOwnedShaderModule(device, module, nullptr);
        }
      }
    }

    VkShaderModule load(const std::filesystem::path &shaderBaseDir,
                        const char *path) {
      std::filesystem::path shaderPath(path);
      if (shaderPath.is_relative()) {
        shaderPath = shaderBaseDir / shaderPath;
      }
      const auto fileData = container::util::readFile(shaderPath);
      modules.push_back(VK_NULL_HANDLE);
      try {
        modules.back() = container::gpu::createShaderModule(device, fileData);
      } catch (...) {
        modules.pop_back();
        throw;
      }
      return modules.back();
    }
  };

  ShaderModuleStore shaderModules{device_->device()};
  const auto loadModule = [&](const char *path) {
    return shaderModules.load(shaderDir, path);
  };

  const auto makeStage = [](VkShaderModule module,
                            VkShaderStageFlagBits stage) {
    VkPipelineShaderStageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage = stage;
    info.module = module;
    switch (stage) {
    case VK_SHADER_STAGE_VERTEX_BIT:
      info.pName = "vertMain";
      break;
    case VK_SHADER_STAGE_FRAGMENT_BIT:
      info.pName = "fragMain";
      break;
    case VK_SHADER_STAGE_GEOMETRY_BIT:
      info.pName = "geomMain";
      break;
    default:
      info.pName = "main";
      break;
    }
    return info;
  };

  // ---- load shader modules --------------------------------------------------
  VkShaderModule depthPrepassVert =
      loadModule("spv_shaders/depth_prepass.vert.spv");
  VkShaderModule depthPrepassFrag =
      loadModule("spv_shaders/depth_prepass.frag.spv");
  VkShaderModule gBufferVert = loadModule("spv_shaders/gbuffer.vert.spv");
  VkShaderModule gBufferFrag = loadModule("spv_shaders/gbuffer.frag.spv");
  VkShaderModule dirVert =
      loadModule("spv_shaders/deferred_directional.vert.spv");
  VkShaderModule dirFrag =
      loadModule("spv_shaders/deferred_directional.frag.spv");
  VkShaderModule stencilVert = loadModule("spv_shaders/light_stencil.vert.spv");
  VkShaderModule stencilFrag = loadModule("spv_shaders/light_stencil.frag.spv");
  VkShaderModule pointVert = loadModule("spv_shaders/point_light.vert.spv");
  VkShaderModule pointFrag = loadModule("spv_shaders/point_light.frag.spv");
  VkShaderModule pointDbgVert =
      loadModule("spv_shaders/point_light_stencil_debug.vert.spv");
  VkShaderModule pointDbgFrag =
      loadModule("spv_shaders/point_light_stencil_debug.frag.spv");
  VkShaderModule forwardOpaqueVert =
      loadModule("spv_shaders/forward_opaque.vert.spv");
  VkShaderModule forwardOpaqueFrag =
      loadModule("spv_shaders/forward_opaque.frag.spv");
  VkShaderModule skyVert = loadModule("spv_shaders/forward_sky.vert.spv");
  VkShaderModule skyFrag = loadModule("spv_shaders/forward_sky.frag.spv");
  VkShaderModule transVert =
      loadModule("spv_shaders/forward_transparent.vert.spv");
  VkShaderModule transFrag =
      loadModule("spv_shaders/forward_transparent.frag.spv");
  VkShaderModule transPickVert =
      loadModule("spv_shaders/transparent_pick.vert.spv");
  VkShaderModule transPickFrag =
      loadModule("spv_shaders/transparent_pick.frag.spv");
  VkShaderModule postVert = loadModule("spv_shaders/post_process.vert.spv");
  VkShaderModule postFrag = loadModule("spv_shaders/post_process.frag.spv");
  VkShaderModule dbgVert = loadModule("spv_shaders/geometry_debug.vert.spv");
  VkShaderModule dbgFrag = loadModule("spv_shaders/geometry_debug.frag.spv");
  VkShaderModule nvVert = loadModule("spv_shaders/normal_validation.vert.spv");
  VkShaderModule nvGeom = loadModule("spv_shaders/normal_validation.geom.spv");
  VkShaderModule nvFrag = loadModule("spv_shaders/normal_validation.frag.spv");
  VkShaderModule wfVert = loadModule("spv_shaders/wireframe_debug.vert.spv");
  VkShaderModule wfFrag = loadModule("spv_shaders/wireframe_debug.frag.spv");
  VkShaderModule wfFbVert =
      loadModule("spv_shaders/wireframe_fallback.vert.spv");
  VkShaderModule wfFbGeom =
      loadModule("spv_shaders/wireframe_fallback.geom.spv");
  VkShaderModule wfFbFrag =
      loadModule("spv_shaders/wireframe_fallback.frag.spv");
  VkShaderModule selOutlineVert =
      loadModule("spv_shaders/selection_outline.vert.spv");
  VkShaderModule selOutlineFrag =
      loadModule("spv_shaders/selection_outline.frag.spv");
  VkShaderModule snVert = loadModule("spv_shaders/surface_normals.vert.spv");
  VkShaderModule snGeom = loadModule("spv_shaders/surface_normals.geom.spv");
  VkShaderModule snFrag = loadModule("spv_shaders/surface_normals.frag.spv");
  VkShaderModule onVert = loadModule("spv_shaders/object_normals.vert.spv");
  VkShaderModule onFrag = loadModule("spv_shaders/object_normals.frag.spv");
  VkShaderModule lgVert = loadModule("spv_shaders/light_gizmo.vert.spv");
  VkShaderModule lgFrag = loadModule("spv_shaders/light_gizmo.frag.spv");
  VkShaderModule lgPickVert =
      loadModule("spv_shaders/light_gizmo_pick.vert.spv");
  VkShaderModule lgPickFrag =
      loadModule("spv_shaders/light_gizmo_pick.frag.spv");
  VkShaderModule tgVert = loadModule("spv_shaders/transform_gizmo.vert.spv");
  VkShaderModule tgFrag = loadModule("spv_shaders/transform_gizmo.frag.spv");
  VkShaderModule sdVert = loadModule("spv_shaders/shadow_depth.vert.spv");
  VkShaderModule sdFrag = loadModule("spv_shaders/shadow_depth.frag.spv");
  VkShaderModule lsdVert =
      loadModule("spv_shaders/local_shadow_depth.vert.spv");
  VkShaderModule lsdFrag =
      loadModule("spv_shaders/local_shadow_depth.frag.spv");
  VkShaderModule tlVert = loadModule("spv_shaders/tiled_lighting.vert.spv");
  VkShaderModule tlFrag = loadModule("spv_shaders/tiled_lighting.frag.spv");

  // ---- shader stage arrays --------------------------------------------------
  std::array<VkPipelineShaderStageCreateInfo, 2> depthPrepassStages = {
      makeStage(depthPrepassVert, VK_SHADER_STAGE_VERTEX_BIT),
      makeStage(depthPrepassFrag, VK_SHADER_STAGE_FRAGMENT_BIT)};
  std::array<VkPipelineShaderStageCreateInfo, 2> gBufferStages = {
      makeStage(gBufferVert, VK_SHADER_STAGE_VERTEX_BIT),
      makeStage(gBufferFrag, VK_SHADER_STAGE_FRAGMENT_BIT)};
  std::array<VkPipelineShaderStageCreateInfo, 2> dirStages = {
      makeStage(dirVert, VK_SHADER_STAGE_VERTEX_BIT),
      makeStage(dirFrag, VK_SHADER_STAGE_FRAGMENT_BIT)};
  std::array<VkPipelineShaderStageCreateInfo, 2> stencilStages = {
      makeStage(stencilVert, VK_SHADER_STAGE_VERTEX_BIT),
      makeStage(stencilFrag, VK_SHADER_STAGE_FRAGMENT_BIT)};
  std::array<VkPipelineShaderStageCreateInfo, 2> pointStages = {
      makeStage(pointVert, VK_SHADER_STAGE_VERTEX_BIT),
      makeStage(pointFrag, VK_SHADER_STAGE_FRAGMENT_BIT)};
  std::array<VkPipelineShaderStageCreateInfo, 2> pointDbgStages = {
      makeStage(pointDbgVert, VK_SHADER_STAGE_VERTEX_BIT),
      makeStage(pointDbgFrag, VK_SHADER_STAGE_FRAGMENT_BIT)};
  std::array<VkPipelineShaderStageCreateInfo, 2> forwardOpaqueStages = {
      makeStage(forwardOpaqueVert, VK_SHADER_STAGE_VERTEX_BIT),
      makeStage(forwardOpaqueFrag, VK_SHADER_STAGE_FRAGMENT_BIT)};
  std::array<VkPipelineShaderStageCreateInfo, 2> skyStages = {
      makeStage(skyVert, VK_SHADER_STAGE_VERTEX_BIT),
      makeStage(skyFrag, VK_SHADER_STAGE_FRAGMENT_BIT)};
  std::array<VkPipelineShaderStageCreateInfo, 2> transStages = {
      makeStage(transVert, VK_SHADER_STAGE_VERTEX_BIT),
      makeStage(transFrag, VK_SHADER_STAGE_FRAGMENT_BIT)};
  std::array<VkPipelineShaderStageCreateInfo, 2> transPickStages = {
      makeStage(transPickVert, VK_SHADER_STAGE_VERTEX_BIT),
      makeStage(transPickFrag, VK_SHADER_STAGE_FRAGMENT_BIT)};
  std::array<VkPipelineShaderStageCreateInfo, 2> postStages = {
      makeStage(postVert, VK_SHADER_STAGE_VERTEX_BIT),
      makeStage(postFrag, VK_SHADER_STAGE_FRAGMENT_BIT)};
  std::array<VkPipelineShaderStageCreateInfo, 2> dbgStages = {
      makeStage(dbgVert, VK_SHADER_STAGE_VERTEX_BIT),
      makeStage(dbgFrag, VK_SHADER_STAGE_FRAGMENT_BIT)};
  std::array<VkPipelineShaderStageCreateInfo, 3> nvStages = {
      makeStage(nvVert, VK_SHADER_STAGE_VERTEX_BIT),
      makeStage(nvGeom, VK_SHADER_STAGE_GEOMETRY_BIT),
      makeStage(nvFrag, VK_SHADER_STAGE_FRAGMENT_BIT)};
  std::array<VkPipelineShaderStageCreateInfo, 2> wfStages = {
      makeStage(wfVert, VK_SHADER_STAGE_VERTEX_BIT),
      makeStage(wfFrag, VK_SHADER_STAGE_FRAGMENT_BIT)};
  std::array<VkPipelineShaderStageCreateInfo, 3> wfFbStages = {
      makeStage(wfFbVert, VK_SHADER_STAGE_VERTEX_BIT),
      makeStage(wfFbGeom, VK_SHADER_STAGE_GEOMETRY_BIT),
      makeStage(wfFbFrag, VK_SHADER_STAGE_FRAGMENT_BIT)};
  std::array<VkPipelineShaderStageCreateInfo, 2> selOutlineStages = {
      makeStage(selOutlineVert, VK_SHADER_STAGE_VERTEX_BIT),
      makeStage(selOutlineFrag, VK_SHADER_STAGE_FRAGMENT_BIT)};
  std::array<VkPipelineShaderStageCreateInfo, 3> snStages = {
      makeStage(snVert, VK_SHADER_STAGE_VERTEX_BIT),
      makeStage(snGeom, VK_SHADER_STAGE_GEOMETRY_BIT),
      makeStage(snFrag, VK_SHADER_STAGE_FRAGMENT_BIT)};
  std::array<VkPipelineShaderStageCreateInfo, 2> onStages = {
      makeStage(onVert, VK_SHADER_STAGE_VERTEX_BIT),
      makeStage(onFrag, VK_SHADER_STAGE_FRAGMENT_BIT)};
  std::array<VkPipelineShaderStageCreateInfo, 2> lgStages = {
      makeStage(lgVert, VK_SHADER_STAGE_VERTEX_BIT),
      makeStage(lgFrag, VK_SHADER_STAGE_FRAGMENT_BIT)};
  std::array<VkPipelineShaderStageCreateInfo, 2> lgPickStages = {
      makeStage(lgPickVert, VK_SHADER_STAGE_VERTEX_BIT),
      makeStage(lgPickFrag, VK_SHADER_STAGE_FRAGMENT_BIT)};
  std::array<VkPipelineShaderStageCreateInfo, 2> tgStages = {
      makeStage(tgVert, VK_SHADER_STAGE_VERTEX_BIT),
      makeStage(tgFrag, VK_SHADER_STAGE_FRAGMENT_BIT)};
  std::array<VkPipelineShaderStageCreateInfo, 2> sdStages = {
      makeStage(sdVert, VK_SHADER_STAGE_VERTEX_BIT),
      makeStage(sdFrag, VK_SHADER_STAGE_FRAGMENT_BIT)};
  std::array<VkPipelineShaderStageCreateInfo, 2> lsdStages = {
      makeStage(lsdVert, VK_SHADER_STAGE_VERTEX_BIT),
      makeStage(lsdFrag, VK_SHADER_STAGE_FRAGMENT_BIT)};
  std::array<VkPipelineShaderStageCreateInfo, 2> tlStages = {
      makeStage(tlVert, VK_SHADER_STAGE_VERTEX_BIT),
      makeStage(tlFrag, VK_SHADER_STAGE_FRAGMENT_BIT)};

  // ---- vertex input states --------------------------------------------------
  const auto bindingDesc = container::geometry::Vertex::bindingDescription();
  const auto attribDescs = container::geometry::Vertex::attributeDescriptions();

  // Pipeline variants intentionally use the smallest vertex layout each shader
  // needs. Depth and shadow passes skip normal/tangent attributes, while the
  // G-buffer keeps the full layout for material and normal-map evaluation.
  VkPipelineVertexInputStateCreateInfo fullVertexInput{};
  fullVertexInput.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  fullVertexInput.vertexBindingDescriptionCount = 1;
  fullVertexInput.vertexAttributeDescriptionCount =
      static_cast<uint32_t>(attribDescs.size());
  fullVertexInput.pVertexBindingDescriptions = &bindingDesc;
  fullVertexInput.pVertexAttributeDescriptions = attribDescs.data();

  std::array<VkVertexInputAttributeDescription, 1> posOnlyAttribs = {
      attribDescs[0]};
  std::array<VkVertexInputAttributeDescription, 2> posTexAttribs = {
      attribDescs[0], attribDescs[2]};
  std::array<VkVertexInputAttributeDescription, 4> posTexNormAttribs = {
      attribDescs[0], attribDescs[2], attribDescs[3], attribDescs[5]};
  std::array<VkVertexInputAttributeDescription, 3> posTexNormNoTex1Attribs = {
      attribDescs[0], attribDescs[2], attribDescs[3]};
  std::array<VkVertexInputAttributeDescription, 6>
      posColorTexNormTangentAttribs = {attribDescs[0], attribDescs[1],
                                       attribDescs[2], attribDescs[3],
                                       attribDescs[4], attribDescs[5]};

  VkPipelineVertexInputStateCreateInfo posOnlyInput = fullVertexInput;
  posOnlyInput.vertexAttributeDescriptionCount = 1;
  posOnlyInput.pVertexAttributeDescriptions = posOnlyAttribs.data();

  VkPipelineVertexInputStateCreateInfo posTexInput = fullVertexInput;
  posTexInput.vertexAttributeDescriptionCount = 2;
  posTexInput.pVertexAttributeDescriptions = posTexAttribs.data();

  VkPipelineVertexInputStateCreateInfo posTexNormInput = fullVertexInput;
  posTexNormInput.vertexAttributeDescriptionCount =
      static_cast<uint32_t>(posTexNormAttribs.size());
  posTexNormInput.pVertexAttributeDescriptions = posTexNormAttribs.data();

  VkPipelineVertexInputStateCreateInfo posTexNormNoTex1Input = fullVertexInput;
  posTexNormNoTex1Input.vertexAttributeDescriptionCount =
      static_cast<uint32_t>(posTexNormNoTex1Attribs.size());
  posTexNormNoTex1Input.pVertexAttributeDescriptions =
      posTexNormNoTex1Attribs.data();

  VkPipelineVertexInputStateCreateInfo posColorTexNormTangentInput =
      fullVertexInput;
  posColorTexNormTangentInput.vertexAttributeDescriptionCount =
      static_cast<uint32_t>(posColorTexNormTangentAttribs.size());
  posColorTexNormTangentInput.pVertexAttributeDescriptions =
      posColorTexNormTangentAttribs.data();

  VkPipelineVertexInputStateCreateInfo emptyVertexInput{};
  emptyVertexInput.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

  // ---- input assemblies -----------------------------------------------------
  VkPipelineInputAssemblyStateCreateInfo triAssembly{};
  triAssembly.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  triAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkPipelineInputAssemblyStateCreateInfo pointAssembly = triAssembly;
  pointAssembly.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;

  VkPipelineInputAssemblyStateCreateInfo lineAssembly = triAssembly;
  lineAssembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

  // ---- viewport state (dynamic) --------------------------------------------
  VkPipelineViewportStateCreateInfo vpState{};
  vpState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  vpState.viewportCount = 1;
  vpState.scissorCount = 1;

  // ---- rasterizers ----------------------------------------------------------
  VkPipelineRasterizationStateCreateInfo sceneRaster{};
  sceneRaster.sType =
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  sceneRaster.polygonMode = VK_POLYGON_MODE_FILL;
  sceneRaster.lineWidth = 1.0f;
  // Imported glTF winding is repaired during load. Single-sided primitives,
  // including open surfaces, use back-face culling; materially double-sided
  // and ambiguous mixed-winding primitives route to the no-cull variants.
  sceneRaster.cullMode = VK_CULL_MODE_BACK_BIT;
  // glTF authors geometry with counter-clockwise front faces. Keep that
  // winding as the raster front face for scene passes; compensating for the
  // negative-height viewport here causes the G-buffer to shade back faces and
  // makes direct lighting reject visible surfaces.
  sceneRaster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

  VkPipelineRasterizationStateCreateInfo noCullRaster = sceneRaster;
  noCullRaster.cullMode = VK_CULL_MODE_NONE;

  VkPipelineRasterizationStateCreateInfo frontCullRaster = sceneRaster;
  frontCullRaster.cullMode = VK_CULL_MODE_FRONT_BIT;

  // Fullscreen passes draw a synthetic triangle from SV_VertexID and should not
  // inherit scene culling assumptions.
  VkPipelineRasterizationStateCreateInfo fullscreenRaster = sceneRaster;
  fullscreenRaster.cullMode = VK_CULL_MODE_NONE;

  VkPipelineRasterizationStateCreateInfo normalLineRaster = sceneRaster;
  normalLineRaster.cullMode = VK_CULL_MODE_NONE;

  VkPipelineRasterizationStateCreateInfo wfFbRaster = sceneRaster;

  VkPipelineRasterizationStateCreateInfo wfRaster = sceneRaster;
  wfRaster.polygonMode = VK_POLYGON_MODE_LINE;
  wfRaster.lineWidth = 1.0f;

  VkPipelineRasterizationStateCreateInfo wfDepthRaster = wfRaster;
  // Wireframe lines are rendered against the filled depth prepass. With
  // reverse-Z, a small positive depth bias pulls the line fragments slightly
  // toward the camera so precision differences do not reject almost all edges.
  wfDepthRaster.depthBiasEnable = VK_TRUE;
  wfDepthRaster.depthBiasConstantFactor = 1.0f;
  wfDepthRaster.depthBiasSlopeFactor = 1.0f;

  VkPipelineRasterizationStateCreateInfo wfFbDepthRaster = wfFbRaster;
  wfFbDepthRaster.depthBiasEnable = VK_TRUE;
  wfFbDepthRaster.depthBiasConstantFactor = 1.0f;
  wfFbDepthRaster.depthBiasSlopeFactor = 1.0f;

  VkPipelineRasterizationStateCreateInfo wfFrontCullRaster = wfRaster;
  wfFrontCullRaster.cullMode = VK_CULL_MODE_FRONT_BIT;

  VkPipelineRasterizationStateCreateInfo wfDepthFrontCullRaster = wfDepthRaster;
  wfDepthFrontCullRaster.cullMode = VK_CULL_MODE_FRONT_BIT;

  VkPipelineRasterizationStateCreateInfo wfFbFrontCullRaster = wfFbRaster;
  wfFbFrontCullRaster.cullMode = VK_CULL_MODE_FRONT_BIT;

  VkPipelineRasterizationStateCreateInfo wfFbDepthFrontCullRaster =
      wfFbDepthRaster;
  wfFbDepthFrontCullRaster.cullMode = VK_CULL_MODE_FRONT_BIT;

  // ---- multisample ----------------------------------------------------------
  VkPipelineMultisampleStateCreateInfo sceneMsaa{};
  sceneMsaa.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  sceneMsaa.rasterizationSamples = sceneSampleCount;
  VkPipelineMultisampleStateCreateInfo singleSample{};
  singleSample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  singleSample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  // ---- color blend attachments ----------------------------------------------
  VkPipelineColorBlendAttachmentState opaqueAttach{};
  opaqueAttach.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

  VkPipelineColorBlendAttachmentState additiveAttach = opaqueAttach;
  additiveAttach.blendEnable = VK_TRUE;
  additiveAttach.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
  additiveAttach.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
  additiveAttach.colorBlendOp = VK_BLEND_OP_ADD;
  additiveAttach.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  additiveAttach.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  additiveAttach.alphaBlendOp = VK_BLEND_OP_ADD;

  VkPipelineColorBlendAttachmentState overlayAttach = opaqueAttach;
  overlayAttach.blendEnable = VK_TRUE;
  overlayAttach.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  overlayAttach.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  overlayAttach.colorBlendOp = VK_BLEND_OP_ADD;
  overlayAttach.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  overlayAttach.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
  overlayAttach.alphaBlendOp = VK_BLEND_OP_ADD;

  VkPipelineColorBlendAttachmentState noColorAttach = opaqueAttach;
  noColorAttach.colorWriteMask = 0;

  std::array<VkPipelineColorBlendAttachmentState, 6> gBufAttachs = {
      opaqueAttach, opaqueAttach, opaqueAttach,
      opaqueAttach, opaqueAttach, opaqueAttach};
  std::array<VkPipelineColorBlendAttachmentState, 1> opaqueArr = {opaqueAttach};
  std::array<VkPipelineColorBlendAttachmentState, 1> additiveArr = {
      additiveAttach};
  std::array<VkPipelineColorBlendAttachmentState, 1> overlayArr = {
      overlayAttach};
  std::array<VkPipelineColorBlendAttachmentState, 1> noColorArr = {
      noColorAttach};

  // blend state helpers
  auto makeBlend = [](const VkPipelineColorBlendAttachmentState *arr,
                      uint32_t count) {
    VkPipelineColorBlendStateCreateInfo b{};
    b.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    b.attachmentCount = count;
    b.pAttachments = arr;
    return b;
  };

  VkPipelineColorBlendStateCreateInfo noBlend = makeBlend(nullptr, 0);
  VkPipelineColorBlendStateCreateInfo gBufBlend =
      makeBlend(gBufAttachs.data(), static_cast<uint32_t>(gBufAttachs.size()));
  VkPipelineColorBlendStateCreateInfo opaqueBlend =
      makeBlend(opaqueArr.data(), 1);
  VkPipelineColorBlendStateCreateInfo addBlend =
      makeBlend(additiveArr.data(), 1);
  VkPipelineColorBlendStateCreateInfo overlayBlend =
      makeBlend(overlayArr.data(), 1);
  VkPipelineColorBlendStateCreateInfo noColorBlend =
      makeBlend(noColorArr.data(), 1);

  // ---- dynamic states -------------------------------------------------------
  std::array<VkDynamicState, 2> dynStates = {VK_DYNAMIC_STATE_VIEWPORT,
                                             VK_DYNAMIC_STATE_SCISSOR};
  std::array<VkDynamicState, 3> lineDynStates = {VK_DYNAMIC_STATE_VIEWPORT,
                                                 VK_DYNAMIC_STATE_SCISSOR,
                                                 VK_DYNAMIC_STATE_LINE_WIDTH};
  std::array<VkDynamicState, 3> shadowDynStates = {VK_DYNAMIC_STATE_VIEWPORT,
                                                   VK_DYNAMIC_STATE_SCISSOR,
                                                   VK_DYNAMIC_STATE_DEPTH_BIAS};
  VkPipelineDynamicStateCreateInfo dynState{};
  dynState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynState.dynamicStateCount = static_cast<uint32_t>(dynStates.size());
  dynState.pDynamicStates = dynStates.data();

  VkPipelineDynamicStateCreateInfo lineDynState{};
  lineDynState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  lineDynState.dynamicStateCount = static_cast<uint32_t>(lineDynStates.size());
  lineDynState.pDynamicStates = lineDynStates.data();

  VkPipelineDynamicStateCreateInfo shadowDynState{};
  shadowDynState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  shadowDynState.dynamicStateCount =
      static_cast<uint32_t>(shadowDynStates.size());
  shadowDynState.pDynamicStates = shadowDynStates.data();

  // ---- depth/stencil states -------------------------------------------------
  VkPipelineDepthStencilStateCreateInfo depthPrepassDS{};
  depthPrepassDS.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthPrepassDS.depthTestEnable = VK_TRUE;
  depthPrepassDS.depthWriteEnable = VK_TRUE;
  depthPrepassDS.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;

  VkPipelineDepthStencilStateCreateInfo gBufDS = depthPrepassDS;
  gBufDS.depthWriteEnable = VK_FALSE;
  // Replaying geometry after the depth prepass with an exact compare can
  // drop fragments on detailed meshes due to tiny rasterization/precision
  // differences between passes. Reverse-Z keeps nearer fragments at larger
  // depth values, so GREATER_OR_EQUAL preserves the prepass rejection while
  // allowing numerically equivalent fragments to shade.
  gBufDS.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;

  VkPipelineDepthStencilStateCreateInfo noDS{};
  noDS.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

  VkPipelineDepthStencilStateCreateInfo normalLineDS = depthPrepassDS;
  normalLineDS.depthWriteEnable = VK_FALSE;

  VkPipelineDepthStencilStateCreateInfo wfDepthDS = normalLineDS;
  VkPipelineDepthStencilStateCreateInfo wfNoDepthDS = noDS;

  VkPipelineDepthStencilStateCreateInfo selectionMaskDS = noDS;
  selectionMaskDS.stencilTestEnable = VK_TRUE;
  selectionMaskDS.front = {VK_STENCIL_OP_KEEP,
                           VK_STENCIL_OP_REPLACE,
                           VK_STENCIL_OP_KEEP,
                           VK_COMPARE_OP_ALWAYS,
                           0xff,
                           0xff,
                           1};
  selectionMaskDS.back = selectionMaskDS.front;

  VkPipelineDepthStencilStateCreateInfo selectionOutlineDS = noDS;
  selectionOutlineDS.stencilTestEnable = VK_TRUE;
  selectionOutlineDS.front = {VK_STENCIL_OP_KEEP,
                              VK_STENCIL_OP_KEEP,
                              VK_STENCIL_OP_KEEP,
                              VK_COMPARE_OP_NOT_EQUAL,
                              0xff,
                              0x00,
                              1};
  selectionOutlineDS.back = selectionOutlineDS.front;

  VkPipelineDepthStencilStateCreateInfo stencilDS = depthPrepassDS;
  stencilDS.depthWriteEnable = VK_FALSE;
  stencilDS.stencilTestEnable = VK_TRUE;
  stencilDS.front = {VK_STENCIL_OP_KEEP,
                     VK_STENCIL_OP_KEEP,
                     VK_STENCIL_OP_DECREMENT_AND_WRAP,
                     VK_COMPARE_OP_ALWAYS,
                     0xff,
                     0xff,
                     0};
  stencilDS.back = {VK_STENCIL_OP_KEEP,
                    VK_STENCIL_OP_KEEP,
                    VK_STENCIL_OP_INCREMENT_AND_WRAP,
                    VK_COMPARE_OP_ALWAYS,
                    0xff,
                    0xff,
                    0};

  VkPipelineDepthStencilStateCreateInfo pointLightDS = noDS;
  pointLightDS.stencilTestEnable = VK_TRUE;
  pointLightDS.front = {VK_STENCIL_OP_KEEP,
                        VK_STENCIL_OP_KEEP,
                        VK_STENCIL_OP_KEEP,
                        VK_COMPARE_OP_NOT_EQUAL,
                        0xff,
                        0x00,
                        0};
  pointLightDS.back = pointLightDS.front;

  VkPipelineDepthStencilStateCreateInfo transparentDS = depthPrepassDS;
  transparentDS.depthWriteEnable = VK_FALSE;

  VkPipelineDepthStencilStateCreateInfo transparentPickDS = depthPrepassDS;
  transparentPickDS.depthWriteEnable = VK_TRUE;
  transparentPickDS.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;

  // ---- push constant ranges -------------------------------------------------
  VkPushConstantRange scenePCR{};
  scenePCR.stageFlags =
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  scenePCR.size = sizeof(BindlessPushConstants);

  VkPushConstantRange lightPCR{};
  lightPCR.stageFlags =
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  lightPCR.size = sizeof(LightPushConstants);

  VkPushConstantRange postPCR{};
  postPCR.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  postPCR.size = sizeof(PostProcessPushConstants);

  VkPushConstantRange wfPCR{};
  wfPCR.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  wfPCR.size = sizeof(WireframePushConstants);

  VkPushConstantRange snPCR{};
  snPCR.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT |
                     VK_SHADER_STAGE_FRAGMENT_BIT;
  snPCR.size = sizeof(SurfaceNormalPushConstants);

  VkPushConstantRange transformGizmoPCR{};
  transformGizmoPCR.stageFlags =
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  transformGizmoPCR.size = sizeof(TransformGizmoPushConstants);

  VkPushConstantRange nvPCR{};
  nvPCR.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT |
                     VK_SHADER_STAGE_FRAGMENT_BIT;
  nvPCR.size = sizeof(NormalValidationPushConstants);

  VkPushConstantRange shadowPCR{};
  shadowPCR.stageFlags =
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  shadowPCR.size = sizeof(ShadowPushConstants);

  VkPushConstantRange tiledLightPCR{};
  tiledLightPCR.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  tiledLightPCR.size = sizeof(TiledLightingPushConstants);

  // ---- pipeline layouts -----------------------------------------------------
  PipelineLayouts layouts;
  layouts.scene = pipelineManager_.createPipelineLayout(
      {descriptorLayouts.scene}, {scenePCR});
  layouts.transparent = pipelineManager_.createPipelineLayout(
      {descriptorLayouts.scene, descriptorLayouts.light, descriptorLayouts.oit,
       descriptorLayouts.lighting},
      {scenePCR});
  layouts.lighting = pipelineManager_.createPipelineLayout(
      {descriptorLayouts.lighting, descriptorLayouts.light,
       descriptorLayouts.scene},
      {lightPCR});
  layouts.lightGizmo = pipelineManager_.createPipelineLayout(
      {descriptorLayouts.lighting, descriptorLayouts.lightGizmo}, {lightPCR});
  layouts.tiledLighting = pipelineManager_.createPipelineLayout(
      {descriptorLayouts.lighting, descriptorLayouts.tiled,
       descriptorLayouts.scene, descriptorLayouts.light},
      {tiledLightPCR});
  layouts.shadow = pipelineManager_.createPipelineLayout(
      {descriptorLayouts.scene, descriptorLayouts.shadow}, {shadowPCR});
  layouts.postProcess = pipelineManager_.createPipelineLayout(
      {descriptorLayouts.postProcess, descriptorLayouts.oit}, {postPCR});
  layouts.wireframe =
      pipelineManager_.createPipelineLayout({descriptorLayouts.scene}, {wfPCR});
  layouts.normalValidation =
      pipelineManager_.createPipelineLayout({descriptorLayouts.scene}, {nvPCR});
  layouts.surfaceNormal =
      pipelineManager_.createPipelineLayout({descriptorLayouts.scene}, {snPCR});
  layouts.transformGizmo = pipelineManager_.createPipelineLayout(
      {descriptorLayouts.lighting}, {transformGizmoPCR});
  layouts.layoutRegistry = buildGraphicsPipelineLayoutRegistry(layouts);

  // ---- base pipeline create info --------------------------------------------
  // scene geometry base (depth prepass / gbuffer)
  RenderingGraphicsPipelineCreateInfo scenePCI{};
  scenePCI.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  scenePCI.pVertexInputState = &posTexInput;
  scenePCI.pInputAssemblyState = &triAssembly;
  scenePCI.pViewportState = &vpState;
  scenePCI.pRasterizationState = &sceneRaster;
  scenePCI.pMultisampleState = &sceneMsaa;
  scenePCI.pDepthStencilState = &depthPrepassDS;
  scenePCI.pColorBlendState = &noBlend;
  scenePCI.pDynamicState = &dynState;
  scenePCI.layout = layouts.scene;
  scenePCI.renderPass = renderPasses.depthPrepass;

  // fullscreen quad base (lighting passes)
  RenderingGraphicsPipelineCreateInfo fsPCI{};
  fsPCI.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  fsPCI.pVertexInputState = &emptyVertexInput;
  fsPCI.pInputAssemblyState = &triAssembly;
  fsPCI.pViewportState = &vpState;
  fsPCI.pRasterizationState = &fullscreenRaster;
  fsPCI.pMultisampleState = &singleSample;
  fsPCI.pDepthStencilState = &noDS;
  fsPCI.pColorBlendState = &opaqueBlend;
  fsPCI.pDynamicState = &dynState;
  fsPCI.layout = layouts.lighting;
  fsPCI.renderPass = renderPasses.lighting;

  // scene mesh base (transparent / debug variants)
  RenderingGraphicsPipelineCreateInfo meshPCI{};
  meshPCI.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  meshPCI.pVertexInputState = &posColorTexNormTangentInput;
  meshPCI.pInputAssemblyState = &triAssembly;
  meshPCI.pViewportState = &vpState;
  meshPCI.pRasterizationState = &sceneRaster;
  meshPCI.pMultisampleState = &singleSample;
  meshPCI.pDepthStencilState = &transparentDS;
  meshPCI.pColorBlendState = &noColorBlend;
  meshPCI.pDynamicState = &dynState;
  meshPCI.layout = layouts.transparent;
  meshPCI.renderPass = renderPasses.lighting;
  meshPCI.stageCount = static_cast<uint32_t>(transStages.size());
  meshPCI.pStages = transStages.data();

  RenderingGraphicsPipelineCreateInfo transparentPickPCI = meshPCI;
  transparentPickPCI.stageCount = static_cast<uint32_t>(transPickStages.size());
  transparentPickPCI.pStages = transPickStages.data();
  transparentPickPCI.pDepthStencilState = &transparentPickDS;
  transparentPickPCI.pColorBlendState = &opaqueBlend;
  transparentPickPCI.layout = layouts.scene;
  transparentPickPCI.renderPass = renderPasses.transparentPick;

  // ---- create pipelines -----------------------------------------------------
  GraphicsPipelines pipelines;

  // Depth prepass
  scenePCI.pVertexInputState = &posTexNormInput;
  scenePCI.stageCount = static_cast<uint32_t>(depthPrepassStages.size());
  scenePCI.pStages = depthPrepassStages.data();
  pipelines.depthPrepass = pipelineManager_.createGraphicsPipeline(
      scenePCI, "depth_prepass_pipeline");

  RenderingGraphicsPipelineCreateInfo depthFrontCullPCI = scenePCI;
  depthFrontCullPCI.pRasterizationState = &frontCullRaster;
  pipelines.depthPrepassFrontCull = pipelineManager_.createGraphicsPipeline(
      depthFrontCullPCI, "depth_prepass_front_cull_pipeline");

  RenderingGraphicsPipelineCreateInfo depthNoCullPCI = scenePCI;
  depthNoCullPCI.pRasterizationState = &noCullRaster;
  pipelines.depthPrepassNoCull = pipelineManager_.createGraphicsPipeline(
      depthNoCullPCI, "depth_prepass_no_cull_pipeline");

  RenderingGraphicsPipelineCreateInfo bimDepthPCI = scenePCI;
  bimDepthPCI.renderPass = renderPasses.bimDepthPrepass;
  pipelines.bimDepthPrepass = pipelineManager_.createGraphicsPipeline(
      bimDepthPCI, "bim_depth_prepass_pipeline");

  RenderingGraphicsPipelineCreateInfo bimDepthFrontCullPCI = bimDepthPCI;
  bimDepthFrontCullPCI.pRasterizationState = &frontCullRaster;
  pipelines.bimDepthPrepassFrontCull = pipelineManager_.createGraphicsPipeline(
      bimDepthFrontCullPCI, "bim_depth_prepass_front_cull_pipeline");

  RenderingGraphicsPipelineCreateInfo bimDepthNoCullPCI = bimDepthPCI;
  bimDepthNoCullPCI.pRasterizationState = &noCullRaster;
  pipelines.bimDepthPrepassNoCull = pipelineManager_.createGraphicsPipeline(
      bimDepthNoCullPCI, "bim_depth_prepass_no_cull_pipeline");

  // GBuffer
  RenderingGraphicsPipelineCreateInfo gBufPCI = scenePCI;
  gBufPCI.stageCount = static_cast<uint32_t>(gBufferStages.size());
  gBufPCI.pStages = gBufferStages.data();
  // The deferred G-buffer shader consumes vertex color, normal, tangent, and
  // both UV sets for PBR material evaluation.
  gBufPCI.pVertexInputState = &posColorTexNormTangentInput;
  gBufPCI.pDepthStencilState = &gBufDS;
  gBufPCI.pColorBlendState = &gBufBlend;
  gBufPCI.renderPass = renderPasses.gBuffer;
  pipelines.gBuffer =
      pipelineManager_.createGraphicsPipeline(gBufPCI, "gbuffer_pipeline");

  RenderingGraphicsPipelineCreateInfo gBufFrontCullPCI = gBufPCI;
  gBufFrontCullPCI.pRasterizationState = &frontCullRaster;
  pipelines.gBufferFrontCull = pipelineManager_.createGraphicsPipeline(
      gBufFrontCullPCI, "gbuffer_front_cull_pipeline");

  RenderingGraphicsPipelineCreateInfo gBufNoCullPCI = gBufPCI;
  gBufNoCullPCI.pRasterizationState = &noCullRaster;
  pipelines.gBufferNoCull = pipelineManager_.createGraphicsPipeline(
      gBufNoCullPCI, "gbuffer_no_cull_pipeline");

  RenderingGraphicsPipelineCreateInfo bimGBufPCI = gBufPCI;
  bimGBufPCI.renderPass = renderPasses.bimGBuffer;
  pipelines.bimGBuffer = pipelineManager_.createGraphicsPipeline(
      bimGBufPCI, "bim_gbuffer_pipeline");

  RenderingGraphicsPipelineCreateInfo bimGBufFrontCullPCI = bimGBufPCI;
  bimGBufFrontCullPCI.pRasterizationState = &frontCullRaster;
  pipelines.bimGBufferFrontCull = pipelineManager_.createGraphicsPipeline(
      bimGBufFrontCullPCI, "bim_gbuffer_front_cull_pipeline");

  RenderingGraphicsPipelineCreateInfo bimGBufNoCullPCI = bimGBufPCI;
  bimGBufNoCullPCI.pRasterizationState = &noCullRaster;
  pipelines.bimGBufferNoCull = pipelineManager_.createGraphicsPipeline(
      bimGBufNoCullPCI, "bim_gbuffer_no_cull_pipeline");

  // Shadow depth
  VkPipelineRasterizationStateCreateInfo shadowRaster = sceneRaster;
  // Shadow cascades render with a positive-height viewport, so they preserve
  // glTF's native CCW winding in framebuffer space.
  shadowRaster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  // Keep shadow caster winding policy aligned with the single-sided scene path.
  shadowRaster.cullMode = VK_CULL_MODE_BACK_BIT;
  shadowRaster.depthBiasEnable = VK_TRUE;
  shadowRaster.depthBiasConstantFactor = 0.0f;
  shadowRaster.depthBiasClamp = 0.0f;
  shadowRaster.depthBiasSlopeFactor = 0.0f;

  RenderingGraphicsPipelineCreateInfo sdPCI = scenePCI;
  sdPCI.stageCount = static_cast<uint32_t>(sdStages.size());
  sdPCI.pStages = sdStages.data();
  sdPCI.pVertexInputState = &posTexNormInput;
  sdPCI.pInputAssemblyState = &triAssembly;
  sdPCI.pRasterizationState = &shadowRaster;
  sdPCI.pMultisampleState = &singleSample;
  sdPCI.pDepthStencilState = &depthPrepassDS;
  sdPCI.pColorBlendState = &noBlend;
  sdPCI.pDynamicState = &shadowDynState;
  sdPCI.layout = layouts.shadow;
  sdPCI.renderPass = renderPasses.shadow;
  pipelines.shadowDepth =
      pipelineManager_.createGraphicsPipeline(sdPCI, "shadow_depth_pipeline");

  VkPipelineRasterizationStateCreateInfo shadowFrontCullRaster = shadowRaster;
  shadowFrontCullRaster.cullMode = VK_CULL_MODE_FRONT_BIT;

  RenderingGraphicsPipelineCreateInfo sdFrontCullPCI = sdPCI;
  sdFrontCullPCI.pRasterizationState = &shadowFrontCullRaster;
  pipelines.shadowDepthFrontCull = pipelineManager_.createGraphicsPipeline(
      sdFrontCullPCI, "shadow_depth_front_cull_pipeline");

  VkPipelineRasterizationStateCreateInfo shadowNoCullRaster = shadowRaster;
  shadowNoCullRaster.cullMode = VK_CULL_MODE_NONE;

  RenderingGraphicsPipelineCreateInfo sdNoCullPCI = sdPCI;
  sdNoCullPCI.pRasterizationState = &shadowNoCullRaster;
  pipelines.shadowDepthNoCull = pipelineManager_.createGraphicsPipeline(
      sdNoCullPCI, "shadow_depth_no_cull_pipeline");

  RenderingGraphicsPipelineCreateInfo localSdPCI = sdPCI;
  localSdPCI.stageCount = static_cast<uint32_t>(lsdStages.size());
  localSdPCI.pStages = lsdStages.data();
  pipelines.localShadowDepth = pipelineManager_.createGraphicsPipeline(
      localSdPCI, "local_shadow_depth_pipeline");

  RenderingGraphicsPipelineCreateInfo localSdFrontCullPCI = localSdPCI;
  localSdFrontCullPCI.pRasterizationState = &shadowFrontCullRaster;
  pipelines.localShadowDepthFrontCull = pipelineManager_.createGraphicsPipeline(
      localSdFrontCullPCI, "local_shadow_depth_front_cull_pipeline");

  RenderingGraphicsPipelineCreateInfo localSdNoCullPCI = localSdPCI;
  localSdNoCullPCI.pRasterizationState = &shadowNoCullRaster;
  pipelines.localShadowDepthNoCull = pipelineManager_.createGraphicsPipeline(
      localSdNoCullPCI, "local_shadow_depth_no_cull_pipeline");

  // Directional light
  fsPCI.stageCount = static_cast<uint32_t>(dirStages.size());
  fsPCI.pStages = dirStages.data();
  pipelines.directionalLight = pipelineManager_.createGraphicsPipeline(
      fsPCI, "directional_light_pipeline");

  // Stencil volume
  RenderingGraphicsPipelineCreateInfo stencilPCI = fsPCI;
  stencilPCI.stageCount = static_cast<uint32_t>(stencilStages.size());
  stencilPCI.pStages = stencilStages.data();
  stencilPCI.pDepthStencilState = &stencilDS;
  stencilPCI.pColorBlendState = &noColorBlend;
  pipelines.stencilVolume = pipelineManager_.createGraphicsPipeline(
      stencilPCI, "stencil_volume_pipeline");

  // Point light
  RenderingGraphicsPipelineCreateInfo pointPCI = fsPCI;
  pointPCI.stageCount = static_cast<uint32_t>(pointStages.size());
  pointPCI.pStages = pointStages.data();
  pointPCI.pDepthStencilState = &pointLightDS;
  pointPCI.pColorBlendState = &addBlend;
  pipelines.pointLight =
      pipelineManager_.createGraphicsPipeline(pointPCI, "point_light_pipeline");

  // Point light stencil debug
  RenderingGraphicsPipelineCreateInfo pointDbgPCI = pointPCI;
  pointDbgPCI.stageCount = static_cast<uint32_t>(pointDbgStages.size());
  pointDbgPCI.pStages = pointDbgStages.data();
  pointDbgPCI.pColorBlendState = &opaqueBlend;
  pipelines.pointLightStencilDebug = pipelineManager_.createGraphicsPipeline(
      pointDbgPCI, "point_light_stencil_debug_pipeline");

  // Forward opaque
  RenderingGraphicsPipelineCreateInfo forwardOpaquePCI = meshPCI;
  // Compare against the original prepass samples, then resolve shaded color.
  forwardOpaquePCI.renderPass = renderPasses.forwardLighting;
  forwardOpaquePCI.pMultisampleState = &sceneMsaa;
  forwardOpaquePCI.stageCount =
      static_cast<uint32_t>(forwardOpaqueStages.size());
  forwardOpaquePCI.pStages = forwardOpaqueStages.data();
  forwardOpaquePCI.pDepthStencilState = &transparentDS;
  forwardOpaquePCI.pColorBlendState = &opaqueBlend;
  const VkPipeline forwardOpaquePipeline =
      pipelineManager_.createGraphicsPipeline(forwardOpaquePCI,
                                              "forward_opaque_pipeline");
  pipelines.extraHandles.push_back(RegisteredPipelineHandle{
      .key = {RenderTechniqueId::ForwardRaster, "forward-opaque"},
      .pipeline = forwardOpaquePipeline});

  // Lighting must use the same culling routes as the depth prepass. Otherwise
  // a visible back face writes depth but leaves the cleared HDR color behind.
  RenderingGraphicsPipelineCreateInfo forwardOpaqueFrontCullPCI =
      forwardOpaquePCI;
  forwardOpaqueFrontCullPCI.pRasterizationState = &frontCullRaster;
  pipelines.extraHandles.push_back(RegisteredPipelineHandle{
      .key = {RenderTechniqueId::ForwardRaster, "forward-opaque-front-cull"},
      .pipeline = pipelineManager_.createGraphicsPipeline(
          forwardOpaqueFrontCullPCI, "forward_opaque_front_cull_pipeline")});

  RenderingGraphicsPipelineCreateInfo forwardOpaqueNoCullPCI = forwardOpaquePCI;
  forwardOpaqueNoCullPCI.pRasterizationState = &noCullRaster;
  pipelines.extraHandles.push_back(RegisteredPipelineHandle{
      .key = {RenderTechniqueId::ForwardRaster, "forward-opaque-no-cull"},
      .pipeline = pipelineManager_.createGraphicsPipeline(
          forwardOpaqueNoCullPCI, "forward_opaque_no_cull_pipeline")});

  // Fill only clear depth samples before the opaque color resolve. Using the
  // MSAA prepass depth avoids painting sky over partially covered silhouettes.
  VkPipelineDepthStencilStateCreateInfo skyDS = transparentDS;
  skyDS.depthTestEnable = VK_TRUE;
  skyDS.depthWriteEnable = VK_FALSE;
  skyDS.depthCompareOp = VK_COMPARE_OP_EQUAL;
  skyDS.stencilTestEnable = VK_FALSE;
  RenderingGraphicsPipelineCreateInfo skyPCI = fsPCI;
  skyPCI.renderPass = renderPasses.forwardLighting;
  skyPCI.layout = layouts.transparent;
  skyPCI.pMultisampleState = &sceneMsaa;
  skyPCI.pDepthStencilState = &skyDS;
  skyPCI.stageCount = static_cast<uint32_t>(skyStages.size());
  skyPCI.pStages = skyStages.data();
  pipelines.extraHandles.push_back(RegisteredPipelineHandle{
      .key = {RenderTechniqueId::ForwardRaster, "forward-sky"},
      .pipeline = pipelineManager_.createGraphicsPipeline(
          skyPCI, "forward_sky_pipeline")});

  // Transparent (OIT)
  pipelines.transparent =
      pipelineManager_.createGraphicsPipeline(meshPCI, "transparent_pipeline");

  RenderingGraphicsPipelineCreateInfo transparentFrontCullPCI = meshPCI;
  transparentFrontCullPCI.pRasterizationState = &frontCullRaster;
  pipelines.transparentFrontCull = pipelineManager_.createGraphicsPipeline(
      transparentFrontCullPCI, "transparent_front_cull_pipeline");

  RenderingGraphicsPipelineCreateInfo transparentNoCullPCI = meshPCI;
  transparentNoCullPCI.pRasterizationState = &noCullRaster;
  pipelines.transparentNoCull = pipelineManager_.createGraphicsPipeline(
      transparentNoCullPCI, "transparent_no_cull_pipeline");

  // Transparent picking
  pipelines.transparentPick = pipelineManager_.createGraphicsPipeline(
      transparentPickPCI, "transparent_pick_pipeline");

  RenderingGraphicsPipelineCreateInfo transparentPickFrontCullPCI = transparentPickPCI;
  transparentPickFrontCullPCI.pRasterizationState = &frontCullRaster;
  pipelines.transparentPickFrontCull = pipelineManager_.createGraphicsPipeline(
      transparentPickFrontCullPCI, "transparent_pick_front_cull_pipeline");

  RenderingGraphicsPipelineCreateInfo transparentPickNoCullPCI = transparentPickPCI;
  transparentPickNoCullPCI.pRasterizationState = &noCullRaster;
  pipelines.transparentPickNoCull = pipelineManager_.createGraphicsPipeline(
      transparentPickNoCullPCI, "transparent_pick_no_cull_pipeline");

  // Post process
  RenderingGraphicsPipelineCreateInfo postPCI = fsPCI;
  postPCI.stageCount = static_cast<uint32_t>(postStages.size());
  postPCI.pStages = postStages.data();
  postPCI.layout = layouts.postProcess;
  postPCI.renderPass = renderPasses.postProcess;
  pipelines.postProcess =
      pipelineManager_.createGraphicsPipeline(postPCI, "post_process_pipeline");

  // Geometry debug
  RenderingGraphicsPipelineCreateInfo dbgPCI = meshPCI;
  dbgPCI.stageCount = static_cast<uint32_t>(dbgStages.size());
  dbgPCI.pStages = dbgStages.data();
  dbgPCI.pVertexInputState = &posOnlyInput;
  dbgPCI.pInputAssemblyState = &pointAssembly;
  dbgPCI.pColorBlendState = &overlayBlend;
  dbgPCI.pDepthStencilState = &noDS;
  dbgPCI.layout = layouts.scene;
  pipelines.geometryDebug = pipelineManager_.createGraphicsPipeline(
      dbgPCI, "geometry_debug_pipeline");

  // Normal validation
  RenderingGraphicsPipelineCreateInfo nvPCI = meshPCI;
  nvPCI.stageCount = static_cast<uint32_t>(nvStages.size());
  nvPCI.pStages = nvStages.data();
  nvPCI.pVertexInputState = &posTexNormNoTex1Input;
  nvPCI.pInputAssemblyState = &triAssembly;
  nvPCI.pColorBlendState = &overlayBlend;
  nvPCI.pDepthStencilState = &normalLineDS;
  nvPCI.layout = layouts.normalValidation;
  nvPCI.renderPass = renderPasses.lighting;
  pipelines.normalValidation = pipelineManager_.createGraphicsPipeline(
      nvPCI, "normal_validation_pipeline");

  RenderingGraphicsPipelineCreateInfo nvFrontCullPCI = nvPCI;
  nvFrontCullPCI.pRasterizationState = &frontCullRaster;
  pipelines.normalValidationFrontCull = pipelineManager_.createGraphicsPipeline(
      nvFrontCullPCI, "normal_validation_front_cull_pipeline");

  RenderingGraphicsPipelineCreateInfo nvNoCullPCI = nvPCI;
  nvNoCullPCI.pRasterizationState = &noCullRaster;
  pipelines.normalValidationNoCull = pipelineManager_.createGraphicsPipeline(
      nvNoCullPCI, "normal_validation_no_cull_pipeline");

  // Wireframe depth + wireframe no-depth
  const bool useNativeWireframe =
      device_->enabledFeatures().fillModeNonSolid == VK_TRUE;

  RenderingGraphicsPipelineCreateInfo wfPCI = meshPCI;
  wfPCI.pVertexInputState = &posTexNormNoTex1Input;
  wfPCI.pInputAssemblyState = &triAssembly;
  wfPCI.pColorBlendState = &overlayBlend;
  wfPCI.layout = layouts.wireframe;
  wfPCI.renderPass = renderPasses.lighting;
  if (useNativeWireframe) {
    wfPCI.stageCount = static_cast<uint32_t>(wfStages.size());
    wfPCI.pStages = wfStages.data();
    wfPCI.pRasterizationState = &wfDepthRaster;
    wfPCI.pDynamicState = &lineDynState;
  } else {
    wfPCI.stageCount = static_cast<uint32_t>(wfFbStages.size());
    wfPCI.pStages = wfFbStages.data();
    wfPCI.pRasterizationState = &wfFbDepthRaster;
    wfPCI.pDynamicState = &dynState;
  }
  wfPCI.pDepthStencilState = &wfDepthDS;
  pipelines.wireframeDepth = pipelineManager_.createGraphicsPipeline(
      wfPCI, "wireframe_depth_pipeline");

  if (useNativeWireframe) {
    wfPCI.pRasterizationState = &wfDepthFrontCullRaster;
  } else {
    wfPCI.pRasterizationState = &wfFbDepthFrontCullRaster;
  }
  pipelines.wireframeDepthFrontCull = pipelineManager_.createGraphicsPipeline(
      wfPCI, "wireframe_depth_front_cull_pipeline");

  if (useNativeWireframe) {
    wfPCI.pRasterizationState = &wfRaster;
  } else {
    wfPCI.pRasterizationState = &wfFbRaster;
  }

  wfPCI.pDepthStencilState = &wfNoDepthDS;
  pipelines.wireframeNoDepth = pipelineManager_.createGraphicsPipeline(
      wfPCI, "wireframe_no_depth_pipeline");

  if (useNativeWireframe) {
    wfPCI.pRasterizationState = &wfFrontCullRaster;
  } else {
    wfPCI.pRasterizationState = &wfFbFrontCullRaster;
  }
  pipelines.wireframeNoDepthFrontCull = pipelineManager_.createGraphicsPipeline(
      wfPCI, "wireframe_no_depth_front_cull_pipeline");

  RenderingGraphicsPipelineCreateInfo selectionMaskPCI = meshPCI;
  selectionMaskPCI.stageCount = static_cast<uint32_t>(wfStages.size());
  selectionMaskPCI.pStages = wfStages.data();
  selectionMaskPCI.pVertexInputState = &posTexNormNoTex1Input;
  selectionMaskPCI.pInputAssemblyState = &triAssembly;
  selectionMaskPCI.pRasterizationState = &noCullRaster;
  selectionMaskPCI.pColorBlendState = &noColorBlend;
  selectionMaskPCI.pDepthStencilState = &selectionMaskDS;
  selectionMaskPCI.pDynamicState = &dynState;
  selectionMaskPCI.layout = layouts.wireframe;
  selectionMaskPCI.renderPass = renderPasses.lighting;
  pipelines.selectionMask = pipelineManager_.createGraphicsPipeline(
      selectionMaskPCI, "selection_mask_pipeline");

  RenderingGraphicsPipelineCreateInfo selectionOutlinePCI = meshPCI;
  selectionOutlinePCI.stageCount =
      static_cast<uint32_t>(selOutlineStages.size());
  selectionOutlinePCI.pStages = selOutlineStages.data();
  selectionOutlinePCI.pVertexInputState = &posTexNormNoTex1Input;
  selectionOutlinePCI.pInputAssemblyState = &triAssembly;
  selectionOutlinePCI.pRasterizationState = &noCullRaster;
  selectionOutlinePCI.pColorBlendState = &overlayBlend;
  selectionOutlinePCI.pDepthStencilState = &selectionOutlineDS;
  selectionOutlinePCI.pDynamicState = &dynState;
  selectionOutlinePCI.layout = layouts.wireframe;
  selectionOutlinePCI.renderPass = renderPasses.lighting;
  pipelines.selectionOutline = pipelineManager_.createGraphicsPipeline(
      selectionOutlinePCI, "selection_outline_pipeline");

  RenderingGraphicsPipelineCreateInfo floorPlanPCI = wfPCI;
  floorPlanPCI.stageCount = static_cast<uint32_t>(wfStages.size());
  floorPlanPCI.pStages = wfStages.data();
  floorPlanPCI.pInputAssemblyState = &lineAssembly;
  floorPlanPCI.pRasterizationState = &normalLineRaster;
  floorPlanPCI.pColorBlendState = &overlayBlend;
  floorPlanPCI.pDynamicState = &lineDynState;
  floorPlanPCI.layout = layouts.wireframe;
  floorPlanPCI.renderPass = renderPasses.lighting;
  floorPlanPCI.pDepthStencilState = &wfDepthDS;
  pipelines.bimFloorPlanDepth = pipelineManager_.createGraphicsPipeline(
      floorPlanPCI, "bim_floor_plan_depth_pipeline");

  floorPlanPCI.pDepthStencilState = &wfNoDepthDS;
  pipelines.bimFloorPlanNoDepth = pipelineManager_.createGraphicsPipeline(
      floorPlanPCI, "bim_floor_plan_no_depth_pipeline");

  RenderingGraphicsPipelineCreateInfo pointCloudPCI = meshPCI;
  pointCloudPCI.stageCount = static_cast<uint32_t>(wfStages.size());
  pointCloudPCI.pStages = wfStages.data();
  pointCloudPCI.pVertexInputState = &posTexNormNoTex1Input;
  pointCloudPCI.pInputAssemblyState = &pointAssembly;
  pointCloudPCI.pRasterizationState = &noCullRaster;
  pointCloudPCI.pColorBlendState = &overlayBlend;
  pointCloudPCI.pDynamicState = &dynState;
  pointCloudPCI.layout = layouts.wireframe;
  pointCloudPCI.renderPass = renderPasses.lighting;
  pointCloudPCI.pDepthStencilState = &wfDepthDS;
  pipelines.bimPointCloudDepth = pipelineManager_.createGraphicsPipeline(
      pointCloudPCI, "bim_point_cloud_depth_pipeline");

  pointCloudPCI.pDepthStencilState = &wfNoDepthDS;
  pipelines.bimPointCloudNoDepth = pipelineManager_.createGraphicsPipeline(
      pointCloudPCI, "bim_point_cloud_no_depth_pipeline");

  RenderingGraphicsPipelineCreateInfo curvePCI = pointCloudPCI;
  curvePCI.pInputAssemblyState = &lineAssembly;
  curvePCI.pRasterizationState = &normalLineRaster;
  curvePCI.pDynamicState = &lineDynState;
  curvePCI.pDepthStencilState = &wfDepthDS;
  pipelines.bimCurveDepth = pipelineManager_.createGraphicsPipeline(
      curvePCI, "bim_curve_depth_pipeline");

  curvePCI.pDepthStencilState = &wfNoDepthDS;
  pipelines.bimCurveNoDepth = pipelineManager_.createGraphicsPipeline(
      curvePCI, "bim_curve_no_depth_pipeline");

  RenderingGraphicsPipelineCreateInfo capFillPCI = pointCloudPCI;
  capFillPCI.pInputAssemblyState = &triAssembly;
  capFillPCI.pRasterizationState = &noCullRaster;
  capFillPCI.pDynamicState = &dynState;
  capFillPCI.pDepthStencilState = &wfDepthDS;
  pipelines.bimSectionClipCapFill = pipelineManager_.createGraphicsPipeline(
      capFillPCI, "bim_section_clip_cap_fill_pipeline");

  RenderingGraphicsPipelineCreateInfo capHatchPCI = capFillPCI;
  capHatchPCI.pInputAssemblyState = &lineAssembly;
  capHatchPCI.pRasterizationState = &normalLineRaster;
  capHatchPCI.pDynamicState = &lineDynState;
  pipelines.bimSectionClipCapHatch = pipelineManager_.createGraphicsPipeline(
      capHatchPCI, "bim_section_clip_cap_hatch_pipeline");

  // Surface normal lines
  RenderingGraphicsPipelineCreateInfo snPCI = meshPCI;
  snPCI.stageCount = static_cast<uint32_t>(snStages.size());
  snPCI.pStages = snStages.data();
  snPCI.pVertexInputState = &posTexNormNoTex1Input;
  snPCI.pInputAssemblyState = &triAssembly;
  snPCI.pRasterizationState = &normalLineRaster;
  snPCI.pColorBlendState = &overlayBlend;
  snPCI.pDepthStencilState = &normalLineDS;
  snPCI.pDynamicState = &dynState;
  snPCI.layout = layouts.surfaceNormal;
  snPCI.renderPass = renderPasses.lighting;
  pipelines.surfaceNormalLine = pipelineManager_.createGraphicsPipeline(
      snPCI, "surface_normal_line_pipeline");

  // Object normals debug
  RenderingGraphicsPipelineCreateInfo onPCI = meshPCI;
  onPCI.stageCount = static_cast<uint32_t>(onStages.size());
  onPCI.pStages = onStages.data();
  onPCI.pVertexInputState = &posTexNormNoTex1Input;
  onPCI.pColorBlendState = &overlayBlend;
  onPCI.pDepthStencilState = &normalLineDS;
  onPCI.layout = layouts.scene;
  pipelines.objectNormalDebug = pipelineManager_.createGraphicsPipeline(
      onPCI, "object_normal_debug_pipeline");

  RenderingGraphicsPipelineCreateInfo onFrontCullPCI = onPCI;
  onFrontCullPCI.pRasterizationState = &frontCullRaster;
  pipelines.objectNormalDebugFrontCull =
      pipelineManager_.createGraphicsPipeline(
          onFrontCullPCI, "object_normal_debug_front_cull_pipeline");

  RenderingGraphicsPipelineCreateInfo onNoCullPCI = onPCI;
  onNoCullPCI.pRasterizationState = &noCullRaster;
  pipelines.objectNormalDebugNoCull = pipelineManager_.createGraphicsPipeline(
      onNoCullPCI, "object_normal_debug_no_cull_pipeline");

  // Light gizmo
  RenderingGraphicsPipelineCreateInfo lgPCI = fsPCI;
  lgPCI.stageCount = static_cast<uint32_t>(lgStages.size());
  lgPCI.pStages = lgStages.data();
  lgPCI.pVertexInputState = &emptyVertexInput;
  lgPCI.pInputAssemblyState = &triAssembly;
  lgPCI.pColorBlendState = &overlayBlend;
  lgPCI.pDepthStencilState = &noDS;
  lgPCI.layout = layouts.lightGizmo;
  lgPCI.renderPass = renderPasses.postProcess;
  pipelines.lightGizmo =
      pipelineManager_.createGraphicsPipeline(lgPCI, "light_gizmo_pipeline");

  RenderingGraphicsPipelineCreateInfo lgCoveragePCI = lgPCI;
  lgCoveragePCI.pInputAssemblyState = &lineAssembly;
  pipelines.lightGizmoCoverage = pipelineManager_.createGraphicsPipeline(
      lgCoveragePCI, "light_gizmo_coverage_pipeline");

  RenderingGraphicsPipelineCreateInfo lgPickPCI = lgPCI;
  lgPickPCI.stageCount = static_cast<uint32_t>(lgPickStages.size());
  lgPickPCI.pStages = lgPickStages.data();
  lgPickPCI.pColorBlendState = &opaqueBlend;
  lgPickPCI.renderPass = renderPasses.transparentPick;
  pipelines.lightGizmoPick = pipelineManager_.createGraphicsPipeline(
      lgPickPCI, "light_gizmo_pick_pipeline");

  // Transform gizmo
  RenderingGraphicsPipelineCreateInfo tgPCI = fsPCI;
  tgPCI.stageCount = static_cast<uint32_t>(tgStages.size());
  tgPCI.pStages = tgStages.data();
  tgPCI.pVertexInputState = &emptyVertexInput;
  tgPCI.pInputAssemblyState = &lineAssembly;
  tgPCI.pColorBlendState = &overlayBlend;
  tgPCI.pDepthStencilState = &noDS;
  tgPCI.pDynamicState = &lineDynState;
  tgPCI.layout = layouts.transformGizmo;
  tgPCI.renderPass = renderPasses.transformGizmos;
  pipelines.transformGizmo = pipelineManager_.createGraphicsPipeline(
      tgPCI, "transform_gizmo_pipeline");
  RenderingGraphicsPipelineCreateInfo tgSolidPCI = tgPCI;
  tgSolidPCI.pInputAssemblyState = &triAssembly;
  tgSolidPCI.pDynamicState = &dynState;
  pipelines.transformGizmoSolid = pipelineManager_.createGraphicsPipeline(
      tgSolidPCI, "transform_gizmo_solid_pipeline");
  RenderingGraphicsPipelineCreateInfo tgOverlayPCI = tgPCI;
  tgOverlayPCI.renderPass = renderPasses.postProcess;
  pipelines.transformGizmoOverlay = pipelineManager_.createGraphicsPipeline(
      tgOverlayPCI, "transform_gizmo_overlay_pipeline");
  RenderingGraphicsPipelineCreateInfo tgSolidOverlayPCI = tgSolidPCI;
  tgSolidOverlayPCI.renderPass = renderPasses.postProcess;
  pipelines.transformGizmoSolidOverlay =
      pipelineManager_.createGraphicsPipeline(
          tgSolidOverlayPCI, "transform_gizmo_solid_overlay_pipeline");

  // Tiled point light (fullscreen, additive blend, no stencil)
  RenderingGraphicsPipelineCreateInfo tlPCI = fsPCI;
  tlPCI.stageCount = static_cast<uint32_t>(tlStages.size());
  tlPCI.pStages = tlStages.data();
  tlPCI.pVertexInputState = &emptyVertexInput;
  tlPCI.pColorBlendState = &addBlend;
  tlPCI.pDepthStencilState = &noDS;
  tlPCI.layout = layouts.tiledLighting;
  tlPCI.renderPass = renderPasses.lighting;
  pipelines.tiledPointLight = pipelineManager_.createGraphicsPipeline(
      tlPCI, "tiled_point_light_pipeline");

  pipelines.handleRegistry = buildGraphicsPipelineHandleRegistry(pipelines);

  return {layouts, pipelines};
}

} // namespace container::renderer
