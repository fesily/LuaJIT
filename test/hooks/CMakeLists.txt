foreach(vm IN ITEMS luajit luajit-arenagc)
  if(vm STREQUAL "luajit")
    set(variant default)
    set(lib luajit-5.1)
  else()
    set(variant arenagc)
    set(lib luajit-5.1-arenagc)
  endif()
  add_library(hookprobe-${variant} MODULE probe.c)
  target_include_directories(hookprobe-${variant} PRIVATE "${LUAJIT_DIR}/src")
  target_link_libraries(hookprobe-${variant} PRIVATE ${lib})
  set_target_properties(hookprobe-${variant} PROPERTIES PREFIX ""
    OUTPUT_NAME hookprobe
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${variant}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${variant}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${variant}")
  set(modes native)
  if(NOT LUA_COMPAT_TAILCALL_COUNT_ON)
    list(APPEND modes nocount)
  endif()
  if(NOT LUA_COMPAT_TAILCALL_CFRAME_ON)
    list(APPEND modes nocframe)
  endif()
  add_test(NAME ${vm}_compat51_tailcall_hooks_native
    COMMAND ${CMAKE_COMMAND} -E env
      "LUA_CPATH=$<TARGET_FILE_DIR:hookprobe-${variant}>/?${CMAKE_SHARED_MODULE_SUFFIX};;"
      $<TARGET_FILE:${vm}> "${LUAJIT_DIR}/test/compat51/tailcall_hooks.lua" ${modes})
  set_tests_properties(${vm}_compat51_tailcall_hooks_native PROPERTIES
    WORKING_DIRECTORY "${LUAJIT_DIR}" LABELS luajit_hooks TIMEOUT 30)
  if(LUA_COMPAT_TAILCALL_COUNT_ON)
    add_test(NAME ${vm}_compat51_tailcall_depth_native
      COMMAND ${CMAKE_COMMAND} -E env
        "LUA_CPATH=$<TARGET_FILE_DIR:hookprobe-${variant}>/?${CMAKE_SHARED_MODULE_SUFFIX};;"
        $<TARGET_FILE:${vm}> "${LUAJIT_DIR}/test/compat51/tailcall_depth.lua" native)
    set_tests_properties(${vm}_compat51_tailcall_depth_native PROPERTIES
      WORKING_DIRECTORY "${LUAJIT_DIR}" LABELS luajit_hooks TIMEOUT 120)
  endif()
endforeach()
