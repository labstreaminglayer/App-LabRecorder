#include "recording.h"
#include "xdfwriter.h"

int main(int argc, char **argv) {
	if (argc < 3 || (argc == 2 && std::string(argv[1]) == "-h")) {
		std::cout << "Usage: " << argv[0] << " outputfile.xdf 'searchstr' ['searchstr2' ...]\n\n"
				  << "searchstr can be anything accepted by lsl_resolve_bypred\n";
		std::cout << "Keep in mind that your shell might remove quotes\n";
		std::cout << "Examples:\n\t" << argv[0] << " foo.xdf 'type=\"EEG\"' ";
		std::cout << " 'host=\"LabPC1\" or host=\"LabPC2\"'\n\t";
		std::cout << argv[0] << " foo.xdf'name=\"Tobii and type=\"Eyetracker\"'\n";
		return 1;
	}

	std::vector<lsl::stream_info> infos = lsl::resolve_streams(), recordstreams;

	for (int i = 2; i < argc; ++i) {
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
	recording r(argv[1], recordstreams, watchfor, sync_options, true);
	std::cin.get();
	return 0;
}
