cmake_minimum_required(VERSION 3.20)

project(TelemetryLinkConsumer LANGUAGES CXX)

find_package(TelemetryLink 1 CONFIG REQUIRED)

add_executable(telemetry_link_consumer main.cpp)
target_link_libraries(telemetry_link_consumer PRIVATE TelemetryLink::telemetry_link)

enable_testing()
add_test(NAME telemetry_link_consumer COMMAND telemetry_link_consumer)
