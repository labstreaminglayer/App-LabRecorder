#pragma once

// A deterministic inlet for recording finalization tests. Only this test target
// sees it; the recorder and XDF writer are compiled unchanged, without a live
// network dependency.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace lsl {
using clock = std::chrono::steady_clock;
constexpr int post_clocksync = 1;
enum channel_format_t { cf_int8, cf_int16, cf_int32, cf_float32, cf_double64, cf_string };
struct timeout_error : std::runtime_error {
	timeout_error() : std::runtime_error("test timeout") {}
};
struct inlet_state {
	enum mode {
		delayed_result,
		unavailable,
		stalled_worker,
		permanent_stall,
		failed_transfer,
		queued,
		queued_unknown,
		stalled_transfer
	} behavior;
	std::atomic<bool> query_started{false}, query_finished{false};
	std::atomic<int> query_calls{0};
	clock::time_point result_ready;
	std::mutex mutex;
	std::deque<std::pair<clock::time_point, double>> samples;
	std::atomic<int> pulled{0};
	void enqueue(double delay, double timestamp) {
		std::lock_guard<std::mutex> lock(mutex);
		samples.emplace_back(clock::now() + std::chrono::duration_cast<clock::duration>(
			std::chrono::duration<double>(delay)), timestamp);
	}
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
	bool matches_query(const std::string &) const { return true; }
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
inline std::vector<stream_info> resolve_streams() {
	return {stream_info(std::make_shared<inlet_state>(std::getenv("LSL_TEST_STALL")
														  ? inlet_state::permanent_stall
														  : inlet_state::delayed_result))};
}

class stream_inlet {
	stream_info info_;
	bool sent_sample_ = false;
	bool clocksync_ = false;

  public:
	explicit stream_inlet(const stream_info &info) : info_(info) {}
	void open_stream(double) {}
	void close_stream() {}
	void set_postprocessing(int flags) { clocksync_ = (flags & post_clocksync) != 0; }
	stream_info info(double) { return info_; }
	int get_channel_count() const { return 1; }
	template <class T> double pull_sample(std::vector<T> &sample, double timeout) {
		if (info_.state->behavior == inlet_state::queued ||
			info_.state->behavior == inlet_state::queued_unknown) {
			const auto until = clock::now() + std::chrono::duration<double>(timeout);
			do {
				{
					std::lock_guard<std::mutex> lock(info_.state->mutex);
					auto &samples = info_.state->samples;
					if (!samples.empty() && samples.front().first <= clock::now()) {
						const auto ts = samples.front().second;
						samples.pop_front();
						sample.assign(1, T{});
						++info_.state->pulled;
						return ts + (clocksync_ ? 100.0 : 0.0);
					}
				}
				if (timeout == 0) break;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			} while (clock::now() < until);
			return 0;
		}
		if (info_.state->behavior == inlet_state::failed_transfer) {
			info_.state->query_started = true;
			throw std::runtime_error("simulated transfer failure");
		}
		if (!sent_sample_) {
			sent_sample_ = true;
			if (info_.state->behavior == inlet_state::stalled_transfer) {
				info_.state->query_started = true;
				std::this_thread::sleep_for(std::chrono::seconds(3));
			}
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
		if (state.behavior == inlet_state::queued || state.behavior == inlet_state::stalled_transfer) return 100.0;
		if (state.behavior == inlet_state::queued_unknown) {
			if (state.query_calls++ % 2) throw std::runtime_error("clock service lost");
			throw timeout_error();
		}
		std::lock_guard<std::mutex> lock(state.mutex);
		if (state.query_calls++ == 0)
			state.result_ready = clock::now() + std::chrono::milliseconds(600);
		state.query_started = true;
		if (state.behavior == inlet_state::permanent_stall) {
			std::cout << "TEST: offset query stalled" << std::endl;
			for (;;)
				std::this_thread::sleep_for(std::chrono::hours(1));
		}
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
