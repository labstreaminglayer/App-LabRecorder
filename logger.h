#include <mutex>
#include <iostream>

namespace Logger {
	static std::mutex print_lock;

	inline void log_info(const std::string &msg) {
		std::lock_guard<std::mutex> lock(print_lock);
		std::cout << msg << std::endl;
	}
	inline void log_warning(const std::string &msg) {
		std::lock_guard<std::mutex> lock(print_lock);
		std::cout << msg << std::endl;
	}
	inline void log_error(const std::string &msg) {
		std::lock_guard<std::mutex> lock(print_lock);
		std::cerr << msg << std::endl;
	}
}; // namespace Logger