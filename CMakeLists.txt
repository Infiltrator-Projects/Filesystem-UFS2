cmake_minimum_required(VERSION 3.20)
# VERSION is the single source of truth for the package and runtime worker.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" UFS2_VERSION LIMIT_COUNT 1)
if(NOT UFS2_VERSION MATCHES "^[0-9]+[.][0-9]+[.][0-9]+$")
  message(FATAL_ERROR "VERSION must contain a semantic version (major.minor.patch)")
endif()
project(FilesystemUFS2 VERSION ${UFS2_VERSION} LANGUAGES C)
include(CTest)
include(FetchContent)
find_package(OpenSSL REQUIRED)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(INFILTRATR_COMMON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(INFILTRATR_COMMON_BUILD_SHARED OFF CACHE BOOL "" FORCE)
FetchContent_Declare(infiltratr_common
 GIT_REPOSITORY https://github.com/Infiltrator-Projects/Libraries.git
 GIT_TAG d23a52693fd3212368d327e1c1302e07ae9510ec)
FetchContent_MakeAvailable(infiltratr_common)
# Common is a link-time dependency; its development files must not be installed
# by this standalone filesystem package.
set_property(DIRECTORY "${infiltratr_common_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/version.h" "#define LD_VERSION \"${PROJECT_VERSION}\"\n")
# Use only the source-owned V2 native reader and Common V2 adapter.
# Historical worker/formatter and Linux kernel source remain uninstalled.
add_library(ufs-driver SHARED
 native/ufs_native.c
 module/ufs_module.c)
set_target_properties(ufs-driver PROPERTIES PREFIX "" OUTPUT_NAME "UFS2" SUFFIX ".so")
target_compile_definitions(ufs-driver PRIVATE
 _GNU_SOURCE _FILE_OFFSET_BITS=64 IFS_UFS_MAJOR=2 IFS_UFS_V2_PRODUCTION=1)
target_compile_options(ufs-driver PRIVATE
 -Wall -Wextra -Wpedantic -Werror -ffunction-sections -fdata-sections -fvisibility=hidden)
target_link_options(ufs-driver PRIVATE
 -Wl,-z,defs -Wl,--gc-sections
 "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/module/v2-only.map")
target_include_directories(ufs-driver PRIVATE
 "${CMAKE_CURRENT_SOURCE_DIR}/native" "${CMAKE_CURRENT_SOURCE_DIR}/compat"
 "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(ufs-driver PRIVATE InfiltratrCommon::Common OpenSSL::Crypto)

# Freestanding reader uses exactly the native sources and Common V2 callbacks
# of the hosted module. It is deliberately not embedded or published before
# passing the production BOOT validator and real UEFI read integration.
set(UFS2_FIRMWARE_DIR "${CMAKE_CURRENT_BINARY_DIR}/firmware")
set(UFS2_FIRMWARE_ELF "${UFS2_FIRMWARE_DIR}/UFS2.so")
add_custom_command(OUTPUT "${UFS2_FIRMWARE_ELF}"
 COMMAND "${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_SOURCE_DIR}/module/firmware"
  -B "${UFS2_FIRMWARE_DIR}" -DCMAKE_BUILD_TYPE=Release
  "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
  "-DCOMMON_INCLUDE_DIR=${infiltratr_common_SOURCE_DIR}/include"
  "-DCOMMON_SOURCE_DIR=${infiltratr_common_SOURCE_DIR}"
 COMMAND "${CMAKE_COMMAND}" --build "${UFS2_FIRMWARE_DIR}" --parallel 2
 DEPENDS module/firmware/CMakeLists.txt module/firmware/runtime.c
  native/ufs_native.c module/ufs_module.c
  "${infiltratr_common_SOURCE_DIR}/src/arithmetic.c"
  "${infiltratr_common_SOURCE_DIR}/include/infiltratr/filesystem_module_v2.h"
 VERBATIM)
add_custom_target(ufs2-firmware-elf DEPENDS "${UFS2_FIRMWARE_ELF}")
# The installed file remains one hosted shared object: the same freestanding
# V2 ELF is embedded byte-for-byte in its non-allocated BOOT section.
if(NOT CMAKE_OBJCOPY)
 find_program(CMAKE_OBJCOPY NAMES objcopy llvm-objcopy REQUIRED)
endif()
add_dependencies(ufs-driver ufs2-firmware-elf)
set_property(TARGET ufs-driver APPEND PROPERTY LINK_DEPENDS "${UFS2_FIRMWARE_ELF}")
add_custom_command(TARGET ufs-driver POST_BUILD
 COMMAND "${CMAKE_OBJCOPY}" --add-section
  ".infiltrator_boot=${UFS2_FIRMWARE_ELF}"
  --set-section-flags .infiltrator_boot=readonly
  "$<TARGET_FILE:ufs-driver>"
 VERBATIM)


if(BUILD_TESTING)
 add_executable(ufs-native-format-test tests/test_format.c)
 target_include_directories(ufs-native-format-test PRIVATE "${infiltratr_common_SOURCE_DIR}/include")
 target_link_libraries(ufs-native-format-test PRIVATE ${CMAKE_DL_LIBS})
 target_compile_options(ufs-native-format-test PRIVATE -Wall -Wextra -Wpedantic -Werror)
 add_test(NAME ufs2-native-format COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/tests/format_media.sh" "$<TARGET_FILE:ufs-native-format-test>" "$<TARGET_FILE:ufs-driver>")
 add_executable(ufs-module-test tests/test_module.c)
 target_include_directories(ufs-module-test PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/compat"
  "${CMAKE_CURRENT_SOURCE_DIR}/native")
 target_link_libraries(ufs-module-test PRIVATE ufs-driver InfiltratrCommon::Common)
 add_test(NAME ufs-module-interface COMMAND ufs-module-test)
 add_test(NAME ufs2-only-common-v2-export
   COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_v2_exports.sh"
     "$<TARGET_FILE:ufs-driver>")
endif()
install(TARGETS ufs-driver LIBRARY DESTINATION /System/Filesystems)
# Runtime payload: one filesystem shared object and its single PDF manual.
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/docs/UFS.pdf"
        DESTINATION /System/Resources/Documentation RENAME Filesystem-UFS2.pdf)
