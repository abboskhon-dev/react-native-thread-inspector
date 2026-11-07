cmake_minimum_required(VERSION 3.13)
project(rnti CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../cpp/core)

add_library(rnti SHARED
  src/main/cpp/ThreadInspectorJni.cpp
  ${CORE_DIR}/Analysis.cpp
  ${CORE_DIR}/EventRing.cpp
  ${CORE_DIR}/Export.cpp
  ${CORE_DIR}/IncidentController.cpp
  ${CORE_DIR}/IncidentWriter.cpp
  ${CORE_DIR}/Inspector.cpp
  ${CORE_DIR}/Recorder.cpp
  ${CORE_DIR}/Watchdog.cpp
)
target_include_directories(rnti PRIVATE ${CORE_DIR})
target_compile_options(rnti PRIVATE -Wall -O2 -fvisibility=hidden)
target_link_libraries(rnti log)
