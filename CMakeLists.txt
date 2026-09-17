cmake_minimum_required(VERSION 3.28)
project(remy LANGUAGES CXX)

option(DEBUG_OUT "Print debug output" OFF)

message(STATUS "DEBUG_OUT = ${DEBUG_OUT}")

if("${GIT_REVISION}" STREQUAL "")
  set(GIT_REVISION "$ENV{GIT_REVISION}")
endif()
find_package(Git QUIET)
if(GIT_FOUND)
  if("${GIT_REVISION}" STREQUAL "")
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_SOURCE_DIR}" show -s "--format=%h %as \"%s\""
      OUTPUT_VARIABLE GIT_REVISION
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
      RESULT_VARIABLE GIT_RESULT
    )
  endif()
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_SOURCE_DIR}" status -s
    OUTPUT_VARIABLE GIT_STATUS
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE GIT_RESULT
  )
  if(NOT "${GIT_STATUS}" STREQUAL "")
    string(APPEND GIT_REVISION " (dirty)")
  endif()
endif()
if(NOT "${GIT_REVISION}" STREQUAL "")
  string(REPLACE "\"" "\\\"" GIT_REVISION ${GIT_REVISION})
  add_compile_definitions(GIT_REVISION="${GIT_REVISION}")
  message(STATUS "GIT_REVISION = \"${GIT_REVISION}\"")
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

