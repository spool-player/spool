cmake_minimum_required(VERSION 3.22)
project(AndroidPackaging VERSION 1.0.0 LANGUAGES CXX)

set(ANDROID ON)
set(BUILD_TESTING ON)
set(SPOOL_SPLASH_DIR "${CMAKE_CURRENT_BINARY_DIR}/splash")
file(MAKE_DIRECTORY "${SPOOL_SPLASH_DIR}/res")

# Configure the real packaging functions on a host without Android Qt or an NDK.
set(stub_source "${CMAKE_CURRENT_BINARY_DIR}/stub.cpp")
file(WRITE "${stub_source}" "int main() { return 0; }\n")
add_library(MPV::MPV INTERFACE IMPORTED)
add_library(Qt6::Core INTERFACE IMPORTED)
add_library(spool-core STATIC EXCLUDE_FROM_ALL "${stub_source}")
function(qt_add_executable target)
    add_executable(${target} EXCLUDE_FROM_ALL "${stub_source}")
endfunction()
function(qt_add_resources)
endfunction()
function(target_sources target)
    _target_sources(${target} PRIVATE "${stub_source}")
endfunction()

include(cmake/platform/Android.cmake)
include(tests/platform/MobileTestBundles.cmake)
qt_add_executable(spool)
spool_configure_android_targets(spool spool-core)
spool_add_mobile_test_bundles()
set_property(TARGET spool-mobile-test-fixtures PROPERTY EXCLUDE_FROM_ALL TRUE)

# androiddeployqt reads this field to set the APK's Gradle minSdkVersion.
foreach(target IN ITEMS spool spool-tests spool-e2e-tests)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/android-${target}-deployment-settings.json"
        CONTENT "{\"android-min-sdk-version\": \"$<TARGET_PROPERTY:${target},QT_ANDROID_MIN_SDK_VERSION>\"}\n")
endforeach()
