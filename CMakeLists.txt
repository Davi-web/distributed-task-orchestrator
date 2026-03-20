cmake_minimum_required(VERSION 3.16)
project(distributed-task-orchestrator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ─────────────────────────────────────────────
# Find dependencies
# ─────────────────────────────────────────────

find_package(Threads REQUIRED)
find_package(Protobuf REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(GRPC REQUIRED grpc++)

# Find grpc_cpp_plugin
find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin
    HINTS /usr/bin /usr/local/bin
          $ENV{HOME}/vcpkg/installed/x64-windows/tools/grpc
          $ENV{VCPKG_ROOT}/installed/x64-windows/tools/grpc
)
if(NOT GRPC_CPP_PLUGIN)
    message(FATAL_ERROR "grpc_cpp_plugin not found. Install gRPC or set GRPC_CPP_PLUGIN.")
endif()

message(STATUS "Protobuf: ${Protobuf_VERSION}")
message(STATUS "gRPC plugin: ${GRPC_CPP_PLUGIN}")

# ─────────────────────────────────────────────
# Generate protobuf + gRPC sources
# ─────────────────────────────────────────────

set(PROTO_FILE "${CMAKE_SOURCE_DIR}/proto/orchestrator.proto")
set(PROTO_GEN_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

set(PROTO_SRCS "${PROTO_GEN_DIR}/orchestrator.pb.cc")
set(PROTO_HDRS "${PROTO_GEN_DIR}/orchestrator.pb.h")
set(GRPC_SRCS  "${PROTO_GEN_DIR}/orchestrator.grpc.pb.cc")
set(GRPC_HDRS  "${PROTO_GEN_DIR}/orchestrator.grpc.pb.h")

add_custom_command(
    OUTPUT ${PROTO_SRCS} ${PROTO_HDRS} ${GRPC_SRCS} ${GRPC_HDRS}
    COMMAND protobuf::protoc
        --proto_path=${CMAKE_SOURCE_DIR}/proto
        --cpp_out=${PROTO_GEN_DIR}
        --grpc_out=${PROTO_GEN_DIR}
        --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
        ${PROTO_FILE}
    DEPENDS ${PROTO_FILE}
    COMMENT "Generating protobuf and gRPC sources"
)

# Library with generated sources
add_library(proto_lib STATIC ${PROTO_SRCS} ${GRPC_SRCS})
target_include_directories(proto_lib PUBLIC ${PROTO_GEN_DIR})
target_link_libraries(proto_lib PUBLIC
    protobuf::libprotobuf
    ${GRPC_LIBRARIES}
)
target_include_directories(proto_lib PUBLIC ${GRPC_INCLUDE_DIRS})

# ─────────────────────────────────────────────
# Coordinator
# ─────────────────────────────────────────────

add_executable(coordinator
    coordinator/main.cpp
    coordinator/services.cpp
)
target_include_directories(coordinator PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${PROTO_GEN_DIR}
)
target_link_libraries(coordinator PRIVATE
    proto_lib
    Threads::Threads
)

# ─────────────────────────────────────────────
# Worker
# ─────────────────────────────────────────────

add_executable(worker
    worker/main.cpp
)
target_include_directories(worker PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${PROTO_GEN_DIR}
)
target_link_libraries(worker PRIVATE
    proto_lib
    Threads::Threads
)

# ─────────────────────────────────────────────
# Client
# ─────────────────────────────────────────────

add_executable(client
    client/main.cpp
)
target_include_directories(client PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${PROTO_GEN_DIR}
)
target_link_libraries(client PRIVATE
    proto_lib
    Threads::Threads
)
