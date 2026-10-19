foreach(vm IN ITEMS luajit luajit-arenagc)
  if(vm STREQUAL "luajit")
    set(variant default)
    set(lib luajit-5.1)
  else()
    set(variant arenagc)
    set(lib luajit-5.1-arenagc)
  endif()
  add_library(tailcallprobe-${variant} MODULE probe.c)
  target_include_directories(tailcallprobe-${variant} PRIVATE "${LUAJIT_DIR}/src")
  target_link_libraries(tailcallprobe-${variant} PRIVATE ${lib})
  set_target_properties(tailcallprobe-${variant} PROPERTIES PREFIX ""
    OUTPUT_NAME tailcallprobe
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${variant}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${variant}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${variant}")
  set(modes interp off)
  if(NOT LUAJIT_DISABLE_JIT)
    list(APPEND modes jit)
  endif()
  set(count_mode)
  if(NOT LUA_COMPAT_TAILCALL_COUNT_ON)
    set(count_mode nocount)
  endif()
  set(frame_mode compat)
  if(NOT LUA_COMPAT_TAILCALL_CFRAME_ON)
    set(frame_mode stock)
  endif()
  foreach(script modes dispatch)
    add_test(NAME ${vm}_tailcall_${script}
      COMMAND ${CMAKE_COMMAND} -E env
        "LUA_CPATH=$<TARGET_FILE_DIR:tailcallprobe-${variant}>/?${CMAKE_SHARED_MODULE_SUFFIX};;"
        $<TARGET_FILE:${vm}> "${CMAKE_CURRENT_SOURCE_DIR}/${script}.lua" ${frame_mode})
    set_tests_properties(${vm}_tailcall_${script} PROPERTIES
      WORKING_DIRECTORY "${LUAJIT_DIR}" LABELS luajit_tailcall TIMEOUT 60)
  endforeach()
  foreach(mode IN LISTS modes)
    add_test(NAME ${vm}_tailcall_${mode}
      COMMAND ${CMAKE_COMMAND} -E env
        "LUA_CPATH=$<TARGET_FILE_DIR:tailcallprobe-${variant}>/?${CMAKE_SHARED_MODULE_SUFFIX};;"
        $<TARGET_FILE:${vm}> "${CMAKE_CURRENT_SOURCE_DIR}/test.lua" ${mode} ${count_mode})
    set_tests_properties(${vm}_tailcall_${mode} PROPERTIES
      WORKING_DIRECTORY "${LUAJIT_DIR}" LABELS luajit_tailcall TIMEOUT 60)
  endforeach()
  if(NOT LUAJIT_DISABLE_JIT)
    add_test(NAME ${vm}_tailcall_native
      COMMAND ${CMAKE_COMMAND} -E env
        "LUA_CPATH=$<TARGET_FILE_DIR:tailcallprobe-${variant}>/?${CMAKE_SHARED_MODULE_SUFFIX};;"
        $<TARGET_FILE:${vm}> "${CMAKE_CURRENT_SOURCE_DIR}/native.lua")
    set_tests_properties(${vm}_tailcall_native PROPERTIES
      WORKING_DIRECTORY "${LUAJIT_DIR}" LABELS luajit_tailcall TIMEOUT 60)
  endif()
endforeach()
