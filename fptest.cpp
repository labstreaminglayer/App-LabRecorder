#include <iostream>

int main() {
	const double sampling_rate = 500,
	      sample_interval = 1.0 / sampling_rate,
	      first_timestamp = 60*60*24*365*48; // e.g. a unix timestamp
	double last_timestamp = first_timestamp;
	int full = 0, deduced = 0;
	const int iterations = 100000;
	double timestamps[iterations];
	for(int i=0;i<iterations; ++i)
		timestamps[i] = first_timestamp + i/sampling_rate;
	for(int i=0; i<iterations; ++i) {
		if(last_timestamp + sample_interval == timestamps[i])
			deduced++;
		else full++;
		last_timestamp = timestamps[i];
	}
	std::cout << "Deduced: " << deduced << "\nFully written: " << full << std::endl;
	return 0;
}


