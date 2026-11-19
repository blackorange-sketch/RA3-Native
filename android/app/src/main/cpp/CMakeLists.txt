cmake_minimum_required(VERSION 3.22)
project(ra3native CXX C)
set(CMAKE_CXX_STANDARD 17)
set(RT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../runtime)
if(NOT LIFTED_DIR)
  message(FATAL_ERROR "LIFTED_DIR (generated lifted_*.cpp) is required: run the lifter first (see .github/workflows/android.yml)")
endif()
file(GLOB LIFTED_SRC ${LIFTED_DIR}/lifted_*.cpp)
set(GLUE ${ANDROID_NDK}/sources/android/native_app_glue)
add_library(ra3native SHARED
  android_main.cpp
  ${GLUE}/android_native_app_glue.c
  ${RT}/guest.cpp ${RT}/imports.cpp ${RT}/pe_loader.cpp ${RT}/heap.cpp ${RT}/rt.cpp ${RT}/fmt.cpp
  ${RT}/crt.cpp ${RT}/kernel32.cpp ${RT}/misc.cpp ${RT}/input.cpp ${RT}/hud.cpp ${RT}/ws2.cpp ${RT}/dsound.cpp ${RT}/d3d9.cpp ${RT}/d3d9_dev.cpp ${RT}/d3d9_fx.cpp ${RT}/d3d9_gl.cpp ${RT}/glrec.cpp ${RT}/perfhint.cpp ${RT}/gfx_gles.cpp ${RT}/launch.cpp ${RT}/prof.cpp ${RT}/etc2.cpp
  ${RT}/third_party/etcpak/ProcessRGB.cpp ${RT}/third_party/etcpak/Tables.cpp ${RT}/third_party/etcpak/Dither.cpp
  ${LIFTED_SRC})
target_include_directories(ra3native PRIVATE ${GLUE} ${RT} ${LIFTED_DIR})
# RA3_TRACE (per-call ring trace for crash reports) is off for speed; fatal faults print a host frame walk instead.
# Android 10+: native ELF TLS (emulated TLS cost ~1.5% of the game thread in the profile)
target_compile_options(ra3native PRIVATE -O2 -fno-strict-aliasing -w -fno-emulated-tls)
target_link_libraries(ra3native android log EGL GLESv3 aaudio)
set_target_properties(ra3native PROPERTIES LINK_FLAGS "-Wl,-u,ANativeActivity_onCreate")
