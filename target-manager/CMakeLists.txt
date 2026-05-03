cmake_minimum_required(VERSION 3.22)
project(target_manager CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(DEEPSTREAM_DIR "/opt/nvidia/deepstream/deepstream" CACHE PATH
    "DeepStream install root (symlink to deepstream-7.1 on host and in container)")

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB      REQUIRED IMPORTED_TARGET glib-2.0)
pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET gstreamer-1.0)
pkg_check_modules(CURL      REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(PROTOBUF  REQUIRED IMPORTED_TARGET protobuf)

# Pin to apt's protoc; NVIDIA's DS container ships a newer /opt/proto/bin/protoc
# whose generated code requires libprotobuf >= 3.21, but apt's libprotobuf is 3.12.
find_program(PROTOC NAMES protoc PATHS /usr/bin NO_DEFAULT_PATH REQUIRED)

set(PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../wss" CACHE PATH "Directory containing .proto files")
set(PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${PROTO_GEN_DIR}")

file(GLOB PROTO_FILES CONFIGURE_DEPENDS "${PROTO_DIR}/*.proto")

set(PROTO_GEN_SRCS "")
foreach(proto ${PROTO_FILES})
    get_filename_component(proto_name "${proto}" NAME_WE)
    set(gen_cc "${PROTO_GEN_DIR}/${proto_name}.pb.cc")
    set(gen_h  "${PROTO_GEN_DIR}/${proto_name}.pb.h")
    add_custom_command(
        OUTPUT  "${gen_cc}" "${gen_h}"
        COMMAND "${PROTOC}" --cpp_out=${PROTO_GEN_DIR} -I${PROTO_DIR} "${proto}"
        DEPENDS "${proto}"
        COMMENT "protoc ${proto_name}.proto"
        VERBATIM
    )
    list(APPEND PROTO_GEN_SRCS "${gen_cc}")
endforeach()

add_library(target_manager STATIC
    target_manager.cpp
    geolocation.cpp
    ${PROTO_GEN_SRCS}
)

target_include_directories(target_manager
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${DEEPSTREAM_DIR}/sources/includes
    PRIVATE
        ${PROTO_GEN_DIR}
)

target_link_libraries(target_manager
    PUBLIC
        PkgConfig::GLIB
        PkgConfig::GSTREAMER
    PRIVATE
        PkgConfig::CURL
        PkgConfig::PROTOBUF
)

target_compile_options(target_manager PRIVATE -Wall -Wextra -Wno-unused-parameter)
