cmake_minimum_required(VERSION 3.20)
project(retm_stitch_performance C ASM)
enable_testing()
set(LJ_PERF_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../../.." CACHE PATH "LuaJIT source to measure")
add_subdirectory("${LJ_PERF_SOURCE}" luajit)
foreach(vm IN ITEMS luajit luajit-arenagc)
  if(vm STREQUAL "luajit")
    set(variant default)
    set(lib luajit-5.1)
  else()
    set(variant arenagc)
    set(lib luajit-5.1-arenagc)
  endif()
  add_library(retmprobe-${variant} MODULE ../../retm_eval/retmprobe.c)
  target_include_directories(retmprobe-${variant} PRIVATE "${LJ_PERF_SOURCE}/src")
  target_link_libraries(retmprobe-${variant} PRIVATE ${lib})
  set_target_properties(retmprobe-${variant} PROPERTIES PREFIX ""
    OUTPUT_NAME retmprobe
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${variant}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${variant}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${variant}")
endforeach()
