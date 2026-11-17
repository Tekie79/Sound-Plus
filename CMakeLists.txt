cmake_minimum_required(VERSION 3.24)
set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS")
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Target architecture")
project(SoundPlus VERSION 0.1.0 LANGUAGES C CXX OBJCXX)
if(NOT APPLE)
  message(FATAL_ERROR "This milestone targets macOS Apple Silicon")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(JUCE_DIR "${CMAKE_SOURCE_DIR}/vendor/JUCE-8.0.6" CACHE PATH "JUCE 8.0.6 source")
add_subdirectory("${JUCE_DIR}" juce EXCLUDE_FROM_ALL)
find_package(SQLite3 REQUIRED)
add_library(soundplus_core STATIC
  src/Catalog.cpp src/LibraryService.cpp src/MacSources.mm src/AudioStream.cpp src/AudioIPC.cpp)
target_include_directories(soundplus_core PUBLIC src)
target_compile_definitions(soundplus_core PUBLIC
  JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=0
  JUCE_APPLICATION_NAME_STRING="SoundPlus" JUCE_APPLICATION_VERSION_STRING="0.1.0")
target_link_libraries(soundplus_core PUBLIC SQLite::SQLite3
  juce::juce_audio_utils juce::juce_gui_extra
  "-framework CoreServices" "-framework Foundation")
target_compile_options(soundplus_core PRIVATE -Wall -Wextra -Wpedantic)
juce_add_gui_app(SoundPlus PRODUCT_NAME "SoundPlus" COMPANY_NAME "SoundPlus"
  BUNDLE_ID "org.soundplus.workstation" MICROPHONE_PERMISSION_ENABLED FALSE)
juce_add_console_app(SoundPlusAudio PRODUCT_NAME "SoundPlusAudio")
target_sources(SoundPlusAudio PRIVATE src/AudioMain.cpp src/AudioEngine.cpp)
target_link_libraries(SoundPlusAudio PRIVATE soundplus_core)
target_sources(SoundPlus PRIVATE src/Main.cpp src/LibraryView.cpp)
target_link_libraries(SoundPlus PRIVATE soundplus_core)
add_dependencies(SoundPlus SoundPlusAudio)
set_property(TARGET SoundPlus APPEND PROPERTY LINK_DEPENDS "$<TARGET_FILE:SoundPlusAudio>")
add_custom_command(TARGET SoundPlus POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:SoundPlus>/Contents/Helpers"
  COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:SoundPlusAudio>"
    "$<TARGET_BUNDLE_DIR:SoundPlus>/Contents/Helpers/SoundPlusAudio")
include(CTest)
if(BUILD_TESTING)
  juce_add_console_app(SoundPlusTests PRODUCT_NAME "SoundPlusTests")
  target_sources(SoundPlusTests PRIVATE tests/Tests.cpp)
  target_link_libraries(SoundPlusTests PRIVATE soundplus_core)
  add_test(NAME library_and_audio COMMAND SoundPlusTests
    "$<TARGET_FILE:SoundPlus>" "${CMAKE_BINARY_DIR}/test-output")
  set_tests_properties(library_and_audio PROPERTIES TIMEOUT 90)
endif()
