// Isolate the Qt state machine from stream discovery and recording timing. Backend
// completion semantics are tested separately against the real recording.cpp.
#include "recording.h"
#include <atomic>
#include <thread>

std::atomic<bool> release_finalization{false};
std::atomic<int> recording_starts{0};

struct recording::completion {
	std::atomic<bool> stopping{false};
	std::promise<std::string> result;
};

recording::recording(const std::string &, const std::vector<lsl::stream_info> &,
					 const std::vector<std::string> &, std::map<std::string, int>, bool)
	: completion_(std::make_shared<completion>()),
	  result_(completion_->result.get_future().share()) {
	++recording_starts;
}
recording::~recording() { requestStop(); }
void recording::requestStop() noexcept {
	if (completion_->stopping.exchange(true)) return;
	std::thread([done = completion_] {
		while (!release_finalization)
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		done->result.set_value("");
	}).detach();
}
bool recording::waitForFinished(std::chrono::milliseconds timeout) const {
	return result_.wait_for(timeout) == std::future_status::ready;
}
std::string recording::finalizationError() const { return result_.get(); }
