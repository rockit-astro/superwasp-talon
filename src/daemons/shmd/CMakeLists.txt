cmake_minimum_required (VERSION 2.8)
project (shmd)

set(SHMD_SRC shmd.c) 

include_directories ("${CORE_LIBS_DIR}/astro")
include_directories ("${CORE_LIBS_DIR}/misc")

add_executable(shmd ${SHMD_SRC})

target_link_libraries (shmd astro m misc)

install (TARGETS shmd DESTINATION bin)

