cmake_minimum_required(VERSION 3.28)
project(remy LANGUAGES CXX)

option(DEBUG_OUT "Print debug output" OFF)

message(STATUS "DEBUG_OUT = ${DEBUG_OUT}")

find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_SOURCE_DIR}" show -s "--format=%h %as \"%s\""
    OUTPUT_VARIABLE GIT_REVISION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE GIT_RESULT
  )
  if(GIT_RESULT EQUAL 0)
    string(REPLACE "\"" "\\\"" GIT_REVISION ${GIT_REVISION})
    add_compile_definitions(GIT_REVISION="${GIT_REVISION}")
    message(STATUS "GIT_REVISION = \"${GIT_REVISION}\"")
  endif()
endif()

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

