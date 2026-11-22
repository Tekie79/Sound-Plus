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
include(cmake/JuceNativeScale.cmake)
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
  src/MidiModel.cpp src/NativeSampler.cpp src/MidiInput.cpp
  src/InputRecording.cpp
  src/DesignInputSession.cpp src/DesignMidiSession.cpp
  src/HostedRack.cpp src/PluginProcessing.cpp src/PluginSession.cpp src/SourcePools.cpp src/DesignPoolSession.cpp
  src/ChannelDetails.cpp src/ChannelLayout.cpp src/LayoutDesign.cpp src/Catalog.cpp src/LibraryService.cpp src/MacSources.mm src/AudioStream.cpp src/AudioIPC.cpp
  src/ModelPack.cpp src/AIProcess.cpp src/Waveform.cpp src/WaveformPreview.cpp src/Reference.cpp src/RetrievalIndex.cpp
  src/PerformanceState.cpp src/InsertChain.cpp src/DesignModel.cpp src/TrackModel.cpp src/PerformTake.cpp src/TakeCapture.cpp src/DesignAudio.cpp src/DesignSession.cpp src/LiveInsertSession.cpp src/DesignTakeSession.cpp src/DesignRecovery.cpp src/DesignLive.cpp src/AudioPlaybackService.cpp src/CanvasEditing.cpp src/IconRegistry.cpp)
target_include_directories(soundplus_core PUBLIC src)
target_compile_definitions(soundplus_core PUBLIC
  JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=0
  JUCE_APPLICATION_NAME_STRING="SoundPlus" JUCE_APPLICATION_VERSION_STRING="0.1.0")
target_link_libraries(soundplus_core PUBLIC SQLite::SQLite3
  juce::juce_audio_utils juce::juce_gui_extra juce::juce_cryptography juce::juce_dsp ModelManifest SoundPlusIcons
  "-framework CoreServices" "-framework Foundation" "-framework AVFoundation")
target_compile_options(soundplus_core PRIVATE -Wall -Wextra -Wpedantic)
juce_add_gui_app(SoundPlus PRODUCT_NAME "SoundPlus" COMPANY_NAME "SoundPlus"
  BUNDLE_ID "org.soundplus.workstation" MICROPHONE_PERMISSION_ENABLED TRUE
  MICROPHONE_PERMISSION_TEXT "SoundPlus uses your microphone or audio interface only when you arm an Audio Track or enable input monitoring.")
juce_add_console_app(SoundPlusPlugin PRODUCT_NAME "SoundPlusPlugin")
target_sources(SoundPlusPlugin PRIVATE src/PluginWorkerMain.cpp)
target_compile_definitions(SoundPlusPlugin PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_AU=1)
target_link_libraries(SoundPlusPlugin PRIVATE soundplus_core)
# Native editor windows belong to an identified GUI helper, never the main app.
juce_add_gui_app(SoundPlusPluginEditor PRODUCT_NAME "SoundPlusPluginEditor" COMPANY_NAME "SoundPlus"
  BUNDLE_ID "org.soundplus.plugin-editor" MICROPHONE_PERMISSION_ENABLED FALSE)
target_sources(SoundPlusPluginEditor PRIVATE src/PluginWorkerMain.cpp)
target_compile_definitions(SoundPlusPluginEditor PRIVATE SOUNDPLUS_PLUGIN_EDITOR_APP=1)
target_compile_definitions(SoundPlusPluginEditor PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_AU=1)
target_link_libraries(SoundPlusPluginEditor PRIVATE soundplus_core)

