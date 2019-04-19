cmake_minimum_required(VERSION 3.4)
project(odbootd LANGUAGES C VERSION 0.1)

option(WITH_ODBOOTD "Compile odbootd" ON)
option(WITH_ODBOOT_CLIENT "Compile odboot-client" ON)

include(GNUInstallDirs)

if (WITH_ODBOOTD)
	add_executable(odbootd odbootd.c)
	target_link_libraries(odbootd pthread)
	install(TARGETS odbootd RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})
endif (WITH_ODBOOTD)

if (WITH_ODBOOT_CLIENT)
	add_executable(odboot-client odboot-client.c)

	find_library(LIBUSB_LIBRARIES usb-1.0)
	find_path(LIBUSB_INCLUDE_DIR libusb-1.0/libusb.h)
	if (NOT LIBUSB_LIBRARIES OR NOT LIBUSB_INCLUDE_DIR)
		message(SEND_ERROR "Unable to find libusb-1.0")
	endif ()

	target_link_libraries(odboot-client PRIVATE ${LIBUSB_LIBRARIES})
	target_include_directories(odboot-client PRIVATE ${LIBUSB_INCLUDE_DIR})

	install(TARGETS odboot-client RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif (WITH_ODBOOT_CLIENT)
