#include "recording.h"

void print_usage(char* name) {
	std::cout << "Usage: " << name << " outputfile.xdf 'searchstr' ['searchstr2' ...]\n\n"
			  << "searchstr can be anything accepted by lsl_resolve_bypred\n";
	std::cout << "Keep in mind that your shell might remove quotes\n";
	std::cout << "Examples:\n\t" << name << " foo.xdf type='EEG' ";
	std::cout << " \"host='LabPC1' or host='LabPC2' and starts-with(name,'MyShinySensor')\"\n\t";
	std::cout << name << " foo.xdf \"name='Tobii' and type='Eyetracker'\"\n";
}


int main(int argc, char **argv) {
	if (argc < 3 || (argc == 2 && std::string(argv[1]) == "-h")) {
		print_usage(argv[0]);
		return 1;
	}

	std::vector<lsl::stream_info> infos = lsl::resolve_streams(), recordstreams;
	std::vector<std::string> watchfor;

	for (const auto &info : infos) {
		std::cout << "Available: " << info.name() << '@' << info.hostname() << std::endl;
	}

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
			std::cout << '"' << argv[i] << "\" matched no stream, will keep watching\n";
			watchfor.emplace_back(argv[i]);
		}
	}

	std::map<std::string, int> sync_options;
	std::cout << "Starting the recording" << std::endl;
	recording r(argv[1], recordstreams, watchfor, sync_options, true);

	while (true) std::this_thread::yield();	
}
