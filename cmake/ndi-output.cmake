# NDI SDK integration for the multiview NDI output feature.
#
# Unlike Spout (which we statically link from a vendored submodule), NDI is
# introduced as a *build-time header-only* dependency: we compile against the
# installed NDI SDK headers but never link its import library and never bundle
# its runtime DLL. The plugin locates and loads Processing.NDI.Lib.x64.dll
# dynamically at runtime (see src/multiview-ndi-runtime.cpp), so the only thing
# this module needs is the SDK include directory.
#
# The headers are also vendored in deps/ndi/include (see deps/ndi/README.md) so
# CI and contributors without the SDK installed can still build the NDI backend
# — the same approach DistroAV takes. An installed SDK is preferred and the
# vendored copy is the fallback, so detection effectively always succeeds. The
# non-fatal disable branch below only triggers if the vendored headers are also
# missing (e.g. a partial checkout).
#
# Windows + macOS + Linux (the runtime loads cross-platform via Qt's QLibrary).
# Set the NDI_SDK_DIR environment variable to override and use an installed SDK.

# `Processing.NDI.Lib.h` is the umbrella header; finding it pins the Include dir.
# Search order: explicit NDI_SDK_DIR override, then the default installer
# locations (Windows + macOS), then the vendored fallback. Both runtimes are
# supported at load time, so either SDK version's headers suffice (we only use
# the ABI-frozen v5 function subset).
find_path(
  NDI_SDK_INCLUDE_DIR
  NAMES Processing.NDI.Lib.h
  HINTS "$ENV{NDI_SDK_DIR}" "$ENV{NDI_SDK_DIR}/Include"
  PATHS
    "C:/Program Files/NDI/NDI 6 SDK/Include"
    "C:/Program Files/NDI/NDI 5 SDK/Include"
    "C:/Program Files/NewTek/NDI 5 SDK/Include"
    "/Library/NDI SDK for Apple/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/deps/ndi/include"
)

if(NDI_SDK_INCLUDE_DIR)
  message(STATUS "NDI SDK found: ${NDI_SDK_INCLUDE_DIR} (NDI output enabled)")
else()
  message(
    STATUS
    "NDI SDK not found - multiview NDI output disabled. "
    "Install the NDI SDK (or set NDI_SDK_DIR) to build/test the NDI backend."
  )
  set(ENABLE_NDI_OUTPUT OFF)
endif()
