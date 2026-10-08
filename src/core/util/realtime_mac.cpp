#include "util/realtime_mac.h"

#include <AudioToolbox/AudioWorkInterval.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>

#include <map>
#include <mutex>

#include "util/common.h"

namespace brack::realtime {

namespace {
uint32_t toAbsolute(double seconds) {
    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    return (uint32_t)(seconds * 1e9 * timebase.denom / timebase.numer);
}
}  // namespace

std::string makeThisThreadRealtime(double sampleRate, unsigned periodFrames) {
    const double period = periodFrames / sampleRate;
    thread_time_constraint_policy_data_t policy{};
    policy.period = toAbsolute(period);
    policy.computation = toAbsolute(period / 2);
    policy.constraint = toAbsolute(period);
    policy.preemptible = 1;
    const kern_return_t r = thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                                              (thread_policy_t)&policy, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (r != KERN_SUCCESS) return std::string("thread_policy_set: ") + mach_error_string(r);
    return thisThreadIsRealtime() ? std::string() : "the thread did not take the policy";
}

namespace {
os_workgroup_interval_t g_interval = nullptr;
thread_local bool t_joined = false;
}  // namespace

void prepareWorkInterval() {
    static std::once_flag once;
    std::call_once(once, [] { g_interval = AudioWorkIntervalCreate("Brack plugin host", OS_CLOCK_MACH_ABSOLUTE_TIME, nullptr); });
}

std::string joinWorkInterval() {
    if (t_joined) return {};
    prepareWorkInterval();
    if (!g_interval) return "AudioWorkIntervalCreate failed";
    os_workgroup_join_token_s token{};  // joined for the rest of the thread's life
    if (const int r = os_workgroup_join(g_interval, &token)) return "os_workgroup_join: " + std::to_string(r);
    t_joined = true;
    return {};
}

void beginBlock(double seconds) {
    if (!t_joined) return;
    const uint64_t now = mach_absolute_time();
    static_cast<void>(os_workgroup_interval_start(g_interval, now, now + toAbsolute(seconds), nullptr));
}

void endBlock() {
    if (t_joined) static_cast<void>(os_workgroup_interval_finish(g_interval, nullptr));
}

bool thisThreadIsRealtime() {
    thread_time_constraint_policy_data_t policy{};
    mach_msg_type_number_t count = THREAD_TIME_CONSTRAINT_POLICY_COUNT;
    boolean_t isDefault = false;
    const kern_return_t r = thread_policy_get(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                                              (thread_policy_t)&policy, &count, &isDefault);
    return r == KERN_SUCCESS && !isDefault;
}

void report(const char* what, const std::string& why) {
    const std::string outcome =
        std::string(what) + (why.empty() ? ": real-time (time-constraint policy)" : ": normal priority (" + why + ")");
    static std::mutex mutex;
    static std::map<std::string, std::string> last;  // what -> outcome
    std::lock_guard lock(mutex);
    std::string& before = last[what];
    if (before == outcome) return;
    before = outcome;
    log(why.empty() ? LogLevel::Info : LogLevel::Warning, outcome);
}

}  // namespace brack::realtime
