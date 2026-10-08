# cmake/Assets.cmake
# Model generation and MaterialX asset copy targets.

find_package(Python3 COMPONENTS Interpreter REQUIRED)

# --- MaterialX materials ------------------------------------------------

add_custom_target(copy_materials ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/materials
            ${CMAKE_BINARY_DIR}/materials
    COMMENT "Copying MaterialX assets to build directory"
)

# --- glTF model generation ----------------------------------------------

set(MODELS_DIR "${CMAKE_SOURCE_DIR}/models")
set(MODELS_OUTPUT_DIR "${CMAKE_BINARY_DIR}/models")

add_custom_command(
    OUTPUT
        "${MODELS_OUTPUT_DIR}/basic_cube.gltf"
        "${MODELS_OUTPUT_DIR}/cube_baseColor.png"
        "${MODELS_OUTPUT_DIR}/cube_emissive.png"
        "${MODELS_OUTPUT_DIR}/cube_metallicRoughness.png"
        "${MODELS_OUTPUT_DIR}/cube_normal.png"
        "${MODELS_OUTPUT_DIR}/cube_occlusion.png"
        "${MODELS_OUTPUT_DIR}/basic_triangle.gltf"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MODELS_OUTPUT_DIR}"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/cmake/generate_pbr_cube_assets.py"
            --output-dir "${MODELS_OUTPUT_DIR}"
            --static-model "${MODELS_DIR}/basic_triangle.gltf"
    DEPENDS
        "${CMAKE_SOURCE_DIR}/cmake/generate_pbr_cube_assets.py"
        "${MODELS_DIR}/basic_triangle.gltf"
    COMMENT "Generating glTF assets with Python"
    VERBATIM
)

set(GENERATE_MODEL_OUTPUTS
    "${MODELS_OUTPUT_DIR}/basic_cube.gltf"
    "${MODELS_OUTPUT_DIR}/cube_baseColor.png"
    "${MODELS_OUTPUT_DIR}/cube_emissive.png"
    "${MODELS_OUTPUT_DIR}/cube_metallicRoughness.png"
    "${MODELS_OUTPUT_DIR}/cube_normal.png"
    "${MODELS_OUTPUT_DIR}/cube_occlusion.png"
    "${MODELS_OUTPUT_DIR}/basic_triangle.gltf"
)

# --- glTF Sample Models download ----------------------------------------

if(ENABLE_SAMPLE_MODEL_DOWNLOAD)
    set(GLTF_SAMPLE_MODELS_DIR "${MODELS_OUTPUT_DIR}/glTF-Sample-Models")
    set(GLTF_SAMPLE_MODELS_STAMP "${GLTF_SAMPLE_MODELS_DIR}/.fetched")
    set(GLTF_SAMPLE_MODELS_ARCHIVE_URL
        "https://github.com/KhronosGroup/glTF-Sample-Models/archive/d7a3cc8e51d7c573771ae77a57f16b0662a905c6.zip")

    add_custom_command(
        OUTPUT "${GLTF_SAMPLE_MODELS_STAMP}"
        COMMAND ${CMAKE_COMMAND}
                "-DDESTINATION:PATH=${GLTF_SAMPLE_MODELS_DIR}"
                "-DSTAMP_FILE:FILEPATH=${GLTF_SAMPLE_MODELS_STAMP}"
                "-DREPO_ARCHIVE_URL:STRING=${GLTF_SAMPLE_MODELS_ARCHIVE_URL}"
                "-DGLTF_SAMPLE_MODELS_DOWNLOAD_TIMEOUT_SECONDS:STRING=${GLTF_SAMPLE_MODELS_DOWNLOAD_TIMEOUT_SECONDS}"
                "-DGLTF_SAMPLE_MODELS_INACTIVITY_TIMEOUT_SECONDS:STRING=${GLTF_SAMPLE_MODELS_INACTIVITY_TIMEOUT_SECONDS}"
                -P "${CMAKE_SOURCE_DIR}/cmake/fetch_gltf_sample_models.cmake"
        DEPENDS "${CMAKE_SOURCE_DIR}/cmake/fetch_gltf_sample_models.cmake"
        COMMENT "Downloading pinned glTF Sample Models archive"
        VERBATIM
    )

    list(APPEND GENERATE_MODEL_OUTPUTS "${GLTF_SAMPLE_MODELS_STAMP}")
endif()

add_custom_target(generate_models ALL
    DEPENDS ${GENERATE_MODEL_OUTPUTS}
)

# --- local validation models -------------------------------------------

if(EXISTS "${MODELS_DIR}/validation")
    add_custom_target(copy_validation_models ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${MODELS_DIR}/validation"
                "${MODELS_OUTPUT_DIR}/validation"
        COMMENT "Copying validation glTF assets to build directory"
    )

    add_dependencies(generate_models copy_validation_models)
endif()

# --- buildingSMART IFC5 sample files download ----------------------------

