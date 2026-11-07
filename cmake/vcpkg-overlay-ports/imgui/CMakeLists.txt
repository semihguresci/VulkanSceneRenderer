cmake_minimum_required(VERSION 3.23)
project(imgui VERSION 1.93.0 LANGUAGES CXX)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(glfw3 3.5.1 EXACT CONFIG REQUIRED)
find_package(Vulkan 1.4 REQUIRED)
if(NOT IMGUI_SOURCE_DIR)
  message(FATAL_ERROR "IMGUI_SOURCE_DIR must identify the pinned official source")
endif()
file(TO_CMAKE_PATH "${IMGUI_SOURCE_DIR}" IMGUI_SOURCE_DIR)
add_library(imgui STATIC
  "${IMGUI_SOURCE_DIR}/imgui.cpp" "${IMGUI_SOURCE_DIR}/imgui_demo.cpp"
  "${IMGUI_SOURCE_DIR}/imgui_draw.cpp" "${IMGUI_SOURCE_DIR}/imgui_tables.cpp"
  "${IMGUI_SOURCE_DIR}/imgui_widgets.cpp" "${IMGUI_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp"
  "${IMGUI_SOURCE_DIR}/backends/imgui_impl_glfw.cpp" "${IMGUI_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp")
add_library(imgui::imgui ALIAS imgui)
target_compile_features(imgui PUBLIC cxx_std_11)
target_include_directories(imgui PUBLIC "$<BUILD_INTERFACE:${IMGUI_SOURCE_DIR}>" "$<INSTALL_INTERFACE:include>")
target_link_libraries(imgui PUBLIC glfw Vulkan::Vulkan)
install(TARGETS imgui EXPORT imgui-targets ARCHIVE DESTINATION lib)
file(GLOB imgui_headers "${IMGUI_SOURCE_DIR}/*.h")
install(FILES ${imgui_headers} "${IMGUI_SOURCE_DIR}/misc/cpp/imgui_stdlib.h"
  "${IMGUI_SOURCE_DIR}/backends/imgui_impl_glfw.h" "${IMGUI_SOURCE_DIR}/backends/imgui_impl_vulkan.h" DESTINATION include)
install(EXPORT imgui-targets NAMESPACE imgui:: FILE imgui-targets.cmake DESTINATION share/imgui)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/imgui-config.cmake"
  "include(CMakeFindDependencyMacro)\nfind_dependency(glfw3 3.5.1 EXACT CONFIG)\nfind_dependency(Vulkan 1.4)\ninclude(\"\${CMAKE_CURRENT_LIST_DIR}/imgui-targets.cmake\")\n")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/imgui-config-version.cmake" VERSION 1.93.0 COMPATIBILITY ExactVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/imgui-config.cmake" "${CMAKE_CURRENT_BINARY_DIR}/imgui-config-version.cmake" DESTINATION share/imgui)
