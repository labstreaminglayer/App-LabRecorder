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
	 * Stops and joins every recording thread, then closes and flushes the file. Network waits
	 * observe shutdown promptly; a slow disk write must finish before destruction returns.
	 */
	~recording();

	/// Ask all recording threads to wrap up. Returns immediately.
	void requestStop() noexcept;

private:
	struct impl;
	/// Workers retain the state while running; destruction joins them before releasing it.
	std::shared_ptr<impl> impl_;
};

#endif
