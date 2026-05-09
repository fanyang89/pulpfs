CPMAddPackage(URI "https://github.com/google/googletest.git@1.17.0")

include(GoogleTest)

add_executable(pulpfs_unit_tests
  fuse_file_io_test.cc
  meta_state_test.cc
)

target_link_libraries(pulpfs_unit_tests PRIVATE
  pulpfs_core
  GTest::gtest_main
)

gtest_discover_tests(pulpfs_unit_tests)
