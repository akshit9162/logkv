cmake_minimum_required(VERSION 3.16)
project(logkv LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

option(LOGKV_SANITIZE "Build with address + undefined sanitizers" OFF)
if(LOGKV_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()

add_library(logkv STATIC src/store.cpp)
target_include_directories(logkv PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

enable_testing()

add_executable(test_store tests/test_store.cpp)
target_link_libraries(test_store PRIVATE logkv)
add_test(NAME store COMMAND test_store)

add_executable(bench bench/bench.cpp)
target_link_libraries(bench PRIVATE logkv)

add_executable(crashwriter tools/crashwriter.cpp)
target_link_libraries(crashwriter PRIVATE logkv)

add_executable(verify tools/verify.cpp)
target_link_libraries(verify PRIVATE logkv)
