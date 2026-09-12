cmake_minimum_required(VERSION 3.18)
project(fsx VERSION 1.0.0 LANGUAGES CXX)

option(FSX_ENABLE_ASAN "Enable AddressSanitizer/UBSan" OFF)
option(FSX_BUILD_TESTS "Build tests" ON)
option(FSX_ENABLE_LTO "Enable interprocedural optimization when supported" ON)
option(FSX_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(fsxcore
    src/core/error.cpp
    src/core/size.cpp
    src/core/crc.cpp
    src/core/hash.cpp
    src/core/progress.cpp
    src/core/cancel.cpp
    src/core/uuid.cpp
    src/core/fault.cpp
    src/core/file_util.cpp
    src/io/device.cpp
    src/io/scheduler.cpp
    src/io/safety.cpp
    src/fs/fat.cpp
    src/fs/exfat.cpp
    src/fs/probe.cpp
    src/fs/xfs.cpp
    src/fs/ntfs.cpp
    src/fs/btrfs.cpp
    src/fs/iso9660.cpp
    src/fs/ufs.cpp
    src/ext4/ext4.cpp
    src/ext4/internal.cpp
    src/ext4/allocation.cpp
    src/ext4/extents.cpp
    src/ext4/metadata.cpp
    src/ext4/relocate.cpp
    src/ext4/mutate.cpp
    src/ext4/resize.cpp
    src/ext4/maintenance.cpp
    src/ext4/verify.cpp
    src/partition/gpt.cpp
    src/partition/mbr.cpp
    src/partition/sync.cpp
    src/transaction/journal.cpp
    src/transaction/recovery.cpp
    src/transaction/validate.cpp
    src/ops/benchmark.cpp
    src/ops/image.cpp
    src/ops/scrub.cpp
    src/ops/clone.cpp
)

target_include_directories(fsxcore PUBLIC include)
target_compile_options(fsxcore PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2
    -Wundef -Wcast-align -Wcast-qual -Wnull-dereference
)

if(FSX_ENABLE_ASAN AND (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
  set(FSX_SAN_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_compile_options(fsxcore PRIVATE ${FSX_SAN_FLAGS})
endif()

find_package(Threads REQUIRED)
target_link_libraries(fsxcore PUBLIC Threads::Threads)

include(CheckIPOSupported)
if(FSX_ENABLE_LTO)
  check_ipo_supported(RESULT FSX_IPO_SUPPORTED OUTPUT FSX_IPO_ERROR)
  if(FSX_IPO_SUPPORTED)
    set_property(TARGET fsxcore PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
  endif()
endif()

add_executable(fsx src/cli/main.cpp)
target_link_libraries(fsx PRIVATE fsxcore)
target_compile_options(fsx PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2
    -Wundef -Wcast-align -Wcast-qual -Wnull-dereference
)
if(FSX_WARNINGS_AS_ERRORS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(fsxcore PRIVATE -Werror)
  target_compile_options(fsx PRIVATE -Werror)
endif()
if(FSX_ENABLE_LTO AND FSX_IPO_SUPPORTED)
  set_property(TARGET fsx PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(fsxcore PRIVATE -fstack-protector-strong)
  target_compile_options(fsx PRIVATE -fstack-protector-strong)
  if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    target_compile_definitions(fsxcore PRIVATE _FORTIFY_SOURCE=3)
    target_compile_definitions(fsx PRIVATE _FORTIFY_SOURCE=3)
  endif()
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(fsx PRIVATE -Wl,-z,relro,-z,now)
endif()
if(FSX_ENABLE_ASAN AND (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
  target_compile_options(fsx PRIVATE ${FSX_SAN_FLAGS})
  target_link_options(fsx PRIVATE -fsanitize=address,undefined)
endif()

install(TARGETS fsx RUNTIME DESTINATION bin)
install(FILES docs/fsx.8 DESTINATION share/man/man8)

if(FSX_BUILD_TESTS)
  enable_testing()

  # Keep assertions active in test executables even for Release builds.
  # CMake normally defines NDEBUG for Release; tests that use assert() must
  # still execute their checks or a release validation run becomes meaningless.
  function(fsx_enable_test_assertions target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(${target} PRIVATE -UNDEBUG)
    endif()
  endfunction()
  add_executable(fsx-tests tests/test_core.cpp)
  target_link_libraries(fsx-tests PRIVATE fsxcore)
  fsx_enable_test_assertions(fsx-tests)
  if(FSX_ENABLE_ASAN AND (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
    target_compile_options(fsx-tests PRIVATE ${FSX_SAN_FLAGS})
    target_link_options(fsx-tests PRIVATE -fsanitize=address,undefined)
  endif()
  add_test(NAME fsx-core COMMAND fsx-tests)
  add_executable(fsx-step2-tests tests/test_step2.cpp)
  target_link_libraries(fsx-step2-tests PRIVATE fsxcore)
  fsx_enable_test_assertions(fsx-step2-tests)
  if(FSX_ENABLE_ASAN AND (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
    target_compile_options(fsx-step2-tests PRIVATE ${FSX_SAN_FLAGS})
    target_link_options(fsx-step2-tests PRIVATE -fsanitize=address,undefined)
  endif()
  add_test(NAME fsx-step2 COMMAND fsx-step2-tests)

  add_executable(fsx-step3-tests tests/test_step3.cpp)
  target_link_libraries(fsx-step3-tests PRIVATE fsxcore)
  fsx_enable_test_assertions(fsx-step3-tests)
  if(FSX_ENABLE_ASAN AND (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
    target_compile_options(fsx-step3-tests PRIVATE ${FSX_SAN_FLAGS})
    target_link_options(fsx-step3-tests PRIVATE -fsanitize=address,undefined)
  endif()
  add_test(NAME fsx-step3 COMMAND fsx-step3-tests)

  add_executable(fsx-step4-tests tests/test_step4.cpp)
  target_link_libraries(fsx-step4-tests PRIVATE fsxcore)
  fsx_enable_test_assertions(fsx-step4-tests)
  if(FSX_ENABLE_ASAN AND (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
    target_compile_options(fsx-step4-tests PRIVATE ${FSX_SAN_FLAGS})
    target_link_options(fsx-step4-tests PRIVATE -fsanitize=address,undefined)
  endif()
  add_test(NAME fsx-step4 COMMAND fsx-step4-tests)

  add_executable(fsx-step5-tests tests/test_step5.cpp)
  target_link_libraries(fsx-step5-tests PRIVATE fsxcore)
  fsx_enable_test_assertions(fsx-step5-tests)
  if(FSX_ENABLE_ASAN AND (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
    target_compile_options(fsx-step5-tests PRIVATE ${FSX_SAN_FLAGS})
    target_link_options(fsx-step5-tests PRIVATE -fsanitize=address,undefined)
  endif()
  add_test(NAME fsx-step5 COMMAND fsx-step5-tests)

  add_executable(fsx-step6-tests tests/test_step6.cpp)
  target_link_libraries(fsx-step6-tests PRIVATE fsxcore)
  fsx_enable_test_assertions(fsx-step6-tests)
  if(FSX_ENABLE_ASAN AND (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
    target_compile_options(fsx-step6-tests PRIVATE ${FSX_SAN_FLAGS})
    target_link_options(fsx-step6-tests PRIVATE -fsanitize=address,undefined)
  endif()
  add_test(NAME fsx-step6 COMMAND fsx-step6-tests)

  add_executable(fsx-fault-matrix-tests tests/test_fault_matrix.cpp)
  target_link_libraries(fsx-fault-matrix-tests PRIVATE fsxcore)
  fsx_enable_test_assertions(fsx-fault-matrix-tests)
  if(FSX_ENABLE_ASAN AND (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
    target_compile_options(fsx-fault-matrix-tests PRIVATE ${FSX_SAN_FLAGS})
    target_link_options(fsx-fault-matrix-tests PRIVATE -fsanitize=address,undefined)
  endif()
  add_test(NAME fsx-fault-matrix COMMAND fsx-fault-matrix-tests)

  add_executable(fsx-corruption-tests tests/test_corruption.cpp)
  target_link_libraries(fsx-corruption-tests PRIVATE fsxcore)
  fsx_enable_test_assertions(fsx-corruption-tests)
  if(FSX_ENABLE_ASAN AND (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
    target_compile_options(fsx-corruption-tests PRIVATE ${FSX_SAN_FLAGS})
    target_link_options(fsx-corruption-tests PRIVATE -fsanitize=address,undefined)
  endif()
  add_test(NAME fsx-corruption-corpus COMMAND fsx-corruption-tests)

  add_executable(fsx-final-hardening-tests tests/test_final_hardening.cpp)
  target_link_libraries(fsx-final-hardening-tests PRIVATE fsxcore)
  fsx_enable_test_assertions(fsx-final-hardening-tests)
  if(FSX_ENABLE_ASAN AND (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
    target_compile_options(fsx-final-hardening-tests PRIVATE ${FSX_SAN_FLAGS})
    target_link_options(fsx-final-hardening-tests PRIVATE -fsanitize=address,undefined)
  endif()
  add_test(NAME fsx-final-hardening COMMAND fsx-final-hardening-tests)

  add_executable(fsx-publication-tests tests/test_publication.cpp)
  target_link_libraries(fsx-publication-tests PRIVATE fsxcore)
  fsx_enable_test_assertions(fsx-publication-tests)
  if(FSX_ENABLE_ASAN AND (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
    target_compile_options(fsx-publication-tests PRIVATE ${FSX_SAN_FLAGS})
    target_link_options(fsx-publication-tests PRIVATE -fsanitize=address,undefined)
  endif()
  add_test(NAME fsx-publication COMMAND fsx-publication-tests)
endif()
