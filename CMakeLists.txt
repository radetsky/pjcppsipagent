cmake_minimum_required(VERSION 3.16)
project(pjcppagent VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Warnings as errors for clean builds
add_compile_options(-Wall -Wextra -Werror -pedantic)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PJPROJECT REQUIRED libpjproject)

# pjproject is static; need all transitive deps from Libs.private
execute_process(
  COMMAND pkg-config --static --libs libpjproject
  OUTPUT_VARIABLE PJPROJECT_STATIC_LIBS
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
separate_arguments(PJPROJECT_STATIC_LIBS)

# gRPC: prefer CONFIG, fall back to pkg_check_modules
if(APPLE)
  set(gRPC_DIR "/opt/homebrew/lib/cmake/grpc" CACHE PATH "gRPC cmake config dir")
endif()
find_package(gRPC QUIET CONFIG)
if(NOT gRPC_FOUND)
  pkg_check_modules(GRPC REQUIRED grpc++)
endif()

# Use matching protoc if available (protobuf 5.29 vs @3 3.20)
# On macOS Homebrew the non-@3 version is at /opt/homebrew/opt/protobuf/bin/protoc
if(APPLE AND EXISTS "/opt/homebrew/opt/protobuf/bin/protoc")
  set(Protobuf_PROTOC_EXECUTABLE "/opt/homebrew/opt/protobuf/bin/protoc" CACHE FILEPATH "protoc from the main protobuf package" FORCE)
endif()

find_package(Protobuf REQUIRED)

option(BUILD_TESTS ON)

include(FetchContent)

# FetchContent sections
FetchContent_Declare(CLI11
  GIT_REPOSITORY  https://github.com/CLIUtils/CLI11.git
  GIT_TAG         v2.6.2
  GIT_SHALLOW     TRUE
  DOWNLOAD_ONLY   True
)
FetchContent_MakeAvailable(CLI11)
list(APPEND CLI11_INCLUDE_DIRS ${cli11_SOURCE_DIR}/include)

FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY  https://github.com/nlohmann/json.git
  GIT_TAG         v3.11.3
  GIT_SHALLOW     TRUE
  DOWNLOAD_ONLY   True
)
FetchContent_MakeAvailable(nlohmann_json)
list(APPEND nlohmann_json_INCLUDE_DIRS ${nlohmann_json_SOURCE_DIR}/include)

if(BUILD_TESTS)
  FetchContent_Declare(googletest
    GIT_REPOSITORY  https://github.com/google/googletest.git
    GIT_TAG         v1.14.0
    GIT_SHALLOW     TRUE
  )
  set(GTEST_HAS_PTHREAD ON CACHE BOOL "" FORCE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

# ---------------------------------------------------------------------------
# Proto code generation (M1)
# ---------------------------------------------------------------------------
set(PROTO_FILE protos/call_agent.proto)
set(PROTO_GEN_DIR ${CMAKE_BINARY_DIR}/gen)

find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin)

file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

add_custom_command(
  OUTPUT
    ${PROTO_GEN_DIR}/call_agent.pb.cc
    ${PROTO_GEN_DIR}/call_agent.pb.h
    ${PROTO_GEN_DIR}/call_agent.grpc.pb.cc
    ${PROTO_GEN_DIR}/call_agent.grpc.pb.h
  COMMAND ${Protobuf_PROTOC_EXECUTABLE}
    --proto_path=${CMAKE_SOURCE_DIR}/protos
    --cpp_out=${PROTO_GEN_DIR}
    --grpc_out=${PROTO_GEN_DIR}
    --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
    ${CMAKE_SOURCE_DIR}/${PROTO_FILE}
  DEPENDS ${CMAKE_SOURCE_DIR}/${PROTO_FILE}
  COMMENT "Generating C++ code from call_agent.proto"
)

add_library(proto_gen STATIC
  ${PROTO_GEN_DIR}/call_agent.pb.cc
  ${PROTO_GEN_DIR}/call_agent.grpc.pb.cc
)
target_include_directories(proto_gen PUBLIC ${PROTO_GEN_DIR})
target_include_directories(proto_gen PRIVATE ${Protobuf_INCLUDE_DIR})
target_link_libraries(proto_gen PRIVATE
  ${Protobuf_LIBRARY}
  gRPC::grpc++
  gRPC::grpc++_unsecure
)

# agent_core static library (args, silence, events, tts_stub - no PJSIP deps)
add_library(agent_core STATIC
  src/args.cpp
  src/silence.cpp
  src/events.cpp
  src/tts_stub.cpp
)
target_include_directories(agent_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CLI11_INCLUDE_DIRS}
  ${nlohmann_json_INCLUDE_DIRS}
)
target_link_libraries(agent_core PRIVATE nlohmann_json::nlohmann_json)

# pjcppagent binary (sip_agent and call_executor use PJSIP, excluded from agent_core)
add_executable(pjcppagent src/main.cpp src/sip_agent.cpp src/call_executor.cpp)
target_include_directories(pjcppagent BEFORE PRIVATE
  ${PROTO_GEN_DIR}
)
target_include_directories(pjcppagent PRIVATE
  ${Protobuf_INCLUDE_DIR}
)
target_include_directories(pjcppagent SYSTEM PRIVATE
  ${PJPROJECT_INCLUDE_DIRS}
)
target_compile_options(pjcppagent PRIVATE ${PJPROJECT_CFLAGS_OTHER})
target_link_directories(pjcppagent PRIVATE
  ${PJPROJECT_LIBRARY_DIRS}
  /opt/homebrew/lib
)
target_link_libraries(pjcppagent PRIVATE
  agent_core
  proto_gen
  ${Protobuf_LIBRARY}
  gRPC::grpc++
  gRPC::grpc++_unsecure
  ${PJPROJECT_LIBRARIES}
  # -static --libs private deps
  resample-aarch64-apple-darwin24.5.0
  gsmcodec-aarch64-apple-darwin24.5.0
  speex-aarch64-apple-darwin24.5.0
  ilbccodec-aarch64-apple-darwin24.5.0
  g7221codec-aarch64-apple-darwin24.5.0
  yuv-aarch64-apple-darwin24.5.0
  webrtc-aarch64-apple-darwin24.5.0
  opus
  SDL2
  avdevice avformat avcodec swscale avutil
  opencore-amrnb opencore-amrwb
  "-framework CoreAudio"
  "-framework CoreServices"
  "-framework AudioUnit"
  "-framework AudioToolbox"
  "-framework Foundation"
  "-framework AppKit"
  "-framework AVFoundation"
  "-framework CoreGraphics"
  "-framework QuartzCore"
  "-framework CoreVideo"
  "-framework CoreMedia"
  "-framework Metal"
  "-framework MetalKit"
  "-framework VideoToolbox"
)

# Unit tests
if(BUILD_TESTS)
  enable_testing()
  add_executable(unit_tests
    tests/unit/test_args.cpp
    tests/unit/test_silence.cpp
    tests/unit/test_events.cpp
  )
  target_include_directories(unit_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CLI11_INCLUDE_DIRS}
    ${nlohmann_json_INCLUDE_DIRS}
    ${gtest_SOURCE_DIR}/include
    ${gmock_SOURCE_DIR}/include
  )
  target_link_libraries(unit_tests
    agent_core
    GTest::gtest_main
    GTest::gmock_main
  )

  include(GoogleTest)
  gtest_discover_tests(unit_tests)
endif()
