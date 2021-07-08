cmake_minimum_required (VERSION 3.0)
project (xmisc)

set(XMISC_SRC catbrowse.c xtools.c)

include_directories ("${CORE_LIBS_DIR}/astro")
include_directories ("${CORE_LIBS_DIR}/misc")


add_library(xmisc SHARED ${XMISC_SRC})

install (TARGETS xmisc DESTINATION lib)
