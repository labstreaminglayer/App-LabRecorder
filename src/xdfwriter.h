#pragma once

#include "conversions.h"
#include "xdf_defs.h"

#include <cassert>
#include <mutex>
#include <sstream>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

#ifdef XDFZ_SUPPORT
#include <boost/iostreams/filtering_stream.hpp>
using outfile_t = boost::iostreams::filtering_ostream;
#else
#include <fstream>
using outfile_t = std::ofstream;
#endif

/**
 * @brief The XDFVersion enum indicates which XDF features can be used
 */
enum class XDFVersion { v10 = 100, v11 = 110 };

class XDFWriter {
private:
	outfile_t file_;
	void _write_chunk_header(
		chunk_tag_t tag, std::size_t length, const streamid_t *streamid_p = nullptr);
	std::unordered_map<streamid_t, Stream> streams;
	std::mutex write_mut;
	const XDFVersion version;

	// write a generic chunk
	void _write_chunk(
		chunk_tag_t tag, const std::string &content, const streamid_t *streamid_p = nullptr);

	const Stream &get_stream(streamid_t streamid) const {
		auto streamit = streams.find(streamid);
		if (streamit == streams.end()) throw std::runtime_error("Unknown stream id");
		return streamit->second;
	}

public:
	/**
	 * @brief XDFWriter Construct a XDFWriter object
	 * @param filename  Filename to write to
	 */
	XDFWriter(const std::string &filename, XDFVersion version = XDFVersion::v10);

	template <typename T>
	void write_data_chunk(
		streamid_t streamid, const std::vector<double> &timestamps, const std::vector<T> &chunk) {
		assert(timestamps.size() * get_stream(streamid).nchannels == chunk.size());
		write_data_chunk(streamid, timestamps, chunk.data());
	}
	template <typename T>
	void write_data_chunk(
		streamid_t streamid, const std::vector<double> &timestamps, const T *chunk) {
		if (version == XDFVersion::v10)
			write_data_chunk_3(streamid, timestamps, chunk);
		else
			write_data_chunk_7(streamid, timestamps, chunk);
	}

	template <typename T>
	void write_data_chunk_nested(streamid_t streamid, const std::vector<double> &timestamps,
		const std::vector<std::vector<T>> &chunk);

	template <typename T>
	void write_data_chunk_3(
		streamid_t streamid, const std::vector<double> &timestamps, const T *chunk);
	template <typename T>
	void write_data_chunk_7(
		streamid_t streamid, const std::vector<double> &timestamps, const T *chunk);

	/**
	 * Write the stream header and add the stream header to the file
	 * @brief write_stream_header Write the stream header, see also
	 * @see https://github.com/sccn/xdf/wiki/Specifications#clockoffset-chunk
	 * @param streamid Numeric stream identifier
	 * @param content XML-formatted stream header
	 * @param nchannels Number of channels
	 * @param sampletype data type of stream contents
	 * @param name Stream name
	 */
	void add_stream(streamid_t streamid, const std::string &content, uint32_t nchannels,
		Stream::Sampletype sampletype, std::string name);
	/**
	 * @brief write_stream_footer
	 * @see https://github.com/sccn/xdf/wiki/Specifications#streamfooter-chunk
	 */
	void write_stream_footer(streamid_t streamid, const std::string &content);
	/**
	 * @brief write_stream_offset Record the time discrepancy between the
	 * streaming and the recording PC
	 * @see https://github.com/sccn/xdf/wiki/Specifications#clockoffset-chunk
	 */
	void write_stream_offset(streamid_t streamid, double collectiontime, double offset);
	/**
	 * @brief write_boundary_chunk Insert a boundary chunk that's mostly used
	 * to recover from errors in XDF files by providing a restart marker.
	 */
	void write_boundary_chunk();
};

