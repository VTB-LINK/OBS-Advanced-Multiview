cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_project_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core Svg)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets Qt6::Svg)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp
    src/amv-logging.hpp
    src/amv-logging.cpp
    src/multiview-instance.hpp
    src/multiview-instance.cpp
    src/config-manager.hpp
    src/config-manager.cpp
    src/layout-engine.hpp
    src/layout-engine.cpp
    src/grid-preview-widget.hpp
    src/grid-preview-widget.cpp
    src/manager-dialog.hpp
    src/manager-dialog.cpp
    src/cell-display-settings-dialog.hpp
    src/cell-display-settings-dialog.cpp
    src/external-output-settings-dialog.hpp
    src/external-output-settings-dialog.cpp
    src/signal-lost-settings-dialog.hpp
    src/signal-lost-settings-dialog.cpp
    src/amv-frontend-cache.hpp
    src/amv-frontend-cache.cpp
    src/amv-instance-core.hpp
    src/amv-instance-core.cpp
    src/amv-instance-core-sources.cpp
    src/amv-instance-core-draw.cpp
    src/amv-instance-core-vu.cpp
    src/amv-instance-core-label.cpp
    src/amv-instance-core-image.cpp
    src/amv-instance-core-safe-area.cpp
    src/amv-instance-core-highlight.cpp
    src/amv-instance-core-status.cpp
    src/amv-instance-core-lost-image.cpp
    src/amv-instance-core-health.cpp
    src/multiview-window.hpp
    src/multiview-window.cpp
    src/multiview-window-context-menu.cpp
    src/multiview-output.hpp
    src/multiview-output.cpp
    src/multiview-output-spout.hpp
    src/multiview-output-spout.cpp
    src/multiview-output-ndi.hpp
    src/multiview-output-ndi.cpp
    src/multiview-ndi-runtime.hpp
    src/multiview-ndi-runtime.cpp
    src/signal-provider.hpp
    src/signal-provider.cpp
    src/signal-provider-ffmpeg.cpp
    src/signal-provider-ndi.cpp
    src/signal-provider-spout.cpp
    src/signal-provider-vlc.cpp
    src/provider-settings-forms.hpp
    src/provider-settings-forms-common.hpp
    src/provider-settings-forms-ffmpeg.cpp
    src/provider-settings-forms-ndi.cpp
    src/provider-settings-forms-spout.cpp
    src/provider-settings-forms-vlc.cpp
    src/edit-source-dialog.hpp
    src/edit-source-dialog.cpp
    src/source-picker.hpp
    src/source-picker.cpp
)

# ---------------------------------------------------------------------------
# Multiview output backends (issue #11)
#
# Approach B: dedicated senders that transmit the composed multiview directly,
# independent of OBS's source/scene system. The output layer is designed to be
# backend-agnostic — Spout lands first, NDI reuses the same pipeline later.
#
# Each backend has its own SDK-acquisition module under cmake/ because their
# linkage models differ (Spout = static link of the vendored SpoutDX submodule;
# NDI = runtime DLL load). The ENABLE_*_OUTPUT options share a naming scheme.
# ---------------------------------------------------------------------------
if(OS_WINDOWS)
  option(ENABLE_SPOUT_OUTPUT "Build multiview Spout output (Windows/DirectX)" ON)
else()
  set(ENABLE_SPOUT_OUTPUT OFF)
endif()

if(ENABLE_SPOUT_OUTPUT)
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/spout-output.cmake")
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE spout-dx)
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE AMV_ENABLE_SPOUT_OUTPUT)
endif()

# NDI is cross-platform (unlike Spout's DirectX). The runtime is loaded via Qt's
# QLibrary and the backend uses only libobs + the NDI table, so it builds on
# every platform NDI ships a runtime for: Windows, macOS, and Linux.
if(OS_WINDOWS OR OS_MACOS OR OS_LINUX)
  option(ENABLE_NDI_OUTPUT "Build multiview NDI output (Windows/macOS/Linux)" ON)
else()
  set(ENABLE_NDI_OUTPUT OFF)
endif()

# ndi-output.cmake detects the installed NDI SDK and may turn ENABLE_NDI_OUTPUT
# back OFF when it is absent (e.g. CI), so re-check before wiring it up. NDI is
# header-only at build time + runtime DLL load — no import library to link.
if(ENABLE_NDI_OUTPUT)
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ndi-output.cmake")
endif()

if(ENABLE_NDI_OUTPUT)
  target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "${NDI_SDK_INCLUDE_DIR}")
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE AMV_ENABLE_NDI_OUTPUT)
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
