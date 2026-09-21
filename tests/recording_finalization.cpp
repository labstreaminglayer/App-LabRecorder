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

static void independent_progress_test() {
	auto stalled = std::make_shared<lsl::inlet_state>(lsl::inlet_state::stalled_transfer);
	auto active = std::make_shared<lsl::inlet_state>(lsl::inlet_state::queued);
	const std::string filename = "finalization-independent.xdf";
	recording rec(filename, {lsl::stream_info(stalled), lsl::stream_info(active)}, {}, {}, false);
	const auto deadline = lsl::clock::now() + std::chrono::seconds(2);
	while (!stalled->query_started && lsl::clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	require(stalled->query_started, "stalled transfer never started");
	for (int i = 0; i < 7; ++i) active->enqueue(0.1 + i * 0.5, lsl::local_clock() - 110 + i);
	rec.requestStop();
	std::this_thread::sleep_for(std::chrono::milliseconds(2500));
	require(rec.finalizationProgress().idle >= std::chrono::milliseconds(2400),
		"activity on one inlet concealed another inlet's stall");
	require(rec.waitForFinished(std::chrono::seconds(6)), "workers did not eventually finish");
	std::filesystem::remove(filename);
}

static void catchup_test(const std::string &scenario) {
	const bool unknown = scenario == "unknown_clock";
	auto state = std::make_shared<lsl::inlet_state>(unknown
		? lsl::inlet_state::queued_unknown : lsl::inlet_state::queued);
	const std::string filename = "finalization-" + scenario + ".xdf";
	std::map<std::string, int> options;
	if (scenario == "clocksync") options["FinalizationTest (localhost)"] = lsl::post_clocksync;
	// Deliberately disable offset chunks: cutoff comparison must still use clock mapping.
	recording rec(filename, {lsl::stream_info(state)}, {}, options, false);
	const double raw_now = lsl::local_clock() - 100.0;
	const bool slow = scenario == "catchup";
	const bool early = scenario == "finish_now";
	const bool duplicates = scenario == "nonadvancing";
	if (duplicates) {
		for (int i = 0; i < 15; ++i) state->enqueue(0.1 + i * 0.3, raw_now - 1);
	} else if (slow) {
		for (int i = 0; i < 7; ++i) state->enqueue(0.1 + i, raw_now - 10 + i);
	} else {
		state->enqueue(0.1, raw_now - 1);
		state->enqueue(0.3, raw_now + 0.2); // excluded, even after repeated Stop
		state->enqueue(0.6, raw_now - 0.5); // older sample arriving AFTER crossing cutoff
	}
	rec.requestStop();
	const auto started = lsl::clock::now();
	bool intervened = false;
	while (!rec.waitForFinished(std::chrono::milliseconds(20))) {
		const auto elapsed = lsl::clock::now() - started;
		if (!intervened && elapsed > std::chrono::milliseconds(400)) {
			if (early) rec.finishCollecting();
			else rec.requestStop();
			intervened = true;
		}
		if (slow && elapsed < std::chrono::seconds(7))
			require(rec.finalizationProgress().idle < std::chrono::milliseconds(1500),
				"advancing backlog was mistaken for a stall");
		require(elapsed < std::chrono::seconds(12), "catch-up did not finish");
	}
	require(rec.finalizationError().empty(), "catch-up failed");
	if (slow) require(lsl::clock::now() - started > std::chrono::seconds(7),
		"catch-up stopped before delayed upstream samples arrived");
	std::ifstream file(filename, std::ios::binary);
	const std::string data((std::istreambuf_iterator<char>(file)), {});
	const int count = duplicates ? state->pulled.load() : slow ? 7 : early ? 1 : unknown ? 3 : 2;
	if (duplicates) {
		require(count < 15, "non-advancing timestamps kept collection open indefinitely");
		require(lsl::clock::now() - started < std::chrono::seconds(3), "fallback grace was not bounded");
	}
	require(data.find("<sample_count>" + std::to_string(count) + "</sample_count>") != std::string::npos,
		"cutoff lost pre-stop data or retained post-stop data");
	const auto reason = early ? "user_requested" : unknown ? "clock_unavailable"
		: (slow || duplicates) ? "inactivity_timeout" : "cutoff_observed";
	require(data.find(std::string("<reason>") + reason + "</reason>") != std::string::npos,
		"footer does not explain why collection ended");
	file.close();
	std::filesystem::remove(filename);
}

int main(int argc, char **argv) {
	try {
		require(argc == 2, "expected a finalization test scenario");
		const std::string scenario = argv[1];
		if (scenario == "independent_progress") {
			independent_progress_test();
			return 0;
		}
		if (scenario == "catchup" || scenario == "cutoff" || scenario == "clocksync" ||
			scenario == "unknown_clock" || scenario == "finish_now" || scenario == "nonadvancing") {
			catchup_test(scenario);
			return 0;
		}
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
			require(elapsed < 3.0, "stop waited for the full offset query budget");
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