if(ENABLE_BIM_SAMPLE_MODEL_DOWNLOAD)
    set(BIM_SAMPLE_MODELS_DIR "${MODELS_OUTPUT_DIR}/buildingSMART-IFC5-development")
    set(BIM_SAMPLE_MODELS_STAMP "${BIM_SAMPLE_MODELS_DIR}/.fetched")
    set(BIM_SAMPLE_MODELS_REF_URL
        "https://api.github.com/repos/buildingSMART/IFC5-development/git/ref/heads/main")
    set(IFC_SAMPLE_MODELS_DIR "${MODELS_OUTPUT_DIR}/buildingSMART-Sample-Test-Files")
    set(IFC_SAMPLE_MODELS_STAMP "${IFC_SAMPLE_MODELS_DIR}/.fetched")
    set(IFC_SAMPLE_MODELS_REF_URL
        "https://api.github.com/repos/buildingsmart-community/Community-Sample-Test-Files/git/ref/heads/main")

    add_custom_target(fetch_bim_sample_models
        COMMAND ${CMAKE_COMMAND}
                "-DDESTINATION:PATH=${BIM_SAMPLE_MODELS_DIR}"
                "-DSTAMP_FILE:FILEPATH=${BIM_SAMPLE_MODELS_STAMP}"
                "-DREPO_REF_URL:STRING=${BIM_SAMPLE_MODELS_REF_URL}"
                -P "${CMAKE_SOURCE_DIR}/cmake/fetch_buildingsmart_ifc5_models.cmake"
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/cmake/materialize_lfs_models.py"
                --root "${BIM_SAMPLE_MODELS_DIR}" --repository buildingSMART/IFC5-development
        DEPENDS "${CMAKE_SOURCE_DIR}/cmake/fetch_buildingsmart_ifc5_models.cmake"
                "${CMAKE_SOURCE_DIR}/cmake/materialize_lfs_models.py"
        COMMENT "Checking latest buildingSMART IFC5-development archive"
        VERBATIM
    )

    add_custom_target(fetch_ifc_sample_models
        COMMAND ${CMAKE_COMMAND}
                "-DDESTINATION:PATH=${IFC_SAMPLE_MODELS_DIR}"
                "-DSTAMP_FILE:FILEPATH=${IFC_SAMPLE_MODELS_STAMP}"
                "-DREPO_REF_URL:STRING=${IFC_SAMPLE_MODELS_REF_URL}"
                -P "${CMAKE_SOURCE_DIR}/cmake/fetch_buildingsmart_sample_test_files.cmake"
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/cmake/materialize_lfs_models.py"
                --root "${IFC_SAMPLE_MODELS_DIR}" --repository buildingsmart-community/Community-Sample-Test-Files
        DEPENDS "${CMAKE_SOURCE_DIR}/cmake/fetch_buildingsmart_sample_test_files.cmake"
                "${CMAKE_SOURCE_DIR}/cmake/materialize_lfs_models.py"
        COMMENT "Checking latest buildingSMART Sample-Test-Files archive"
        VERBATIM
    )

    add_dependencies(generate_models fetch_bim_sample_models)
    add_dependencies(generate_models fetch_ifc_sample_models)
endif()

# --- OpenUSD sample model downloads -------------------------------------

if(ENABLE_USD_SAMPLE_MODEL_DOWNLOAD)
    set(USD_SAMPLE_MODELS_DIR "${MODELS_OUTPUT_DIR}/OpenUSD-Sample-Assets")
    set(USD_SAMPLE_MODELS_STAMP "${USD_SAMPLE_MODELS_DIR}/.fetched")
    set(USD_KITCHEN_SET_URL "https://openusd.org/files/Kitchen_set.zip?")
    set(USD_POINT_INSTANCED_MEDCITY_URL
        "https://openusd.org/files/PointInstancedMedCity.zip?")

    add_custom_target(fetch_usd_sample_models
        COMMAND ${CMAKE_COMMAND}
                "-DDESTINATION:PATH=${USD_SAMPLE_MODELS_DIR}"
                "-DSTAMP_FILE:FILEPATH=${USD_SAMPLE_MODELS_STAMP}"
                "-DKITCHEN_SET_URL:STRING=${USD_KITCHEN_SET_URL}"
                "-DMEDCITY_URL:STRING=${USD_POINT_INSTANCED_MEDCITY_URL}"
                -P "${CMAKE_SOURCE_DIR}/cmake/fetch_openusd_sample_models.cmake"
        DEPENDS "${CMAKE_SOURCE_DIR}/cmake/fetch_openusd_sample_models.cmake"
        COMMENT "Checking OpenUSD sample model archives"
        VERBATIM
    )

    add_custom_target(download_usd_models
        DEPENDS fetch_usd_sample_models
        COMMENT "Downloading OpenUSD sample model archives"
    )

    add_dependencies(generate_models fetch_usd_sample_models)
endif()

# --- HDR environment maps -----------------------------------------------

if(EXISTS "${CMAKE_SOURCE_DIR}/hdr")
    add_custom_target(copy_hdr ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_SOURCE_DIR}/hdr
                ${CMAKE_BINARY_DIR}/hdr
        COMMENT "Copying HDR environment maps to build directory"
    )
endif()
