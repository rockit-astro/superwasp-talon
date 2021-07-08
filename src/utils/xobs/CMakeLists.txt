cmake_minimum_required (VERSION 3.0)
project (xobs)

set(XOBS_SRC 
        xobs.c 
        calaxes.c 
        cli.c
        clicommands.c
        cliutil.c
        config.c 
        control.c 
        dome.c 
        fifos.c 
        gui.c 
        paddle.c 
        query.c 
        scope.c 
        skymap.c 
        sound.c 
        telrun.c 
        tips.c 
        update.c 
        xephem.c
) 

include_directories ("${CORE_LIBS_DIR}/astro")
include_directories ("${CORE_LIBS_DIR}/misc")
include_directories ("${CORE_LIBS_DIR}/xmisc")

add_executable(xobs ${XOBS_SRC})

target_link_libraries (xobs astro m misc xmisc Xm Xt Xpm X11)

install (TARGETS xobs DESTINATION bin)
