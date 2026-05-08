find_package(Protobuf REQUIRED)
find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)

pkg_check_modules(GRPC REQUIRED IMPORTED_TARGET grpc++)

set(PULPFS_PROTOBUF_LIBRARIES ${Protobuf_LIBRARIES})
if(TARGET Protobuf::libprotobuf)
    set(PULPFS_PROTOBUF_LIBRARIES Protobuf::libprotobuf)
endif()

add_library(pulpfs_proto pulpfs.proto)
target_include_directories(pulpfs_proto PUBLIC "${CMAKE_CURRENT_BINARY_DIR}" ${Protobuf_INCLUDE_DIRS})
target_link_libraries(pulpfs_proto PUBLIC ${PULPFS_PROTOBUF_LIBRARIES} PkgConfig::GRPC)

protobuf_generate(
  TARGET pulpfs_proto
  LANGUAGE cpp
)

protobuf_generate(
    TARGET pulpfs_proto
    LANGUAGE grpc
    GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
    PLUGIN "protoc-gen-grpc=${GRPC_CPP_PLUGIN}"
)
