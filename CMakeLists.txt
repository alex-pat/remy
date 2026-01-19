cmake_minimum_required(VERSION 3.31) # Possibly older
project(remy LANGUAGES CXX)

option(DEBUG_OUT "Print debug output" OFF)

message(STATUS "DEBUG_OUT = ${DEBUG_OUT}")

add_definitions(-Werror -pedantic)

if(DEBUG_OUT)
    add_definitions(-DDEBUG_OUT)
endif()

include_directories(${PROJECT_SOURCE_DIR}/include)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(Boost_USE_STATIC_LIBS ON)
find_package(Boost REQUIRED COMPONENTS coroutine program_options)
include_directories(${Boost_INCLUDE_DIRS})

add_subdirectory(src)

