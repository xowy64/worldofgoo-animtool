cmake_minimum_required(VERSION 3.21...4.2)
set(CMAKE_CXX_STANDARD 23)

project(animtool VERSION 1.0 LANGUAGES CXX)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_subdirectory(external)

file(GLOB_RECURSE SOURCES "src/*.cpp" "src/*.h")

add_executable(animtool ${SOURCES})

target_include_directories(animtool PRIVATE
    ${CMAKE_SOURCE_DIR}/src/Animation/
    ${CMAKE_SOURCE_DIR}/src/Animation/movie/
    ${CMAKE_SOURCE_DIR}/src/Convert/
    ${CMAKE_SOURCE_DIR}/src/XML/
    ${CMAKE_SOURCE_DIR}/src/)

set(static_flag -static)

target_link_libraries(animtool PRIVATE tinyxml2)
target_link_options(animtool PRIVATE ${static_flag})
