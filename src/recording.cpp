#include "recording.h"
//#include "conversions.h"

#include "xdfwriter.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <list>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#ifdef XDFZ_SUPPORT
#include <boost/algorithm/string/predicate.hpp>
#include <boost/iostreams/device/file_descriptor.hpp>
#include <boost/iostreams/filter/zlib.hpp>
#endif

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
// maximum waiting time for a single time correction query, in seconds
const double max_time_correction_wait = 2;
// blocking network calls are issued in slices of this length (in seconds) so that a shutdown
// request is noticed promptly instead of after the full timeout
const double network_poll_interval = 0.2;
// time granted to the stream threads to drain their inlets and write their footers before the
// inlets are forcibly closed
const auto teardown_grace = std::chrono::milliseconds(300);
// time before reporting a slow worker; finalization still waits for it to finish
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
	// the task is kept alive by the lambda until its body returns
	w->thread = std::thread([task] { (*task)(); });
	return w;
}

// pointer to a stream inlet
using inlet_p = std::shared_ptr<lsl::stream_inlet>;
// Per-stream stop flag shared with the offset worker until it has been joined.
using offset_flag_p = std::shared_ptr<std::atomic<bool>>;
// a list of clock offset estimates (time,value)
using offset_list = std::list<std::pair<double, double>>;
// a map from streamid to offset_list
using offset_lists = std::map<streamid_t, offset_list>;

namespace {

std::mutex log_mut;

/// Write one line, atomically with respect to the other recording threads.
///
/// Every stream has its own thread and they all report progress; an unsynchronised chain of <<
/// lets two of them interleave in the middle of a line, which garbles the log and defeats anything
/// that reads it.
template <class... Args> void log_line(std::ostream &out, Args &&...args) {
	std::ostringstream line;
	(line << ... << std::forward<Args>(args));
	line << '\n';
	std::lock_guard<std::mutex> lock(log_mut);
	out << line.str() << std::flush;
}

template <class... Args> void log_out(Args &&...args) {
	log_line(std::cout, std::forward<Args>(args)...);
}

template <class... Args> void log_err(Args &&...args) {
	log_line(std::cerr, std::forward<Args>(args)...);
}

// time spent waiting between two resolves of a watchlist query; the resolve itself already takes
// resolve_timeout, so together they keep the resolve_interval cadence
const auto resolve_pause = std::chrono::duration_cast<Clock::duration>(
	std::chrono::duration<double>(resolve_interval - resolve_timeout));

/// convert a timeout given in seconds into a Clock duration
inline Clock::duration seconds_to_duration(double seconds) {
	return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
}

} // namespace

// Thread utilities

/**
 * @brief timed_join	Waits up to duration for the worker to finish, then joins it
 * @param w				unique_ptr to a worker. Will be reset on success
 * @param duration		max duration to wait
 * @return true if the worker finished and was joined, false if it is still running
 */
inline bool timed_join(worker_p &w, std::chrono::milliseconds duration = max_join_wait) {
	if (!w) return true;
	// wait on the future rather than calling join() directly: join() has no timeout, so a single
	// call against a hung thread would never return and no deadline could be enforced
	if (w->done.wait_for(duration) != std::future_status::ready) return false;
	w->thread.join();
	w.reset();
	return true;
}

/**
 * @brief join_worker	Join the worker, reporting when it exceeds the expected duration
 * @param w						unique_ptr to a worker. Will be reset either way
 * @param duration				time before reporting that finalization is still waiting
 */
inline void join_worker(worker_p &w, std::chrono::milliseconds duration = max_join_wait) {
	if (!timed_join(w, duration)) {
		log_err("Waiting for a recording worker to finish before closing the file.");
		w->thread.join();
		w.reset();
	}
}

/**
 * @brief timed_join_some	Join whichever workers finish within duration, leave the rest in place
 * @param workers			list of workers. Joined ones are erased from it
 * @param duration			duration to wait, shared across all workers
 */
