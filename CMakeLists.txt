cmake_minimum_required(VERSION 3.20)

project(ETCC VERSION 0.2.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Compiler
# ---------------------------------------------------------------------------

file(GLOB ETCC_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")

add_executable(etcc ${ETCC_SOURCES})
target_include_directories(etcc PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(etcc PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    target_link_libraries(etcc PRIVATE kernel32)
endif()

set_target_properties(etcc PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# ---------------------------------------------------------------------------
# Own C runtime (freestanding Windows x64, built with Clang + llvm-lib)
# ---------------------------------------------------------------------------

find_program(ETCC_CLANG NAMES clang clang.exe)
find_program(ETCC_LLVM_LIB NAMES llvm-lib llvm-lib.exe)

if(NOT ETCC_CLANG)
    message(FATAL_ERROR "clang not found; set -DETCC_CLANG=<path-to-clang>")
endif()
if(NOT ETCC_LLVM_LIB)
    message(FATAL_ERROR "llvm-lib not found; set -DETCC_LLVM_LIB=<path-to-llvm-lib>")
endif()

set(RUNTIME_BUILD_DIR "${CMAKE_BINARY_DIR}/runtime")
set(RUNTIME_LIB "${RUNTIME_BUILD_DIR}/etccrt.lib")
file(MAKE_DIRECTORY "${RUNTIME_BUILD_DIR}")

set(RUNTIME_SOURCES
    runtime/src/crt0.c
    runtime/src/string.c
    runtime/src/stdlib.c
    runtime/src/stdio.c
)

set(RUNTIME_CFLAGS
    --target=x86_64-pc-windows-msvc
    -c
    -O2
    -ffreestanding
    -fno-builtin
    -fno-stack-protector
    -fno-asynchronous-unwind-tables
    -Wall
    -Wextra
    -Wpedantic
    "-I${CMAKE_CURRENT_SOURCE_DIR}/runtime/src"
)

set(RUNTIME_OBJECTS)
foreach(runtime_src IN LISTS RUNTIME_SOURCES)
    get_filename_component(runtime_name "${runtime_src}" NAME_WE)
    set(runtime_obj "${RUNTIME_BUILD_DIR}/${runtime_name}.obj")
    add_custom_command(
        OUTPUT "${runtime_obj}"
        COMMAND "${ETCC_CLANG}" ${RUNTIME_CFLAGS}
                "${CMAKE_CURRENT_SOURCE_DIR}/${runtime_src}" -o "${runtime_obj}"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/${runtime_src}"
            "${CMAKE_CURRENT_SOURCE_DIR}/runtime/src/rt_internal.h"
        COMMENT "Building ETCC runtime ${runtime_name}.obj"
        VERBATIM
    )
    list(APPEND RUNTIME_OBJECTS "${runtime_obj}")
endforeach()

add_custom_command(
    OUTPUT "${RUNTIME_LIB}"
    COMMAND "${ETCC_LLVM_LIB}" /nologo "/out:${RUNTIME_LIB}" ${RUNTIME_OBJECTS}
    DEPENDS ${RUNTIME_OBJECTS}
    COMMENT "Archiving ETCC runtime lib"
    VERBATIM
)

add_custom_target(etcc_runtime ALL DEPENDS "${RUNTIME_LIB}")
add_dependencies(etcc etcc_runtime)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

enable_testing()

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(
        NAME etcc_tests
        COMMAND "${CMAKE_COMMAND}" -E env
                "ETCC=$<TARGET_FILE:etcc>"
                "ETCC_TEST_WORK=${CMAKE_BINARY_DIR}/tests"
                "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.py"
    )
    set_tests_properties(etcc_tests PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        TIMEOUT 300
    )

    add_custom_target(check
        COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure
        DEPENDS etcc etcc_runtime
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        USES_TERMINAL
    )
else()
    message(WARNING "Python 3 not found; the 'etcc_tests' CTest target is disabled")
endif()

# ---------------------------------------------------------------------------
# Install (keeps the runtime layout discoverable by the driver)
# ---------------------------------------------------------------------------

include(GNUInstallDirs)
install(TARGETS etcc RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES "${RUNTIME_LIB}" DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/runtime/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
