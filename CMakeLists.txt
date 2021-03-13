cmake_minimum_required(VERSION 3.5)
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.14" CACHE STRING "Minimum MacOS deployment version")

project(LabRecorder
	DESCRIPTION "Record and write LabStreamingLayer streams to an XDF file"
	HOMEPAGE_URL "https://github.com/labstreaminglayer/App-LabRecorder/"
	LANGUAGES C CXX
	VERSION 1.14.0)

# Needed for customized MacOSXBundleInfo.plist.in
SET(CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake" ${CMAKE_MODULE_PATH})

find_package(LSL REQUIRED
	HINTS ${LSL_INSTALL_ROOT}
	"${CMAKE_CURRENT_LIST_DIR}/../../LSL/liblsl/build/"
	"${CMAKE_CURRENT_LIST_DIR}/../../LSL/liblsl/build/install"
	"${CMAKE_CURRENT_LIST_DIR}/../../LSL/liblsl/out/build/x64-Release"
	"${CMAKE_CURRENT_LIST_DIR}/../../LSL/liblsl/out/install/x64-Release"
	PATH_SUFFIXES share/LSL
)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED On)

set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
find_package(Qt5 REQUIRED COMPONENTS Widgets Network)

find_package(Threads REQUIRED)

add_subdirectory(xdfwriter)

add_executable(${PROJECT_NAME} MACOSX_BUNDLE #WIN32
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
	Qt5::Widgets
	Qt5::Network
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
	get_target_property(QT5_QMAKE_EXECUTABLE Qt5::qmake IMPORTED_LOCATION)
	get_filename_component(QT5_WINDEPLOYQT_EXECUTABLE ${QT5_QMAKE_EXECUTABLE} PATH)
	set(QT5_WINDEPLOYQT_EXECUTABLE "${QT5_WINDEPLOYQT_EXECUTABLE}/windeployqt.exe")

	add_custom_command(
		TARGET ${PROJECT_NAME} POST_BUILD
		COMMAND ${QT5_WINDEPLOYQT_EXECUTABLE} --qmldir
			${CMAKE_CURRENT_SOURCE_DIR} $<TARGET_FILE_DIR:${PROJECT_NAME}>)
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

set(CPACK_DEBIAN_LABRECORDER_PACKAGE_SECTION "science" CACHE INTERNAL "")
LSLGenerateCPackConfig()
