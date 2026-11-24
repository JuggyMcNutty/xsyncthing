# xsyncthing: one project, two builds.
#   Host (Linux):  cmake -S . -B build-host -G Ninja && cmake --build build-host && ctest --test-dir build-host
#   Console:       cmake -S . -B build-xsdk -G Ninja -DCMAKE_TOOLCHAIN_FILE=third_party/xsdk/cmake/xsdk.cmake
# The xsdk toolchain file sets XSDK; code tells the targets apart by __XSDK__.
cmake_minimum_required(VERSION 3.25)
project(xsyncthing VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_C_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(XST_TITLE_ID 0x58535943 CACHE STRING "The .xex title ID (\"XSYC\")")
set(XST_WARNINGS -Wall -Wextra -Werror)

configure_file(src/core/version.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/xst/version.h" @ONLY)

include(cmake/wolfssl.cmake)

# The portable core: no xsdk calls, builds on the host and the console alike.
add_library(xst_core STATIC
	src/core/base32.cpp
	src/core/device_id.cpp
	src/core/hello.cpp
	src/core/protowire.cpp
	src/core/sha256.cpp
	src/core/version.cpp
	src/net/bep_handshake.cpp
	src/net/identity.cpp
	src/net/tcp.cpp
	src/net/tls.cpp
)
target_include_directories(xst_core PUBLIC src "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_options(xst_core PRIVATE ${XST_WARNINGS})
target_link_libraries(xst_core PUBLIC xst_wolfssl)

if(XSDK)
	add_library(xst_platform STATIC src/platform/xsdk/platform.cpp)
	target_link_libraries(xst_platform PUBLIC xst_core)
	target_compile_options(xst_platform PRIVATE ${XST_WARNINGS})

	add_executable(xsyncthing src/main_xsdk.cpp)
	target_link_libraries(xsyncthing PRIVATE xst_platform xst_core)
	target_compile_options(xsyncthing PRIVATE ${XST_WARNINGS})
	xsdk_add_xex(xsyncthing TITLE_ID ${XST_TITLE_ID} NAME "xsyncthing" VERSION ${PROJECT_VERSION}.0)

	# The on-console checks (tests/console): wolfCrypt's suite, identity, and a handshake with a peer.
	add_executable(xst-console-test tests/console/console_test.cpp)
	target_link_libraries(xst-console-test PRIVATE xst_wolfcrypt_test xst_platform xst_core)
	target_compile_options(xst-console-test PRIVATE ${XST_WARNINGS})
	xsdk_add_xex(xst-console-test TITLE_ID 0x58535954 NAME "xsyncthing tests" VERSION ${PROJECT_VERSION}.0)
else()
	add_library(xst_platform STATIC src/platform/posix/platform.cpp)
	target_link_libraries(xst_platform PUBLIC xst_core)
	target_compile_options(xst_platform PRIVATE ${XST_WARNINGS})

	add_executable(xsyncthing src/main_host.cpp)
	target_link_libraries(xsyncthing PRIVATE xst_platform xst_core)
	target_compile_options(xsyncthing PRIVATE ${XST_WARNINGS})

	add_executable(xst-handshake tools/handshake.cpp)
	target_link_libraries(xst-handshake PRIVATE xst_core)
	target_compile_options(xst-handshake PRIVATE ${XST_WARNINGS})

	add_executable(xst-wolfcrypt-test tools/wolfcrypt_test_main.c)
	target_link_libraries(xst-wolfcrypt-test PRIVATE xst_wolfcrypt_test xst_wolfssl)

	include(CTest)
	if(BUILD_TESTING)
		add_subdirectory(tests/unit)
	endif()
endif()
