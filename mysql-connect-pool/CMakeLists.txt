cmake_minimum_required(VERSION 3.14)
project(mysql_connect_pool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)

add_executable(pool_demo main.cpp)
target_link_libraries(pool_demo PRIVATE Threads::Threads)

if(MSVC)
    target_compile_options(pool_demo PRIVATE /W4)
else()
    target_compile_options(pool_demo PRIVATE -Wall -Wextra)
endif()
