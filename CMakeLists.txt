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

# The portable core: no xsdk calls, builds on the host and the console alike.
add_library(xst_core STATIC
	src/core/version.cpp
)
target_include_directories(xst_core PUBLIC src "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_options(xst_core PRIVATE ${XST_WARNINGS})

if(XSDK)
	add_library(xst_platform STATIC src/platform/xsdk/platform.cpp)
	target_link_libraries(xst_platform PUBLIC xst_core)
	target_compile_options(xst_platform PRIVATE ${XST_WARNINGS})

	add_executable(xsyncthing src/main_xsdk.cpp)
	target_link_libraries(xsyncthing PRIVATE xst_platform xst_core)
	target_compile_options(xsyncthing PRIVATE ${XST_WARNINGS})
	xsdk_add_xex(xsyncthing TITLE_ID ${XST_TITLE_ID} NAME "xsyncthing" VERSION ${PROJECT_VERSION}.0)
else()
	add_library(xst_platform STATIC src/platform/posix/platform.cpp)
	target_link_libraries(xst_platform PUBLIC xst_core)
	target_compile_options(xst_platform PRIVATE ${XST_WARNINGS})

	add_executable(xsyncthing src/main_host.cpp)
	target_link_libraries(xsyncthing PRIVATE xst_platform xst_core)
	target_compile_options(xsyncthing PRIVATE ${XST_WARNINGS})

	include(CTest)
	if(BUILD_TESTING)
		add_subdirectory(tests/unit)
	endif()
endif()
