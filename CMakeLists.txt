cmake_minimum_required(VERSION 3.12)
#set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum MacOS deployment version")

project(LabRecorder
	DESCRIPTION "Record and write LabStreamingLayer streams to an XDF file"
	HOMEPAGE_URL "https://github.com/labstreaminglayer/App-LabRecorder/"
	LANGUAGES C CXX
	VERSION 1.16.5
)

# Needed for customized MacOSXBundleInfo.plist.in
SET(CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake" ${CMAKE_MODULE_PATH})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED On)

option(BUILD_GUI "Build the GUI, set to off for CLI only build" ON)

option(BUILD_LIBLSL_FROM_SOURCE
    "Build liblsl from a local subdirectory, rather than using find_package(LSL)" OFF
)

set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO")
set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "")

# Dependencies

## LSL
if(BUILD_LIBLSL_FROM_SOURCE)
	set(LIBLSL_SOURCE_PATH
		"${CMAKE_CURRENT_LIST_DIR}/liblsl"
		CACHE PATH
		"Path to the local liblsl sources for building from source."
	)
	message(STATUS "Building liblsl from source at: ${LIBLSL_SOURCE_PATH}")
	add_subdirectory("${LIBLSL_SOURCE_PATH}" liblsl_bin)
	# This is an alias to match the usage below, so we can do:
	#    target_link_libraries(... LSL::lsl)
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

if (BUILD_GUI)
	## Qt
	set(CMAKE_AUTOMOC ON)  # The later version of this in LSLCMake is somehow not enough.
	set(CMAKE_AUTORCC ON)
	set(CMAKE_AUTOUIC ON)
	find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core)
	find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Widgets Network DBus)
endif(BUILD_GUI)

## Threads
find_package(Threads REQUIRED)

# Targets

## xdfwriter - stand alone library
add_subdirectory(xdfwriter)

if (BUILD_GUI)
	add_executable(${PROJECT_NAME} MACOSX_BUNDLE)

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

	target_link_libraries(${PROJECT_NAME}
		PRIVATE
		xdfwriter
		Qt${QT_VERSION_MAJOR}::Core
		Qt${QT_VERSION_MAJOR}::Widgets
		Qt${QT_VERSION_MAJOR}::Network
		Qt${QT_VERSION_MAJOR}::DBus
		Threads::Threads
		LSL::lsl
	)
endif(BUILD_GUI)


add_executable(LabRecorderCLI MACOSX_BUNDLE
	src/clirecorder.cpp
	src/recording.h
	src/recording.cpp
)

target_link_libraries(LabRecorderCLI
	PRIVATE
	xdfwriter
	Threads::Threads
	LSL::lsl
)

installLSLApp(xdfwriter)
installLSLApp(testxdfwriter)
installLSLApp(LabRecorderCLI)
if (BUILD_GUI)
	installLSLApp(${PROJECT_NAME})
	installLSLAuxFiles(${PROJECT_NAME}
		${PROJECT_NAME}.cfg
		LICENSE
		README.md
	)
else()
	installLSLAuxFiles(LabRecorderCLI
		${PROJECT_NAME}.cfg
		LICENSE
		README.md
	)
endif(BUILD_GUI)


if (WIN32)
	if(BUILD_GUI)
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
	else()
		add_custom_command(TARGET LabRecorderCLI POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
			$<TARGET_FILE:LSL::lsl>
			$<TARGET_FILE:xdfwriter>
			$<TARGET_FILE_DIR:LabRecorderCLI>)
	endif(BUILD_GUI)
endif()

if (BUILD_GUI)
	add_custom_command(
		TARGET ${PROJECT_NAME} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy
		${CMAKE_CURRENT_SOURCE_DIR}//${PROJECT_NAME}.cfg
		$<TARGET_FILE_DIR:${PROJECT_NAME}>)
else()
	add_custom_command(
		TARGET LabRecorderCLI POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy
		${CMAKE_CURRENT_SOURCE_DIR}//${PROJECT_NAME}.cfg
		$<TARGET_FILE_DIR:LabRecorderCLI>)
endif(BUILD_GUI)

if(Qt6_FOUND AND BUILD_GUI)
	set_target_properties(${PROJECT_NAME} PROPERTIES
		QT_ANDROID_EXTRA_LIBS "${CMAKE_CURRENT_BINARY_DIR}/liblsl_bin/liblsl.so")
	qt_finalize_executable(${PROJECT_NAME})
endif(Qt6_FOUND AND BUILD_GUI)

set(CPACK_DEBIAN_LABRECORDER_PACKAGE_SECTION "science" CACHE INTERNAL "")
LSLGenerateCPackConfig()

if(APPLE AND NOT DEFINED ENV{GITHUB_ACTIONS})
	# Qt6 QtNetwork depends on libbrotidec which depends on libbroticommon but whose search path uses @loader_path.
	#  Unfortunately, macdeployqt does not seem to traverse @loader_path dependencies.
	#  So we are forced to call `fixup_bundle`. For now, we only do this if homebrew is present
	#  because that seems to be where the bad dependency is coming from.
	# Note that fixup_bundle also destroys the codesigning so we have to redo that.
 	# TODO: Checkout supercollider apple-specific stuff, e.g.: https://github.com/supercollider/supercollider/blob/develop/CMakeLists.txt#L260-L262
	execute_process(
        COMMAND brew --prefix
        RESULT_VARIABLE BREW_LIB
        OUTPUT_VARIABLE BREW_PREFIX
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
	if (BREW_LIB EQUAL 0 AND EXISTS "${BREW_PREFIX}")
		install(CODE
				"
					include(BundleUtilities)
					fixup_bundle(\"${CMAKE_INSTALL_PREFIX}/${PROJECT_NAME}/${PROJECT_NAME}.app\" \"\" \"${BREW_PREFIX}/lib\")
					execute_process(COMMAND codesign --force --deep --sign - ${CMAKE_INSTALL_PREFIX}/${PROJECT_NAME}/${PROJECT_NAME}.app)
				"
			)
	endif()
endif(APPLE AND NOT DEFINED ENV{GITHUB_ACTIONS})
