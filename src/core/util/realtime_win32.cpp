#include "util/realtime_win32.h"

#include <map>
#include <mutex>

#include "util/common.h"

namespace brack::realtime {

void report(const char* what, const std::string& why) {
    const std::string outcome =
        std::string(what) + (why.empty() ? ": real-time (MMCSS Pro Audio)" : ": normal priority (" + why + ")");
    static std::mutex mutex;
    static std::map<std::string, std::string> last;  // what -> outcome
    std::lock_guard lock(mutex);
    std::string& before = last[what];
    if (before == outcome) return;
    before = outcome;
    log(why.empty() ? LogLevel::Info : LogLevel::Warning, outcome);
}

}  // namespace brack::realtime
