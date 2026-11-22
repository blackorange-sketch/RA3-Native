cmake_minimum_required(VERSION 3.22)
project(ra3native CXX C)
set(CMAKE_CXX_STANDARD 17)
set(RT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../runtime)
if(NOT LIFTED_DIR)
  message(FATAL_ERROR "LIFTED_DIR (generated lifted_*.cpp) is required: run the lifter first (see .github/workflows/android.yml)")
endif()
set(GLUE ${ANDROID_NDK}/sources/android/native_app_glue)
set(RUNTIME_SRC
  android_main.cpp
  ${GLUE}/android_native_app_glue.c
  ${RT}/guest.cpp ${RT}/imports.cpp ${RT}/pe_loader.cpp ${RT}/heap.cpp ${RT}/rt.cpp ${RT}/fmt.cpp
  ${RT}/crt.cpp ${RT}/msvcp80.cpp ${RT}/steam.cpp ${RT}/kernel32.cpp ${RT}/misc.cpp ${RT}/input.cpp ${RT}/hud.cpp ${RT}/ws2.cpp ${RT}/dsound.cpp ${RT}/d3d9.cpp ${RT}/d3d9_dev.cpp ${RT}/d3d9_fx.cpp ${RT}/d3d9_gl.cpp ${RT}/glrec.cpp ${RT}/perfhint.cpp ${RT}/gfx_gles.cpp ${RT}/launch.cpp ${RT}/prof.cpp ${RT}/etc2.cpp
  ${RT}/third_party/etcpak/ProcessRGB.cpp ${RT}/third_party/etcpak/Tables.cpp ${RT}/third_party/etcpak/Dither.cpp)

# one library per game build (runtime/game_build.h): the runtime is compiled into each, with that build's lifted code
function(game_library name lifted)
  file(GLOB src ${lifted}/lifted_*.cpp)
  add_library(${name} SHARED ${RUNTIME_SRC} ${src})
  target_include_directories(${name} PRIVATE ${GLUE} ${RT} ${lifted})
  # RA3_TRACE (per-call ring trace for crash reports) is off for speed; fatal faults print a host frame walk instead.
  # Android 10+: native ELF TLS (emulated TLS cost ~1.5% of the game thread in the profile)
  target_compile_options(${name} PRIVATE -O2 -fno-strict-aliasing -w -fno-emulated-tls)
  target_link_libraries(${name} android log EGL GLESv3 aaudio)
  set_target_properties(${name} PROPERTIES LINK_FLAGS "-Wl,-u,ANativeActivity_onCreate")
endfunction()

game_library(ra3native ${LIFTED_DIR})
# the Steam build (ra3_1.13.game), when its lifted code is given
if(LIFTED_DIR_113)
  game_library(ra3native113 ${LIFTED_DIR_113})
  target_compile_definitions(ra3native113 PRIVATE RA3_GAME_113=1)
endif()
