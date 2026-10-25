cmake_minimum_required(VERSION 3.22)

# C is required by JUCE's own root CMakeLists (it errors out at line 40 without it),
# even though nothing here compiles C.
project(PedalBoardProbes LANGUAGES C CXX)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(PEDALBOARD_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../Source")
set(PEDALBOARD_JUCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../vendor/JUCE")

if (NOT EXISTS "${PEDALBOARD_JUCE_DIR}/modules/juce_core/juce_core.h")
    message(FATAL_ERROR "JUCE checkout missing at ${PEDALBOARD_JUCE_DIR}")
endif()

add_subdirectory("${PEDALBOARD_JUCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/_juce" EXCLUDE_FROM_ALL)

# The DSP bench compiles the real effect headers, so it needs what they include:
# juce_dsp for the filters and convolution, and juce_gui_basics because CabinetEffect
# reaches for FileChooser (which lives there, not in gui_extra). EffectFactory.cpp is
# the one translation unit of the app that gets built in.
# The cabinet effect includes PedalBoardIRData.h, which juce_add_binary_data
# generates. The probe declares its own copy of that target rather than reaching
# into the application's build directory: the bench then configures and builds on
# its own. The file list mirrors the one in the top-level CMakeLists.txt, which
# remains the source of truth.
set(PEDALBOARD_IR_FILES
    "${CMAKE_CURRENT_LIST_DIR}/../Resources/IRs/ir_1x12_v30_closed.wav"
    "${CMAKE_CURRENT_LIST_DIR}/../Resources/IRs/ir_2x12_open_back.wav"
    "${CMAKE_CURRENT_LIST_DIR}/../Resources/IRs/ir_4x12_high_gain.wav")

foreach (_ir IN LISTS PEDALBOARD_IR_FILES)
    if (NOT EXISTS "${_ir}")
        message(FATAL_ERROR "Missing impulse response ${_ir}\nGenerate them: node Resources/make_irs.mjs")
    endif()
endforeach()

juce_add_binary_data(ProbeIRs
                     SOURCES      ${PEDALBOARD_IR_FILES}
                     HEADER_NAME  "PedalBoardIRData.h"
                     NAMESPACE    PedalBoardIR)

add_executable(dsp_probe
    dsp_probe.cpp
    "${PEDALBOARD_SOURCE_DIR}/Common/PedalLanguage.cpp"
    "${PEDALBOARD_SOURCE_DIR}/Effects/EffectFactory.cpp")
target_include_directories(dsp_probe PRIVATE
    "${PEDALBOARD_SOURCE_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/../vendor/signalsmith-stretch/repo/include"
    "${CMAKE_CURRENT_LIST_DIR}/../vendor/signalsmith-stretch/linear/include")
target_compile_features(dsp_probe PRIVATE cxx_std_17)
target_compile_options(dsp_probe PRIVATE /utf-8 /fp:fast /bigobj)
target_compile_definitions(dsp_probe PRIVATE JUCE_DEFINE_MAIN=0 JUCE_PLUGINHOST_VST3=1)
target_link_libraries(dsp_probe PRIVATE
    ProbeIRs
    juce::juce_audio_processors
    juce::juce_dsp
    juce::juce_gui_basics
    juce::juce_recommended_config_flags)

# Console probe that prints what an AudioDeviceManager actually sees and opens.
add_executable(device_probe device_probe.cpp)
target_include_directories(device_probe PRIVATE "${PEDALBOARD_SOURCE_DIR}")
target_compile_features(device_probe PRIVATE cxx_std_17)
target_compile_options(device_probe PRIVATE /utf-8 /bigobj)
target_compile_definitions(device_probe PRIVATE JUCE_DEFINE_MAIN=0)
target_link_libraries(device_probe PRIVATE
    juce::juce_audio_devices
    juce::juce_recommended_config_flags)

# Loads one VST3 effect through the same JUCE classes the Plugin Host uses and
# prints its class list, parameters, processing result and state size. This is
# the diagnostic for "the card says a plugin is loaded": it checks the hosting
# API independently of the UI. Usage: vst3_probe <plugin.vst3>
add_executable(vst3_probe vst3_probe.cpp)
target_compile_features(vst3_probe PRIVATE cxx_std_17)
target_compile_options(vst3_probe PRIVATE /utf-8 /bigobj)
target_compile_definitions(vst3_probe PRIVATE JUCE_DEFINE_MAIN=0 JUCE_PLUGINHOST_VST3=1)
target_link_libraries(vst3_probe PRIVATE
    juce::juce_audio_processors
    juce::juce_dsp
    juce::juce_recommended_config_flags)

# Opens VB-CABLE the way the router does and prints the format the driver
# actually granted, plus the callback cadence. See the comment at its top.
add_executable(cable_probe cable_probe.cpp)
target_include_directories(cable_probe PRIVATE "${PEDALBOARD_SOURCE_DIR}")
target_compile_features(cable_probe PRIVATE cxx_std_17)
target_compile_options(cable_probe PRIVATE /utf-8 /bigobj)
target_compile_definitions(cable_probe PRIVATE JUCE_DEFINE_MAIN=0)
target_link_libraries(cable_probe PRIVATE
    juce::juce_audio_devices
    juce::juce_recommended_config_flags)

# Sends a known sine through the real VBCableRouter and records CABLE Output, so
# "the cable is running" is measured rather than inferred. See its header comment.
add_executable(virtual_path_probe
    virtual_path_probe.cpp
    "${PEDALBOARD_SOURCE_DIR}/Common/PedalLanguage.cpp"
    "${PEDALBOARD_SOURCE_DIR}/VirtualDevice/VBCableRouter.cpp")
target_include_directories(virtual_path_probe PRIVATE "${PEDALBOARD_SOURCE_DIR}")
target_compile_features(virtual_path_probe PRIVATE cxx_std_17)
target_compile_options(virtual_path_probe PRIVATE /utf-8 /bigobj)
target_compile_definitions(virtual_path_probe PRIVATE JUCE_DEFINE_MAIN=0)
target_link_libraries(virtual_path_probe PRIVATE
    juce::juce_audio_devices
    juce::juce_gui_basics
    juce::juce_recommended_config_flags)
