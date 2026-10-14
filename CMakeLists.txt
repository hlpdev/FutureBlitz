cmake_minimum_required(VERSION 3.28)
project(FutureBlitz VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)
include(cmake/warnings.cmake)

add_subdirectory(compiler)
add_subdirectory(runtime)
add_subdirectory(cli)
add_subdirectory(lsp)
