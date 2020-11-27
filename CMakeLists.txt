cmake_minimum_required(VERSION 3.5)

project(LabRecorder
	DESCRIPTION "Record and write LabStreamingLayer streams to an XDF file"
	HOMEPAGE_URL "https://github.com/labstreaminglayer/App-LabRecorder/"
	LANGUAGES C CXX
	VERSION 1.14.0)

find_package(LSL REQUIRED
	HINTS ${LSL_INSTALL_ROOT}
	"${CMAKE_CURRENT_LIST_DIR}/../../LSL/liblsl/build/"
	"${CMAKE_CURRENT_LIST_DIR}/../../LSL/liblsl/build/install"
	"${CMAKE_CURRENT_LIST_DIR}/../../LSL/liblsl/out/build/x64-Release"
	"${CMAKE_CURRENT_LIST_DIR}/../../LSL/liblsl/out/install/x64-Release"
	PATH_SUFFIXES share/LSL
)

option(LABRECORDER_XDFZ "use Boost.Iostreams for XDFZ support" Off)
option(LABRECORDER_BOOST_TYPE_CONVERSIONS "Use boost for type conversions" Off)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED On)

set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
find_package(Qt5 REQUIRED COMPONENTS Widgets Network)

find_package(Threads REQUIRED)

add_executable(${PROJECT_NAME} MACOSX_BUNDLE #WIN32
	src/main.cpp
	src/mainwindow.cpp
	src/mainwindow.h
	src/mainwindow.ui
	src/recording.h
	src/recording.cpp
	src/conversions.h
	src/tcpinterface.h
	src/tcpinterface.cpp
	src/xdfwriter.h
	src/xdfwriter.cpp
)

add_executable(LabRecorderCLI MACOSX_BUNDLE
	src/clirecorder.cpp
	src/recording.h
	src/recording.cpp
	src/xdfwriter.h
	src/xdfwriter.cpp
)

add_executable(testxdfwriter
	src/test_xdf_writer.cpp
	src/xdfwriter.h
	src/xdfwriter.cpp
)

target_link_libraries(${PROJECT_NAME}
	PRIVATE
	Qt5::Widgets
	Qt5::Network
	Threads::Threads
	LSL::lsl
)

target_link_libraries(LabRecorderCLI
	PRIVATE
	Threads::Threads
	LSL::lsl
)

# Test for floating point format and endianness
if(NOT LABRECORDER_SKIP_CPUTESTS)
	message(STATUS "Determining endianness and floating point format")
	try_run(IS_LITTLE_ENDIAN IS_IEC559
		${CMAKE_CURRENT_BINARY_DIR}
		"${CMAKE_CURRENT_SOURCE_DIR}/src/test_iec559_and_little_endian.cpp"
		CMAKE_FLAGS "-DCMAKE_CXX_STANDARD=14" "-DCMAKE_CXX_STANDARD_REQUIRED=On"
		COMPILE_OUTPUT_VARIABLE IEC559_COMPILE)
	if(NOT IS_IEC559)
		message(WARNING "IEC559 test: ${IEC559_COMPILE}")
	endif()
	message(STATUS "Little endian: ${IS_LITTLE_ENDIAN}")
	message(STATUS "IEC559: ${IS_IEC559}")
	set(LABRECORDER_SKIP_CPUTESTS ON CACHE BOOL "Assume CPU tests were already run")
	if(NOT IS_LITTLE_ENDIAN OR  NOTIS_IEC559)
		message(WARNING "The target cpu is non-standard. Force enabling Boost type conversions.")
		set(LABRECORDER_BOOST_TYPE_CONVERSIONS ON CACHE BOOL "Force enabled Boost type conversions" FORCE)
	endif()
endif()

if(LABRECORDER_BOOST_TYPE_CONVERSIONS)
	message(STATUS "Searching Boost for type conversions")
	find_package(Boost REQUIRED)
	target_link_libraries(${PROJECT_NAME} PRIVATE Boost::boost)
	target_compile_definitions(${PROJECT_NAME} PRIVATE EXOTIC_ARCH_SUPPORT)
endif()

# Enable xdfz support if Boost::iostreams and Boost.zlib (Windows) or plain zlib (Unix) was found
if(LABRECORDER_XDFZ)
	find_package(Boost REQUIRED COMPONENTS iostreams)
	if(WIN32)
		find_package(Boost REQUIRED COMPONENTS zlib)
		target_link_libraries(${PROJECT_NAME} PRIVATE Boost::iostreams Boost::zlib)
	else()
		find_package(ZLIB REQUIRED)
		target_link_libraries(${PROJECT_NAME} PRIVATE Boost::iostreams ${ZLIB_LIBRARIES})
	endif()
	message(STATUS "Found zlib, enabling support for xdfz files")
	target_compile_definitions(${PROJECT_NAME} PRIVATE XDFZ_SUPPORT=1)
endif()

installLSLApp(${PROJECT_NAME})
installLSLApp(LabRecorderCLI)
installLSLApp(testxdfwriter)
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
			$<TARGET_FILE_DIR:${PROJECT_NAME}>)
	add_custom_command(
		TARGET ${PROJECT_NAME} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy
			${CMAKE_CURRENT_SOURCE_DIR}//${PROJECT_NAME}.cfg
			$<TARGET_FILE_DIR:${PROJECT_NAME}>)
endif()

set(CPACK_DEBIAN_LABRECORDER_PACKAGE_SECTION "science" CACHE INTERNAL "")
LSLGenerateCPackConfig()
