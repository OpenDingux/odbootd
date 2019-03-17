cmake_minimum_required(VERSION 3.4)
project(odbootd LANGUAGES C VERSION 0.1)

add_executable(odbootd odbootd.c)

target_link_libraries(odbootd pthread)

include(GNUInstallDirs)
install(TARGETS odbootd RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})
