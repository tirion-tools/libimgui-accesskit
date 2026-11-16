# libimgui-accesskit: accessibility and text for Dear ImGui applications on Linux (C++20).
#
#   imgui_accesskit::bridge   AccessKit (AT-SPI) bridge                   SDL3, AccessKit C
#   imgui_accesskit::text     Pango/cairo text runs, single-line editor   Dear ImGui, SDL3, OpenGL, pangocairo, fontconfig
#   imgui_accesskit::cef      CEF accessibility tree mirror               standard library only
#
# Consumers add_subdirectory() this directory and switch modules off with IMGUI_ACCESSKIT_{BRIDGE,TEXT,CEF}
# (a consumer of only the cef module needs none of the other dependencies). The consumer's targets are
# used when they exist: imgui, SDL3::SDL3-static or SDL3::SDL3, accesskit, PkgConfig::PANGOCAIRO. Otherwise
# SDL3 and AccessKit come from find_package (SDL3's CMake package, an accesskit-c release with
# find_package(ACCESSKIT)), pangocairo and fontconfig from pkg-config, and Dear ImGui's core is compiled
# from the sources in IMGUI_ACCESSKIT_IMGUI_DIR.
#
# Built on its own (tools/gate.sh) it also builds the GoogleTest suites with strict warnings as errors,
# and with -DIMGUI_ACCESSKIT_FUZZ=ON (clang) the libFuzzer target instead.
cmake_minimum_required(VERSION 3.25)
project(imgui_accesskit VERSION 0.1.1 LANGUAGES CXX)

option(IMGUI_ACCESSKIT_BRIDGE "Build imgui_accesskit::bridge (SDL3, AccessKit C)" ON)
option(IMGUI_ACCESSKIT_TEXT "Build imgui_accesskit::text (Dear ImGui, SDL3, OpenGL, pangocairo, fontconfig)" ON)
option(IMGUI_ACCESSKIT_CEF "Build imgui_accesskit::cef (standard library only)" ON)

set(_imgui_accesskit_strict -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion -Werror)

function(_imgui_accesskit_library name alias)
  add_library(${name} STATIC ${ARGN})
  add_library(imgui_accesskit::${alias} ALIAS ${name})
  target_include_directories(${name} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_features(${name} PUBLIC cxx_std_20)
  if(PROJECT_IS_TOP_LEVEL)
    target_compile_options(${name} PRIVATE ${_imgui_accesskit_strict})
  endif()
endfunction()

# ---- dependencies (the consumer's targets first)
if(IMGUI_ACCESSKIT_BRIDGE OR IMGUI_ACCESSKIT_TEXT)
  if(TARGET SDL3::SDL3-static)
    set(_imgui_accesskit_sdl SDL3::SDL3-static)
  elseif(TARGET SDL3::SDL3)
    set(_imgui_accesskit_sdl SDL3::SDL3)
  else()
    find_package(SDL3 3.4 REQUIRED CONFIG GLOBAL)
    set(_imgui_accesskit_sdl SDL3::SDL3)
  endif()
endif()
if(IMGUI_ACCESSKIT_BRIDGE AND NOT TARGET accesskit)
  find_package(ACCESSKIT REQUIRED GLOBAL)
endif()
if(IMGUI_ACCESSKIT_TEXT)
  if(NOT TARGET PkgConfig::PANGOCAIRO)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PANGOCAIRO REQUIRED IMPORTED_TARGET GLOBAL pangocairo fontconfig)
  endif()
  find_package(OpenGL REQUIRED GLOBAL)
  if(NOT TARGET imgui)
    set(IMGUI_ACCESSKIT_IMGUI_DIR "" CACHE PATH "Dear ImGui sources, compiled when the consumer has no imgui target")
    if(NOT EXISTS "${IMGUI_ACCESSKIT_IMGUI_DIR}/imgui.h")
      message(FATAL_ERROR "imgui_accesskit: no imgui target; set IMGUI_ACCESSKIT_IMGUI_DIR to Dear ImGui's sources")
    endif()
    add_library(imgui STATIC
      ${IMGUI_ACCESSKIT_IMGUI_DIR}/imgui.cpp
      ${IMGUI_ACCESSKIT_IMGUI_DIR}/imgui_draw.cpp
      ${IMGUI_ACCESSKIT_IMGUI_DIR}/imgui_tables.cpp
      ${IMGUI_ACCESSKIT_IMGUI_DIR}/imgui_widgets.cpp)
    target_include_directories(imgui SYSTEM PUBLIC ${IMGUI_ACCESSKIT_IMGUI_DIR})
    target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
  endif()
