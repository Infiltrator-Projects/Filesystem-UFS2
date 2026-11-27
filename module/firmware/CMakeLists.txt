# SPDX-License-Identifier: GPL-3.0-or-later
# Exact native UFS2 decoder and V2 callbacks from the hosted UFS2.so.
cmake_minimum_required(VERSION 3.20)
project(UFS2BootReader LANGUAGES C)
set(COMMON_INCLUDE_DIR "" CACHE PATH "Pinned Common V2 headers")
set(COMMON_SOURCE_DIR "" CACHE PATH "Pinned Common V2 sources")
if(NOT EXISTS "${COMMON_INCLUDE_DIR}/infiltratr/filesystem_module_v2.h"
   OR NOT EXISTS "${COMMON_SOURCE_DIR}/src/arithmetic.c")
  message(FATAL_ERROR "The pinned Common V2 header and arithmetic source are required")
endif()
add_library(ufs2-boot MODULE
  ../../module/ufs_module.c
  ../../native/ufs_native.c
  "${COMMON_SOURCE_DIR}/src/arithmetic.c"
  "${COMMON_SOURCE_DIR}/src/core.c"
  runtime.c)
set_target_properties(ufs2-boot PROPERTIES
  PREFIX "" OUTPUT_NAME "UFS2" SUFFIX ".so" C_VISIBILITY_PRESET hidden)
target_compile_features(ufs2-boot PRIVATE c_std_11)
target_include_directories(ufs2-boot PRIVATE
  "${COMMON_INCLUDE_DIR}"
  "${COMMON_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../native"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../compat")
# Common core also contains unrelated floating-point conversion routines.
# Compile that translation unit without the integer-only register constraint;
# discard unused routines at link time. All filesystem code remains integer-only.
set_source_files_properties("${COMMON_SOURCE_DIR}/src/core.c" PROPERTIES
  COMPILE_OPTIONS "-mno-general-regs-only;-fno-tree-vectorize;-fno-tree-slp-vectorize")
target_compile_definitions(ufs2-boot PRIVATE
  IFS_UFS_MAJOR=2 IFS_UFS_V2_PRODUCTION=1 IFS_UFS_FIRMWARE_BUILD=1)
# Firmware uses AMD64 SysV without CET, SIMD state assumptions,
# packed relative relocations or non-4-KiB load alignment. Hosted builds
# retain their separate native optimization profile.
target_compile_options(ufs2-boot PRIVATE
  -Wall -Wextra -Wpedantic -Werror -ffreestanding -fno-builtin
  -ffunction-sections -fdata-sections -fno-tree-loop-distribute-patterns
  -fno-stack-protector -mno-red-zone -fcf-protection=none -mgeneral-regs-only -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
target_link_options(ufs2-boot PRIVATE
  -nostdlib -Wl,-z,nopack-relative-relocs,-z,max-page-size=4096,-Bsymbolic,-z,defs,-z,noexecstack,-z,relro,-z,now,--gc-sections)