inline void timed_join_some(std::list<worker_p> &workers, std::chrono::milliseconds duration) {
	const auto deadline = Clock::now() + duration;
	for (auto it = workers.begin(); it != workers.end();) {
		const auto remaining =
			std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
		if (timed_join(*it, std::max(remaining, std::chrono::milliseconds(0))))
			it = workers.erase(it);
		else
			++it;
	}
}

/**
 * @brief join_workers	Join all workers before their writer can be destroyed
 * @param workers				list of workers. Guaranteed to be empty afterwards.
 * @param duration				duration to wait, shared across all workers
 */
inline void join_workers(
	std::list<worker_p> &workers, std::chrono::milliseconds duration = max_join_wait) {
	timed_join_some(workers, duration);
	if (!workers.empty()) {
		log_out(workers.size(), " stream threads still running!");
		for (auto &w : workers) w->thread.join();
		workers.clear();
	}
}

/**
 * The recording state, and the thread bodies that operate on it.
 *
 * Every recording thread holds a shared_ptr to this, as does the recording object.
 * stop_and_join() joins all workers, including nested workers, before the recording handle
 * releases its reference and closes the file. A fast stop must not leave buffered output owned
 * by a detached worker that process exit could kill before the writer flushes.
 */
struct recording::impl : std::enable_shared_from_this<recording::impl> {
	impl(const std::string &filename, std::map<std::string, int> syncOptions, bool collect_offsets)
		: file_(filename), offsets_enabled_(collect_offsets), unsorted_(false), streamid_(0),
		  shutdown_(false), headers_to_finish_(0), streaming_to_finish_(0),
		  sync_options_by_stream_(std::move(syncOptions)) {}

	/// stop_and_join() leaves the worker containers empty before this state is destroyed.
	~impl() = default;

	/// Spawn the recording threads. Separate from the constructor because the threads need a
	/// shared_ptr to this, which shared_from_this() cannot hand out during construction.
	void start(
		const std::vector<lsl::stream_info> &streams, const std::vector<std::string> &watchfor);

	/// Ask the threads to finish and join them all before closing the file.
	/// Called from the recording object, never from a recording thread.
	void stop_and_join() noexcept;

