# SpoutDX integration for the multiview Spout output feature.
#
# Builds a small, isolated static library (`spout-dx`) from the vendored
# Spout2 submodule (deps/Spout2). We compile only the source files SpoutDX
# actually needs — the same list SpoutDX's own CMakeLists uses for its
# `SpoutDX_static` target — instead of pulling in Spout2's root project,
# which would also build the OpenGL core, the shared DLLs, install rules,
# and force the /MT runtime (Spout's SPOUT_BUILD_CMT default). Keeping the
# sources in a dedicated target also keeps them out of the plugin's Qt
# AUTOMOC scan and away from our own warning flags.
#
# Windows-only: Spout is a DirectX shared-texture mechanism.

set(_spout_root "${CMAKE_CURRENT_SOURCE_DIR}/deps/Spout2/SPOUTSDK")
set(_spout_dx_dir "${_spout_root}/SpoutDirectX/SpoutDX")
set(_spout_gl_dir "${_spout_root}/SpoutGL")

if(NOT EXISTS "${_spout_dx_dir}/SpoutDX.cpp")
  message(
    FATAL_ERROR
    "ENABLE_SPOUT_OUTPUT is ON but the Spout2 submodule is missing. "
    "Run: git submodule update --init deps/Spout2 "
    "(or configure with -DENABLE_SPOUT_OUTPUT=OFF)."
  )
endif()

# Exactly the sources SpoutDX_static compiles (SpoutDX.cpp + the SpoutGL
# subset it depends on). The full Spout_static (GL extensions) is not needed.
add_library(
  spout-dx
  STATIC
  "${_spout_dx_dir}/SpoutDX.cpp"
  "${_spout_gl_dir}/SpoutCopy.cpp"
  "${_spout_gl_dir}/SpoutDirectX.cpp"
  "${_spout_gl_dir}/SpoutFrameCount.cpp"
  "${_spout_gl_dir}/SpoutSenderNames.cpp"
  "${_spout_gl_dir}/SpoutSharedMemory.cpp"
  "${_spout_gl_dir}/SpoutUtils.cpp"
)

# SPOUT_BUILD_STATIC mirrors SpoutDX's own static target. With neither
# SPOUT_BUILD_DLL nor SPOUT_IMPORT_DLL defined, SpoutCommon.h resolves
# SPOUT_DLLEXP to nothing, so symbols are plain static — exactly what we want.
target_compile_definitions(spout-dx PRIVATE SPOUT_BUILD_STATIC)

# Consumers get the SpoutDX.h and SpoutGL headers via these include dirs.
target_include_directories(spout-dx PUBLIC "${_spout_dx_dir}" "${_spout_gl_dir}")

# SpoutDirectX creates D3D11 devices and shares DXGI textures.
target_link_libraries(spout-dx PUBLIC d3d11 dxgi)

# This is vendored third-party code: don't subject it to the plugin's strict
# warnings-as-errors. The project adds /WX (and -Werror) at directory scope;
# target-scope options come later on the command line, so /WX- wins. Spout's
# sources also carry stray non-UTF-8 bytes (C4828) that we simply silence.
if(MSVC)
  target_compile_options(spout-dx PRIVATE /w /WX-)
else()
  target_compile_options(spout-dx PRIVATE -w)
endif()

# Match the plugin's dynamic runtime (/MD) — do NOT inherit Spout's /MT default.
set_property(TARGET spout-dx PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# Keep the third-party target tidy in IDE solution explorers.
set_target_properties(spout-dx PROPERTIES FOLDER "deps")

unset(_spout_root)
unset(_spout_dx_dir)
unset(_spout_gl_dir)
