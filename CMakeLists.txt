cmake_minimum_required(VERSION 3.16)
project(orderbook)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(orderbook
    src/main.cpp
    src/orderbook.cpp
)

target_include_directories(orderbook PRIVATE include third_party/unordered_dense/include)
target_compile_options(orderbook PRIVATE -O3 -march=native)
target_link_libraries(orderbook PRIVATE pthread)