#ifndef RECORDING_H
#define RECORDING_H

#include "xdfwriter.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <list>
#include <lsl_cpp.h>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

// timings in the recording process (e.g., rate of boundary chunks and for cases where a stream
// hangs) approx. interval between boundary chunks
const auto boundary_interval = std::chrono::seconds(10);
// approx. interval between offset measurements
const auto offset_interval = std::chrono::seconds(5);
// approx. interval between resolves for outstanding streams on the watchlist, in seconds
const double resolve_interval = 5;
// timeout of a single resolve attempt, in seconds; the rest of resolve_interval is spent in an
// interruptible wait so that a shutdown request need not wait out a resolve
const double resolve_timeout = 1;
// approx. interval between pulling chunks from outlets
const auto chunk_interval = std::chrono::milliseconds(500);
// maximum waiting time for moving past the headers phase while recording
const auto max_headers_wait = std::chrono::seconds(10);
// maximum waiting time for moving into the footers phase while recording
const auto max_footers_wait = std::chrono::seconds(2);
// maximum waiting time for subscribing to a stream, in seconds (if exceeded, stream subscription
// will take place later)
const double max_open_wait = 5;
// maximum waiting time for a single time correction query
const auto max_time_correction_wait = std::chrono::seconds(2);
// blocking network calls are issued in slices of this length (in seconds) so that a shutdown
// request is noticed promptly instead of after the full timeout
const double network_poll_interval = 0.2;
// time granted to the stream threads to drain their inlets and write their footers before the
// inlets are forcibly closed
const auto teardown_grace = std::chrono::milliseconds(300);
// maximum time that we wait to join a thread
const auto max_join_wait = std::chrono::seconds(2);

// steady_clock (not high_resolution_clock, which is an alias for the wall clock in some standard
// libraries) so that waits are unaffected by clock adjustments
using Clock = std::chrono::steady_clock;

using streamid_t = uint32_t;

/// thrown by the interruptible helpers when the recording is being torn down
class shutdown_requested : public std::runtime_error {
public:
	explicit shutdown_requested(const std::string &what) : std::runtime_error(what) {}
};

/**
 * A thread paired with a future that becomes ready once the thread body has returned.
 *
 * std::thread::join() blocks indefinitely, so polling it cannot enforce a deadline: a single call
 * against a hung thread never comes back. The future can be waited on with a timeout, and only
 * once it is ready do we join (which then returns promptly). A std::packaged_task future is used
 * rather than std::async because the latter blocks in its future destructor.
 */
struct worker {
	std::thread thread;
	std::future<void> done;
};
// pointer to a worker thread
using worker_p = std::unique_ptr<worker>;

/// start a worker thread running fn
template <class F> worker_p spawn_worker(F &&fn) {
	auto task = std::make_shared<std::packaged_task<void()>>(std::forward<F>(fn));
	auto w = std::make_unique<worker>();
	w->done = task->get_future();
	// the task is kept alive by the lambda, so the worker may be detached safely
	w->thread = std::thread([task] { (*task)(); });
	return w;
}

// pointer to a stream inlet
using inlet_p = std::shared_ptr<lsl::stream_inlet>;
// pointer to a per-stream flag asking that stream's offset thread to finish. Shared rather than
// referenced so that an offset thread which had to be detached cannot outlive its flag.
using offset_flag_p = std::shared_ptr<std::atomic<bool>>;
// a list of clock offset estimates (time,value)
using offset_list = std::list<std::pair<double, double>>;
// a map from streamid to offset_list
using offset_lists = std::map<streamid_t, offset_list>;


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

	/** Destructor.
	 * Stops the recording and closes the file.
	 */
	~recording();

	/// Ask all recording threads to wrap up. Returns immediately; the threads are joined by the
	/// destructor.
	void requestStop() noexcept;

