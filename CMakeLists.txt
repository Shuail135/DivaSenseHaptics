cmake_minimum_required(VERSION 3.24)
project(DivaSenseHaptics VERSION 0.1.0 LANGUAGES CXX)

option(DIVASENSE_BUILD_CORE_TESTS "Build portable core smoke tests" OFF)

if(WIN32)
    add_library(DivaSenseHaptics SHARED
        src/ModMain.cpp
        src/Log.cpp
        src/Config.cpp
        src/HapticEngine.cpp
        src/ProcessLoopback.cpp
        src/DualSenseHid.cpp
        src/DualSenseAudio.cpp
        src/InputHaptics.cpp
        src/JudgementHaptics.cpp
        src/JudgementHook.cpp
        src/MenuHaptics.cpp
        src/DscChart.cpp
        src/ChartAwareness.cpp
        src/IatHook.cpp
        src/ChartCapture.cpp
        src/ChartMemoryScan.cpp
        src/DscProbe.cpp
    )

    target_compile_features(DivaSenseHaptics PRIVATE cxx_std_20)
    target_compile_definitions(DivaSenseHaptics PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00
        WINVER=0x0A00
    )
    target_include_directories(DivaSenseHaptics PRIVATE src)

    target_link_libraries(DivaSenseHaptics PRIVATE
        ole32
        mmdevapi
        avrt
        propsys
        setupapi
        hid
        d3d11
        dxgi
    )

    if(MSVC)
        target_compile_options(DivaSenseHaptics PRIVATE /W4 /permissive- /EHsc)
        set_property(TARGET DivaSenseHaptics PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()

    set_target_properties(DivaSenseHaptics PROPERTIES
        OUTPUT_NAME "DivaSenseHaptics"
        PREFIX ""
    )

    add_custom_command(TARGET DivaSenseHaptics POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:DivaSenseHaptics>/mod"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/mod/config.toml"
            "$<TARGET_FILE_DIR:DivaSenseHaptics>/mod/config.toml"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/mod/haptics.ini"
            "$<TARGET_FILE_DIR:DivaSenseHaptics>/mod/haptics.ini"
    )
elseif(NOT DIVASENSE_BUILD_CORE_TESTS)
    message(FATAL_ERROR "DivaSenseHaptics runtime is Windows-only. Use -DDIVASENSE_BUILD_CORE_TESTS=ON to build the portable smoke test on this platform.")
else()
    message(STATUS "Non-Windows host: building portable core smoke test only.")
endif()

if(DIVASENSE_BUILD_CORE_TESTS)
    enable_testing()
    add_executable(DivaSenseCoreSmoke
        tests/EngineSmoke.cpp
        src/Config.cpp
        src/HapticEngine.cpp
        src/InputHaptics.cpp
        src/JudgementHaptics.cpp
        src/DscChart.cpp
        src/DscProbe.cpp
        src/ChartAwareness.cpp
        src/Log.cpp
    )
    target_compile_features(DivaSenseCoreSmoke PRIVATE cxx_std_20)
    target_include_directories(DivaSenseCoreSmoke PRIVATE src)
    if(WIN32)
        target_sources(DivaSenseCoreSmoke PRIVATE src/MenuHaptics.cpp)
        target_compile_definitions(DivaSenseCoreSmoke PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
        target_link_libraries(DivaSenseCoreSmoke PRIVATE d3d11 dxgi)
    endif()
    if(MSVC)
        target_compile_options(DivaSenseCoreSmoke PRIVATE /W4 /permissive- /EHsc)
        set_property(TARGET DivaSenseCoreSmoke PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        target_compile_options(DivaSenseCoreSmoke PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME DivaSenseCoreSmoke COMMAND DivaSenseCoreSmoke)
endif()
