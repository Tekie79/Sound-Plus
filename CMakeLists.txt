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
set(ORT_DIR "${CMAKE_SOURCE_DIR}/vendor/onnxruntime-1.22.1")
if(NOT EXISTS "${ORT_DIR}/build/native/include/onnxruntime_cxx_api.h")
  message(FATAL_ERROR "Run python3 scripts/fetch_runtime.py to install the pinned native AI runtime")
endif()
juce_add_binary_data(ModelManifest HEADER_NAME ModelManifestData.h SOURCES resources/clap-model.json)
file(GLOB SOUNDPLUS_ICON_SVGS CONFIGURE_DEPENDS resources/icons/lucide/*.svg resources/icons/custom/*.svg)
juce_add_binary_data(SoundPlusIcons HEADER_NAME IconAssets.h NAMESPACE IconAssets SOURCES ${SOUNDPLUS_ICON_SVGS})
add_library(soundplus_core STATIC
  src/Catalog.cpp src/LibraryService.cpp src/MacSources.mm src/AudioStream.cpp src/AudioIPC.cpp
  src/ModelPack.cpp src/AIProcess.cpp src/Waveform.cpp src/WaveformPreview.cpp src/Reference.cpp src/RetrievalIndex.cpp
  src/PerformanceState.cpp src/DesignModel.cpp src/DesignAudio.cpp src/DesignSession.cpp src/DesignRecovery.cpp src/DesignLive.cpp src/AudioPlaybackService.cpp src/CanvasEditing.cpp src/IconRegistry.cpp)
target_include_directories(soundplus_core PUBLIC src)
target_compile_definitions(soundplus_core PUBLIC
  JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=0
  JUCE_APPLICATION_NAME_STRING="SoundPlus" JUCE_APPLICATION_VERSION_STRING="0.1.0")
target_link_libraries(soundplus_core PUBLIC SQLite::SQLite3
  juce::juce_audio_utils juce::juce_gui_extra juce::juce_cryptography juce::juce_dsp ModelManifest SoundPlusIcons
  "-framework CoreServices" "-framework Foundation")
target_compile_options(soundplus_core PRIVATE -Wall -Wextra -Wpedantic)
juce_add_gui_app(SoundPlus PRODUCT_NAME "SoundPlus" COMPANY_NAME "SoundPlus"
  BUNDLE_ID "org.soundplus.workstation" MICROPHONE_PERMISSION_ENABLED FALSE)
juce_add_console_app(SoundPlusAudio PRODUCT_NAME "SoundPlusAudio")
juce_add_console_app(SoundPlusAI PRODUCT_NAME "SoundPlusAI")
target_sources(SoundPlusAI PRIVATE src/AIHelperMain.cpp src/ClapRuntime.cpp)
target_include_directories(SoundPlusAI PRIVATE "${ORT_DIR}/build/native/include")
target_link_libraries(SoundPlusAI PRIVATE soundplus_core "${ORT_DIR}/runtimes/osx-arm64/native/libonnxruntime.dylib")
# AI is isolated in a 13.3+ helper. The library/audition application retains its 12.0 minimum.
target_compile_options(SoundPlusAI PRIVATE -mmacosx-version-min=13.3)
target_link_options(SoundPlusAI PRIVATE -mmacosx-version-min=13.3)
set_target_properties(SoundPlusAI PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "@executable_path/../Frameworks")
target_sources(SoundPlusAudio PRIVATE src/AudioMain.cpp src/AudioEngine.cpp)
target_link_libraries(SoundPlusAudio PRIVATE soundplus_core)
target_sources(SoundPlus PRIVATE src/Main.cpp src/LibraryView.cpp src/DesignerView.cpp src/PerformView.cpp src/SourceCard.cpp src/DesignerCanvas.cpp src/Benchmark.cpp src/BenchmarkReliability.cpp)
target_link_libraries(SoundPlus PRIVATE soundplus_core)
add_dependencies(SoundPlus SoundPlusAudio SoundPlusAI)
set_property(TARGET SoundPlus APPEND PROPERTY LINK_DEPENDS "$<TARGET_FILE:SoundPlusAudio>")
set_property(TARGET SoundPlus APPEND PROPERTY LINK_DEPENDS "$<TARGET_FILE:SoundPlusAI>")
add_custom_command(TARGET SoundPlus POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:SoundPlus>/Contents/Helpers"
  COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:SoundPlusAudio>"
    "$<TARGET_BUNDLE_DIR:SoundPlus>/Contents/Helpers/SoundPlusAudio"
  COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:SoundPlusAI>"
    "$<TARGET_BUNDLE_DIR:SoundPlus>/Contents/Helpers/SoundPlusAI"
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:SoundPlus>/Contents/Frameworks"
  COMMAND ${CMAKE_COMMAND} -E copy "${ORT_DIR}/runtimes/osx-arm64/native/libonnxruntime.dylib"
    "$<TARGET_BUNDLE_DIR:SoundPlus>/Contents/Frameworks/libonnxruntime.1.22.1.dylib"
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:SoundPlus>/Contents/Resources/Licenses"
  COMMAND ${CMAKE_COMMAND} -E copy "${ORT_DIR}/LICENSE"
    "$<TARGET_BUNDLE_DIR:SoundPlus>/Contents/Resources/Licenses/ONNXRuntime-MIT.txt"
  COMMAND ${CMAKE_COMMAND} -E copy "${ORT_DIR}/ThirdPartyNotices.txt"
    "$<TARGET_BUNDLE_DIR:SoundPlus>/Contents/Resources/Licenses/ONNXRuntime-ThirdPartyNotices.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/resources/licenses"
    "$<TARGET_BUNDLE_DIR:SoundPlus>/Contents/Resources/Licenses")
include(CTest)
if(BUILD_TESTING)
  juce_add_console_app(SoundPlusTests PRODUCT_NAME "SoundPlusTests")
  target_sources(SoundPlusTests PRIVATE tests/Tests.cpp tests/PerformanceTests.cpp tests/SemanticTests.cpp tests/SimilarTests.cpp tests/DesignTests.cpp tests/DesignSessionTests.cpp tests/CanvasGestureTests.cpp tests/TransportInteractionTests.cpp tests/FadeTests.cpp src/DesignerCanvas.cpp)
  target_link_libraries(SoundPlusTests PRIVATE soundplus_core)
  target_sources(SoundPlusTests PRIVATE tests/IconTests.cpp tests/PerformanceStateTests.cpp tests/WaveformPreviewTests.cpp tests/ReliabilityTests.cpp tests/AudioHealthTests.cpp src/LibraryView.cpp src/Benchmark.cpp src/BenchmarkReliability.cpp)
  add_test(NAME library_stability COMMAND SoundPlusTests --library-stability "${CMAKE_BINARY_DIR}/library-stability-output" "$<TARGET_FILE:SoundPlus>")
  set_tests_properties(library_stability PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
  add_test(NAME waveform_scheduling COMMAND SoundPlusTests --waveform-scheduling "${CMAKE_BINARY_DIR}/waveform-scheduling-output")
  set_tests_properties(waveform_scheduling PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
  add_test(NAME audio_health COMMAND SoundPlusTests --audio-health "${CMAKE_BINARY_DIR}/audio-health-output" "$<TARGET_FILE:SoundPlus>")
  set_tests_properties(audio_health PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
  add_test(NAME waveform_previews COMMAND SoundPlusTests --waveform-previews "${CMAKE_BINARY_DIR}/waveform-preview-test-output")
  set_tests_properties(waveform_previews PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
  add_test(NAME performance_state COMMAND SoundPlusTests --performance-state "${CMAKE_BINARY_DIR}/performance-state-test-output" "$<TARGET_FILE:SoundPlus>")
  set_tests_properties(performance_state PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
  target_sources(SoundPlusTests PRIVATE tests/ViewCommandTests.cpp src/DesignerView.cpp src/PerformView.cpp src/SourceCard.cpp)
  add_test(NAME designer_view_commands COMMAND SoundPlusTests --view-commands
    "${CMAKE_BINARY_DIR}/view-command-test-output" "$<TARGET_FILE:SoundPlus>")
  set_tests_properties(designer_view_commands PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
  add_test(NAME icon_system COMMAND SoundPlusTests --icon-system "${CMAKE_BINARY_DIR}/icon-test-output")
  set_tests_properties(icon_system PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
  add_test(NAME library_and_audio COMMAND SoundPlusTests
    "$<TARGET_FILE:SoundPlus>" "${CMAKE_BINARY_DIR}/test-output")
  set_tests_properties(library_and_audio PROPERTIES TIMEOUT 90)
  add_test(NAME library_selection_audition COMMAND SoundPlusTests --selection-audition
    "${CMAKE_BINARY_DIR}/library-selection-test-output" "$<TARGET_FILE:SoundPlus>")
  set_tests_properties(library_selection_audition PROPERTIES TIMEOUT 40 RUN_SERIAL TRUE)
  add_test(NAME library_performance_regression COMMAND SoundPlusTests --performance
    "${CMAKE_BINARY_DIR}/performance-test-output" "$<TARGET_FILE:SoundPlus>")
  set_tests_properties(library_performance_regression PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
  add_test(NAME library_semantic_integrity COMMAND SoundPlusTests --semantic-integrity
    "${CMAKE_BINARY_DIR}/semantic-test-output" "$<TARGET_FILE:SoundPlus>")
  set_tests_properties(library_semantic_integrity PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
  add_test(NAME library_similar_integrity COMMAND SoundPlusTests --similar-integrity
    "${CMAKE_BINARY_DIR}/similar-test-output" "$<TARGET_FILE:SoundPlus>")
  set_tests_properties(library_similar_integrity PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
  add_test(NAME designer_transport COMMAND SoundPlusTests --designer-transport "${CMAKE_BINARY_DIR}/designer-transport-output" "$<TARGET_FILE:SoundPlus>")
  set_tests_properties(designer_transport PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
  add_test(NAME designer_canvas COMMAND SoundPlusTests --canvas-gestures "${CMAKE_BINARY_DIR}/designer-gesture-output")
  set_tests_properties(designer_canvas PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
  add_test(NAME designer_session COMMAND SoundPlusTests --designer-session "${CMAKE_BINARY_DIR}/designer-session-output")
  set_tests_properties(designer_session PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
  add_test(NAME designer_workflow COMMAND SoundPlusTests --designer "${CMAKE_BINARY_DIR}/designer-test-output" "$<TARGET_FILE:SoundPlus>")
  set_tests_properties(designer_workflow PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
  add_test(NAME designer_fades COMMAND SoundPlusTests --designer-fades "${CMAKE_BINARY_DIR}/designer-fade-output" "$<TARGET_FILE:SoundPlus>")
  set_tests_properties(designer_fades PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
endif()
