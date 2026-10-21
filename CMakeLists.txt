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
  src/eval/raypair/network.cpp
  src/search/search.cpp
)
find_package(Threads REQUIRED)
target_link_libraries(nuenagon PRIVATE Threads::Threads)
target_include_directories(nuenagon PRIVATE src)

# SIMD（AVX2）を使うか。ON なら AVX2 のコードを使い、OFF なら SIMD を使わない、どの x86-64 でも動くコードにする。
# 結果は両方で同じになる（tests/test_n5_acceptance.py が確かめる）。
option(NUENAGON_SIMD "AVX2 を使う（OFF なら SIMD を使わない）" ON)
if(NUENAGON_SIMD)
  target_compile_definitions(nuenagon PRIVATE NUENAGON_SIMD_AVX2=1)
  if(MSVC)
    target_compile_options(nuenagon PRIVATE /arch:AVX2)
  else()
    target_compile_options(nuenagon PRIVATE -mavx2)
  endif()
else()
  target_compile_definitions(nuenagon PRIVATE NUENAGON_SIMD_AVX2=0)
endif()

if(MSVC)
  target_compile_options(nuenagon PRIVATE /utf-8 /W3)
else()
  target_compile_options(nuenagon PRIVATE -Wall -Wextra)
endif()
