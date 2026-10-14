cmake_minimum_required(VERSION 3.20)
project(retm_stitch_baseline C ASM)
enable_testing()
set(LJ_BASELINE_SOURCE "" CACHE PATH "Independent c04ae378 LuaJIT checkout")
if(NOT IS_DIRECTORY "${LJ_BASELINE_SOURCE}/src")
  message(FATAL_ERROR "Set LJ_BASELINE_SOURCE to the independent baseline checkout")
endif()
add_subdirectory("${LJ_BASELINE_SOURCE}" luajit)
foreach(vm IN ITEMS luajit luajit-arenagc)
  if(vm STREQUAL "luajit")
    set(variant default)
    set(lib luajit-5.1)
  else()
    set(variant arenagc)
    set(lib luajit-5.1-arenagc)
  endif()
  add_library(retmprobe-${variant} MODULE ../../retm_eval/retmprobe.c)
  target_include_directories(retmprobe-${variant} PRIVATE "${LJ_BASELINE_SOURCE}/src")
  target_link_libraries(retmprobe-${variant} PRIVATE ${lib})
  set_target_properties(retmprobe-${variant} PROPERTIES PREFIX ""
    OUTPUT_NAME retmprobe
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${variant}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${variant}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${variant}")
  foreach(case IN ITEMS counts vararg expanded prefix big frames flush luatails error yield)
    add_test(NAME ${vm}_baseline_${case}
      COMMAND ${CMAKE_COMMAND} -E env
        "LUA_CPATH=$<TARGET_FILE_DIR:retmprobe-${variant}>/?${CMAKE_SHARED_MODULE_SUFFIX};;"
        $<TARGET_FILE:${vm}> "${CMAKE_CURRENT_LIST_DIR}/../../retm_eval/probe.lua" ${case})
    set_tests_properties(${vm}_baseline_${case} PROPERTIES TIMEOUT 30 LABELS retm_baseline)
  endforeach()
endforeach()
