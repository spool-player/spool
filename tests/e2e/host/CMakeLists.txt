cmake_minimum_required(VERSION 3.22)
project(SpoolDeviceJourney LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Network WebSockets)

include(../../../tools/cmake/Spoolet.cmake)
add_executable(spool-device-journey ../AppJourneyTest.cpp ../JellyfinFixture.cpp ../../TestRunner.cpp)
target_include_directories(spool-device-journey PRIVATE ../..)
target_compile_definitions(spool-device-journey PRIVATE SPOOL_DEVICE_JOURNEY_ONLY=1 SPOOL_E2E_BINARY=1
    TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../.." SPOOL_E2E_SPOOLET="$<TARGET_FILE:spoolet>")
target_link_libraries(spool-device-journey PRIVATE Qt6::Core Qt6::Gui Qt6::Network Qt6::WebSockets)
add_dependencies(spool-device-journey spoolet)

file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/native-spool-e2e-path.txt"
    CONTENT "$<TARGET_FILE:spool-device-journey>\n")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/native-spoolet-path.txt"
    CONTENT "$<TARGET_FILE:spoolet>\n")
