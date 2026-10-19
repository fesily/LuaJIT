cmake_minimum_required(VERSION 3.20)
project(tailcall_tests C ASM)
enable_testing()
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../.." luajit)
