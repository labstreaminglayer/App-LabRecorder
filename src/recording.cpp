#include "recording.h"
//#include "conversions.h"

#include <algorithm>
#include <set>
#include <sstream>
#ifdef XDFZ_SUPPORT
#include <boost/algorithm/string/predicate.hpp>
#include <boost/iostreams/device/file_descriptor.hpp>
#include <boost/iostreams/filter/zlib.hpp>
#endif

namespace {

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
 * @brief timed_join_or_detach	Join the worker or detach it if not possible within specified
 * duration
 * @param w						unique_ptr to a worker. Will be reset either way
 * @param duration				max duration to wait
 */
inline void timed_join_or_detach(worker_p &w, std::chrono::milliseconds duration = max_join_wait) {
	if (!timed_join(w, duration)) {
		w->thread.detach();
		w.reset();
		std::cerr << "Thread didn't join in time!" << std::endl;
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
 * @brief timed_join_or_detach	Join the workers or detach those that don't finish in time
 * @param workers				list of workers. Guaranteed to be empty afterwards.
 * @param duration				duration to wait, shared across all workers
 */
inline void timed_join_or_detach(
	std::list<worker_p> &workers, std::chrono::milliseconds duration = max_join_wait) {
	timed_join_some(workers, duration);
	if (!workers.empty()) {
		std::cout << workers.size() << " stream threads still running!" << std::endl;
		for (auto &w : workers) w->thread.detach();
		workers.clear();
	}
}

recording::recording(const std::string &filename, const std::vector<lsl::stream_info> &streams,
	const std::vector<std::string> &watchfor, std::map<std::string, int> syncOptions,
	bool collect_offsets)
	: file_(filename), offsets_enabled_(collect_offsets), unsorted_(false), streamid_(0),
	  shutdown_(false), headers_to_finish_(0), streaming_to_finish_(0),
	  sync_options_by_stream_(std::move(syncOptions)) {
	// create a recording thread for each stream
	for (const auto &stream : streams)
		stream_threads_.emplace_back(
			spawn_worker([this, stream] { record_from_streaminfo(stream, true); }));
	// create a resolve-and-record thread for each item in the watchlist
	for (const auto &query : watchfor)
		stream_threads_.emplace_back(
			spawn_worker([this, query] { record_from_query_results(query); }));
	// create a boundary chunk writer thread
	boundary_thread_ = spawn_worker([this] { record_boundaries(); });
}

recording::~recording() {
	try {
		// set the shutdown flag (from now on no more new streams) and wake every waiting thread
		requestStop();

		// give the stream threads a moment to drain their inlets and write their footers by
		// themselves; closing an inlet discards what it still holds, so that is a last resort
		timed_join_some(stream_threads_, teardown_grace);
		if (!stream_threads_.empty()) {
			// a thread is stuck in a blocking socket call; closing its inlet aborts that call
			close_active_inlets();
			timed_join_or_detach(stream_threads_, max_join_wait);
		}
		timed_join_or_detach(boundary_thread_, max_join_wait);
		std::cout << "Closing the file." << std::endl;
	} catch (std::exception &e) {
		std::cout << "Error while closing the recording: " << e.what() << std::endl;
	}
}

void recording::requestStop() noexcept {
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

bool recording::wait_until_shutdown(Clock::time_point deadline, const std::atomic<bool> *extra) {
	std::unique_lock<std::mutex> lock(shutdown_mut_);
	return shutdown_cv_.wait_until(
		lock, deadline, [this, extra] { return shutdown_.load() || (extra && extra->load()); });
}

void recording::stop_offsets(const offset_flag_p &offset_shutdown) noexcept {
	{
		std::lock_guard<std::mutex> lock(shutdown_mut_);
		*offset_shutdown = true;
	}
	shutdown_cv_.notify_all();
}

void recording::register_inlet(const inlet_p &in) {
	std::lock_guard<std::mutex> lock(inlets_mut_);
	active_inlets_.push_back(in);
}

void recording::unregister_inlet(const inlet_p &in) noexcept {
	if (!in) return;
	std::lock_guard<std::mutex> lock(inlets_mut_);
	active_inlets_.erase(
		std::remove(active_inlets_.begin(), active_inlets_.end(), in), active_inlets_.end());
}

void recording::close_active_inlets() noexcept {
	std::lock_guard<std::mutex> lock(inlets_mut_);
	for (auto &in : active_inlets_) {
		try {
			in->close_stream();
		} catch (std::exception &e) {
			std::cerr << "Error while closing an inlet: " << e.what() << std::endl;
		}
	}
}

bool recording::open_inlet(const inlet_p &in) {
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

lsl::stream_info recording::fetch_info(const inlet_p &in) {
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

void recording::record_from_query_results(const std::string &query) {
	try {
		std::set<std::string> known_uids;		// set of previously seen stream uid's
		std::set<std::string> known_source_ids; // set of previously seen source id's
		std::list<worker_p> threads;			// our spawned threads
		std::cout << "Watching for a stream with properties " << query << std::endl;
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
						std::cout << "Found a new stream named " << result.name()
								  << ", adding it to the recording." << std::endl;
						// start a new recording thread
						threads.emplace_back(spawn_worker(
							[this, result] { record_from_streaminfo(result, false); }));
						// ... and add it to the lists of known id's
						known_uids.insert(result.uid());
						if (!result.source_id().empty())
							known_source_ids.insert(result.source_id());
					}
			}
			if (wait_for_shutdown(resolve_pause)) break;
		}
		// wait for all our threads to join
		timed_join_or_detach(threads, max_join_wait);
	} catch (std::exception &e) {
		std::cout << "Error in the record_from_query_results thread: " << e.what() << std::endl;
	}
}

void recording::record_from_streaminfo(const lsl::stream_info &src, bool phase_locked) {
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
				std::cout << "Opened the stream " << src.name() << "." << std::endl;
			else if (!shutdown_)
				std::cout
					<< "Subscribing to the stream " << src.name()
					<< " is taking relatively long; collection from this stream will be delayed."
					<< std::endl;

			// retrieve the stream header & get its XML version. The nominal rate is taken from
			// the same info, saving a second round trip to the source.
			const lsl::stream_info info = fetch_info(in);
			nominal_srate = info.nominal_srate();
			file_.write_stream_header(streamid, info.as_xml());
			std::cout << "Received header for stream " << src.name() << "." << std::endl;

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
			std::cout << "Started data collection for stream " << src.name() << "." << std::endl;

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
			std::cerr << "Error while recording from " << src.name() << ": " << e.what()
					  << std::endl;
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

			std::cout << "Wrote footer for stream " << src.name() << "." << std::endl;
			leave_footers_phase(phase_locked);
		} catch (std::exception &) {
			leave_footers_phase(phase_locked);
			throw;
		}
	} catch (shutdown_requested &e) {
		std::cout << "Recording from " << src.name() << " ended: " << e.what() << std::endl;
	} catch (std::exception &e) {
		std::cout << "Error in the record_from_streaminfo thread: " << e.what() << std::endl;
	}
	unregister_inlet(in);
}

void recording::record_boundaries() {
	try {
		while (!shutdown_) {
			if (wait_for_shutdown(boundary_interval)) break;
			file_.write_boundary_chunk();
		}
	} catch (std::exception &e) {
		std::cout << "Error in the record_boundaries thread: " << e.what() << std::endl;
	}
}

void recording::record_offsets(
	streamid_t streamid, inlet_p in, offset_flag_p offset_shutdown) noexcept {
	try {
		while (!shutdown_ && !*offset_shutdown) {
			// sleep for the interval
			if (wait_for_shutdown(offset_interval, offset_shutdown.get())) break;

			// query the time offset, again in short slices so that a stop is noticed promptly
			double offset = 0, now = 0;
			bool have_offset = false;
			const auto deadline = Clock::now() + max_time_correction_wait;
			while (!shutdown_ && !*offset_shutdown && Clock::now() < deadline) {
				try {
					offset = in->time_correction(network_poll_interval);
					now = lsl::local_clock();
					have_offset = true;
					break;
				} catch (lsl::timeout_error &) {}
			}
			if (!have_offset) {
				if (shutdown_ || *offset_shutdown) break;
				std::cerr << "Timeout in time correction query for stream " << streamid
						  << std::endl;
				continue;
			}

			file_.write_stream_offset(streamid, now, offset);
			// also append to the offset lists
			std::lock_guard<std::mutex> lock(offset_mut_);
			offset_lists_[streamid].emplace_back(now - offset, offset);
		}
	} catch (std::exception &e) {
		std::cout << "Error in the record_offsets thread: " << e.what() << std::endl;
	}
	std::cout << "Offsets thread is finished" << std::endl;
}

void recording::enter_headers_phase(bool phase_locked) {
	if (phase_locked) {
		std::lock_guard<std::mutex> lock(phase_mut_);
		headers_to_finish_++;
	}
}

void recording::leave_headers_phase(bool phase_locked) {
	if (phase_locked) {
		std::unique_lock<std::mutex> lock(phase_mut_);
		headers_to_finish_--;
		lock.unlock();
		ready_for_streaming_.notify_all();
	}
}

void recording::enter_streaming_phase(bool phase_locked) {
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

void recording::leave_streaming_phase(bool phase_locked) {
	if (phase_locked) {
		std::unique_lock<std::mutex> lock(phase_mut_);
		streaming_to_finish_--;
		lock.unlock();
		ready_for_footers_.notify_all();
	}
}

void recording::enter_footers_phase(bool phase_locked) {
	if (phase_locked) {
		std::unique_lock<std::mutex> lock(phase_mut_);
		// see enter_streaming_phase: a footer written slightly out of order beats no footer at all
		ready_for_footers_.wait_for(lock, max_footers_wait,
			[this]() { return this->ready_for_footers() || shutdown_.load(); });
	}
}

template <class T>
void recording::typed_transfer_loop(streamid_t streamid, double srate, const inlet_p &in,
	double &first_timestamp, double &last_timestamp, uint64_t &sample_count) {
	// optionally start an offset collection thread for this stream
	auto offset_shutdown = std::make_shared<std::atomic<bool>>(false);
	worker_p offset_thread(offsets_enabled_
			? spawn_worker([this, streamid, in, offset_shutdown] {
				  record_offsets(streamid, in, offset_shutdown);
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

		// Pull the first sample
		first_timestamp = 0.0;
		while (!shutdown_ && first_timestamp == 0.0)
			first_timestamp = last_timestamp = in->pull_sample(chunk, network_poll_interval);
		if (first_timestamp != 0.0) {
			// written directly: the very first sample anchors the stream and must keep its
			// timestamp even when the nominal interval is zero
			timestamps.assign(1, first_timestamp);
			file_.write_data_chunk(streamid, timestamps, chunk, (uint32_t)in->get_channel_count());
			sample_count += timestamps.size();
		}

		auto next_pull = Clock::now() + chunk_interval;
		while (!shutdown_) {
			// get a chunk from the stream
			in->pull_chunk_multiplexed(chunk, &timestamps, 1e-6);
			write_chunk();
			if (wait_until_shutdown(next_pull)) break;
			next_pull += chunk_interval;
		}

		if (first_timestamp != 0.0) {
			// one final non-blocking pull, so that samples already buffered in the inlet when the
			// stop arrived end up in the file rather than being dropped
			try {
				in->pull_chunk_multiplexed(chunk, &timestamps, 0.0);
				write_chunk();
			} catch (std::exception &e) {
				// the inlet was closed under us during teardown; the footer matters more
				std::cerr << "Could not drain stream " << streamid << " on stop: " << e.what()
						  << std::endl;
			}
		}
	} catch (std::exception &) {
		stop_offsets(offset_shutdown);
		timed_join_or_detach(offset_thread);
		throw;
	}
	stop_offsets(offset_shutdown);
	timed_join_or_detach(offset_thread);
}
