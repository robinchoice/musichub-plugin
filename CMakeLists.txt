cmake_minimum_required(VERSION 3.22)
project(MusicHubPlugin VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(APPLE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
endif()

include(FetchContent)
FetchContent_Declare(JUCE
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 9.0.3
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

# JUCE bundles the AAX SDK, so Pro Tools builds need no extra download.
# Running the result in a release Pro Tools still needs PACE signing.
set(MUSICHUB_FORMATS VST3 Standalone)
if(APPLE)
  list(APPEND MUSICHUB_FORMATS AU AAX)
elseif(WIN32)
  list(APPEND MUSICHUB_FORMATS AAX)
endif()

juce_add_plugin(MusicHub
  PRODUCT_NAME "Music Hub"
  COMPANY_NAME "Music Hub"
  BUNDLE_ID lol.diespaetzles.hub.plugin
  PLUGIN_MANUFACTURER_CODE Mhub
  PLUGIN_CODE Mhab
  FORMATS ${MUSICHUB_FORMATS}
  VST3_CATEGORIES Fx Tools
  AU_MAIN_TYPE kAudioUnitType_Effect
  AAX_CATEGORY AAX_ePlugInCategory_Effect
  IS_SYNTH FALSE
  NEEDS_MIDI_INPUT FALSE
  NEEDS_MIDI_OUTPUT FALSE
  EDITOR_WANTS_KEYBOARD_FOCUS FALSE
  COPY_PLUGIN_AFTER_BUILD FALSE)

juce_generate_juce_header(MusicHub)

target_sources(MusicHub PRIVATE
  src/Engine.cpp
  src/Loudness.cpp
  src/HubClient.cpp
  src/HubSession.cpp
  src/PluginProcessor.cpp
  src/PluginEditor.cpp)

target_compile_definitions(MusicHub PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=1
  JUCE_LOAD_CURL_SYMBOLS_LAZILY=1
  JUCE_USE_MP3AUDIOFORMAT=1
  JUCE_VST3_CAN_REPLACE_VST2=0
  JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(MusicHub
  PRIVATE
    juce::juce_audio_utils
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# Engine and loudness meter tests, no plugin host needed
juce_add_console_app(MusicHubTests PRODUCT_NAME "MusicHubTests")
target_sources(MusicHubTests PRIVATE
  tests/EngineTest.cpp
  src/Engine.cpp
  src/Loudness.cpp)
target_include_directories(MusicHubTests PRIVATE src)
target_compile_definitions(MusicHubTests PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
target_link_libraries(MusicHubTests
  PRIVATE
    juce::juce_audio_basics
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

enable_testing()
add_test(NAME engine COMMAND MusicHubTests)
