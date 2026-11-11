find_package(Threads REQUIRED)

# Standalone benchmark without dependencies; also checks the round trips and prints hashes of the compressed data.
add_executable(terse_benchmark terse_benchmark.cpp)
target_include_directories(terse_benchmark PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_features(terse_benchmark PRIVATE cxx_std_20)
target_link_libraries(terse_benchmark PRIVATE Threads::Threads)

# Google Benchmark version, built if Google Benchmark is found (set benchmark_DIR or CMAKE_PREFIX_PATH if it is not
# installed in a standard location).
find_package(benchmark QUIET)
if(benchmark_FOUND)
  message(STATUS "Google Benchmark ${benchmark_VERSION} found: building terse_gbenchmark")
  add_executable(terse_gbenchmark terse_gbenchmark.cpp)
  target_include_directories(terse_gbenchmark PRIVATE ${PROJECT_SOURCE_DIR}/include)
  target_compile_features(terse_gbenchmark PRIVATE cxx_std_20)
  target_link_libraries(terse_gbenchmark PRIVATE benchmark::benchmark Threads::Threads)
else()
  message(STATUS "Google Benchmark not found: terse_gbenchmark is not built")
endif()
