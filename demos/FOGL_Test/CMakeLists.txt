cmake_minimum_required(VERSION 3.10)

# -----------------------------------------------------------------------------
#
# Project settings
#
# -----------------------------------------------------------------------------
project(FOGL_Test)

set(CMAKE_C_FLAGS "-std=c99")

set(MY_EXTERNAL_LIBS "")

set(MY_HEADER_FILES ../../final_platform_layer.h)
set(MY_HEADER_FILES ../../final_dynamic_opengl.h)

set(MY_INCLUDE_DIRS "")

set(MY_SOURCE_FILES fogl_test.c)

# -----------------------------------------------------------------------------
#
# Do not change the following lines
#
# -----------------------------------------------------------------------------

set(MY_EXECUTABLE_NAME ${PROJECT_NAME})
set(MY_EXECUTABLE_PATH ../../${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}-${CMAKE_BUILD_TYPE}/)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${MY_EXECUTABLE_PATH})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${MY_EXECUTABLE_PATH})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${MY_EXECUTABLE_PATH})

include_directories(../../ ${MY_INCLUDE_DIRS})

add_executable(${PROJECT_NAME} ${MY_HEADER_FILES} ${MY_SOURCE_FILES})

target_link_libraries(${PROJECT_NAME} ${MY_EXTERNAL_LIBS} ${CMAKE_DL_LIBS})