juce_add_console_app(SoundPlusAudio PRODUCT_NAME "SoundPlusAudio")
juce_add_console_app(SoundPlusAI PRODUCT_NAME "SoundPlusAI")
target_sources(SoundPlusAI PRIVATE src/AIHelperMain.cpp src/ClapRuntime.cpp)
target_include_directories(SoundPlusAI PRIVATE "${ORT_DIR}/build/native/include")
target_link_libraries(SoundPlusAI PRIVATE soundplus_core "${ORT_DIR}/runtimes/osx-arm64/native/libonnxruntime.dylib")
# AI is isolated in a 13.3+ helper. The library/audition application retains its 12.0 minimum.
target_compile_options(SoundPlusAI PRIVATE -mmacosx-version-min=13.3)
target_link_options(SoundPlusAI PRIVATE -mmacosx-version-min=13.3)
set_target_properties(SoundPlusAI PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "@executable_path/../Frameworks")
target_sources(SoundPlusAudio PRIVATE src/AudioMain.cpp src/AudioEngine.cpp src/MacAudioInput.cpp)
target_link_libraries(SoundPlusAudio PRIVATE soundplus_core)
target_sources(SoundPlus PRIVATE src/PluginProcessPanel.cpp src/Main.cpp src/LibraryView.cpp src/DesignerLayouts.cpp src/DesignerView.cpp src/DesignerTakes.cpp src/DesignerPools.cpp src/PoolMappingReview.cpp src/SourcePoolView.cpp src/PerformView.cpp src/SourceCard.cpp src/DesignerCanvas.cpp src/AutomationEditor.cpp src/MultitrackEditor.cpp src/MidiEditor.cpp src/Benchmark.cpp src/BenchmarkReliability.cpp)
target_link_libraries(SoundPlus PRIVATE soundplus_core)
add_dependencies(SoundPlus SoundPlusAudio SoundPlusAI SoundPlusPlugin SoundPlusPluginEditor)
set_property(TARGET SoundPlus APPEND PROPERTY LINK_DEPENDS "$<TARGET_FILE:SoundPlusAudio>" "$<TARGET_FILE:SoundPlusPlugin>")
set_property(TARGET SoundPlus APPEND PROPERTY LINK_DEPENDS "$<TARGET_FILE:SoundPlusAI>" "$<TARGET_FILE:SoundPlusPluginEditor>")
add_custom_command(TARGET SoundPlus POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:SoundPlus>/Contents/Helpers"
  COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:SoundPlusPlugin>"
    "$<TARGET_BUNDLE_DIR:SoundPlus>/Contents/Helpers/SoundPlusPlugin"
  COMMAND ${CMAKE_COMMAND} -E remove_directory "$<TARGET_BUNDLE_DIR:SoundPlus>/Contents/Helpers/SoundPlusPluginEditor.app"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "$<TARGET_BUNDLE_DIR:SoundPlusPluginEditor>"
    "$<TARGET_BUNDLE_DIR:SoundPlus>/Contents/Helpers/SoundPlusPluginEditor.app"
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
  target_sources(SoundPlusTests PRIVATE tests/Tests.cpp tests/MultitrackTests.cpp tests/PerformanceTests.cpp tests/SemanticTests.cpp tests/SimilarTests.cpp tests/DesignTests.cpp tests/DesignSessionTests.cpp tests/CanvasGestureTests.cpp tests/TransportInteractionTests.cpp tests/FadeTests.cpp src/DesignerCanvas.cpp src/AutomationEditor.cpp src/MultitrackEditor.cpp src/MidiEditor.cpp)
  target_link_libraries(SoundPlusTests PRIVATE soundplus_core)
  target_sources(SoundPlusTests PRIVATE tests/PlaybackSearchTests.cpp)
  target_sources(SoundPlusTests PRIVATE tests/InputRecordingTests.cpp tests/MidiTests.cpp)
  add_test(NAME midi_sequencing COMMAND SoundPlusTests --midi "${CMAKE_BINARY_DIR}/midi-tests")
  set_tests_properties(midi_sequencing PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
  add_test(NAME audio_input_recording COMMAND SoundPlusTests --audio-input "${CMAKE_BINARY_DIR}/audio-input-tests")
  set_tests_properties(audio_input_recording PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
  target_sources(SoundPlusTests PRIVATE tests/SourcePoolTests.cpp tests/PoolCorrectionTests.cpp)
  add_test(NAME multitrack_automation COMMAND SoundPlusTests --multitrack "${CMAKE_BINARY_DIR}/multitrack-tests")
  set_tests_properties(multitrack_automation PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
  add_test(NAME pool_corrections COMMAND SoundPlusTests --pool-corrections "${CMAKE_BINARY_DIR}/pool-corrections-output" "$<TARGET_FILE:SoundPlus>")
  set_tests_properties(pool_corrections PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
  add_test(NAME source_pools COMMAND SoundPlusTests --source-pools "${CMAKE_BINARY_DIR}/source-pool-test-output" "$<TARGET_FILE:SoundPlus>")
  set_tests_properties(source_pools PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
  add_test(NAME library_playback_search COMMAND SoundPlusTests --playback-search "${CMAKE_BINARY_DIR}/playback-search-output" "$<TARGET_FILE:SoundPlus>")
  set_tests_properties(library_playback_search PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
  target_sources(SoundPlusTests PRIVATE tests/MultichannelCorrectionsTests.cpp)
  add_test(NAME multichannel_corrections COMMAND SoundPlusTests --multichannel-corrections "${CMAKE_BINARY_DIR}/multichannel-corrections-output" "$<TARGET_FILE:SoundPlus>")
  set_tests_properties(multichannel_corrections PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
  target_sources(SoundPlusTests PRIVATE tests/MultichannelTests.cpp tests/TakeIntegrityTests.cpp)
  add_test(NAME multichannel COMMAND SoundPlusTests --multichannel "${CMAKE_BINARY_DIR}/multichannel-test-output" "$<TARGET_FILE:SoundPlus>")
  set_tests_properties(multichannel PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
  add_test(NAME take_integrity COMMAND SoundPlusTests --take-integrity "${CMAKE_BINARY_DIR}/take-integrity-output" "$<TARGET_FILE:SoundPlus>")
  set_tests_properties(take_integrity PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
  target_sources(SoundPlusTests PRIVATE tests/DeviceCaptureTests.cpp src/AudioEngine.cpp src/MacAudioInput.cpp)
  add_test(NAME capture_device_lifecycle COMMAND SoundPlusTests --device-capture "${CMAKE_BINARY_DIR}/device-capture-output")
  set_tests_properties(capture_device_lifecycle PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
  target_sources(SoundPlusTests PRIVATE tests/IconTests.cpp tests/PerformanceStateTests.cpp tests/WaveformPreviewTests.cpp tests/ReliabilityTests.cpp tests/AudioHealthTests.cpp tests/TakeTests.cpp src/LibraryView.cpp src/Benchmark.cpp src/BenchmarkReliability.cpp)
  add_test(NAME recorded_takes COMMAND SoundPlusTests --takes "${CMAKE_BINARY_DIR}/take-test-output" "$<TARGET_FILE:SoundPlus>")
  set_tests_properties(recorded_takes PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
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
  target_sources(SoundPlusTests PRIVATE tests/ViewCommandTests.cpp src/PluginProcessPanel.cpp src/DesignerLayouts.cpp src/DesignerView.cpp src/DesignerTakes.cpp src/DesignerPools.cpp src/PoolMappingReview.cpp src/SourcePoolView.cpp src/PerformView.cpp src/SourceCard.cpp)
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

if(BUILD_TESTING)
  juce_add_plugin(SoundPlusFixture COMPANY_NAME "SoundPlus QA" PLUGIN_MANUFACTURER_CODE SpQA
    PLUGIN_CODE SpF1 FORMATS VST3 PRODUCT_NAME "SoundPlus QA Effect" COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Fx IS_SYNTH FALSE NEEDS_MIDI_INPUT TRUE NEEDS_MIDI_OUTPUT TRUE)
  target_sources(SoundPlusFixture PRIVATE tests/PluginFixture.cpp)
  target_compile_definitions(SoundPlusFixture PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 SOUNDPLUS_FIXTURE_STEREO=0)
  target_link_libraries(SoundPlusFixture PRIVATE juce::juce_audio_processors)
  juce_add_plugin(SoundPlusStereoFixture COMPANY_NAME "SoundPlus QA" PLUGIN_MANUFACTURER_CODE SpQA
    PLUGIN_CODE SpF2 FORMATS VST3 PRODUCT_NAME "SoundPlus QA Stereo" COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Fx IS_SYNTH FALSE NEEDS_MIDI_INPUT TRUE NEEDS_MIDI_OUTPUT TRUE)
  target_sources(SoundPlusStereoFixture PRIVATE tests/PluginFixture.cpp)
  target_compile_definitions(SoundPlusStereoFixture PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 SOUNDPLUS_FIXTURE_STEREO=1)
  target_link_libraries(SoundPlusStereoFixture PRIVATE juce::juce_audio_processors)
  juce_add_plugin(SoundPlusInstrumentFixture COMPANY_NAME "SoundPlus QA" PLUGIN_MANUFACTURER_CODE SpQA
    PLUGIN_CODE SpF3 FORMATS VST3 PRODUCT_NAME "SoundPlus QA Instrument" COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Instrument Synth IS_SYNTH TRUE NEEDS_MIDI_INPUT TRUE NEEDS_MIDI_OUTPUT FALSE)
  target_sources(SoundPlusInstrumentFixture PRIVATE tests/PluginFixture.cpp)
  target_compile_definitions(SoundPlusInstrumentFixture PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 SOUNDPLUS_FIXTURE_STEREO=0)
  target_link_libraries(SoundPlusInstrumentFixture PRIVATE juce::juce_audio_processors)

endif()

if(BUILD_TESTING)
  target_sources(SoundPlusTests PRIVATE tests/PluginProcessingTests.cpp)
  target_include_directories(SoundPlusTests PRIVATE "${JUCE_DIR}/modules/juce_audio_processors/format_types/VST3_SDK")
  juce_add_plugin(SoundPlusMidiFixture COMPANY_NAME "SoundPlus" BUNDLE_ID "org.soundplus.midi-fixture" PLUGIN_MANUFACTURER_CODE SpQA
    PLUGIN_CODE SpM1 FORMATS VST3 PRODUCT_NAME "SoundPlus MIDI Oracle" COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Instrument Synth IS_SYNTH TRUE NEEDS_MIDI_INPUT TRUE NEEDS_MIDI_OUTPUT FALSE)
  target_sources(SoundPlusMidiFixture PRIVATE tests/MidiInstrumentFixture.cpp)
  target_compile_definitions(SoundPlusMidiFixture PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
  target_link_libraries(SoundPlusMidiFixture PRIVATE juce::juce_audio_processors)
  target_sources(SoundPlusTests PRIVATE tests/MidiInstrumentTests.cpp)
  add_dependencies(SoundPlusTests SoundPlusMidiFixture_VST3)
  add_test(NAME midi_instruments COMMAND SoundPlusTests --midi-instruments "${CMAKE_BINARY_DIR}/midi-instrument-tests" "$<TARGET_FILE:SoundPlus>" "$<TARGET_FILE_DIR:SoundPlusMidiFixture_VST3>/../..")
  set_tests_properties(midi_instruments PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
  add_dependencies(SoundPlusTests SoundPlusFixture_VST3 SoundPlusStereoFixture_VST3 SoundPlusInstrumentFixture_VST3 SoundPlusPlugin)
  add_test(NAME plugin_processing COMMAND SoundPlusTests --plugin-processing "${CMAKE_BINARY_DIR}/plugin-processing-test-output" "$<TARGET_FILE:SoundPlus>" "$<TARGET_FILE_DIR:SoundPlusFixture_VST3>/../.." "$<TARGET_FILE_DIR:SoundPlusStereoFixture_VST3>/../..")
  add_test(NAME plugin_correctness COMMAND SoundPlusTests --plugin-correctness "${CMAKE_BINARY_DIR}/plugin-correctness/tests" "$<TARGET_FILE:SoundPlus>" "$<TARGET_FILE_DIR:SoundPlusFixture_VST3>/../.." "$<TARGET_FILE_DIR:SoundPlusStereoFixture_VST3>/../..")
  add_test(NAME plugin_gate_a COMMAND SoundPlusTests --plugin-gate-a "${CMAKE_BINARY_DIR}/plugin-editor-live/gate-a" "$<TARGET_FILE:SoundPlus>" "$<TARGET_FILE_DIR:SoundPlusFixture_VST3>/../.." "$<TARGET_FILE_DIR:SoundPlusStereoFixture_VST3>/../..")
  target_sources(SoundPlusTests PRIVATE tests/OrderedCorrectionTests.cpp)
  add_test(NAME ordered_corrections COMMAND SoundPlusTests --ordered-corrections "${CMAKE_BINARY_DIR}/plugin-takes/corrections" "${CMAKE_SOURCE_DIR}/tests/fixtures/legacy-v7")
  set_tests_properties(ordered_corrections PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
  add_test(NAME plugin_takes COMMAND SoundPlusTests --plugin-takes "${CMAKE_BINARY_DIR}/plugin-takes/tests" "$<TARGET_FILE:SoundPlus>" "$<TARGET_FILE_DIR:SoundPlusFixture_VST3>/../..")
  set_tests_properties(plugin_takes PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)
  add_test(NAME ordered_inserts COMMAND SoundPlusTests --ordered-inserts "${CMAKE_BINARY_DIR}/ordered-inserts/tests" "$<TARGET_FILE:SoundPlus>" "$<TARGET_FILE_DIR:SoundPlusFixture_VST3>/../.." "$<TARGET_FILE_DIR:SoundPlusStereoFixture_VST3>/../..")
  set_tests_properties(ordered_inserts PROPERTIES TIMEOUT 240 RUN_SERIAL TRUE)
  add_test(NAME native_view_scale COMMAND SoundPlusTests --native-view-scale)
  add_test(NAME plugin_live COMMAND SoundPlusTests --plugin-live "${CMAKE_BINARY_DIR}/plugin-editor-live/tests" "$<TARGET_FILE:SoundPlus>" "$<TARGET_FILE_DIR:SoundPlusFixture_VST3>/../..")
  set_tests_properties(plugin_live PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
  set_tests_properties(plugin_gate_a PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
  set_tests_properties(plugin_correctness PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
  set_tests_properties(plugin_processing PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE ENVIRONMENT "SOUNDPLUS_QUALIFY_AU=1")
endif()

# Optional instrumented production-component/worker QA driver; never bundled in the app.
option(SOUNDPLUS_BUILD_PLUGIN_ASAN_QA "Build AddressSanitizer plugin QA driver" OFF)
if(SOUNDPLUS_BUILD_PLUGIN_ASAN_QA)
  get_target_property(SOUNDPLUS_ASAN_SOURCES soundplus_core SOURCES)
  get_target_property(SOUNDPLUS_ASAN_LIBRARIES soundplus_core LINK_LIBRARIES)
  get_target_property(SOUNDPLUS_ASAN_DEFINITIONS soundplus_core COMPILE_DEFINITIONS)
  add_library(soundplus_plugin_asan_core STATIC ${SOUNDPLUS_ASAN_SOURCES})
  target_include_directories(soundplus_plugin_asan_core PUBLIC src)
  target_compile_definitions(soundplus_plugin_asan_core PUBLIC ${SOUNDPLUS_ASAN_DEFINITIONS})
  target_compile_options(soundplus_plugin_asan_core PRIVATE -fsanitize=address -g -fno-omit-frame-pointer -O1)
  target_link_libraries(soundplus_plugin_asan_core PUBLIC ${SOUNDPLUS_ASAN_LIBRARIES})
  juce_add_console_app(SoundPlusPublicationDebug PRODUCT_NAME "SoundPlusPublicationDebug")
  target_sources(SoundPlusPublicationDebug PRIVATE src/DesignerView.cpp src/DesignerLayouts.cpp src/DesignerTakes.cpp src/DesignerPools.cpp src/PoolMappingReview.cpp src/SourcePoolView.cpp src/PerformView.cpp src/SourceCard.cpp src/DesignerCanvas.cpp src/AutomationEditor.cpp src/MultitrackEditor.cpp src/MidiEditor.cpp tests/PluginProcessingDebugMain.cpp tests/PluginProcessingTests.cpp src/PluginProcessing.cpp src/PluginSession.cpp src/PluginProcessPanel.cpp src/LibraryView.cpp src/Benchmark.cpp src/BenchmarkReliability.cpp)
  target_include_directories(SoundPlusPublicationDebug PRIVATE "${JUCE_DIR}/modules/juce_audio_processors/format_types/VST3_SDK")
  target_compile_options(SoundPlusPublicationDebug PRIVATE -fsanitize=address -g -fno-omit-frame-pointer -O1)
  target_link_options(SoundPlusPublicationDebug PRIVATE -fsanitize=address)
  target_link_libraries(SoundPlusPublicationDebug PRIVATE soundplus_plugin_asan_core)
endif()

if(BUILD_TESTING)
  add_library(SoundPlusMultiFixture MODULE tests/PluginMultiFactory.cpp)
  set_target_properties(SoundPlusMultiFixture PROPERTIES BUNDLE TRUE BUNDLE_EXTENSION vst3
    PREFIX "" OUTPUT_NAME "SoundPlus QA Multi" MACOSX_BUNDLE_GUI_IDENTIFIER "com.soundplus.qa.multi")
  target_include_directories(SoundPlusMultiFixture PRIVATE "${JUCE_DIR}/modules/juce_audio_processors/format_types/VST3_SDK")
  target_link_libraries(SoundPlusMultiFixture PRIVATE "-framework CoreFoundation")
  add_dependencies(SoundPlusMultiFixture SoundPlusFixture_VST3 SoundPlusStereoFixture_VST3)
  add_custom_command(TARGET SoundPlusMultiFixture POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:SoundPlusMultiFixture>/Contents/Resources"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "$<TARGET_FILE_DIR:SoundPlusFixture_VST3>/../.." "$<TARGET_BUNDLE_DIR:SoundPlusMultiFixture>/Contents/Resources/A.vst3"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "$<TARGET_FILE_DIR:SoundPlusStereoFixture_VST3>/../.." "$<TARGET_BUNDLE_DIR:SoundPlusMultiFixture>/Contents/Resources/B.vst3")
  add_dependencies(SoundPlusTests SoundPlusMultiFixture)
endif()

option(SOUNDPLUS_LIVE_FIXTURE_ASAN "Instrument the owned native-editor fixture and live host worker, with ASan runtime in hosts" OFF)
if(SOUNDPLUS_LIVE_FIXTURE_ASAN)
  # Capture seeds and live control/reset code are compiled into this worker TU.
  # Instrument the actual host path, not only the loaded owned plugin.
  set_source_files_properties(src/PluginWorkerMain.cpp PROPERTIES
    COMPILE_OPTIONS "-fsanitize=address;-fno-omit-frame-pointer")
  target_compile_options(SoundPlusFixture PRIVATE -fsanitize=address -fno-omit-frame-pointer)
  foreach(target SoundPlusFixture_VST3 juce_vst3_helper SoundPlusPlugin SoundPlusPluginEditor)
    target_link_options(${target} PRIVATE -fsanitize=address)
  endforeach()
endif()
# A host executable must not interpose its private JUCE C++ symbols on a bundle.
# Plugin entry points are resolved from the loaded bundle, not the host executable.
target_link_options(SoundPlusPlugin PRIVATE "-Wl,-no_exported_symbols")
target_link_options(SoundPlusPluginEditor PRIVATE "-Wl,-no_exported_symbols")
