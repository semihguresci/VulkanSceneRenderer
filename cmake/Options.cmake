option(ENABLE_TESTS "Enable building and running tests" ON)
option(ENABLE_BIM_FRAME_CACHE_BENCHMARK "Build the opt-in BIM CPU cache benchmark" OFF)
set(CONTAINER_BIM_CACHE_BASELINE_INCLUDE_DIR "" CACHE PATH
    "Optional captured provider headers for a before/after BIM cache benchmark")
option(ENABLE_WINDOWED_TESTS "Build tests that require a Vulkan runtime and display" OFF)
option(ENABLE_VULKAN_VALIDATION_LAYERS "Enable Vulkan Validation Layers" ON)
option(ENABLE_SAMPLE_MODEL_DOWNLOAD "Download pinned glTF Sample Models during asset generation" ON)
set(GLTF_SAMPLE_MODELS_DOWNLOAD_TIMEOUT_SECONDS "900" CACHE STRING
    "Total timeout, in seconds, for downloading the pinned glTF Sample Models archive")
set(GLTF_SAMPLE_MODELS_INACTIVITY_TIMEOUT_SECONDS "60" CACHE STRING
    "Inactivity timeout, in seconds, for downloading the pinned glTF Sample Models archive")
option(ENABLE_BIM_SAMPLE_MODEL_DOWNLOAD "Download latest buildingSMART IFC5-development files during asset generation" ON)
option(ENABLE_USD_SAMPLE_MODEL_DOWNLOAD "Download OpenUSD sample model archives during asset generation" ON)
option(ENABLE_TINYUSDZ_USD_LOADER "Build the TinyUSDZ-backed USD/USDZ importer" ON)
