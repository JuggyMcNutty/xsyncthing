# Host unit tests: a minimal self-contained runner, so tests need no third-party framework.
add_executable(xst_unit_tests main.cpp test_version.cpp test_device_id.cpp test_hello.cpp test_identity.cpp test_bep.cpp test_vector_ignore.cpp test_sync.cpp test_discovery.cpp test_relay.cpp)
target_link_libraries(xst_unit_tests PRIVATE xst_platform xst_core xst_platform)
target_sources(xst_unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/app.cpp)
target_compile_options(xst_unit_tests PRIVATE ${XST_WARNINGS})
add_test(NAME unit COMMAND xst_unit_tests)
target_compile_definitions(xst_unit_tests PRIVATE XST_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden")
