cmake_minimum_required(VERSION 3.5)

project(xdfwriter
	DESCRIPTION "C++ library for writing XDF files"
	HOMEPAGE_URL "https://github.com/labstreaminglayer/App-LabRecorder/"
	LANGUAGES C CXX
	VERSION 1.16.0)

option(LABRECORDER_XDFZ "use Boost.Iostreams for XDFZ support" Off)
option(LABRECORDER_BOOST_TYPE_CONVERSIONS "Use boost for type conversions" Off)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED On)

add_library(${PROJECT_NAME} xdfwriter.cpp)

add_executable(testxdfwriter test_xdf_writer.cpp)

target_link_libraries(testxdfwriter PRIVATE ${PROJECT_NAME})
target_include_directories(${PROJECT_NAME} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)

# Test for floating point format and endianness
if(NOT LABRECORDER_SKIP_CPUTESTS)
	message(STATUS "Determining endianness and floating point format")
	try_run(IS_LITTLE_ENDIAN IS_IEC559
		${CMAKE_CURRENT_BINARY_DIR}
		"${CMAKE_CURRENT_SOURCE_DIR}/test_iec559_and_little_endian.cpp"
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
	target_compile_definitions(${PROJECT_NAME} PUBLIC EXOTIC_ARCH_SUPPORT)
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
	target_compile_definitions(${PROJECT_NAME} PUBLIC XDFZ_SUPPORT=1)
endif()


