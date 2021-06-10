cmake_minimum_required(VERSION 3.12)
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum MacOS deployment version")

project(LabRecorder
	DESCRIPTION "Record and write LabStreamingLayer streams to an XDF file"
	HOMEPAGE_URL "https://github.com/labstreaminglayer/App-LabRecorder/"
	LANGUAGES C CXX
	VERSION 1.14.1)

# Needed for customized MacOSXBundleInfo.plist.in
SET(CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake" ${CMAKE_MODULE_PATH})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED On)

# Dependencies

## LSL
if(ANDROID)
	set(LIBLSL_SOURCE_PATH "../../LSL/liblsl" CACHE STRING "Path to liblsl sources")

	# force include liblsl as target to build with the android toolchain
	# as path of the normal build process
	add_subdirectory(${LIBLSL_SOURCE_PATH} liblsl_bin)
	add_library(LSL::lsl ALIAS lsl)
else()
	find_package(LSL REQUIRED
		HINTS ${LSL_INSTALL_ROOT}
		"${CMAKE_CURRENT_LIST_DIR}/../../LSL/liblsl/build/"
		"${CMAKE_CURRENT_LIST_DIR}/../../LSL/liblsl/build/install"
		"${CMAKE_CURRENT_LIST_DIR}/../../LSL/liblsl/out/build/x64-Release"
		"${CMAKE_CURRENT_LIST_DIR}/../../LSL/liblsl/out/install/x64-Release"
		PATH_SUFFIXES share/LSL
	)
endif()

## Qt
set(CMAKE_AUTOMOC ON)  # The later version of this in LSLCMake is somehow not enough.
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
find_package(Qt6 COMPONENTS Core Widgets Network DBus)
if(NOT Qt6_FOUND)
	# Require 5.15 so we can use version-agnostic linking.
	find_package(Qt5 5.15 COMPONENTS Core Widgets Network DBus REQUIRED)
	add_executable(${PROJECT_NAME} MACOSX_BUNDLE)
else()
	qt_add_executable(${PROJECT_NAME} MACOSX_BUNDLE MANUAL_FINALIZATION)
endif()

## Threads
find_package(Threads REQUIRED)

# Targets

## xdfwriter - stand alone library
add_subdirectory(xdfwriter)

target_sources(${PROJECT_NAME} PRIVATE
	src/main.cpp
	src/mainwindow.cpp
	src/mainwindow.h
	src/mainwindow.ui
	src/recording.h
	src/recording.cpp
	src/tcpinterface.h
	src/tcpinterface.cpp
)

add_executable(LabRecorderCLI MACOSX_BUNDLE
	src/clirecorder.cpp
	src/recording.h
	src/recording.cpp
)

target_link_libraries(${PROJECT_NAME}
	PRIVATE
	xdfwriter
	Qt::Widgets
	Qt::Network
	Qt::DBus
	Threads::Threads
	LSL::lsl
)

target_link_libraries(LabRecorderCLI
	PRIVATE
	xdfwriter
	Threads::Threads
	LSL::lsl
)

installLSLApp(xdfwriter)
installLSLApp(testxdfwriter)
installLSLApp(${PROJECT_NAME})
installLSLApp(LabRecorderCLI)
installLSLAuxFiles(${PROJECT_NAME}
	${PROJECT_NAME}.cfg
	LICENSE
	README.md
)

if (WIN32)
	get_target_property(QT_QMAKE_EXECUTABLE Qt::qmake IMPORTED_LOCATION)
	get_filename_component(QT_WINDEPLOYQT_EXECUTABLE ${QT_QMAKE_EXECUTABLE} PATH)
	set(QT_WINDEPLOYQT_EXECUTABLE "${QT_WINDEPLOYQT_EXECUTABLE}/windeployqt.exe")

	add_custom_command(
		TARGET ${PROJECT_NAME} POST_BUILD
		COMMAND ${QT_WINDEPLOYQT_EXECUTABLE}
		--no-translations --no-system-d3d-compiler
		--qmldir ${CMAKE_CURRENT_SOURCE_DIR}
		$<TARGET_FILE_DIR:${PROJECT_NAME}>)
	add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
		$<TARGET_FILE:LSL::lsl>
		$<TARGET_FILE:xdfwriter>
		$<TARGET_FILE_DIR:${PROJECT_NAME}>)
	add_custom_command(
		TARGET ${PROJECT_NAME} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy
		${CMAKE_CURRENT_SOURCE_DIR}//${PROJECT_NAME}.cfg
		$<TARGET_FILE_DIR:${PROJECT_NAME}>)
endif()

if(Qt6_FOUND)
	set_target_properties(${PROJECT_NAME} PROPERTIES
		QT_ANDROID_EXTRA_LIBS "${CMAKE_CURRENT_BINARY_DIR}/liblsl_bin/liblsl.so")
	qt_finalize_executable(${PROJECT_NAME})
endif()

set(CPACK_DEBIAN_LABRECORDER_PACKAGE_SECTION "science" CACHE INTERNAL "")
LSLGenerateCPackConfig()