private:
	// the file stream
	XDFWriter file_; // the file output stream
	// static information
	bool offsets_enabled_; // whether to collect time offset information alongside with the stream
						   // contents
	bool unsorted_;		   // whether this file may contain unsorted chunks (e.g., of late streams)

	// streamid allocation
	std::atomic<streamid_t> streamid_; // the highest streamid allocated so far

	// phase-of-recording state (headers, streaming data, or footers)
	std::atomic<bool> shutdown_; // whether we are trying to shut down
	std::condition_variable
		shutdown_cv_; // signals shutdown so that every interruptible wait returns at once
	std::mutex shutdown_mut_; // protects publication of shutdown_ and of the per-stream offset
							  // shutdown flags, which the shutdown_cv_ predicates read under it
	uint32_t headers_to_finish_;   // the number of streams that still need to write their header
								   // (i.e., are not yet ready to write streaming content)
	uint32_t streaming_to_finish_; // the number of streams that still need to finish the streaming
								   // phase (i.e., are not yet ready for writing their footer)
	std::condition_variable
		ready_for_streaming_; // condition variable signaling that all streams have finished writing
							  // their headers and are now ready to write streaming content
	std::condition_variable
		ready_for_footers_; // condition variable signaling that all streams have finished their
							// recording jobs and are now ready to write a footer
	std::mutex phase_mut_;  // a mutex to protect the phase state

	// inlets with potentially pending network I/O, to be aborted if their thread does not stop in
	// time
	std::vector<inlet_p> active_inlets_;
	std::mutex inlets_mut_; // a mutex to protect the active inlet list

	// data structure to collect the time offsets for every stream
	offset_lists
		offset_lists_; // the clock offset lists for each stream (to be written into the footer)
	std::mutex offset_mut_; // a mutex to protect the offset lists

	// data for shutdown / final joining
	std::list<worker_p> stream_threads_; // the spawned stream handling threads
	worker_p boundary_thread_;			 // the spawned boundary-recording thread

	// for enabling online sync options
	std::map<std::string, int> sync_options_by_stream_;

	// === recording thread functions ===

	/// record from results of a query (spawn a recording thread for every result produced by the
	/// query)
	/// @param query The query string
	void record_from_query_results(const std::string &query);

	/// record from a given stream (identified by its streaminfo)
	/// @param src the stream_info from which to record
	/// @param phase_locked whether this is a stream that is locked to the phases (1. Headers, 2.
	/// Streaming Content, 3. Footers)
	///                     Late-added streams (e.g. forgotten devices) are not phase-locked.
	void record_from_streaminfo(const lsl::stream_info &src, bool phase_locked);


	/// record boundary markers every few seconds
	void record_boundaries();

	// record ClockOffset chunks from a given stream
	void record_offsets(
		streamid_t streamid, inlet_p in, offset_flag_p offset_shutdown) noexcept;


	// sample collection loop for a numeric stream
	template <class T>
	void typed_transfer_loop(streamid_t streamid, double srate, const inlet_p &in,
		double &first_timestamp, double &last_timestamp, uint64_t &sample_count);

	// === interruptible waiting & bounded network calls ===

	/// wait until deadline, returning true if the wait was cut short by a shutdown request
	/// @param extra an optional additional flag (e.g. a per-stream offset shutdown) that also ends
	///              the wait
	bool wait_until_shutdown(Clock::time_point deadline, const std::atomic<bool> *extra = nullptr);

	/// wait for timeout, returning true if the wait was cut short by a shutdown request
	bool wait_for_shutdown(Clock::duration timeout, const std::atomic<bool> *extra = nullptr) {
		return wait_until_shutdown(Clock::now() + timeout, extra);
	}

	/// publish a per-stream offset shutdown flag and wake the corresponding offset thread
	void stop_offsets(const offset_flag_p &offset_shutdown) noexcept;

	/// subscribe to a stream, giving up after max_open_wait
	/// @return whether the subscription completed (if not, it will take place later)
	/// @throws shutdown_requested if the recording was stopped while subscribing
	bool open_inlet(const inlet_p &in);

	/// retrieve the full stream info, including the extended description
	/// @throws shutdown_requested if the recording was stopped while retrieving the metadata
	lsl::stream_info fetch_info(const inlet_p &in);

	// === inlet bookkeeping ===

	void register_inlet(const inlet_p &in);
	void unregister_inlet(const inlet_p &in) noexcept;
	/// close every registered inlet, aborting any blocking socket call in progress
	void close_active_inlets() noexcept;

	// === phase registration & condition checks ===
	// writing is coordinated across threads in three phases to keep the file chunks sorted

	void enter_headers_phase(bool phase_locked);

	void leave_headers_phase(bool phase_locked);

	void enter_streaming_phase(bool phase_locked);

	void leave_streaming_phase(bool phase_locked);

	void enter_footers_phase(bool phase_locked);

	void leave_footers_phase(bool) { /* Nothing to do. Ignore warning. */
	}

	/// a condition that indicates that we are ready to write streaming content into the file
	bool ready_for_streaming() const { return headers_to_finish_ <= 0; }
	/// a condition that indicates that we are ready to write footers into the file
	bool ready_for_footers() const { return streaming_to_finish_ <= 0 && headers_to_finish_ <= 0; }

	/// allocate a fresh stream id
	streamid_t fresh_streamid() { return ++streamid_; }
};

#endif
