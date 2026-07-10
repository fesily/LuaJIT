# Added LUA_ADD_EXECUTABLE Ryan Phillips <ryan at trolocsis.com>
# This CMakeLists.txt has been first taken from LuaDist
# Copyright (C) 2007-2011 LuaDist.
# Created by Peter Drahoš
# Redistribution and use of this file is allowed according to the terms of the MIT license.
# Debugged and (now seriously) modified by Ronan Collobert, for Torch7

project(LuaJIT C ASM)
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

SET(LUAJIT_DIR ${CMAKE_CURRENT_SOURCE_DIR})

SET(CMAKE_REQUIRED_INCLUDES
  ${LUAJIT_DIR}
  ${LUAJIT_DIR}/src
  ${CMAKE_CURRENT_BINARY_DIR}
)

OPTION(WITH_AMALG "Build eveything in one shot (needs memory)" ON)
option(WITH_SHARED_LUA "build shared lua" ON)

# Ugly warnings
IF(MSVC)
  ADD_DEFINITIONS(-D_CRT_SECURE_NO_WARNINGS)
ENDIF()

# Various includes
INCLUDE(CheckLibraryExists)
INCLUDE(CheckFunctionExists)
INCLUDE(CheckCSourceCompiles)
INCLUDE(CheckTypeSize)

# LuaJIT specific
option(LUAJIT_DISABLE_FFI "Disable FFI." OFF)
option(LUAJIT_ENABLE_LUA52COMPAT "Enable Lua 5.2 compatibility." OFF)
option(LUAJIT_DISABLE_JIT "Disable JIT." OFF)
option(LUAJIT_CPU_SSE2 "Use SSE2 instead of x87 instructions." ON)
option(LUAJIT_CPU_NOCMOV "Disable NOCMOV." OFF)
option(LUAJIT_DISABLE_GC64 "Disable GC64" OFF)
option(LUA_COMPAT_VARARG_N "Enable legacy Lua 5.1 arg.n population." OFF)
option(LUAJIT_ENABLE_FSANITIZE "Enable fsanitize" OFF)
option(LUAJIT_NUMMODE "num mode" 2)
MARK_AS_ADVANCED(LUAJIT_DISABLE_FFI LUAJIT_ENABLE_LUA52COMPAT
  LUAJIT_DISABLE_GC64
  LUAJIT_DISABLE_JIT LUAJIT_CPU_SSE2 LUAJIT_CPU_NOCMOV)

IF(LUAJIT_DISABLE_FFI)
  ADD_DEFINITIONS(-DLUAJIT_DISABLE_FFI)
ENDIF()

IF(LUAJIT_ENABLE_LUA52COMPAT)
  ADD_DEFINITIONS(-DLUAJIT_ENABLE_LUA52COMPAT)
ENDIF()

IF(LUAJIT_DISABLE_GC64)
  ADD_DEFINITIONS(-DLUAJIT_DISABLE_GC64)
ENDIF()

IF(LUAJIT_DISABLE_JIT)
  ADD_DEFINITIONS(-DLUAJIT_DISABLE_JIT)
ENDIF()

IF(LUAJIT_CPU_SSE2)
  ADD_DEFINITIONS(-DLUAJIT_CPU_SSE2)
ENDIF()

IF(LUAJIT_CPU_NOCMOV)
  ADD_DEFINITIONS(-DLUAJIT_CPU_NOCMOV)
ENDIF()

IF(LUA_COMPAT_VARARG_N)
  ADD_DEFINITIONS(-DLUA_COMPAT_VARARG_N=1)
ENDIF()

IF(LUAJIT_NUMMODE)
  add_definitions(-DLUAJIT_NUMMODE=${LUAJIT_NUMMODE})
ENDIF()
######

if (LUAJIT_ENABLE_FSANITIZE)
      message(STATUS "luajit debug mode")
      # ASSERT/APICHECK + ASan instrumentation are global.
      # Do NOT define LUAJIT_USE_SYSMALLOC here: lj_arch.h disables LJ_HASGCARENA
      # (and thus LJ_HASGCMARK) under SYSMALLOC, while the arenagc variant still
      # passes -D LJ_HASGCMARK to DynASM. That mismatch makes buildvm fail with
      # undeclared LJ_GC_GRAY. Classic GC needs SYSMALLOC for ASan visibility of
      # dlmalloc; apply it only to the default variant below. Arena GC has its
      # own ASan poison helpers in lj_asan.h (same contract as Makefile ASAN=1).
      add_definitions(-DLUA_USE_APICHECK -DLUA_USE_ASSERT)
      if (NOT MSVC)
        add_link_options(-fsanitize=address)
        add_compile_options(-fsanitize=address)
      else()
        add_link_options(/fsanitize=address)
        add_compile_options(/fsanitize=address)
      endif()
