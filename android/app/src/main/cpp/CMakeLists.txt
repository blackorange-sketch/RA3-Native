cmake_minimum_required(VERSION 3.22)
project(ra3native CXX C)
set(CMAKE_CXX_STANDARD 17)
set(RT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../runtime)
# GAME: 112 (RA3 Native, ra3_1.12.game, lifted code in LIFTED_DIR) or 113 (RA3 Native Steam, ra3_1.13.game, in
# LIFTED_DIR_113) - set by the Gradle flavour
if(NOT GAME)
  set(GAME 112)
endif()
if(GAME STREQUAL "112" AND NOT LIFTED_DIR)
  message(FATAL_ERROR "LIFTED_DIR (generated lifted_*.cpp) is required: run the lifter first (see .github/workflows/android.yml)")
endif()
if(GAME STREQUAL "113" AND NOT LIFTED_DIR_113)
  message(FATAL_ERROR "LIFTED_DIR_113 (lifted code of ra3_1.13.game) is required for the Steam app")
endif()
set(GLUE ${ANDROID_NDK}/sources/android/native_app_glue)
set(RUNTIME_SRC
  android_main.cpp
  ${GLUE}/android_native_app_glue.c
  ${RT}/guest.cpp ${RT}/imports.cpp ${RT}/pe_loader.cpp ${RT}/heap.cpp ${RT}/rt.cpp ${RT}/fmt.cpp
  ${RT}/crt.cpp ${RT}/msvcp80.cpp ${RT}/steam.cpp ${RT}/kernel32.cpp ${RT}/misc.cpp ${RT}/input.cpp ${RT}/hud.cpp ${RT}/ws2.cpp ${RT}/dsound.cpp ${RT}/d3d9.cpp ${RT}/d3d9_dev.cpp ${RT}/d3d9_fx.cpp ${RT}/d3d9_gl.cpp ${RT}/d3d9_ui.cpp ${RT}/d3d9_tex.cpp ${RT}/glrec.cpp ${RT}/gputime.cpp ${RT}/perfhint.cpp ${RT}/gfx_gles.cpp ${RT}/vkb.cpp ${RT}/vksh.cpp ${RT}/d3d9_vk.cpp ${RT}/vma.cpp ${RT}/launch.cpp ${RT}/prof.cpp ${RT}/etc2.cpp
  ${RT}/third_party/etcpak/ProcessRGB.cpp ${RT}/third_party/etcpak/Tables.cpp ${RT}/third_party/etcpak/Dither.cpp)

# glslang (GLSL -> SPIR-V for the Vulkan renderer), the checkout the CI passes as GLSLANG_DIR; without it the
# Vulkan renderer cannot compile shaders
if(GLSLANG_DIR)
  # spirv-opt (SPIRV-Tools in External/spirv-tools, fetched by the CI): shaders reach the driver optimised
  if(IS_DIRECTORY ${GLSLANG_DIR}/External/spirv-tools)
    set(ENABLE_OPT ON CACHE BOOL "" FORCE)
    set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
    set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)
    set(SPIRV_WERROR OFF CACHE BOOL "" FORCE)
    set(SPIRV_TOOLS_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(BUILD_EXTERNAL ON CACHE BOOL "" FORCE)
    set(RA3_SPVOPT 1)
  else()
    set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
    set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
  endif()
  set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
  set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
  set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
  set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
  set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(ENABLE_PCH OFF CACHE BOOL "" FORCE)
  add_subdirectory(${GLSLANG_DIR} glslang EXCLUDE_FROM_ALL)
endif()

# one library per game build (runtime/game_build.h): the runtime is compiled into each, with that build's lifted code
function(game_library name lifted)
  file(GLOB src ${lifted}/lifted_*.cpp)
  add_library(${name} SHARED ${RUNTIME_SRC} ${src})
  target_include_directories(${name} PRIVATE ${GLUE} ${RT} ${lifted})
  # RA3_TRACE (per-call ring trace for crash reports) is off for speed; fatal faults print a host frame walk instead.
  # Android 10+: native ELF TLS (emulated TLS cost ~1.5% of the game thread in the profile)
  target_compile_options(${name} PRIVATE -O2 -fno-strict-aliasing -w -fno-emulated-tls)
  target_link_libraries(${name} android log EGL GLESv3 vulkan aaudio)
  set_target_properties(${name} PROPERTIES LINK_FLAGS "-Wl,-u,ANativeActivity_onCreate")
  if(GLSLANG_DIR)
    target_link_libraries(${name} glslang::glslang glslang::SPIRV glslang::glslang-default-resource-limits)
    target_include_directories(${name} PRIVATE ${GLSLANG_DIR})
    target_compile_definitions(${name} PRIVATE RA3_GLSLANG=1)
    if(RA3_SPVOPT)
      target_compile_definitions(${name} PRIVATE RA3_SPVOPT=1)
    endif()
  endif()
endfunction()

if(GAME STREQUAL "112")
  game_library(ra3native ${LIFTED_DIR})
else()
  # the Steam build (ra3_1.13.game)
  game_library(ra3native113 ${LIFTED_DIR_113})
  target_compile_definitions(ra3native113 PRIVATE RA3_GAME_113=1)
endif()
