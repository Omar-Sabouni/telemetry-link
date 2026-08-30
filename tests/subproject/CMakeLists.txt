cmake_minimum_required(VERSION 3.20)

project(TelemetryLinkSubprojectConsumer LANGUAGES CXX)

add_subdirectory(../.. telemetry-link)

if(TELEMETRY_LINK_BUILD_CLI OR
   TELEMETRY_LINK_BUILD_BENCHMARK OR
   TELEMETRY_LINK_BUILD_TESTS OR
   TELEMETRY_LINK_INSTALL)
    message(FATAL_ERROR "subproject defaults enabled an auxiliary artifact")
endif()

add_executable(telemetry_link_subproject_consumer ../consumer/main.cpp)
target_link_libraries(
    telemetry_link_subproject_consumer
    PRIVATE TelemetryLink::telemetry_link
)

enable_testing()
add_test(
    NAME telemetry_link_subproject_consumer
    COMMAND telemetry_link_subproject_consumer
)
