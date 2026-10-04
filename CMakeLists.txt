cmake_minimum_required(VERSION 3.20)
project(Tessera VERSION 1.0.0 LANGUAGES C)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(TESSERA_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer (GCC/Clang)" OFF)
option(TESSERA_WERROR   "Treat compiler warnings as errors" ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Warnings: strict on every compiler, because in C a warning is often a bug.
# ---------------------------------------------------------------------------
if(MSVC)
  # /utf-8: sources (and their string literals) are UTF-8, whatever the system code page.
  set(TESSERA_WARNINGS /W4 /permissive- /utf-8)
  if(TESSERA_WERROR)
    list(APPEND TESSERA_WARNINGS /WX)
  endif()
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
else()
  set(TESSERA_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes
                       -Wmissing-prototypes -Wpointer-arith -Wcast-qual -Wvla
                       -Wformat=2 -Wundef)
  if(TESSERA_WERROR)
    list(APPEND TESSERA_WARNINGS -Werror)
  endif()
  if(TESSERA_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
  endif()
  # fork/kill/pread/flock and friends.
  add_compile_definitions(_POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)
  if(APPLE)
    # F_FULLFSYNC and flock() are hidden by a strict _POSIX_C_SOURCE on macOS.
    add_compile_definitions(_DARWIN_C_SOURCE)
  endif()
endif()

# ---------------------------------------------------------------------------
# The storage engine, as a static library
# ---------------------------------------------------------------------------
add_library(tessera STATIC
  src/util/crc32c.c
  src/util/u32map.c
  src/util/status.c
  src/os/vfs_mem.c
  src/os/vfs_posix.c
  src/os/vfs_win32.c
  src/storage/wal.c
  src/storage/pager.c
  src/storage/freelist.c
  src/btree/node.c
  src/btree/btree.c
  src/btree/overflow.c
  src/btree/cursor.c
  src/engine/check.c
  src/engine/db.c
  src/query/lexer.c
  src/query/parser.c
  src/query/exec.c
)
target_include_directories(tessera PUBLIC include PRIVATE src)
target_compile_options(tessera PRIVATE ${TESSERA_WARNINGS})

# ---------------------------------------------------------------------------
# Programs
# ---------------------------------------------------------------------------
add_executable(tessera_cli tools/tessera_cli.c)
set_target_properties(tessera_cli PROPERTIES OUTPUT_NAME tessera)
target_include_directories(tessera_cli PRIVATE src)
target_link_libraries(tessera_cli PRIVATE tessera)
target_compile_options(tessera_cli PRIVATE ${TESSERA_WARNINGS})

add_executable(tessera_crashtest tools/crashtest.c tools/proc.c tools/workload.c)
target_include_directories(tessera_crashtest PRIVATE src tools)
target_link_libraries(tessera_crashtest PRIVATE tessera)
target_compile_options(tessera_crashtest PRIVATE ${TESSERA_WARNINGS})

add_executable(tessera_bench tools/bench.c tools/clock.c)
target_include_directories(tessera_bench PRIVATE tools)
target_link_libraries(tessera_bench PRIVATE tessera)
target_compile_options(tessera_bench PRIVATE ${TESSERA_WARNINGS})

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()
add_executable(tessera_tests
  tests/test_main.c
  tests/test_util.c
  tests/test_node.c
  tests/test_btree.c
  tests/test_txn.c
  tests/test_recovery.c
  tests/test_query.c
  tools/workload.c
)
target_include_directories(tessera_tests PRIVATE src tests tools)
target_link_libraries(tessera_tests PRIVATE tessera)
target_compile_options(tessera_tests PRIVATE ${TESSERA_WARNINGS})

add_test(NAME unit COMMAND tessera_tests)
add_test(NAME crash COMMAND tessera_crashtest --rounds 25 --dir ${CMAKE_CURRENT_BINARY_DIR})
