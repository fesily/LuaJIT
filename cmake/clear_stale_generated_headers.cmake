# Removes in-tree generated LuaJIT headers left behind by `make -C luajit/src`.
#
# The LuaJIT C sources include their generated tables with quoted includes
# (`#include "lj_bcdef.h"`), and a quoted include searches the *including file's
# directory* before any -I path. buildvm writes those files into the variant
# directory, so a copy living in ${LUAJIT_DIR}/src silently wins and every
# variant compiles the wrong tables. Observed failure: the arena variant
# (LUAJIT_ENABLE_GCARENA) picked up classic-GC `lj_bcdef.h`/`lj_libdef.h` while
# `lj_vm.S` came from the GCMARK buildvm, and `luaL_openlibs` died with SIGILL
# inside `lj_BC_IFUNCF` (garbage instruction sequence).
#
# The variant copies are touched after a cleanup so the objects that consumed a
# stale header are rebuilt with the correct one on the following build. When
# nothing stale exists this script is a no-op, so it does not cause churn.

set(_stale "")
foreach(_name
    lj_bcdef.h lj_ffdef.h lj_folddef.h lj_libdef.h lj_recdef.h luajit.h
    host/buildvm_arch.h)
  if(EXISTS "${LUAJIT_DIR}/src/${_name}")
    list(APPEND _stale "${_name}")
    file(REMOVE "${LUAJIT_DIR}/src/${_name}")
  endif()
endforeach()

if(_stale)
  string(REPLACE ";" ", " _names "${_stale}")
  message(STATUS "Cleared stale in-tree LuaJIT headers (${_names}); "
                 "rebuilding from ${VARIANT_DIR}")
  foreach(_name lj_bcdef.h lj_ffdef.h lj_folddef.h lj_libdef.h lj_recdef.h luajit.h)
    if(EXISTS "${VARIANT_DIR}/${_name}")
      file(TOUCH "${VARIANT_DIR}/${_name}")
    endif()
  endforeach()
endif()
