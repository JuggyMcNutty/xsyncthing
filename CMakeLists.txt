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

# SQLite (third_party/sqlite, the 3.54.1 amalgamation): the index database. On the console it runs on xsdk's POSIX
# file calls with no locking (one process), threadsafe (several threads), and a small page cache.
add_library(xst_sqlite STATIC third_party/sqlite/sqlite3.c)
target_include_directories(xst_sqlite SYSTEM PUBLIC third_party/sqlite)
target_compile_definitions(xst_sqlite PRIVATE SQLITE_THREADSAFE=1 SQLITE_DEFAULT_MEMSTATUS=0 SQLITE_OMIT_LOAD_EXTENSION
	SQLITE_DQS=0 SQLITE_DEFAULT_CACHE_SIZE=-8192 SQLITE_DEFAULT_WAL_SYNCHRONOUS=1)
target_compile_options(xst_sqlite PRIVATE -w)
if(XSDK)
	# SQLite's own Unix layer needs headers xsdk hasn't got (sys/ioctl.h, fcntl locks): ours replaces it.
	target_compile_definitions(xst_sqlite PRIVATE SQLITE_OS_OTHER=1 SQLITE_OMIT_WAL SQLITE_DEFAULT_LOCKING_MODE=1
		SQLITE_TEMP_STORE=3 SQLITE_OMIT_SHARED_CACHE SQLITE_OMIT_LOAD_EXTENSION)
	target_sources(xst_sqlite PRIVATE src/platform/xsdk/sqlite_vfs.c)
	target_include_directories(xst_sqlite PRIVATE third_party/sqlite)
endif()

# LZ4 (third_party/lz4, pinned tag): BEP's message compression.
add_library(xst_lz4 STATIC third_party/lz4/lib/lz4.c)
target_include_directories(xst_lz4 SYSTEM PUBLIC third_party/lz4/lib)

# The portable core: no xsdk calls, builds on the host and the console alike.
add_library(xst_core STATIC
	src/core/base32.cpp
	src/core/bep.cpp
	src/core/bep_frame.cpp
	src/core/config.cpp
	src/core/db.cpp
	src/core/events.cpp
	src/core/device_id.cpp
	src/core/fs_common.cpp
	src/core/glob.cpp
	src/core/ignore.cpp
	src/core/vector.cpp
	src/core/hello.cpp
	src/core/model.cpp
	src/core/protowire.cpp
	src/core/puller.cpp
	src/core/scanner.cpp
	src/core/sha256.cpp
	src/core/version.cpp
	src/net/bep_connection.cpp
	src/net/bep_handshake.cpp
	src/net/connection_service.cpp
	src/net/identity.cpp
	src/net/local_discovery.cpp
	src/net/tcp.cpp
	src/net/tls.cpp
)
target_include_directories(xst_core PUBLIC src "${CMAKE_CURRENT_BINARY_DIR}/generated")
# local_discovery is a Service of the App (src/app.hpp); app.cpp itself is linked by each main.
target_include_directories(xst_core SYSTEM PUBLIC third_party/include)
target_compile_options(xst_core PRIVATE ${XST_WARNINGS})
target_link_libraries(xst_core PUBLIC xst_wolfssl xst_lz4 xst_sqlite)
# The posix filesystem backend serves both builds (xsdk maps POSIX file calls onto its drives).
target_sources(xst_core PRIVATE src/platform/posix/fs_posix.cpp)

if(XSDK)
	add_library(xst_platform STATIC src/platform/xsdk/platform.cpp src/platform/xsdk/volume.cpp
		src/platform/xsdk/console.cpp)
	target_link_libraries(xst_platform PUBLIC xst_core)
	target_compile_options(xst_platform PRIVATE ${XST_WARNINGS})

	add_executable(xsyncthing src/main_xsdk.cpp src/app.cpp src/services.cpp)
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

	add_executable(xsyncthing src/main_host.cpp src/app.cpp src/services.cpp)
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
