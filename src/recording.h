#ifndef RECORDING_H
#define RECORDING_H

#include <lsl_cpp.h>
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

	/** Destructor.
	 * Asks the recording threads to finish and waits a bounded amount of time for them. A thread
	 * that is still stuck after that is left running; the file is closed once it finishes.
	 */
	~recording();

	/// Ask all recording threads to wrap up. Returns immediately.
	void requestStop() noexcept;

private:
	struct impl;
	/// Shared rather than unique: a recording thread that had to be left running keeps the state
	/// it writes into -- the file, the mutexes, the offset lists -- alive until it is done.
	std::shared_ptr<impl> impl_;
};

#endif
