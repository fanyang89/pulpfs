cmake_minimum_required(VERSION 3.28)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(pulpfs VERSION 0.1.0 DESCRIPTION "" LANGUAGES C CXX)

enable_testing()

set(CPM_SOURCE_CACHE "${CMAKE_CURRENT_SOURCE_DIR}/.cache/cpm" CACHE STRING "CPM cache path")

find_package(PkgConfig REQUIRED)

add_subdirectory(third_party)
add_subdirectory(proto)
add_subdirectory(src)
add_subdirectory(tests)
