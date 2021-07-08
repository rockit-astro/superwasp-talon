cmake_minimum_required (VERSION 3.0)
project (fli)

set(FLI_SRC libfli.c)

add_library(fli SHARED ${FLI_SRC})

install (TARGETS fli DESTINATION lib)
