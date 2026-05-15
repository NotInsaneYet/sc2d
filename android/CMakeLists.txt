cmake_minimum_required(VERSION 3.14)
project(sc2d VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------

set(SOURCES
    main.cpp
    HidDevice.cpp
    SteamController.cpp
    VirtualGamepad.cpp
)

# PassthroughDevice — local-only RP5 built-in controller support
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/PassthroughDevice.cpp)
    list(APPEND SOURCES PassthroughDevice.cpp)
    add_definitions(-DHAS_PASSTHROUGH)
endif()

# ---------------------------------------------------------------------------
# Target
# ---------------------------------------------------------------------------

add_executable(sc2d ${SOURCES})

target_include_directories(sc2d PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# ---------------------------------------------------------------------------
# Android NDK: link libc++_shared, enable logging
# ---------------------------------------------------------------------------

if(ANDROID)
    target_link_libraries(sc2d PRIVATE log)
    target_compile_options(sc2d PRIVATE -Wall -Wextra)
    # Install to /data/local/tmp for testing with adb
    install(TARGETS sc2d DESTINATION ${CMAKE_INSTALL_PREFIX}/bin)
else()
    # Linux desktop — link pthread for std::thread
    target_link_libraries(sc2d PRIVATE pthread)
    target_compile_options(sc2d PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# Build type defaults
# ---------------------------------------------------------------------------

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
