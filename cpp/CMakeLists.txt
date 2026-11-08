cmake_minimum_required(VERSION 3.20)
project(rnti_core CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(RNTI_TSAN "Build with ThreadSanitizer" OFF)
option(RNTI_ASAN "Build with AddressSanitizer and UBSan" OFF)

if(RNTI_TSAN)
  add_compile_options(-fsanitize=thread -g -O1)
  add_link_options(-fsanitize=thread)
endif()
if(RNTI_ASAN)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)
  add_link_options(-fsanitize=address,undefined)
endif()

add_library(rnti_core STATIC
  core/Analysis.cpp
  core/EventRing.cpp
  core/Export.cpp
  core/IncidentController.cpp
  core/IncidentWriter.cpp
  core/Inspector.cpp
  core/Recorder.cpp
  core/TurboModuleTracker.cpp
  core/Watchdog.cpp
)
target_include_directories(rnti_core PUBLIC core)
target_compile_options(rnti_core PRIVATE -Wall -Wextra -Wpedantic)

include(CTest)
if(BUILD_TESTING)
  find_package(Threads REQUIRED)
  add_executable(rnti_tests tests/main.cpp)
  target_link_libraries(rnti_tests PRIVATE rnti_core Threads::Threads)
  target_compile_options(rnti_tests PRIVATE -Wall -Wextra)
  add_test(NAME rnti_tests COMMAND rnti_tests)
endif()
