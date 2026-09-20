#pragma once

// A deterministic inlet for recording finalization tests. Only this test target
// sees it; the recorder and XDF writer are compiled unchanged, without a live
// network dependency.
#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace lsl {
using clock = std::chrono::steady_clock;
enum channel_format_t { cf_int8, cf_int16, cf_int32, cf_float32, cf_double64, cf_string };
struct timeout_error : std::runtime_error {
	timeout_error() : std::runtime_error("test timeout") {}
};
struct inlet_state {
	enum mode { delayed_result, unavailable, stalled_worker } behavior;
	std::atomic<bool> query_started{false}, query_finished{false};
	std::atomic<int> query_calls{0};
	clock::time_point result_ready;
	explicit inlet_state(mode behavior) : behavior(behavior) {}
};

class stream_info {
  public:
	std::shared_ptr<inlet_state> state;
	explicit stream_info(std::shared_ptr<inlet_state> state) : state(std::move(state)) {}
	std::string name() const { return "FinalizationTest"; }
	std::string hostname() const { return "localhost"; }
	std::string uid() const { return "finalization-test"; }
	std::string source_id() const { return uid(); }
	channel_format_t channel_format() const { return cf_float32; }
	double nominal_srate() const { return 100; }
	std::string as_xml() const {
		return "<info><name>FinalizationTest</name><channel_count>1</channel_count>"
			   "<channel_format>float32</channel_format><nominal_srate>100</"
			   "nominal_srate></info>";
	}
};

inline double local_clock() {
	return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}
inline std::vector<stream_info> resolve_stream(const std::string &, int, double) { return {}; }

class stream_inlet {
	stream_info info_;
	bool sent_sample_ = false;

  public:
	explicit stream_inlet(const stream_info &info) : info_(info) {}
	void open_stream(double) {}
	void close_stream() {}
	void set_postprocessing(int) {}
	stream_info info(double) { return info_; }
	int get_channel_count() const { return 1; }
	template <class T> double pull_sample(std::vector<T> &sample, double timeout) {
		if (!sent_sample_) {
			sent_sample_ = true;
			sample.assign(1, T{});
			return 123;
		}
		std::this_thread::sleep_for(std::chrono::duration<double>(timeout));
		return 0;
	}
	template <class T>
	void pull_chunk_multiplexed(std::vector<T> &chunk, std::vector<double> *timestamps, double) {
		chunk.clear();
		timestamps->clear();
	}
	double time_correction(double timeout) {
		auto &state = *info_.state;
		if (state.query_calls++ == 0)
			state.result_ready = clock::now() + std::chrono::milliseconds(600);
		state.query_started = true;
		if (state.behavior == inlet_state::stalled_worker) {
			// Deliberately exceed both the offset grace and outer join deadline. Even
			// an unexpectedly slow worker must finish before the caller can exit the
			// process.
			std::this_thread::sleep_for(std::chrono::milliseconds(2600));
		} else {
			const auto deadline = clock::now() + std::chrono::duration_cast<clock::duration>(
													 std::chrono::duration<double>(timeout));
			if (state.behavior == inlet_state::unavailable || deadline < state.result_ready) {
				std::this_thread::sleep_until(deadline);
				throw timeout_error();
			}
			std::this_thread::sleep_until(state.result_ready);
		}
		state.query_finished = true;
		return 0.0123;
	}
};
} // namespace lsl
