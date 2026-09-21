#include "recording.h"
#include "xdfwriter.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <thread>

int main(int argc, char **argv) {
	int output_arg = 1;
	double stop_timeout = 5.0;
	if (argc > 1 && std::string(argv[1]) == "--stop-timeout") {
		try {
			if (argc < 3) throw std::invalid_argument("missing value");
			size_t used = 0;
			stop_timeout = std::stod(argv[2], &used);
			if (used != std::string(argv[2]).size() || !std::isfinite(stop_timeout) ||
				stop_timeout <= 0 || stop_timeout > 3600)
				throw std::invalid_argument("must be greater than zero and at most 3600 seconds");
			output_arg = 3;
		} catch (const std::exception &e) {
			std::cerr << "Invalid --stop-timeout: " << e.what() << std::endl;
			return 1;
		}
	}
	if (argc < output_arg + 2 || std::string(argv[output_arg]) == "--help" ||
		std::string(argv[output_arg]) == "-h") {
		std::cout
			<< "Usage: " << argv[0]
			<< " [--stop-timeout SECONDS] outputfile.xdf 'searchstr' ['searchstr2' ...]\n"
			<< "Search strings use lsl_resolve_bypred syntax.\n"
			<< "Stop timeout defaults to 5 seconds; an unfinished file exits with status 3.\n";
		return 1;
	}

	std::vector<lsl::stream_info> infos = lsl::resolve_streams(), recordstreams;

	for (int i = output_arg + 1; i < argc; ++i) {
		bool matched = false;
		for (const auto &info : infos) {
			if (info.matches_query(argv[i])) {
				std::cout << "Found " << info.name() << '@' << info.hostname();
				std::cout << " matching '" << argv[i] << "'\n";
				matched = true;
				recordstreams.emplace_back(info);
			}
		}
		if (!matched) {
			std::cout << '"' << argv[i] << "\" matched no stream!\n";
			return 2;
		}
	}

	std::vector<std::string> watchfor;
	std::map<std::string, int> sync_options;
	std::cout << "Starting the recording, press Enter to quit" << std::endl;
	try {
		recording r(argv[output_arg], recordstreams, watchfor, sync_options, true);
		std::cin.get();
		r.requestStop();
		const auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::duration<double>(stop_timeout));
		if (!r.waitForFinished(timeout)) {
			// Even reporting the timeout must not hang if a stalled worker holds an iostream
			// lock or stderr is backed by a blocked pipe. Allow a brief best-effort diagnostic.
			std::thread([] {
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				std::_Exit(3);
			}).detach();
			std::cerr << "Finalization timed out. The recording may be incomplete: "
					  << argv[output_arg] << std::endl;
			// No destructors/atexit handlers: a stalled worker could hold their locks too.
			std::_Exit(3);
		}
		const auto error = r.finalizationError();
		if (!error.empty()) {
			std::cerr << "Finalization failed: " << error << std::endl;
			return 4;
		}
		return 0;
	} catch (const std::exception &e) {
		std::cerr << "Recording failed: " << e.what() << std::endl;
		return 4;
	}
}
