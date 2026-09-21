#include "recording.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>

static void require(bool value, const char *message) {
	if (!value) throw std::runtime_error(message);
}

int main(int argc, char **argv) {
	try {
		require(argc == 2, "expected a finalization test scenario");
		const std::string scenario = argv[1];
		const auto mode = scenario == "delayed"		  ? lsl::inlet_state::delayed_result
						  : scenario == "unavailable" ? lsl::inlet_state::unavailable
						  : scenario == "failed"	  ? lsl::inlet_state::failed_transfer
													  : lsl::inlet_state::stalled_worker;
		auto state = std::make_shared<lsl::inlet_state>(mode);
		const auto filename = "finalization-" + scenario + ".xdf";
		auto rec = std::make_unique<recording>(
			filename, std::vector<lsl::stream_info>{lsl::stream_info(state)},
			std::vector<std::string>{}, std::map<std::string, int>{});
		const auto deadline = lsl::clock::now() + std::chrono::seconds(10);
		while (
			!(scenario == "delayed" ? state->query_finished.load() : state->query_started.load()) &&
			lsl::clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		require(state->query_started, "offset worker never started");
		const auto stop = lsl::clock::now();
		if (scenario == "abandoned") {
			const auto done = rec->completionResult();
			rec.reset();
			require(lsl::clock::now() - stop < std::chrono::milliseconds(100),
				"dropping a handle blocked on a stalled worker");
			require(done.wait_for(std::chrono::seconds(6)) == std::future_status::ready,
				"background cleanup did not finish");
			require(done.get().empty() && state->query_finished,
				"background cleanup did not preserve worker lifetime");
			std::filesystem::remove(filename);
			return 0;
		}
		rec->requestStop();
		rec->requestStop(); // Idempotent, including while a worker is stalled.
		require(lsl::clock::now() - stop < std::chrono::milliseconds(100),
				"Stop blocked the caller");
		if (scenario == "stalled")
			require(!rec->waitForFinished(std::chrono::milliseconds(100)),
					"stalled worker reported done");
		require(rec->waitForFinished(std::chrono::seconds(6)), "finalization did not finish");
		require(rec->finalizationError().empty() == (scenario != "failed"),
				"finalization did not accurately report worker failure");
		const auto elapsed = std::chrono::duration<double>(lsl::clock::now() - stop).count();
		rec.reset();

		// Completion must include closing and flushing the writer, not just requesting stop.
		std::ifstream file(filename, std::ios::binary);
		const std::string contents((std::istreambuf_iterator<char>(file)), {});
		require(contents.substr(0, 4) == "XDF:",
				"writer was not flushed before completion");
		require(contents.find(scenario == "failed"
								  ? "<sample_count>0</sample_count>"
								  : "<sample_count>1</sample_count>") != std::string::npos,
				"stream footer is missing or inconsistent");
		require(contents.find("</clock_offsets></info>") != std::string::npos,
				"stream footer was not completely flushed");
		if (scenario == "delayed") {
			require(state->query_finished, "short waits never obtained the delayed offset");
			require(state->query_calls > 1, "test did not exercise polling across timeouts");
			require(contents.find("<offset>") != std::string::npos, "offset missing from footer");
		} else if (scenario == "unavailable") {
			require(elapsed < 1.0, "stop waited for the full offset query budget");
		} else if (scenario == "stalled") {
			require(state->query_finished, "destructor abandoned a running writer owner");
		}
		file.close();
		std::filesystem::remove(filename);
		std::cout << scenario << ": file finalized; stop took " << elapsed << " s\n";
		return 0;
	} catch (const std::exception &e) {
		std::cerr << e.what() << '\n';
		return 1;
	}
}
