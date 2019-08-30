# This file contains helper functions to make finding liblsl as easy as
# possible for app authors
# Usage: (optionally) set either LSL_INSTALL_ROOT to the path where liblsl was
# installed / an install dir was unzipped or set LIBLSLDIR to the path where
# the liblsl source code and CMakeLists.txt is
# Then, include Findliblsl.cmake
# Findliblsl.cmake will the look for ${LSL_INSTALL_ROOT}/share/LSL/cmake/LSLConfig.cmake
# or ${LIBLSLDIR}/LSLCMake.cmake and ${LIBLSLDIR}/CMakeLists.txt and either import
# the "installed" precompiled liblsl or add the source directory and compile it

macro(lsl_found_liblsl type lslpath)
	message(STATUS "${type} liblsl found in ${lslpath}")
	list(APPEND CMAKE_MODULE_PATH ${lslpath})
	include(LSLCMake)
	return()
endmacro()

# set up LSL if not done already
if(NOT TARGET LSL::lsl)
	# when building out of tree LSL_INSTALL_ROOT should to be specified on the
	# cmd line to find a precompiled liblsl
	file(TO_CMAKE_PATH "${LSL_INSTALL_ROOT}" LSL_INSTALL_ROOT)
	list(APPEND LSL_INSTALL_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../LSL/liblsl/build/install")
	find_package(LSL HINTS ${LSL_INSTALL_ROOT}/share/LSL/ ${LSL_INSTALL_ROOT}/LSL/share/LSL QUIET)
	if(LSL_FOUND)
		lsl_found_liblsl("Precompiled" ${LSL_DIR})
	endif()
	message(WARNING "Precompiled LSL was not found. See https://github.com/labstreaminglayer/labstreaminglayer/blob/master/doc/BUILD.md#lsl_install_root for more information.")

	# Try to find the liblsl source directory otherwise
	file(TO_CMAKE_PATH "${LIBLSLDIR}" LIBLSLDIR)
	find_file(LSLCMAKE_PATH
		"LSLCMake.cmake"
		HINTS ${LIBLSLDIR} "${CMAKE_CURRENT_LIST_DIR}/../../LSL/liblsl"
	)
	if(LSLCMAKE_PATH)
		get_filename_component(LSLDIR_PATH ${LSLCMAKE_PATH} DIRECTORY CACHE)
		message(STATUS "${LSLCMAKE_PATH} ${LSLDIR_PATH}")
		add_subdirectory(${LSLDIR_PATH} liblsl)
		lsl_found_liblsl("Source dir for" ${LSLDIR_PATH})
	endif()
	message(WARNING "LSLCMake wasn't found in '${LIBLSLDIR}'")
	message(FATAL_ERROR "Neither precompiled libls nor liblsl source found")
endif()
