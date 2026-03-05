cmake_minimum_required(VERSION 3.20)
project(spsc_cam_analyzer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_executable(app
  src/main.cpp
  src/globals.cpp
  src/locked_block_pool.cpp
  src/camera_emulator.cpp
  src/analyzer.cpp
)

target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(app PRIVATE -O2 -Wall -Wextra -Wpedantic)
target_link_libraries(app PRIVATE Threads::Threads)