#ifndef RECORDING_H
#define RECORDING_H

#include <lsl_cpp.h>
#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <vector>

/**
 * A recording process using the lab streaming layer.
 * An instance of this class is created with a list of stream references to record from.
 * Upon construction, a file is created and a recording thread is spawned which records
 * data until the instance is destroyed.
 */
class recording {
public:
	/**
	 * Construct a new background recording process.
	 * @param filename The file name to record to (should end in .xdf).
	 * @param streams  An array of LSL streaminfos that identify the set of streams to record into
	 *the file.
	 * @param watchfor An optional "watchlist" of LSL query predicates (see lsl::resolve_bypred) to
	 *resolve streams to record from. This can be a specific stream that you know should be recorded
	 *but is not yet online, or a more generic query (e.g., "record from everything that is out
	 *there").
	 * @param collect_offsets Whether to collect time offset measurements periodically.
	 */
	recording(const std::string &filename, const std::vector<lsl::stream_info> &streams,
		const std::vector<std::string> &watchfor, std::map<std::string, int> syncOptions,
		bool collect_offsets = true);

	/// Requests shutdown without waiting. Callers must observe completion before normal exit.
	~recording();

	/// Fix the recorder-clock cutoff and request catch-up/finalization. Returns immediately.
	void requestStop() noexcept;
	struct FinalizationProgress {
		size_t collecting = 0;
		size_t catching_up = 0;
		size_t fallback_streams = 0;
		// Longest inactivity of any outstanding worker (other streams cannot hide a stall).
		std::chrono::milliseconds idle{0};
	};
	FinalizationProgress finalizationProgress() const;
	/// End catch-up early, preserving footers and recording the explicit truncation reason.
	void finishCollecting() noexcept;
	/// Wait at most timeout for all workers AND the output file to finish. Does not request stop.
	bool waitForFinished(std::chrono::milliseconds timeout) const;
	bool isFinished() const { return waitForFinished(std::chrono::milliseconds(0)); }
	/// Available after completion: empty on success, otherwise a finalization error.
	std::string finalizationError() const;
	/// Retain a completion receipt when releasing the nonblocking recording handle.
	std::shared_future<std::string> completionResult() const { return result_; }
	recording(const recording &) = delete;
	recording &operator=(const recording &) = delete;

private:
	struct impl;
	struct completion;
	// The finalizer owns impl, never the UI handle. Dropping the handle cannot close a file
	// or join a thread on the caller; completion is published only after impl is destroyed.
	std::shared_ptr<completion> completion_;
	std::shared_future<std::string> result_;
};

#endif
