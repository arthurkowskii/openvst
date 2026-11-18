cmake_minimum_required(VERSION 3.22)
project(OpenVSTPrivate VERSION 0.0.1 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF)
set(CMAKE_CXX_FLAGS_RELEASE "-O1 -DNDEBUG")
set(OPENVST_RUNTIME "${CMAKE_SOURCE_DIR}/research/toolchain/1.0.3209/linux/x64/libCmajPerformer.so" CACHE FILEPATH "Explicit trusted release runtime")
set(OPENVST_FIXTURES "${CMAKE_SOURCE_DIR}/artifacts/plugin/fixtures" CACHE PATH "Frozen original fixture directory")
add_subdirectory(research/JUCE build/plugin-juce EXCLUDE_FROM_ALL)
add_library(openvst_core STATIC src/plugin/Processor.cpp src/plugin/ModuleUI.cpp src/plugin/ModuleStore.cpp src/plugin/CatalogueClient.cpp)
target_include_directories(openvst_core PUBLIC src/plugin research/JUCE/modules PRIVATE research/cmajor-release/include)
target_compile_definitions(openvst_core PUBLIC JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 PRIVATE CMAJOR_DLL=1 OPENVST_RUNTIME="${OPENVST_RUNTIME}" OPENVST_FIXTURES="${OPENVST_FIXTURES}")
target_link_libraries(openvst_core PRIVATE juce::juce_audio_processors juce::juce_cryptography ${CMAKE_DL_LIBS})
# Propagate header availability settings to the native test. JUCE's generated
# plugin targets also compile module objects with their per-wrapper settings.
target_compile_definitions(openvst_core INTERFACE JUCE_MODULE_AVAILABLE_juce_audio_processors=1 JUCE_MODULE_AVAILABLE_juce_gui_extra=1 JUCE_MODULE_AVAILABLE_juce_gui_basics=1 JUCE_MODULE_AVAILABLE_juce_graphics=1 JUCE_MODULE_AVAILABLE_juce_events=1 JUCE_MODULE_AVAILABLE_juce_core=1 JUCE_MODULE_AVAILABLE_juce_data_structures=1 JUCE_MODULE_AVAILABLE_juce_audio_basics=1 JUCE_MODULE_AVAILABLE_juce_audio_processors_headless=1 JUCE_MODULE_AVAILABLE_juce_cryptography=1)
foreach(kind IN ITEMS FX Instrument)
    if(kind STREQUAL "Instrument")
        set(synth TRUE)
        set(code OvIn)
    else()
        set(synth FALSE)
        set(code OvFx)
    endif()
    juce_add_plugin(OpenVST${kind} COMPANY_NAME "OpenVST Private" BUNDLE_ID "org.openvst.private.${kind}" PLUGIN_MANUFACTURER_CODE Ovst PLUGIN_CODE ${code} FORMATS VST3 PRODUCT_NAME "OpenVST ${kind}" IS_SYNTH ${synth} NEEDS_MIDI_INPUT ${synth} NEEDS_MIDI_OUTPUT FALSE IS_MIDI_EFFECT FALSE COPY_PLUGIN_AFTER_BUILD FALSE VST3_AUTO_MANIFEST FALSE)
    target_sources(OpenVST${kind} PRIVATE src/plugin/Entry.cpp)
    target_compile_definitions(OpenVST${kind} PRIVATE OPENVST_IS_INSTRUMENT=$<BOOL:${synth}>)
    target_link_libraries(OpenVST${kind} PRIVATE openvst_core)
endforeach()
enable_testing()
add_executable(openvst_processor_tests tests/plugin/ProcessorTests.cpp)
target_compile_definitions(openvst_processor_tests PRIVATE PLUGIN_ARTIFACT_DIR="${CMAKE_SOURCE_DIR}/artifacts/plugin")
target_link_libraries(openvst_processor_tests PRIVATE openvst_core)
add_test(NAME processor_native COMMAND openvst_processor_tests)

# Store fixtures are generated from Python's stdlib zipfile, not checked in as
# binary blobs. The generator refuses nothing itself: the C++ store is the unit
# under test and must reject the hostile archives.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    set(OPENVST_STORE_FIXTURES "${CMAKE_SOURCE_DIR}/artifacts/plugin/packages")
    add_custom_command(
        OUTPUT "${OPENVST_STORE_FIXTURES}/.stamp"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${OPENVST_STORE_FIXTURES}"
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tests/plugin/make_store_fixtures.py" "${OPENVST_STORE_FIXTURES}"
        COMMAND ${CMAKE_COMMAND} -E touch "${OPENVST_STORE_FIXTURES}/.stamp"
        DEPENDS "${CMAKE_SOURCE_DIR}/tests/plugin/make_store_fixtures.py"
        COMMENT "Generating OpenVST module store fixtures")
    add_custom_target(openvst_store_fixtures ALL DEPENDS "${OPENVST_STORE_FIXTURES}/.stamp")
    add_executable(openvst_module_store_tests tests/plugin/ModuleStoreTests.cpp)
    target_compile_definitions(openvst_module_store_tests PRIVATE MODULE_STORE_PACKAGE_DIR="${OPENVST_STORE_FIXTURES}")
    target_link_libraries(openvst_module_store_tests PRIVATE openvst_core juce::juce_cryptography)
    add_dependencies(openvst_module_store_tests openvst_store_fixtures)
    add_test(NAME module_store COMMAND openvst_module_store_tests)
endif()

# End-to-end catalogue probe used by tests/integration/test_catalogue_e2e.py.
add_executable(openvst_catalogue_probe tests/plugin/CatalogueProbe.cpp)
target_link_libraries(openvst_catalogue_probe PRIVATE openvst_core)
