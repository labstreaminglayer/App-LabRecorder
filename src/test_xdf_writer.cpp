#include "xdfwriter.h"

int main(int argc, char **argv) {
	XDFWriter w("test.xdf");
	const uint32_t sid = 0x02C0FFEE;
	const std::string footer(
		"<?xml version=\"1.0\"?>"
		"<info>"
		"<writer>LabRecorder xdfwriter</writer>"
		"<first_timestamp>5.1</first_timestamp>"
		"<last_timestamp>5.9</last_timestamp>"
		"<sample_count>9</sample_count>"
		"<clock_offsets>"
		"<offset><time>50979.76</time><value>-.01</value></offset>"
		"<offset><time>50979.86</time><value>-.02</value></offset>"
		"</clock_offsets></info>");
	w.add_stream(0,
		"<?xml version=\"1.0\"?>"
		"<info>"
		"<name>SendDataC</name>"
		"<type>EEG</type>"
		"<channel_count>3</channel_count>"
		"<nominal_srate>10</nominal_srate>"
		"<channel_format>int16</channel_format>"
		"<created_at>50942.723319709003</created_at>"
		"<desc/>"
		"<uid>xdfwriter_11_int</uid>"
		"</info>",
		3, Stream::Sampletype::int16_, "SendDataC");
	w.add_stream(sid,
		"<?xml version=\"1.0\"?>"
		"<info>"
		"<name>SendDataString</name>"
		"<type>StringMarker</type>"
		"<channel_count>1</channel_count>"
		"<nominal_srate>10</nominal_srate>"
		"<channel_format>string</channel_format>"
		"<created_at>50942.723319709003</created_at>"
		"<desc/>"
		"<uid>xdfwriter_11_int</uid>"
		"</info>",
		1, Stream::Sampletype::string_, "SendDataString");
	w.write_boundary_chunk();

	// write a single int16_t sample
	w.write_data_chunk(0, {5.1}, std::vector<int16_t>{0xC0, 0xFF, 0xEE});

	// write a single std::string sample with a length > 127
	w.write_data_chunk(sid, {5.1}, std::vector<std::string>{footer});

	// write multiple samples
	std::vector<double> ts{5.2, 0, 0, 5.5};
	std::vector<int16_t> data{12, 22, 32, 13, 23, 33, 14, 24, 34, 15, 25, 35};
	std::vector<std::string> data_str{"Hello", "World", "from", "LSL"};
	w.write_data_chunk(0, ts, data);
	w.write_data_chunk(sid, ts, data_str);

	// write data from nested vectors
	ts = std::vector<double>{5.6, 0, 0, 0};
	std::vector<std::vector<int16_t>> data2{{12, 22, 32}, {13, 23, 33}, {14, 24, 34}, {15, 25, 35}};
	std::vector<std::vector<std::string>> data2_str{{"Hello"}, {"World"}, {"from"}, {"LSL"}};
	w.write_data_chunk_nested(0, ts, data2);
	w.write_data_chunk_nested(sid, ts, data2_str);

	w.write_boundary_chunk();
	w.write_stream_offset(0, 6, -.1);
	w.write_stream_offset(0, 7, -.1);
	//w.write_stream_offset(sid, 5, -.2);
	//w.write_stream_offset(sid, 6, -.2);

	w.write_stream_footer(0, footer);
	w.write_stream_footer(sid, footer);

	XDFWriter w2("test_v11.xdf");
	w2.add_stream(0,
		"<?xml version=\"1.0\"?>"
		"<info>"
		"<name>SendDataC</name>"
		"<type>EEG</type>"
		"<channel_count>3</channel_count>"
		"<nominal_srate>10</nominal_srate>"
		"<channel_format>int16</channel_format>"
		"<created_at>50942.723319709003</created_at>"
		"<desc/>"
		"<uid>xdfwriter_11_int</uid>"
		"</info>",
		3, Stream::Sampletype::int16_, "SendDataC");
	w2.add_stream(sid,
		"<?xml version=\"1.0\"?>"
		"<info>"
		"<name>SendDataString</name>"
		"<type>StringMarker</type>"
		"<channel_count>2</channel_count>"
		"<nominal_srate>10</nominal_srate>"
		"<channel_format>string</channel_format>"
		"<created_at>50942.723319709003</created_at>"
		"<desc/>"
		"<uid>xdfwriter_11_str</uid>"
		"</info>",
		2, Stream::Sampletype::string_, "SendDataString");
	w2.write_boundary_chunk();

	// write a single int16_t sample
	w2.write_data_chunk_3(0, {5.1}, std::vector<int16_t>{0xC0, 0xFF, 0xEE}.data());

	// write a single std::string sample with a length > 127
	w2.write_data_chunk_3(sid, {5.1}, std::vector<std::string>{footer, footer}.data());

	// write multiple samples
	std::vector<std::string> data_2ch_str{"Hello", "Hallo", "World", "Welt", "from", "von", "LSL", "LSL"};
	w2.write_data_chunk_7(0, ts, data.data());
	w2.write_data_chunk_7(sid, ts, data_2ch_str.data());

	w2.write_boundary_chunk();
	w2.write_stream_offset(0, 6, -.1);
	w2.write_stream_offset(sid, 5, -.2);

	w2.write_stream_footer(0, footer);
	w2.write_stream_footer(sid, footer);
}