inline void write_ts(std::ostream &out, double ts) {
	// write timestamp
	if (ts == 0)
		out.put(0);
	else {
		// [TimeStampBytes]
		out.put(8);
		// [TimeStamp]
		write_little_endian(out, ts);
	}
}

template <typename T>
void XDFWriter::write_data_chunk_3(
	streamid_t streamid, const std::vector<double> &timestamps, const T *chunk) {
	/**
	  Samples data chunk: [Tag 3] [VLA ChunkLen] [StreamID] [VLA NumSamples]
	  [NumSamples x [VLA TimestampLen] [TimeStampLen]
	  [NumSamples x NumChannels Sample]
	  */
	auto n_samples = timestamps.size();
	auto &stream = get_stream(streamid);

	// generate [Samples] chunk contents...

	std::ostringstream out;
	write_fixlen_int(out, 0x0FFFFFFF); // Placeholder length, will be replaced later
	for (double ts : timestamps) {
		write_ts(out, ts);
		// write sample, get the current position in the chunk array back
		chunk = write_sample_values(out, chunk, stream.nchannels);
	}
	std::string outstr(out.str());
	// Replace length placeholder
	auto s = static_cast<uint32_t>(n_samples);
	std::copy(reinterpret_cast<char *>(&s), reinterpret_cast<char *>(&s + 1), outstr.begin() + 1);

	std::lock_guard<std::mutex> lock(write_mut);
	_write_chunk(chunk_tag_t::samples, outstr, &streamid);
}

template <typename T>
void XDFWriter::write_data_chunk_nested(streamid_t streamid, const std::vector<double> &timestamps,
	const std::vector<std::vector<T>> &chunk) {
	if (chunk.size() == 0) return;
	auto n_samples = timestamps.size();
	if (timestamps.size() != chunk.size())
		throw std::runtime_error("timestamp / sample count mismatch");
	auto n_channels = get_stream(streamid).nchannels;

	// generate [Samples] chunk contents...

	std::ostringstream out;
	write_fixlen_int(out, 0x0FFFFFFF); // Placeholder length, will be replaced later
	auto sample_it = chunk.cbegin();
	for (double ts : timestamps) {
		assert(n_channels == sample_it->size());
		write_ts(out, ts);
		// write sample, get the current position in the chunk array back
		write_sample_values(out, sample_it->data(), n_channels);
		sample_it++;
	}
	std::string outstr(out.str());
	// Replace length placeholder
	auto s = static_cast<uint32_t>(n_samples);
	std::copy(reinterpret_cast<char *>(&s), reinterpret_cast<char *>(&s + 1), outstr.begin() + 1);
	std::lock_guard<std::mutex> lock(write_mut);
	_write_chunk(chunk_tag_t::samples, outstr, &streamid);
}

template <typename T>
void XDFWriter::write_data_chunk_7(
	streamid_t streamid, const std::vector<double> &timestamps, const T *chunk) {
	/**
	  Samples data chunk: [Tag 7] [VLA ChunkLen] [StreamID] [uint32 NumSamples]
	  [Timestamps, double]
	  [NumSamples x NumChannels Sample]
	  */
	auto n_samples = timestamps.size();

	auto &stream = get_stream(streamid);
	auto n_channels = stream.nchannels;

	uint8_t sampletype = lsltype<T>::index;
	if (static_cast<uint8_t>(stream.sampletype) != sampletype)
		throw std::runtime_error("Mismatching sample types");
	auto len = 2 * sizeof(uint32_t) + sizeof(sampletype) + timestamps.size() * sizeof(double) +
			   n_samples * n_channels * sizeof(T);
	std::lock_guard<std::mutex> lock(write_mut);
	_write_chunk_header(chunk_tag_t::samples_v2, len, &streamid);
	write_little_endian(file_, n_samples);
	write_little_endian(file_, n_channels);
	write_little_endian(file_, sampletype);
	write_chunk7_samples(file_, timestamps);
	write_chunk7_samples(file_, chunk, n_samples * n_channels);
}