	void requestStop() noexcept;

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
		shutdown_cv_;		  // signals shutdown so that every interruptible wait returns at once
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
	void record_offsets(streamid_t streamid, inlet_p in, offset_flag_p offset_shutdown) noexcept;

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

void recording::impl::start(
	const std::vector<lsl::stream_info> &streams, const std::vector<std::string> &watchfor) {
	// Each worker owns its state until it has finished; stop_and_join() joins them all.
	auto self = shared_from_this();
	// create a recording thread for each stream
	for (const auto &stream : streams)
		stream_threads_.emplace_back(
			spawn_worker([self, stream] { self->record_from_streaminfo(stream, true); }));
	// create a resolve-and-record thread for each item in the watchlist
	for (const auto &query : watchfor)
		stream_threads_.emplace_back(
			spawn_worker([self, query] { self->record_from_query_results(query); }));
	// create a boundary chunk writer thread
	boundary_thread_ = spawn_worker([self] { self->record_boundaries(); });
}

void recording::impl::stop_and_join() noexcept {
	try {
		// set the shutdown flag (from now on no more new streams) and wake every waiting thread
		requestStop();

		// give the stream threads a moment to drain their inlets and write their footers by
		// themselves; closing an inlet discards what it still holds, so that is a last resort
		timed_join_some(stream_threads_, teardown_grace);
		if (!stream_threads_.empty()) {
			// a thread is stuck in a blocking socket call; closing its inlet aborts that call
			close_active_inlets();
			join_workers(stream_threads_, max_join_wait);
		}
		join_worker(boundary_thread_, max_join_wait);
		log_out("Closing the file.");
	} catch (std::exception &e) {
		log_out("Error while closing the recording: ", e.what());
	}
}

recording::recording(const std::string &filename, const std::vector<lsl::stream_info> &streams,
	const std::vector<std::string> &watchfor, std::map<std::string, int> syncOptions,
	bool collect_offsets)
	: impl_(std::make_shared<impl>(filename, std::move(syncOptions), collect_offsets)) {
	try {
		impl_->start(streams, watchfor);
	} catch (...) {
		// some threads may already be running, and our destructor will not run if we throw
		impl_->stop_and_join();
		throw;
	}
}

recording::~recording() { impl_->stop_and_join(); }

void recording::requestStop() noexcept { impl_->requestStop(); }

void recording::impl::requestStop() noexcept {
	{
		// publish the flag under the mutex that the shutdown_cv_ predicates read it under: a
		// waiter that has just evaluated its predicate as false would otherwise miss the
		// notification below and sleep out its full interval
		std::lock_guard<std::mutex> lock(shutdown_mut_);
		shutdown_ = true;
	}
	shutdown_cv_.notify_all();

	// the phase gates test shutdown_ under phase_mut_, so take and release it for the same reason
	{ std::lock_guard<std::mutex> lock(phase_mut_); }
	ready_for_streaming_.notify_all();
	ready_for_footers_.notify_all();
}

bool recording::impl::wait_until_shutdown(Clock::time_point deadline, const std::atomic<bool> *extra) {
	std::unique_lock<std::mutex> lock(shutdown_mut_);
	return shutdown_cv_.wait_until(
		lock, deadline, [this, extra] { return shutdown_.load() || (extra && extra->load()); });
}

void recording::impl::stop_offsets(const offset_flag_p &offset_shutdown) noexcept {
	{
		std::lock_guard<std::mutex> lock(shutdown_mut_);
		*offset_shutdown = true;
	}
	shutdown_cv_.notify_all();
}

void recording::impl::register_inlet(const inlet_p &in) {
	std::lock_guard<std::mutex> lock(inlets_mut_);
	active_inlets_.push_back(in);
}

void recording::impl::unregister_inlet(const inlet_p &in) noexcept {
	if (!in) return;
	std::lock_guard<std::mutex> lock(inlets_mut_);
	active_inlets_.erase(
		std::remove(active_inlets_.begin(), active_inlets_.end(), in), active_inlets_.end());
}

void recording::impl::close_active_inlets() noexcept {
	std::lock_guard<std::mutex> lock(inlets_mut_);
	for (auto &in : active_inlets_) {
		try {
			in->close_stream();
		} catch (std::exception &e) {
			log_err("Error while closing an inlet: ", e.what());
		}
	}
}

bool recording::impl::open_inlet(const inlet_p &in) {
	// subscribe in short slices: a single open_stream(max_open_wait) would keep us from noticing a
	// stop for up to max_open_wait seconds
	const auto deadline = Clock::now() + seconds_to_duration(max_open_wait);
	while (Clock::now() < deadline && !shutdown_) {
		try {
			in->open_stream(network_poll_interval);
			return true;
		} catch (lsl::timeout_error &) {}
	}
	return false;
}

lsl::stream_info recording::impl::fetch_info(const inlet_p &in) {
	// the metadata receiver is separate from the data receiver, so close_stream() does not abort
	// this call; poll in short slices instead, or an unreachable source blocks us indefinitely.
	// A stop does not cut this off immediately: a source that is still reachable gets a short
	// grace period, so its header (and with it its footer) still makes it into the file.
	auto deadline = Clock::time_point::max();
	while (Clock::now() < deadline) {
		if (shutdown_ && deadline == Clock::time_point::max())
			deadline = Clock::now() + teardown_grace;
		try {
			return in->info(network_poll_interval);
		} catch (lsl::timeout_error &) {}
	}
	throw shutdown_requested("stopped while retrieving the stream metadata");
}

void recording::impl::record_from_query_results(const std::string &query) {
	try {
		std::set<std::string> known_uids;		// set of previously seen stream uid's
		std::set<std::string> known_source_ids; // set of previously seen source id's
		std::list<worker_p> threads;			// our spawned threads
		log_out("Watching for a stream with properties ", query);
		while (!shutdown_) {
			// periodically re-resolve the query. The resolve itself is kept short and the rest of
			// the interval is spent in an interruptible wait, so a stop is noticed quickly.
			const std::vector<lsl::stream_info> results =
				lsl::resolve_stream(query, 0, resolve_timeout);
			// for each result...
			for (const auto &result : results) {
				// if it is a new stream...
				if (!known_uids.count(result.uid()))
					// and doesn't have a previously seen source id...
					if (!result.source_id().empty() &&
						(!known_source_ids.count(result.source_id()))) {
						log_out("Found a new stream named ", result.name(), ", adding it to the recording.");
						// start a new recording thread
						threads.emplace_back(spawn_worker([self = shared_from_this(), result] {
							self->record_from_streaminfo(result, false);
						}));
						// ... and add it to the lists of known id's
						known_uids.insert(result.uid());
						if (!result.source_id().empty())
							known_source_ids.insert(result.source_id());
					}
			}
			if (wait_for_shutdown(resolve_pause)) break;
		}
		// wait for all our threads to join
		join_workers(threads, max_join_wait);
	} catch (std::exception &e) {
		log_out("Error in the record_from_query_results thread: ", e.what());
	}
}

void recording::impl::record_from_streaminfo(const lsl::stream_info &src, bool phase_locked) {
	inlet_p in;
	try {
		// initialised here because a stream that fails mid-recording still writes a footer
		double first_timestamp = 0.0, last_timestamp = 0.0;
		uint64_t sample_count = 0;
		double nominal_srate = 0;
		// obtain a fresh streamid
		streamid_t streamid = fresh_streamid();

		// --- headers phase
		try {
			enter_headers_phase(phase_locked);

			// open an inlet to read from (and subscribe to data immediately)
			in = std::make_shared<lsl::stream_inlet>(src);
			register_inlet(in);
			auto it = sync_options_by_stream_.find(src.name() + " (" + src.hostname() + ")");
			if (it != sync_options_by_stream_.end()) in->set_postprocessing(it->second);

			if (open_inlet(in))
				log_out("Opened the stream ", src.name(), ".");
			else if (!shutdown_)
				log_out("Subscribing to the stream ", src.name(),
					" is taking relatively long; collection from this stream will be delayed.");

			// retrieve the stream header & get its XML version. The nominal rate is taken from
			// the same info, saving a second round trip to the source.
			const lsl::stream_info info = fetch_info(in);
			nominal_srate = info.nominal_srate();
			file_.write_stream_header(streamid, info.as_xml());
			log_out("Received header for stream ", src.name(), ".");

			leave_headers_phase(phase_locked);
		} catch (std::exception &) {
			leave_headers_phase(phase_locked);
			throw;
		}

		// --- streaming phase
		try {
			// this waits until we are done writing all headers for the initial set of
			// (phase-locked) streams (any streams that are discovered later, if any, will not wait)
			// we're doing this so that all headers of the initial set of streams come first, so the
			// XDF file is properly sorted unless we discover some streams later which someone
			// "forgot to turn on" before the recording started; in that case the file would have to
			// be post-processed to be in properly sorted (seekable) format
			enter_streaming_phase(phase_locked);
			log_out("Started data collection for stream ", src.name(), ".");

			// now write the actual sample chunks...
			switch (src.channel_format()) {
			case lsl::cf_int8:
				typed_transfer_loop<char>(
					streamid, nominal_srate, in, first_timestamp, last_timestamp, sample_count);
				break;
			case lsl::cf_int16:
				typed_transfer_loop<int16_t>(
					streamid, nominal_srate, in, first_timestamp, last_timestamp, sample_count);
				break;
			case lsl::cf_int32:
				typed_transfer_loop<int32_t>(
					streamid, nominal_srate, in, first_timestamp, last_timestamp, sample_count);
				break;
			case lsl::cf_float32:
				typed_transfer_loop<float>(
					streamid, nominal_srate, in, first_timestamp, last_timestamp, sample_count);
				break;
			case lsl::cf_double64:
				typed_transfer_loop<double>(
					streamid, nominal_srate, in, first_timestamp, last_timestamp, sample_count);
				break;
			case lsl::cf_string:
				typed_transfer_loop<std::string>(
					streamid, nominal_srate, in, first_timestamp, last_timestamp, sample_count);
				break;
			default:
				// unsupported channel format
				throw std::runtime_error(
					std::string("Unsupported channel format in stream ") += src.name());
			}

			leave_streaming_phase(phase_locked);
		} catch (std::exception &e) {
			leave_streaming_phase(phase_locked);
			// the header is already on disk, so fall through to the footer instead of leaving the
			// stream without one
			log_err("Error while recording from ", src.name(), ": ", e.what());
		}

		// --- footers phase
		try {
			enter_footers_phase(phase_locked);

			// now generate the [StreamFooter] contents
			std::ostringstream footer;
			footer.precision(16);
			// [Content]
			footer << "<?xml version=\"1.0\"?><info><first_timestamp>" << first_timestamp
				   << "</first_timestamp><last_timestamp>" << last_timestamp
				   << "</last_timestamp><sample_count>" << sample_count << "</sample_count>";
			footer << "<clock_offsets>";
			{
				// including the clock_offset list
				std::lock_guard<std::mutex> lock(offset_mut_);
				for (const auto pair : offset_lists_[streamid]) {
					footer << "<offset><time>" << pair.first << "</time><value>" << pair.second
						   << "</value></offset>";
				}
				footer << "</clock_offsets></info>";
			}
			file_.write_stream_footer(streamid, footer.str());

			log_out("Wrote footer for stream ", src.name(), ".");
			leave_footers_phase(phase_locked);
		} catch (std::exception &) {
			leave_footers_phase(phase_locked);
			throw;
		}
	} catch (shutdown_requested &e) {
		log_out("Recording from ", src.name(), " ended: ", e.what());
	} catch (std::exception &e) {
		log_out("Error in the record_from_streaminfo thread: ", e.what());
	}
	unregister_inlet(in);
}

void recording::impl::record_boundaries() {
	try {
		while (!shutdown_) {
			if (wait_for_shutdown(boundary_interval)) break;
			file_.write_boundary_chunk();
		}
	} catch (std::exception &e) {
		log_out("Error in the record_boundaries thread: ", e.what());
	}
}

void recording::impl::record_offsets(
	streamid_t streamid, inlet_p in, offset_flag_p offset_shutdown) noexcept {
	try {
		while (!shutdown_ && !*offset_shutdown) {
			// sleep for the interval
			if (wait_for_shutdown(offset_interval, offset_shutdown.get())) break;

			// liblsl's background measurement survives a time_correction() timeout. Polling
			// waits for that same result; it does not restart the packet exchange.
			const auto deadline = Clock::now() + seconds_to_duration(max_time_correction_wait);
			double offset = 0, now = 0;
			bool have_offset = false;
			while (!shutdown_ && !*offset_shutdown && Clock::now() < deadline) {
				const double remaining = std::chrono::duration<double>(deadline - Clock::now()).count();
				try {
					offset = in->time_correction(std::max(0.0, std::min(network_poll_interval, remaining)));
					now = lsl::local_clock();
					have_offset = true;
					break;
				} catch (lsl::timeout_error &) {}
			}
			if (shutdown_ || *offset_shutdown) break;
			if (!have_offset) {
				log_err("Timeout in time correction query for stream ", streamid);
				continue;
			}

			file_.write_stream_offset(streamid, now, offset);
			// also append to the offset lists
			std::lock_guard<std::mutex> lock(offset_mut_);
			offset_lists_[streamid].emplace_back(now - offset, offset);
		}
	} catch (std::exception &e) {
		log_out("Error in the record_offsets thread: ", e.what());
	}
	log_out("Offsets thread is finished");
}

void recording::impl::enter_headers_phase(bool phase_locked) {
	if (phase_locked) {
		std::lock_guard<std::mutex> lock(phase_mut_);
		headers_to_finish_++;
	}
}

void recording::impl::leave_headers_phase(bool phase_locked) {
	if (phase_locked) {
		std::unique_lock<std::mutex> lock(phase_mut_);
		headers_to_finish_--;
		lock.unlock();
		ready_for_streaming_.notify_all();
	}
}

void recording::impl::enter_streaming_phase(bool phase_locked) {
	if (phase_locked) {
		std::unique_lock<std::mutex> lock(phase_mut_);
		// on shutdown the gate is dropped: the transfer loop exits immediately anyway, and waiting
		// out max_headers_wait for a stream that is never going to report in only delays the
		// footer of this one
		ready_for_streaming_.wait_for(lock, max_headers_wait,
			[this]() { return this->ready_for_streaming() || shutdown_.load(); });
		streaming_to_finish_++;
	}
}

void recording::impl::leave_streaming_phase(bool phase_locked) {
	if (phase_locked) {
		std::unique_lock<std::mutex> lock(phase_mut_);
		streaming_to_finish_--;
		lock.unlock();
		ready_for_footers_.notify_all();
	}
}

void recording::impl::enter_footers_phase(bool phase_locked) {
	if (phase_locked) {
		std::unique_lock<std::mutex> lock(phase_mut_);
		// see enter_streaming_phase: a footer written slightly out of order beats no footer at all
		ready_for_footers_.wait_for(lock, max_footers_wait,
			[this]() { return this->ready_for_footers() || shutdown_.load(); });
	}
}

template <class T>
void recording::impl::typed_transfer_loop(streamid_t streamid, double srate, const inlet_p &in,
	double &first_timestamp, double &last_timestamp, uint64_t &sample_count) {
	// optionally start an offset collection thread for this stream
	auto offset_shutdown = std::make_shared<std::atomic<bool>>(false);
	auto self = shared_from_this();
	worker_p offset_thread(offsets_enabled_
			? spawn_worker([self, streamid, in, offset_shutdown] {
				  self->record_offsets(streamid, in, offset_shutdown);
			  })
			: nullptr);
	try {
		double sample_interval = srate ? 1.0 / srate : 0;

		// temporary data
		std::vector<T> chunk;
		std::vector<double> timestamps;

		// deduce the timestamps that can be deduced and write the chunk out
		auto write_chunk = [&] {
			if (timestamps.empty()) return;
			for (double &ts : timestamps) {
				if (first_timestamp == 0.0) {
					// the first sample anchors the stream and is written verbatim: at a nominal
					// interval of zero the deduction below would otherwise zero out its timestamp
					first_timestamp = last_timestamp = ts;
					continue;
				}
				// if the time stamp can be deduced from the previous one...
				if (last_timestamp + sample_interval == ts) {
					last_timestamp = ts + sample_interval;
					ts = 0;
				} else
					last_timestamp = ts;
			}
			file_.write_data_chunk(streamid, timestamps, chunk, in->get_channel_count());
			sample_count += timestamps.size();
		};

		// Wait for the first sample, unless the stop got here first. A stream held at the headers
		// gate reaches this point with the shutdown already set, having pulled nothing, while its
		// inlet has been subscribed and buffering the whole time -- the drain below picks that up.
		first_timestamp = 0.0;
		while (!shutdown_ && first_timestamp == 0.0) {
			const double ts = in->pull_sample(chunk, network_poll_interval);
			if (ts == 0.0) continue;
			timestamps.assign(1, ts);
			write_chunk();
		}

		auto next_pull = Clock::now() + chunk_interval;
		while (!shutdown_) {
			// get a chunk from the stream
			in->pull_chunk_multiplexed(chunk, &timestamps, 1e-6);
			write_chunk();
			if (wait_until_shutdown(next_pull)) break;
			next_pull += chunk_interval;
		}

		// one final non-blocking pull, so that samples already buffered in the inlet when the stop
		// arrived end up in the file rather than being dropped
		try {
			in->pull_chunk_multiplexed(chunk, &timestamps, 0.0);
			write_chunk();
		} catch (std::exception &e) {
			// the inlet was closed under us during teardown; the footer matters more
			log_err("Could not drain stream ", streamid, " on stop: ", e.what());
		}
	} catch (std::exception &) {
		stop_offsets(offset_shutdown);
		join_worker(offset_thread, teardown_grace);
		throw;
	}
	stop_offsets(offset_shutdown);
	join_worker(offset_thread, teardown_grace);
}
