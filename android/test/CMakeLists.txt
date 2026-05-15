cmake_minimum_required(VERSION 3.14)
project(sc2d VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SOURCES
    main.cpp
    HidDevice.cpp
    SteamController.cpp
    VirtualGamepad.cpp
)

add_executable(sc2d ${SOURCES})
target_include_directories(sc2d PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(ANDROID)
    target_link_libraries(sc2d PRIVATE log)
    target_compile_options(sc2d PRIVATE -Wall)
else()
    target_link_libraries(sc2d PRIVATE pthread)
    target_compile_options(sc2d PRIVATE -Wall -Wpedantic)
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