endif()

# ---- modules
if(IMGUI_ACCESSKIT_CEF)
  _imgui_accesskit_library(imgui_accesskit_cef cef src/cef_tree.cc)
endif()
if(IMGUI_ACCESSKIT_BRIDGE)
  _imgui_accesskit_library(imgui_accesskit_bridge bridge src/bridge.cc)
  target_link_libraries(imgui_accesskit_bridge PUBLIC ${_imgui_accesskit_sdl} PRIVATE accesskit)
endif()
if(IMGUI_ACCESSKIT_TEXT)
  _imgui_accesskit_library(imgui_accesskit_text text src/run_cache.cc src/line_edit.cc)
  target_link_libraries(imgui_accesskit_text PUBLIC imgui ${_imgui_accesskit_sdl} PkgConfig::PANGOCAIRO PRIVATE OpenGL::GL)
endif()

# ---- tests and fuzz target (standalone builds only)
if(PROJECT_IS_TOP_LEVEL)
  option(IMGUI_ACCESSKIT_FUZZ "Build the libFuzzer target fuzz_cef_tree instead of the tests (clang)" OFF)
  if(IMGUI_ACCESSKIT_FUZZ)
    if(NOT IMGUI_ACCESSKIT_CEF)
      message(FATAL_ERROR "imgui_accesskit: IMGUI_ACCESSKIT_FUZZ needs IMGUI_ACCESSKIT_CEF")
    endif()
    set(_fuzz_flags -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all)
    target_compile_options(imgui_accesskit_cef PRIVATE -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=all)
    add_executable(fuzz_cef_tree fuzz/fuzz_cef_tree.cc)
    target_link_libraries(fuzz_cef_tree PRIVATE imgui_accesskit::cef)
    target_compile_options(fuzz_cef_tree PRIVATE ${_imgui_accesskit_strict} ${_fuzz_flags})
    target_link_options(fuzz_cef_tree PRIVATE ${_fuzz_flags})
  else()
    find_package(GTest REQUIRED)
    include(GoogleTest)
    enable_testing()
    function(_imgui_accesskit_test name prefix)
      add_executable(${name} ${ARGN})
      target_link_libraries(${name} PRIVATE GTest::gtest_main)
      target_compile_options(${name} PRIVATE ${_imgui_accesskit_strict})
      gtest_discover_tests(${name} TEST_PREFIX "${prefix}." PROPERTIES ENVIRONMENT "SDL_VIDEO_DRIVER=dummy"
        DISCOVERY_TIMEOUT 60)
    endfunction()
    if(IMGUI_ACCESSKIT_CEF)
      _imgui_accesskit_test(cef_tree_test cef tests/cef_tree_test.cc)
      target_link_libraries(cef_tree_test PRIVATE imgui_accesskit::cef)
    endif()
    if(IMGUI_ACCESSKIT_BRIDGE)
      _imgui_accesskit_test(bridge_test bridge tests/bridge_test.cc)
      target_link_libraries(bridge_test PRIVATE imgui_accesskit::bridge accesskit)
    endif()
    if(IMGUI_ACCESSKIT_TEXT)
      _imgui_accesskit_test(text_test text tests/run_cache_test.cc tests/line_edit_test.cc)
      target_link_libraries(text_test PRIVATE imgui_accesskit::text)
    endif()
  endif()
endif()
