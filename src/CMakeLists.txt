CPMAddPackage(URI "https://github.com/gflags/gflags.git@2.3.0")

CPMAddPackage(
    NAME aws-sdk-cpp
    GITHUB_REPOSITORY aws/aws-sdk-cpp
    GIT_TAG 1.11.799
    GIT_SUBMODULES_RECURSE TRUE
    OPTIONS
        "BUILD_ONLY s3"
        "BUILD_SHARED_LIBS OFF"
        "ENABLE_TESTING OFF"
        "AUTORUN_UNIT_TESTS OFF"
        "ENABLE_UNITY_BUILD ON"
        "USE_CRT_HTTP_CLIENT ON"
)

pkg_check_modules(FUSE3 REQUIRED IMPORTED_TARGET fuse3)

add_executable(pulpfs
  main.cc
  meta_server.cc
  meta_service.cc
  meta_state.cc
  fuse/meta_client.cc
  fuse/fuse_server.cc
  gc_service.cc
  error.cc
  object/s3_store.cc
)

target_compile_definitions(pulpfs PRIVATE
    PULPFS_SOURCE_ROOT="${PROJECT_SOURCE_DIR}/"
    SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE
)

target_include_directories(pulpfs PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(pulpfs PRIVATE "${aws-sdk-cpp_SOURCE_DIR}/src/aws-cpp-sdk-core/include")

target_link_libraries(pulpfs PRIVATE
    gflags::gflags
    spdlog::spdlog
    pulpfs_proto
    raftor
    aws-cpp-sdk-s3
    aws-cpp-sdk-core
    PkgConfig::FUSE3
)
