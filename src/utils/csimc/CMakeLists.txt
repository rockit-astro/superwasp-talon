cmake_minimum_required (VERSION 2.8)
project (csimc)

set(CSIMC_SRC csimc.c boot.c eintrio.c el.c)
 
include_directories ("${CORE_LIBS_DIR}/misc")


add_executable(csimc ${CSIMC_SRC})

target_link_libraries (csimc astro m misc)

install (TARGETS csimc DESTINATION bin)
