#include <utility>

#pragma once

#include <cstdint>
#include <string>

// the currently defined chunk tags
enum class chunk_tag_t : uint16_t {
	fileheader = 1,   // FileHeader chunk
	streamheader = 2, // StreamHeader chunk
	samples = 3,	  // Samples chunk
	clockoffset = 4,  // ClockOffset chunk
	boundary = 5,	 // Boundary chunk
	streamfooter = 6, // StreamFooter chunk
	samples_v2 = 7,   // Samples chunk with optimized layout
	undefined = 0
};

/**
 * @brief The XDFVersion enum indicates which XDF features can be used
 */
enum class XDFVersion { v10 = 100, v11 = 110 };

template<typename T>
struct lsltype {
};

template<> struct lsltype<int8_t> {
	static constexpr int8_t index = 6;
};

template <> struct lsltype<char> : public lsltype<int8_t> {};

template<> struct lsltype<int16_t> {
	static constexpr int8_t index = 5;
};

template<> struct lsltype<int32_t> {
	static constexpr int8_t index = 4;
};

template<> struct lsltype<int64_t> {
	static constexpr int8_t index = 7;
};

template<> struct lsltype<float> {
	static constexpr int8_t index = 1;
};

template<> struct lsltype<double> {
	static constexpr int8_t index = 2;
};

template<> struct lsltype<std::string> {
	static constexpr int8_t index = 3;
};

using streamid_t = uint32_t;

struct Stream {
	const streamid_t streamid;
	const enum class Sampletype {
		float_ = 1,
		double_, string_, int32_, int16_, int8_, int64
	} sampletype;
	const uint32_t nchannels;
	const std::string name;
	Stream(streamid_t streamid, Sampletype sampletype, uint32_t nchannels, std::string name):
		streamid(streamid), sampletype(sampletype), nchannels(nchannels), name(std::move(name)) {}
};

#ifdef XDFZ_SUPPORT
#include <boost/iostreams/filtering_stream.hpp>
using outfile_t = boost::iostreams::filtering_ostream;
using infile_t = boost::iostreams::filtering_istream;
#else
#include <fstream>
using outfile_t = std::ofstream;
using infile_t = std::ifstream;
#endif
