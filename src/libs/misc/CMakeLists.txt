cmake_minimum_required (VERSION 2.8)
project (misc)

set(MISC_SRC crackini.c funcmax.c misc.c rot.c strops.c cliserv.c csimc.c gaussfit.c newton.c running.c telaxes.c configfile.c lstsqr.c telenv.c)

include_directories ("${CORE_LIBS_DIR}/astro")

add_library(misc SHARED ${MISC_SRC})

install (TARGETS misc DESTINATION lib)
