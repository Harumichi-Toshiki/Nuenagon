cmake_minimum_required(VERSION 3.16)
project(Nuenagon LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 単一構成のジェネレータ（Makefile など）では既定を Release にする
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_executable(nuenagon
  src/main.cpp
  src/board/game.cpp
  src/eval/raypair/raypair.cpp
)
target_include_directories(nuenagon PRIVATE src)

if(MSVC)
  target_compile_options(nuenagon PRIVATE /utf-8 /W3)
else()
  target_compile_options(nuenagon PRIVATE -Wall -Wextra)
endif()
