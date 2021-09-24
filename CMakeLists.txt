cmake_minimum_required(VERSION 3.4)
project(odbootd LANGUAGES C VERSION 0.2)

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

	option(STATIC_EXE "Compile statically" OFF)
	if (STATIC_EXE)
		target_link_options(odboot-client PRIVATE -static)
	endif ()

	set(EMBEDDED_INSTALLER "" CACHE PATH "Path to the vmlinuz.bin file to embed (optional)")
	if (EMBEDDED_INSTALLER)
		file(SIZE ${EMBEDDED_INSTALLER} INSTALLER_SIZE)

		file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/empty.c CONTENT "")
		set_source_files_properties(${CMAKE_BINARY_DIR}/empty.c PROPERTIES GENERATED TRUE)
		add_library(empty OBJECT ${CMAKE_BINARY_DIR}/empty.c)

		add_custom_command(OUTPUT installer.o
			WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
			COMMAND ${CMAKE_OBJCOPY}
				--add-section=.image=${EMBEDDED_INSTALLER}
				--set-section-flags=.image=contents,alloc,load,readonly,data
				--add-symbol=__start_image=.image:0
				--add-symbol=__end_image=.image:${INSTALLER_SIZE}
				$<TARGET_OBJECTS:empty> installer.o
			DEPENDS empty
		)

		set_source_files_properties(installer.o PROPERTIES GENERATED TRUE)
		target_sources(odboot-client PRIVATE installer.o)
	endif ()

	target_compile_definitions(odboot-client PRIVATE
		HAS_BUILTIN_INSTALLER=$<BOOL:${EMBEDDED_INSTALLER}>
	)

	find_package(PkgConfig REQUIRED)
	pkg_check_modules(OPK QUIET libopk)
	if (NOT OPK_FOUND)
		message(FATAL_ERROR "Missing dependency: libopk")
	endif()

	pkg_check_modules(USB QUIET libusb-1.0)
	if (NOT USB_FOUND)
		message(FATAL_ERROR "Unable to find libusb-1.0")
	endif ()

	if (STATIC_EXE)
		target_link_libraries(odboot-client PRIVATE
			${OPK_STATIC_LIBRARIES} ${USB_STATIC_LIBRARIES}
		)
	else()
		target_link_libraries(odboot-client PRIVATE
			${OPK_LIBRARIES} ${USB_LIBRARIES}
		)
	endif()

	target_link_directories(odboot-client PRIVATE
		${OPK_LIBRARY_DIRS}
		${USB_LIBRARY_DIRS}
	)
	target_include_directories(odboot-client PRIVATE
		${OPK_INCLUDE_DIRS}
		${USB_INCLUDE_DIRS}
	)
	target_compile_definitions(odboot-client PRIVATE
		${OPK_CFLAGS_OTHER} ${USB_CFLAGS_OTHER}
	)

	install(TARGETS odboot-client RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif (WITH_ODBOOT_CLIENT)
