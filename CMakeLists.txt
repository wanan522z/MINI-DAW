cmake_minimum_required(VERSION 3.24)
project(minidaw VERSION 0.2.0 LANGUAGES C CXX RC)

if(NOT WIN32 OR NOT MSVC)
  message(FATAL_ERROR "Mini DAW requires Windows x64 and the MSVC toolchain.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Mini DAW requires an x64 build; configure with -A x64.")
endif()

# JUCE contains the ASIO and VST3 host interfaces used by this application.
# No headers or sources from the separate Aggregate Audio project are required.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/JUCE/CMakeLists.txt")
  add_subdirectory(third_party/JUCE EXCLUDE_FROM_ALL)
else()
  include(FetchContent)
  FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 29396c22c93392d6738e021b83196283d6e4d850
    GIT_SHALLOW FALSE)
  FetchContent_MakeAvailable(JUCE)
endif()

juce_add_gui_app(mini_daw PRODUCT_NAME "MiniDAW" VERSION "${PROJECT_VERSION}"
  COMPANY_NAME "WanAn522z" NEEDS_WEB_BROWSER FALSE NEEDS_CURL FALSE)
target_sources(mini_daw PRIVATE
  src/mini_daw.cpp
  src/mini_daw_host.cpp
  src/mini_daw_host.h
  src/mini_daw_model.h
  src/mini_daw_verify.h)
target_compile_features(mini_daw PRIVATE cxx_std_20)
target_compile_definitions(mini_daw PRIVATE
  JUCE_ASIO=1 JUCE_WASAPI=0 JUCE_DIRECTSOUND=0
  JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_VST=0
  JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1 NOMINMAX)
target_compile_options(mini_daw PRIVATE /utf-8 /EHsc /bigobj)
target_link_libraries(mini_daw PRIVATE juce::juce_audio_utils juce::juce_gui_extra)

include(CTest)
if(BUILD_TESTING)
  add_executable(mini_daw_routing_test tests/mini_daw_routing_test.cpp)
  target_include_directories(mini_daw_routing_test PRIVATE src)
  target_compile_features(mini_daw_routing_test PRIVATE cxx_std_20)
  target_compile_options(mini_daw_routing_test PRIVATE /utf-8 /EHsc /W4)
  add_test(NAME mini_daw_routing COMMAND mini_daw_routing_test)
  add_test(NAME mini_daw_host COMMAND mini_daw --verify-host
    "${CMAKE_BINARY_DIR}/mini-daw-host-verification.xml")
endif()

install(TARGETS mini_daw RUNTIME DESTINATION .)