endif()

CHECK_TYPE_SIZE("void*" SIZEOF_VOID_P)
IF(SIZEOF_VOID_P EQUAL 8)
  ADD_DEFINITIONS(-D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE)
ENDIF()

if ( WIN32 AND NOT CYGWIN )
  set ( LJVM_MODE peobj )
elseif ( APPLE )
  set ( LJVM_MODE machasm )
else ()
  set ( LJVM_MODE elfasm )
endif ()

IF(NOT WIN32)
  FIND_LIBRARY(DL_LIBRARY "dl")
  IF(DL_LIBRARY)
    SET(CMAKE_REQUIRED_LIBRARIES ${DL_LIBRARY})
    LIST(APPEND LIBS ${DL_LIBRARY})
  ENDIF(DL_LIBRARY)
  CHECK_FUNCTION_EXISTS(dlopen LUA_USE_DLOPEN)
  IF(NOT LUA_USE_DLOPEN)
    MESSAGE(FATAL_ERROR "Cannot compile a useful lua.
Function dlopen() seems not to be supported on your platform.
Apparently you are not on a Windows platform as well.
So lua has no way to deal with shared libraries!")
  ENDIF(NOT LUA_USE_DLOPEN)
ENDIF(NOT WIN32)

check_library_exists(m sin "" LUA_USE_LIBM)
if ( LUA_USE_LIBM )
  list ( APPEND LIBS m )
endif ()

if ( CMAKE_SYSTEM_NAME MATCHES "OpenBSD")
  list ( APPEND LIBS pthread c++abi )
endif ()

## SOURCES
MACRO(LJ_TEST_ARCH stuff)
  CHECK_C_SOURCE_COMPILES("
#undef ${stuff}
#include \"lj_arch.h\"
#if ${stuff}
int main() { return 0; }
#else
#error \"not defined\"
#endif
" ${stuff})
ENDMACRO()

MACRO(LJ_TEST_ARCH_VALUE stuff value)
  CHECK_C_SOURCE_COMPILES("
#undef ${stuff}
#include \"lj_arch.h\"
#if ${stuff} == ${value}
int main() { return 0; }
#else
#error \"not defined\"
#endif
" ${stuff}_${value})
ENDMACRO()


FOREACH(arch X64 X86 ARM ARM64 PPC PPCSPE MIPS)
  LJ_TEST_ARCH(LJ_TARGET_${arch})
  if(LJ_TARGET_${arch})
    STRING(TOLOWER ${arch} TARGET_LJARCH)
    MESSAGE(STATUS "LuaJIT Target: ${TARGET_LJARCH}")
    BREAK()
  ENDIF()
ENDFOREACH()

IF(NOT TARGET_LJARCH)
  MESSAGE(FATAL_ERROR "architecture not supported")
ELSE()
  MESSAGE(STATUS "LuaJIT target ${TARGET_LJARCH}")
ENDIF()

FILE(GLOB jit_files ${LUAJIT_DIR}/src/jit/*.lua)

SET(DASM_ARCH ${TARGET_LJARCH})
SET(DASM_FLAGS)
SET(TARGET_ARCH)
LIST(APPEND TARGET_ARCH "LUAJIT_TARGET=LUAJIT_ARCH_${TARGET_LJARCH}")
LJ_TEST_ARCH_VALUE(LJ_LE 1)
IF(LJ_LE_1)
  SET(DASM_FLAGS ${DASM_FLAGS} -D ENDIAN_LE)
ELSE()
  SET(DASM_FLAGS ${DASM_FLAGS} -D ENDIAN_BE)
ENDIF()
LJ_TEST_ARCH_VALUE(LJ_ARCH_BITS 64)
IF(LJ_ARCH_BITS_64)
  SET(DASM_FLAGS ${DASM_FLAGS} -D P64)
ENDIF()
LJ_TEST_ARCH_VALUE(LJ_HASJIT 1)
IF(LJ_HASJIT_1)
  SET(DASM_FLAGS ${DASM_FLAGS} -D JIT)
ENDIF()
LJ_TEST_ARCH_VALUE(LJ_HASFFI 1)
IF(LJ_HASFFI_1)
  SET(DASM_FLAGS ${DASM_FLAGS} -D FFI)
ENDIF()
LJ_TEST_ARCH_VALUE(LJ_DUALNUM 1)
IF(LJ_DUALNUM_1)
  SET(DASM_FLAGS ${DASM_FLAGS} -D DUALNUM)
ENDIF()
LJ_TEST_ARCH_VALUE(LJ_ARCH_HASFPU 1)
IF(LJ_ARCH_HASFPU_1)
  SET(DASM_FLAGS ${DASM_FLAGS} -D FPU)
  LIST(APPEND TARGET_ARCH "LJ_ARCH_HASFPU=1")
ELSE()
  LIST(APPEND TARGET_ARCH "LJ_ARCH_HASFPU=0")
ENDIF()
LJ_TEST_ARCH_VALUE(LJ_ABI_SOFTFP 1)
IF(NOT LJ_ABI_SOFTFP_1)
  SET(DASM_FLAGS ${DASM_FLAGS} -D HFABI)
  LIST(APPEND TARGET_ARCH "LJ_ABI_SOFTFP=0")
ELSE()
  LIST(APPEND TARGET_ARCH "LJ_ABI_SOFTFP=1")
ENDIF()
LJ_TEST_ARCH_VALUE(LJ_NO_UNWIND 1)
IF(LJ_NO_UNWIND_1)
  SET(DASM_FLAGS ${DASM_FLAGS} -D NO_UNWIND)
  LIST(APPEND TARGET_ARCH "LUAJIT_NO_UNWIND=1")
ENDIF()
IF(WIN32)
  SET(DASM_FLAGS ${DASM_FLAGS} -LN -D WIN)
ENDIF()
IF(TARGET_LJARCH STREQUAL "x86")
  LJ_TEST_ARCH_VALUE(__SSE2__ 1)
  IF(__SSE2__1)
    SET(DASM_FLAGS ${DASM_FLAGS} -D SSE)
  ENDIF()
ENDIF()
IF(TARGET_LJARCH STREQUAL "ppc")
  LJ_TEST_ARCH_VALUE(LJ_ARCH_SQRT 1)
  IF(NOT LJ_ARCH_SQRT_1)
    SET(DASM_FLAGS ${DASM_FLAGS} -D SQRT)
  ENDIF()
  LJ_TEST_ARCH_VALUE(LJ_ARCH_PPC64 1)
  IF(NOT LJ_ARCH_PPC64_1)
    SET(DASM_FLAGS ${DASM_FLAGS} -D GPR64)
  ENDIF()
ENDIF()

IF(APPLE)
  SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DLUAJIT_UNWIND_EXTERNAL")
ELSEIF(NOT WIN32)
  IF(NOT LJ_NO_UNWIND_1)
    # Find out whether the target toolchain always generates unwind tables.
    MESSAGE(STATUS "Checking if the toolchain always generates unwind tables")
    SET(TOOLCHAIN_GENERATES_UNWIND_TABLES 0)
    SET(TMPUNWIND_DIR "${CMAKE_BINARY_DIR}${CMAKE_FILES_DIRECTORY}/CMakeTmp")
    file(WRITE "${TMPUNWIND_DIR}/tmpunwind.c" "extern void b(void);int a(void){b();return 0;}")
    # This seems flimsy; it would be better to use something like CMake's try_compile but
    # there doesn't seem to be a way to avoid linking when using try_compile so this particular
    # code can't use it (at least without modifying the test code)
    execute_process(COMMAND "${CMAKE_C_COMPILER}" -c -x c "${TMPUNWIND_DIR}/tmpunwind.c" -o "${TMPUNWIND_DIR}/tmpunwind.o"
      RESULT_VARIABLE UNWIND_TEST_ERRORED)
    IF(UNWIND_TEST_ERRORED EQUAL 0)
      # Use STRINGS here so that CMake doesn't stop reading the file once it hits a NUL character.
      # Note: STRINGS skips all non-ASCII/binary bytes, but that's okay since we're only checking
      #       for the presence of some ASCII strings.
      file(STRINGS "${TMPUNWIND_DIR}/tmpunwind.o" TMPUNWIND_O)
      string(FIND "${TMPUNWIND_O}" "eh_frame" EH_FRAME_FOUND)
      string(FIND "${TMPUNWIND_O}" "__unwind_info" UNWIND_INFO_FOUND)
      IF(EH_FRAME_FOUND GREATER -1 OR UNWIND_INFO_FOUND GREATER -1)
        SET(TOOLCHAIN_GENERATES_UNWIND_TABLES 1)
      ENDIF()
    ENDIF()

    IF(TOOLCHAIN_GENERATES_UNWIND_TABLES)
      SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DLUAJIT_UNWIND_EXTERNAL")
      MESSAGE(STATUS "Toolchain does generate unwind tables")
    ELSE()
      MESSAGE(STATUS "Toolchain does not generate unwind tables")
    ENDIF()
  ENDIF()
ENDIF()


CHECK_LIBRARY_EXISTS(m sin "" MINILUA_USE_LIBM)

add_custom_command(OUTPUT ${LUAJIT_DIR}/src/luajit_relver.txt
  COMMAND git show -s --format=%ct > ${LUAJIT_DIR}/src/luajit_relver.txt
  WORKING_DIRECTORY ${LUAJIT_DIR}/src/
)

## Source Lists
SET(SRC_LJLIB
  ${LUAJIT_DIR}/src/lib_base.c
  ${LUAJIT_DIR}/src/lib_math.c
  ${LUAJIT_DIR}/src/lib_bit.c
  ${LUAJIT_DIR}/src/lib_string.c
  ${LUAJIT_DIR}/src/lib_table.c
  ${LUAJIT_DIR}/src/lib_io.c
  ${LUAJIT_DIR}/src/lib_os.c
  ${LUAJIT_DIR}/src/lib_package.c
  ${LUAJIT_DIR}/src/lib_debug.c
  ${LUAJIT_DIR}/src/lib_jit.c
  ${LUAJIT_DIR}/src/lib_ffi.c
  ${LUAJIT_DIR}/src/lib_buffer.c
)

SET(SRC_LIBAUX
  ${LUAJIT_DIR}/src/lib_aux.c
  ${LUAJIT_DIR}/src/lib_init.c
)
file (GLOB_RECURSE SRC_LJCORE   "${LUAJIT_DIR}/src/lj_*.c")
list (APPEND SRC_LJCORE ${SRC_LJLIB} ${SRC_LIBAUX})
file (GLOB_RECURSE SRC_BUILDVM_BASE  "${LUAJIT_DIR}/src/host/buildvm*.c")

## GENERATE / COMPILE PER VARIANT
# buildvm intentionally leaks (one-shot host tool). Under ASan that would
# fail the custom command with exit code 1 unless leak detection is off.
# Use TARGET_FILE so cmake -E env gets a real path (not a bare target name).
if(LUAJIT_ENABLE_FSANITIZE)
  set(BUILDVM_HOST_ENV ${CMAKE_COMMAND} -E env "ASAN_OPTIONS=detect_leaks=0")
else()
  set(BUILDVM_HOST_ENV)
endif()

function(add_buildvm_target_for_variant buildvm_target variant_binary_dir output_name mode)
  add_custom_command(OUTPUT "${variant_binary_dir}/${output_name}"
    COMMAND ${BUILDVM_HOST_ENV} $<TARGET_FILE:${buildvm_target}> -m ${mode} -o "${variant_binary_dir}/${output_name}" ${ARGN}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    DEPENDS ${buildvm_target} ${ARGN}
  )
endfunction()

function(add_luajit_variant)
  set(options INSTALL_SUPPORT_FILES)
  set(oneValueArgs NAME LIB_TARGET LIB_OUTPUT_NAME EXE_TARGET)
  set(multiValueArgs COMPILE_DEFINITIONS)
  cmake_parse_arguments(LJVAR "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  foreach(req NAME LIB_TARGET LIB_OUTPUT_NAME EXE_TARGET)
    if(NOT DEFINED LJVAR_${req} OR LJVAR_${req} STREQUAL "")
      message(FATAL_ERROR "add_luajit_variant: missing required argument '${req}'")
    endif()
  endforeach()

  set(variant_binary_dir "${CMAKE_CURRENT_BINARY_DIR}/variants/${LJVAR_NAME}")
  set(variant_jit_dir "${variant_binary_dir}/jit")
  set(variant_dasm_flags ${DASM_FLAGS})
  file(MAKE_DIRECTORY "${variant_binary_dir}")
  file(MAKE_DIRECTORY "${variant_jit_dir}")
  file(COPY ${jit_files} DESTINATION "${variant_jit_dir}")

  list(FIND LJVAR_COMPILE_DEFINITIONS "LUAJIT_ENABLE_GCARENA" variant_uses_gc_arena)
  if(NOT variant_uses_gc_arena EQUAL -1)
    list(APPEND variant_dasm_flags -D LJ_HASGCMARK)
  endif()

  if(LJVAR_INSTALL_SUPPORT_FILES)
    install(
      DIRECTORY "${variant_jit_dir}"
      DESTINATION ${TARGET_MOD_DIR}/scripts
    )
  endif()

  set(minilua_target "minilua-${LJVAR_NAME}")
  add_executable(${minilua_target} ${LUAJIT_DIR}/src/host/minilua.c)
  target_compile_definitions(${minilua_target} PRIVATE ${TARGET_ARCH} ${LJVAR_COMPILE_DEFINITIONS})
  target_include_directories(${minilua_target} PRIVATE
    ${LUAJIT_DIR}
    ${LUAJIT_DIR}/src
    "${variant_binary_dir}")
  if(MINILUA_USE_LIBM)
    target_link_libraries(${minilua_target} m)
  endif()

  set(variant_buildvm_arch_h "${variant_binary_dir}/buildvm_arch.h")
  add_custom_command(OUTPUT "${variant_buildvm_arch_h}"
    COMMAND ${minilua_target} ${LUAJIT_DIR}/dynasm/dynasm.lua ${variant_dasm_flags} -o "${variant_buildvm_arch_h}" ${LUAJIT_DIR}/src/vm_${DASM_ARCH}.dasc
    DEPENDS
      ${LUAJIT_DIR}/dynasm/dynasm.lua
      ${LUAJIT_DIR}/src/vm_${DASM_ARCH}.dasc
      ${LUAJIT_DIR}/src/lj_arch.h
      ${LUAJIT_DIR}/src/lj_bc.h
      ${minilua_target}
  )

  set(variant_luajit_relver_txt "${variant_binary_dir}/luajit_relver.txt")
  add_custom_command(OUTPUT "${variant_luajit_relver_txt}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LUAJIT_DIR}/src/luajit_relver.txt "${variant_luajit_relver_txt}"
    DEPENDS ${LUAJIT_DIR}/src/luajit_relver.txt
  )

  set(variant_luajit_h "${variant_binary_dir}/luajit.h")
  add_custom_command(OUTPUT "${variant_luajit_h}"
    COMMAND ${minilua_target} ${LUAJIT_DIR}/src/host/genversion.lua ${LUAJIT_DIR}/src/luajit_rolling.h "${variant_luajit_relver_txt}" "${variant_luajit_h}"
    DEPENDS ${LUAJIT_DIR}/src/host/genversion.lua ${minilua_target} "${variant_luajit_relver_txt}" ${LUAJIT_DIR}/src/luajit_rolling.h
    WORKING_DIRECTORY ${LUAJIT_DIR}/src/
  )

  set(variant_src_buildvm ${SRC_BUILDVM_BASE} "${variant_buildvm_arch_h}" "${variant_luajit_h}")
  set(buildvm_target "buildvm-${LJVAR_NAME}")
  add_executable(${buildvm_target} ${variant_src_buildvm})
  target_compile_definitions(${buildvm_target} PRIVATE ${TARGET_ARCH} ${LJVAR_COMPILE_DEFINITIONS})
  target_include_directories(${buildvm_target} PRIVATE
    "${variant_binary_dir}"
    ${LUAJIT_DIR}/src
    ${LUAJIT_DIR}/dynasm)

  if (WIN32)
    add_buildvm_target_for_variant(${buildvm_target} "${variant_binary_dir}" lj_vm.obj peobj)
    set(variant_lj_vm_src "${variant_binary_dir}/lj_vm.obj")
  else ()
    add_buildvm_target_for_variant(${buildvm_target} "${variant_binary_dir}" lj_vm.S ${LJVM_MODE})
    set(variant_lj_vm_src "${variant_binary_dir}/lj_vm.S")
  endif ()
  add_buildvm_target_for_variant(${buildvm_target} "${variant_binary_dir}" lj_ffdef.h ffdef ${SRC_LJLIB})
  add_buildvm_target_for_variant(${buildvm_target} "${variant_binary_dir}" lj_bcdef.h bcdef ${SRC_LJLIB})
  add_buildvm_target_for_variant(${buildvm_target} "${variant_binary_dir}" lj_folddef.h folddef ${LUAJIT_DIR}/src/lj_opt_fold.c)
  add_buildvm_target_for_variant(${buildvm_target} "${variant_binary_dir}" lj_recdef.h recdef ${SRC_LJLIB})
  add_buildvm_target_for_variant(${buildvm_target} "${variant_binary_dir}" lj_libdef.h libdef ${SRC_LJLIB})
  add_buildvm_target_for_variant(${buildvm_target} "${variant_binary_dir}" vmdef.lua vmdef ${SRC_LJLIB})

  set(variant_vmdef_lua "${variant_binary_dir}/vmdef.lua")
  set(variant_deps
    ${variant_lj_vm_src}
    "${variant_binary_dir}/lj_ffdef.h"
    "${variant_binary_dir}/lj_bcdef.h"
    "${variant_binary_dir}/lj_libdef.h"
    "${variant_binary_dir}/lj_recdef.h"
    "${variant_binary_dir}/lj_folddef.h"
    "${variant_vmdef_lua}")

  if(LJVAR_INSTALL_SUPPORT_FILES)
    install(FILES "${variant_vmdef_lua}" DESTINATION ${TARGET_MOD_DIR}/scripts/jit)
  endif()

  if(WITH_SHARED_LUA)
    if(WITH_AMALG)
      add_library(${LJVAR_LIB_TARGET} SHARED ${LUAJIT_DIR}/src/ljamalg.c ${variant_deps})
    else()
      add_library(${LJVAR_LIB_TARGET} SHARED ${SRC_LJCORE} ${variant_deps})
    endif()
    set_target_properties(${LJVAR_LIB_TARGET} PROPERTIES OUTPUT_NAME "${LJVAR_LIB_OUTPUT_NAME}")
  else()
    if(WITH_AMALG)
      add_library(${LJVAR_LIB_TARGET} STATIC ${LUAJIT_DIR}/src/ljamalg.c ${variant_deps})
    else()
      add_library(${LJVAR_LIB_TARGET} STATIC ${SRC_LJCORE} ${variant_deps})
    endif()
    if(LJVAR_NAME STREQUAL "default")
      set(variant_static_output_name "luajit")
    else()
      set(variant_static_output_name "${LJVAR_LIB_OUTPUT_NAME}")
    endif()
    set_target_properties(${LJVAR_LIB_TARGET} PROPERTIES
      PREFIX "lib" IMPORT_PREFIX "lib" OUTPUT_NAME "${variant_static_output_name}")
  endif()

  target_compile_definitions(${LJVAR_LIB_TARGET} PRIVATE DO_LUA_INIT ${LJVAR_COMPILE_DEFINITIONS})
  target_include_directories(${LJVAR_LIB_TARGET} PRIVATE
    "${variant_binary_dir}"
    ${LUAJIT_DIR}/src
    ${LUAJIT_DIR}/dynasm)
  target_link_libraries(${LJVAR_LIB_TARGET} ${LIBS})
  install(TARGETS ${LJVAR_LIB_TARGET} RUNTIME DESTINATION .)

  if(WIN32)
    add_executable(${LJVAR_EXE_TARGET} ${LUAJIT_DIR}/src/luajit.c)
    target_compile_definitions(${LJVAR_EXE_TARGET} PRIVATE LUA_BUILD_AS_DLL LJ_DS_DEBUG_GETINFO_PATCH=0 ${LJVAR_COMPILE_DEFINITIONS})
    target_include_directories(${LJVAR_EXE_TARGET} PRIVATE
      "${variant_binary_dir}"
      ${LUAJIT_DIR}/src
      ${LUAJIT_DIR}/dynasm)
    target_link_libraries(${LJVAR_EXE_TARGET} ${LJVAR_LIB_TARGET} ${LIBS})
  else()
    if(WITH_AMALG)
      add_executable(${LJVAR_EXE_TARGET} ${LUAJIT_DIR}/src/luajit.c ${LUAJIT_DIR}/src/ljamalg.c ${variant_deps})
      # When using WITH_AMALG, compile the executable after the variant library
      # to avoid concurrent ljamalg.c builds for the same variant.
      add_dependencies(${LJVAR_EXE_TARGET} ${LJVAR_LIB_TARGET})
    else()
      add_executable(${LJVAR_EXE_TARGET} ${LUAJIT_DIR}/src/luajit.c ${SRC_LJCORE} ${variant_deps})
    endif()
    target_link_libraries(${LJVAR_EXE_TARGET} ${LIBS})
    set_target_properties(${LJVAR_EXE_TARGET} PROPERTIES ENABLE_EXPORTS ON)
  endif()

  target_compile_definitions(${LJVAR_EXE_TARGET} PRIVATE LJ_DS_DEBUG_GETINFO_PATCH=0 ${LJVAR_COMPILE_DEFINITIONS})
  target_include_directories(${LJVAR_EXE_TARGET} PRIVATE
    "${variant_binary_dir}"
    ${LUAJIT_DIR}/src
    ${LUAJIT_DIR}/dynasm)
endfunction()

# Classic GC + ASan: SYSMALLOC so the system allocator is visible to ASan.
# Arena GC deliberately omits SYSMALLOC (see FSANITIZE block above).
set(LUAJIT_DEFAULT_VARIANT_DEFS)
if(LUAJIT_ENABLE_FSANITIZE)
  list(APPEND LUAJIT_DEFAULT_VARIANT_DEFS LUAJIT_USE_SYSMALLOC)
endif()

add_luajit_variant(
  NAME default
  LIB_TARGET luajit-5.1
  LIB_OUTPUT_NAME lua51DS
  EXE_TARGET luajit
  INSTALL_SUPPORT_FILES
  COMPILE_DEFINITIONS ${LUAJIT_DEFAULT_VARIANT_DEFS}
)

add_luajit_variant(
  NAME arenagc
  LIB_TARGET luajit-5.1-arenagc
  LIB_OUTPUT_NAME lua51DS_arenagc
  EXE_TARGET luajit-arenagc
  COMPILE_DEFINITIONS LUAJIT_ENABLE_GCARENA
)

MACRO(LUAJIT_add_custom_commands luajit_target)
  SET(target_srcs "")
  FOREACH(file ${ARGN})
    IF(${file} MATCHES ".*\\.lua$")
      if(NOT IS_ABSOLUTE ${file})
        set(file "${CMAKE_CURRENT_SOURCE_DIR}/${file}")
      endif()
      set(source_file ${file})
      string(LENGTH ${CMAKE_SOURCE_DIR} _luajit_source_dir_length)
      string(LENGTH ${file} _luajit_file_length)
      math(EXPR _begin "${_luajit_source_dir_length} + 1")
      math(EXPR _stripped_file_length "${_luajit_file_length} - ${_luajit_source_dir_length} - 1")
      string(SUBSTRING ${file} ${_begin} ${_stripped_file_length} stripped_file)

      set(generated_file "${CMAKE_BINARY_DIR}/jitted_tmp/${luajit_target}/${stripped_file}_generated${CMAKE_C_OUTPUT_EXTENSION}")

      add_custom_command(
        OUTPUT ${generated_file}
        MAIN_DEPENDENCY ${source_file}
        DEPENDS ${luajit_target}
        COMMAND ${luajit_target}
        ARGS -bg
          ${source_file}
          ${generated_file}
        COMMENT "Building Luajitted ${source_file}: ${generated_file}"
      )

      get_filename_component(basedir ${generated_file} PATH)
      file(MAKE_DIRECTORY ${basedir})

      set(target_srcs ${target_srcs} ${generated_file})
      set_source_files_properties(
        ${generated_file}
        properties
        external_object true # this is an object file
        generated true        # to say that "it is OK that the obj-files do not exist before build time"
      )
    ELSE()
      set(target_srcs ${target_srcs} ${file})
    ENDIF(${file} MATCHES ".*\\.lua$")
  ENDFOREACH(file)
ENDMACRO()

MACRO(LUA_ADD_EXECUTABLE luajit_target)
  LUAJIT_add_custom_commands(${luajit_target} ${ARGN})
  add_executable(${luajit_target} ${target_srcs})
ENDMACRO(LUA_ADD_EXECUTABLE luajit_target)
