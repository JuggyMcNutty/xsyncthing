# Host unit tests: a minimal self-contained runner, so tests need no third-party framework.
add_executable(xst_unit_tests main.cpp test_version.cpp)
target_link_libraries(xst_unit_tests PRIVATE xst_core)
target_compile_options(xst_unit_tests PRIVATE ${XST_WARNINGS})
add_test(NAME unit COMMAND xst_unit_tests)
