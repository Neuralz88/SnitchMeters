cmake_minimum_required(VERSION 3.22)
project(SnitchMeters VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(SNITCH_BUILD_PLUGIN "Build the JUCE plugin + standalone" ON)

# Analysis code: header-only, no JUCE, no Windows headers.
add_library(snitch_dsp INTERFACE)
target_include_directories(snitch_dsp INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

function(snitch_console name src)
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE snitch_dsp)
    if(MSVC)
        target_compile_options(${name} PRIVATE /W4 /D_CRT_SECURE_NO_WARNINGS)
    endif()
endfunction()

snitch_console(dsp_tests tests/dsp_tests.cpp)
snitch_console(profile_ref tools/profile_ref.cpp)
snitch_console(render_check tools/render_check.cpp)

if(SNITCH_BUILD_PLUGIN)
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.4
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)

    juce_add_plugin(SnitchMeters
        COMPANY_NAME "Monkey Hammer"
        PRODUCT_NAME "Snitch Meters"
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT FALSE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        COPY_PLUGIN_AFTER_BUILD FALSE
        PLUGIN_MANUFACTURER_CODE Mnkh
        PLUGIN_CODE Sntc
        VST3_CATEGORIES Fx Analyzer
        FORMATS VST3 Standalone)

    target_sources(SnitchMeters PRIVATE
        plugin/PluginProcessor.cpp
        plugin/PluginEditor.cpp
        plugin/LoopbackCapture.cpp
        plugin/StandaloneApp.cpp)

    target_compile_definitions(SnitchMeters PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1)

    target_link_libraries(SnitchMeters
        PRIVATE
            snitch_dsp
            juce::juce_audio_utils
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags)
endif()
