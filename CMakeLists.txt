cmake_minimum_required(VERSION 3.15)
project(pyterse_and_tersecodec LANGUAGES CXX)


option(BUILD_PYTERSE "Build the pyterse Python extension" ON)
option(BUILD_TERSECODEC "Build the HDF5 filter library (tersecodec) plugin" ON)
option(BUILD_BENCHMARKS "Build the micro benchmarks of compression and decompression" OFF)


set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/AlignCode.cmake)


if(BUILD_PYTERSE)
  add_subdirectory(pyterse)
endif()

if(BUILD_TERSECODEC)
  add_subdirectory(tersecodec)
endif()

if(BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
