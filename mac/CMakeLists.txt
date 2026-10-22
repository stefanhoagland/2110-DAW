# Soundbending Live for macOS: a native multitrack recorder and live console built with JUCE.
# Build:  cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
cmake_minimum_required(VERSION 3.22)

set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Oldest macOS version supported")
project(DAW2110 VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(JUCE
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 8.0.9
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

juce_add_gui_app(DAW2110
  PRODUCT_NAME "Soundbending Live"
  COMPANY_NAME "2110"
  BUNDLE_ID "com.stefanhoagland.daw2110"
  VERSION ${PROJECT_VERSION}
  ICON_BIG Resources/icon.png
  MICROPHONE_PERMISSION_ENABLED TRUE
  MICROPHONE_PERMISSION_TEXT "Soundbending Live records and mixes the inputs of your audio interface.")

target_sources(DAW2110 PRIVATE
  Source/Main.cpp
  Source/AudioEngine.cpp
  Source/MainComponent.cpp
  Source/TrackRow.cpp
  Source/TrackArea.cpp
  Source/Look.cpp
  Source/Console.cpp
  Source/Patch.cpp
  Source/PatchList.cpp
  Source/Usage.cpp
  Source/Plugins.cpp
  Source/Names.cpp
  Source/Timecode.cpp
  Source/Aes67.cpp
  Source/NetworkView.cpp)

target_compile_definitions(DAW2110 PRIVATE
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_PLUGINHOST_VST3=1
  JUCE_PLUGINHOST_AU=$<IF:$<PLATFORM_ID:Darwin>,1,0>
  JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:DAW2110,JUCE_PRODUCT_NAME>"
  JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:DAW2110,JUCE_VERSION>")

juce_add_binary_data(DAW2110Assets SOURCES Resources/logo.svg)

target_link_libraries(DAW2110 PRIVATE
  DAW2110Assets
  juce::juce_audio_utils
  juce::juce_audio_devices
  juce::juce_audio_formats
  juce::juce_gui_extra
  juce::juce_recommended_config_flags
  juce::juce_recommended_lto_flags
  juce::juce_recommended_warning_flags)

# Small checks of the non-audio logic (channel-name CSV, file names).
add_executable(NamesTest Source/Names.cpp Tests/NamesTest.cpp)
target_link_libraries(NamesTest PRIVATE juce::juce_core juce::juce_recommended_config_flags)
target_compile_definitions(NamesTest PRIVATE JUCE_STANDALONE_APPLICATION=1 JUCE_USE_CURL=0)

add_executable(EngineTest Source/AudioEngine.cpp Source/Names.cpp Source/Timecode.cpp Source/Aes67.cpp Tests/EngineTest.cpp)
target_link_libraries(EngineTest PRIVATE juce::juce_audio_devices juce::juce_audio_formats juce::juce_audio_processors juce::juce_events
  juce::juce_recommended_config_flags)
target_compile_definitions(EngineTest PRIVATE JUCE_STANDALONE_APPLICATION=1 JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)

# Timecode maths, LTC decoding of a generated signal, MTC and BWF stamping.
add_executable(TimecodeTest Source/Timecode.cpp Tests/TimecodeTest.cpp)
target_link_libraries(TimecodeTest PRIVATE juce::juce_core juce::juce_recommended_config_flags)
target_compile_definitions(TimecodeTest PRIVATE JUCE_STANDALONE_APPLICATION=1 JUCE_USE_CURL=0)

# AES67: SDP and SAP parsing, and receiving RTP audio over the network.
add_executable(Aes67Test Source/Aes67.cpp Tests/Aes67Test.cpp)
target_link_libraries(Aes67Test PRIVATE juce::juce_audio_basics juce::juce_events juce::juce_recommended_config_flags)
target_compile_definitions(Aes67Test PRIVATE JUCE_STANDALONE_APPLICATION=1 JUCE_USE_CURL=0)
