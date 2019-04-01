#include "xdfwriter.h"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <sstream>

int main(int argc, char** argv) {
	XDFWriter w1("test_chunk3.xdf"), w2("test_chunk7.xdf");
	const uint32_t sid = 0x02C0FFEE, strsid = 0xDEADBEEF;
	const int blocks = argc > 1 ? std::stoi(argv[1]) : 30,
		n_samples = argc > 2 ? std::stoi(argv[2]) : 10000;
	const std::string header("<?xml version=\"1.0\"?>"
	                         "<info>"
	                         "<name>SendDataC</name>"
	                         "<type>EEG</type>"
	                         "<channel_count>64</channel_count>"
	                         "<nominal_srate>1000</nominal_srate>"
	                         "<channel_format>double64</channel_format>"
	                         "<created_at>10</created_at>"
	                         "</info>");
	const std::string strhdr("<?xml version=\"1.0\"?>"
	                         "<info>"
	                         "<name>SendMarker</name>"
	                         "<type>Marker</type>"
	                         "<channel_count>2</channel_count>"
	                         "<nominal_srate>1000</nominal_srate>"
	                         "<channel_format>string</channel_format>"
	                         "<created_at>10</created_at>"
	                         "</info>");
	w1.add_stream(sid, header, 64, Stream::Sampletype::double_, "SendDataC");
	w2.add_stream(sid, header, 64, Stream::Sampletype::double_, "SendDataC");
	w1.add_stream(strsid, strhdr, 2, Stream::Sampletype::string_, "SendMarker");
	w2.add_stream(strsid, strhdr, 2, Stream::Sampletype::string_, "SendMarker");
	w1.write_boundary_chunk();
	w2.write_boundary_chunk();

	// write a single int16_t sample
	const uint32_t n_channels = 64;
	std::vector<double> vals(n_samples * n_channels), timestamps(n_samples);
	std::vector<std::string> strvals(n_samples*2);
	for (auto i = 0u; i < n_samples; i++) {
		for( auto chan = 0u; chan < 2; ++chan)
			strvals[2*i+chan] = "Marker " + std::to_string(i) + (chan?'B':'A');
		for (auto chan = 0u; chan < n_channels; chan++)
			vals[n_channels * i + chan] = sin(i/(chan+1.));
	}

	for (int i = 0; i < blocks; ++i) {
		for(auto j = 0u; j < n_samples; ++j) timestamps[j] = 17 + (i * n_samples + j)/1000.;
		w1.write_boundary_chunk();
		w2.write_boundary_chunk();
		w1.write_data_chunk(sid, timestamps, vals.data(), n_samples, n_channels);
		w2.write_better_data_chunk(sid, timestamps, vals.data(), n_samples, n_channels);
		w1.write_data_chunk(strsid, timestamps, strvals.data(), n_samples, 1);
		w2.write_better_data_chunk(strsid, timestamps, strvals.data(), n_samples, 1);
	}
	w1.write_boundary_chunk();
	w2.write_boundary_chunk();
	w1.write_stream_offset(sid, 6, -.1);
	w2.write_stream_offset(sid, 6, -.1);
	w1.write_stream_offset(strsid, 6, -.1);
	w2.write_stream_offset(strsid, 6, -.1);

	std::ostringstream footer;
	footer <<
	    "<?xml version=\"1.0\"?><info><first_timestamp>10</first_timestamp>"
	    << "<last_timestamp>" << (10+blocks*n_samples/1000.) << "</last_timestamp>"
	    << "<sample_count>" << (blocks*n_samples) << "</sample_count>"
		<< "</info>";
	const auto footerstr = footer.str();
	w1.write_stream_footer(sid, footerstr);
	w2.write_stream_footer(sid, footerstr);
	w1.write_stream_footer(strsid, footerstr);
	w2.write_stream_footer(strsid, footerstr);
	std::cout << "Wrote " << blocks << " chunks (" << n_samples << " samples each)" << std::endl;
}
