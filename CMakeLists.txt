cmake_minimum_required(VERSION 3.20)
project(FilesystemUFS VERSION 1.0.0 LANGUAGES C)
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
 GIT_TAG c76ec95ef66fc728df6a906ad75a6a4dd63b419b)
FetchContent_MakeAvailable(infiltratr_common)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/version.h" "#define LD_VERSION \"1.0.0\"\n")
add_library(ufs-driver SHARED
 native/ufs_native.c native/ufs_worker.c fixtures/ufs_mkfs.c
 compat/legacy_services.c compat/runtime_alloc.c
 module/ufs_module.c module/ufs_payload_verifier.c)
set_source_files_properties(fixtures/ufs_mkfs.c PROPERTIES
 COMPILE_DEFINITIONS "main=ifs_ufs_mkfs_main")
set_source_files_properties(native/ufs_worker.c PROPERTIES
 COMPILE_DEFINITIONS "main=ifs_ufs_worker_main")
set_target_properties(ufs-driver PROPERTIES PREFIX "" OUTPUT_NAME "UFS" SUFFIX ".so")
target_compile_definitions(ufs-driver PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64)
target_compile_options(ufs-driver PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_options(ufs-driver PRIVATE -Wl,-z,defs)
target_include_directories(ufs-driver PRIVATE
 "${CMAKE_CURRENT_SOURCE_DIR}/native" "${CMAKE_CURRENT_SOURCE_DIR}/compat"
 "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(ufs-driver PRIVATE InfiltratrCommon::Common OpenSSL::Crypto)
if(BUILD_TESTING)
 add_executable(ufs-module-test tests/test_module.c)
 target_include_directories(ufs-module-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/compat")
 target_link_libraries(ufs-module-test PRIVATE ufs-driver)
 add_test(NAME ufs-module-interface COMMAND ufs-module-test)
endif()
install(TARGETS ufs-driver LIBRARY DESTINATION /System/Filesystems)
