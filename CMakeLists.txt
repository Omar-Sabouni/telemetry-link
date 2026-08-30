cmake_minimum_required(VERSION 3.20)

project(
    TelemetryLink
    VERSION 1.0.0
    DESCRIPTION "Framed embedded-device telemetry parser"
    LANGUAGES CXX
)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(TELEMETRY_LINK_IS_TOP_LEVEL ON)
else()
    set(TELEMETRY_LINK_IS_TOP_LEVEL OFF)
endif()

if(TELEMETRY_LINK_IS_TOP_LEVEL)
    include(CTest)
    set(TELEMETRY_LINK_BUILD_TESTS_DEFAULT "${BUILD_TESTING}")
else()
    set(TELEMETRY_LINK_BUILD_TESTS_DEFAULT OFF)
endif()

option(
    TELEMETRY_LINK_BUILD_CLI
    "Build the telemetry-link command-line decoder"
    "${TELEMETRY_LINK_IS_TOP_LEVEL}"
)
option(TELEMETRY_LINK_BUILD_BENCHMARK "Build the parse-throughput benchmark" OFF)
option(
    TELEMETRY_LINK_BUILD_TESTS
    "Build the TelemetryLink test suite"
    "${TELEMETRY_LINK_BUILD_TESTS_DEFAULT}"
)
option(
    TELEMETRY_LINK_INSTALL
    "Generate TelemetryLink install and package rules"
    "${TELEMETRY_LINK_IS_TOP_LEVEL}"
)
option(TELEMETRY_LINK_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

if(TELEMETRY_LINK_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)
    set(TELEMETRY_LINK_INSTALL_INCLUDEDIR "${CMAKE_INSTALL_INCLUDEDIR}")
else()
    set(TELEMETRY_LINK_INSTALL_INCLUDEDIR include)
endif()

function(telemetry_link_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
        if(TELEMETRY_LINK_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(
            ${target}
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
        if(TELEMETRY_LINK_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

add_library(telemetry_link STATIC
    src/telemetry_link.cpp
)
add_library(TelemetryLink::telemetry_link ALIAS telemetry_link)

target_compile_features(telemetry_link PUBLIC cxx_std_20)
set_target_properties(
    telemetry_link
    PROPERTIES
        CXX_EXTENSIONS OFF
        EXPORT_NAME telemetry_link
)
target_include_directories(
    telemetry_link
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${TELEMETRY_LINK_INSTALL_INCLUDEDIR}>
)
telemetry_link_set_warnings(telemetry_link)

if(TELEMETRY_LINK_BUILD_CLI)
    add_executable(telemetry_link_cli src/cli.cpp)
    set_target_properties(
        telemetry_link_cli
        PROPERTIES
            CXX_EXTENSIONS OFF
            OUTPUT_NAME telemetry-link
    )
    target_link_libraries(telemetry_link_cli PRIVATE TelemetryLink::telemetry_link)
    target_compile_features(telemetry_link_cli PRIVATE cxx_std_20)
    telemetry_link_set_warnings(telemetry_link_cli)
endif()

if(TELEMETRY_LINK_BUILD_BENCHMARK)
    add_executable(telemetry_link_benchmark bench/benchmark.cpp)
    target_link_libraries(telemetry_link_benchmark PRIVATE TelemetryLink::telemetry_link)
    target_compile_features(telemetry_link_benchmark PRIVATE cxx_std_20)
    set_target_properties(
        telemetry_link_benchmark
        PROPERTIES CXX_EXTENSIONS OFF
    )
    target_compile_definitions(
        telemetry_link_benchmark
        PRIVATE TELEMETRY_LINK_BUILD_CONFIG="$<CONFIG>"
    )
    telemetry_link_set_warnings(telemetry_link_benchmark)
endif()

if(TELEMETRY_LINK_BUILD_TESTS)
    enable_testing()

    add_executable(telemetry_link_tests tests/test_telemetry_link.cpp)
    target_link_libraries(telemetry_link_tests PRIVATE TelemetryLink::telemetry_link)
    target_compile_features(telemetry_link_tests PRIVATE cxx_std_20)
    set_target_properties(
        telemetry_link_tests
        PROPERTIES CXX_EXTENSIONS OFF
    )
    telemetry_link_set_warnings(telemetry_link_tests)

    add_test(NAME telemetry_link_unit COMMAND telemetry_link_tests)
    set_tests_properties(telemetry_link_unit PROPERTIES TIMEOUT 30)

    if(TELEMETRY_LINK_BUILD_CLI)
        find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
        add_test(
            NAME telemetry_link_cli_integration
            COMMAND
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli.py
                --cli $<TARGET_FILE:telemetry_link_cli>
                --simulator ${CMAKE_CURRENT_SOURCE_DIR}/tools/simulate.py
        )
        set_tests_properties(telemetry_link_cli_integration PROPERTIES TIMEOUT 60)

        add_test(
            NAME telemetry_link_report_renderer
            COMMAND
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_report.py
        )
        set_tests_properties(telemetry_link_report_renderer PROPERTIES TIMEOUT 30)
    endif()

    if(TELEMETRY_LINK_BUILD_BENCHMARK)
        add_test(
            NAME telemetry_link_benchmark_smoke
            COMMAND telemetry_link_benchmark 100000 1
        )
        set_tests_properties(
            telemetry_link_benchmark_smoke
            PROPERTIES
                LABELS benchmark
                TIMEOUT 60
        )
    endif()
endif()

if(TELEMETRY_LINK_INSTALL)
    install(
        TARGETS telemetry_link
        EXPORT TelemetryLinkTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    )
    install(
        DIRECTORY include/
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    )

    if(TELEMETRY_LINK_BUILD_CLI)
        install(TARGETS telemetry_link_cli RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    endif()

    set(
        TELEMETRY_LINK_INSTALL_CMAKEDIR
        "${CMAKE_INSTALL_LIBDIR}/cmake/TelemetryLink"
        CACHE STRING "Install location for TelemetryLink CMake package files"
    )

    configure_package_config_file(
        cmake/TelemetryLinkConfig.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/TelemetryLinkConfig.cmake"
        INSTALL_DESTINATION "${TELEMETRY_LINK_INSTALL_CMAKEDIR}"
    )
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/TelemetryLinkConfigVersion.cmake"
        VERSION "${PROJECT_VERSION}"
        COMPATIBILITY SameMajorVersion
    )

    install(
        EXPORT TelemetryLinkTargets
        FILE TelemetryLinkTargets.cmake
        NAMESPACE TelemetryLink::
        DESTINATION "${TELEMETRY_LINK_INSTALL_CMAKEDIR}"
    )
    install(
        FILES
            "${CMAKE_CURRENT_BINARY_DIR}/TelemetryLinkConfig.cmake"
            "${CMAKE_CURRENT_BINARY_DIR}/TelemetryLinkConfigVersion.cmake"
        DESTINATION "${TELEMETRY_LINK_INSTALL_CMAKEDIR}"
    )
endif()
