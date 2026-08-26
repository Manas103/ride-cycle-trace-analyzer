cmake_minimum_required(VERSION 3.15)
project(ride_cycle_trace_analyzer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
  add_compile_options(/O2 /W4 /EHsc)
else()
  add_compile_options(-O3 -Wall -Wextra)
endif()

include_directories(include)

add_executable(trace_tests tests/cpp/test_main.cpp)
add_executable(trace_parser_cli src/trace_parser_cli.cpp)

# The WPF app's P/Invoke target. Windows-only: __declspec(dllexport) and
# __stdcall are MSVC/Windows conventions, and there is no P/Invoke caller
# to build this for under WSL2/GCC.
if(WIN32)
  add_library(TraceEngineNative SHARED src/native_interop.cpp)
endif()

option(ENABLE_SANITIZERS "Build the check suite with ASan and UBSan" OFF)
if(ENABLE_SANITIZERS)
  if(MSVC)
    message(FATAL_ERROR "ENABLE_SANITIZERS is for the GCC/Clang build; see README")
  endif()
  add_executable(trace_tests_asan tests/cpp/test_main.cpp)
  target_compile_options(trace_tests_asan PRIVATE -fsanitize=address,undefined
                         -fno-omit-frame-pointer -g -O1)
  target_link_options(trace_tests_asan PRIVATE -fsanitize=address,undefined)
endif()
