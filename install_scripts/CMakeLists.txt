cmake_minimum_required (VERSION 2.8)
project (install_scripts)

set(PROFILE_DIR /etc/profile.d)
install(FILES profile.d/talon.sh DESTINATION ${PROFILE_DIR})

