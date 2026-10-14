foreach(vm IN ITEMS luajit luajit-arenagc)
  if(vm STREQUAL "luajit")
    set(variant default)
    set(lib luajit-5.1)
  else()
    set(variant arenagc)
    set(lib luajit-5.1-arenagc)
  endif()
  add_library(retmprobe-${variant} MODULE probe.c)
  target_include_directories(retmprobe-${variant} PRIVATE "${LUAJIT_DIR}/src")
  if(variant STREQUAL "arenagc")
    target_compile_definitions(retmprobe-${variant} PRIVATE LUAJIT_ENABLE_GCARENA)
  endif()
  target_link_libraries(retmprobe-${variant} PRIVATE ${lib})
  set_target_properties(retmprobe-${variant} PROPERTIES PREFIX ""
    OUTPUT_NAME retmprobe
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${variant}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${variant}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${variant}")
  foreach(case IN ITEMS fixed vararg expanded multret guards frames luatails error
                        yield flush reentry coroutine lifecycle prefix blacklist gc spills interp)
    add_test(NAME ${vm}_retm_${case}
      COMMAND ${CMAKE_COMMAND} -E env
        "LUA_CPATH=$<TARGET_FILE_DIR:retmprobe-${variant}>/?${CMAKE_SHARED_MODULE_SUFFIX};;"
        $<TARGET_FILE:${vm}> "${CMAKE_CURRENT_SOURCE_DIR}/test.lua" ${case})
    # CMAKE_SHARED_MODULE_SUFFIX already starts with a dot.
    set_tests_properties(${vm}_retm_${case} PROPERTIES LABELS "luajit_retm" TIMEOUT 30)
  endforeach()
endforeach()
