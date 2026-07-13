cmake_minimum_required(VERSION 3.16)
project(lob_engine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")

include_directories(${CMAKE_SOURCE_DIR}/include)

find_package(Threads REQUIRED)

add_executable(bench src/bench.cpp)
add_executable(replay src/replay.cpp)
add_executable(bench_mt src/bench_mt.cpp)
target_link_libraries(bench_mt PRIVATE Threads::Threads)
add_executable(match_bench src/match_bench.cpp)
add_executable(signal_bench src/signal_bench.cpp)
add_executable(backtest src/backtest.cpp)
add_executable(market_maker src/market_maker.cpp)
add_executable(test_book tests/test_book.cpp)
add_executable(test_match tests/test_match.cpp)

enable_testing()
add_test(NAME correctness COMMAND test_book)
add_test(NAME matching COMMAND test_match)
