cmake_minimum_required(VERSION 3.5)

project(LabRecorder
	LANGUAGES CXX
	VERSION 1.12.0)

include(Findliblsl.cmake)

option(LABRECORDER_XDFZ "use Boost.Iostreams for XDFZ support" Off)
option(LABRECORDER_BOOST_TYPE_CONVERSIONS "Use boost for type conversions" Off)

# GENERAL CONFIG #
set(META_PROJECT_DESCRIPTION "Record LabStreamingLayer streams to XDF data file.")

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED On)

find_package(Qt5 REQUIRED COMPONENTS Widgets)

find_package(Threads REQUIRED)

add_executable(${PROJECT_NAME} MACOSX_BUNDLE #WIN32
	main.cpp
	mainwindow.cpp
	mainwindow.h
	mainwindow.ui
	recording.h
	recording.cpp
	conversions.h
	xdfwriter.h
	xdfwriter.cpp
)

add_executable(LabRecorderCLI MACOSX_BUNDLE
	clirecorder.cpp
	recording.h
	recording.cpp
	xdfwriter.h
	xdfwriter.cpp
)

add_executable(testxdfwriter
	test_xdf_writer.cpp
	xdfwriter.h
	xdfwriter.cpp
)

target_link_libraries(${PROJECT_NAME}
	PRIVATE
	Qt5::Widgets
	Threads::Threads
	LSL::lsl
)

target_link_libraries(LabRecorderCLI
	PRIVATE
	Threads::Threads
	LSL::lsl
)

# Test for floating point format and endianness
try_run(IS_LITTLE_ENDIAN IS_IEC559
	${CMAKE_CURRENT_BINARY_DIR}
	"${CMAKE_CURRENT_SOURCE_DIR}/test_iec559_and_little_endian.cpp"
	CMAKE_FLAGS "-DCMAKE_CXX_STANDARD=14" "-DCMAKE_CXX_STANDARD_REQUIRED=On"
	COMPILE_OUTPUT_VARIABLE IEC559_COMPILE)
message(STATUS "Little endian: ${IS_LITTLE_ENDIAN}")
message(STATUS "IEC559: ${IS_IEC559}")
if(NOT IS_IEC559)
	message(WARNING "IEC559 test: ${IEC559_COMPILE}")
endif()


if(LABRECORDER_BOOST_TYPE_CONVERSIONS OR NOT IS_LITTLE_ENDIAN OR NOT IS_IEC559)
	message(STATUS "Trying to use Boost for type conversions")
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
)

LSLGenerateCPackConfig()
