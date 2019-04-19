cmake_minimum_required(VERSION 3.4)
project(odbootd LANGUAGES C VERSION 0.1)

add_executable(odbootd odbootd.c)
add_executable(odboot-client odboot-client.c)

find_library(LIBUSB_LIBRARIES usb-1.0)
find_path(LIBUSB_INCLUDE_DIR libusb-1.0/libusb.h)
if (NOT LIBUSB_LIBRARIES OR NOT LIBUSB_INCLUDE_DIR)
	message(SEND_ERROR "Unable to find libusb-1.0")
endif ()

target_link_libraries(odbootd pthread)
target_link_libraries(odboot-client PRIVATE ${LIBUSB_LIBRARIES})
target_include_directories(odboot-client PRIVATE ${LIBUSB_INCLUDE_DIR})

include(GNUInstallDirs)
install(TARGETS odbootd RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})
install(TARGETS odboot-client RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
