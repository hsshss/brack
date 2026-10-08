// The C API (brack.h) keeps up with the engine: every public Engine method is listed below with
// the C functions that offer it, or with the reason it is not offered, and every C function is
// accounted for. A method or function added on one side only fails this test. Given the .NET
// binding's NativeMethods.cs too, every C function must be declared there, and nothing else.
#include <cstdio>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

const std::map<std::string, std::vector<std::string>> kOffered = {
    {"setConfig", {"brack_set_config"}},
    {"config", {"brack_get_config"}},
    {"startDevice", {"brack_start"}},
    {"startManual", {"brack_start_manual"}},
    {"stop", {"brack_stop"}},
    {"render", {"brack_render"}},
    {"renderPosition", {"brack_get_render_position"}},
    {"addPlugin", {"brack_add_plugin", "brack_add_plugin_ex"}},
    {"removePlugin", {"brack_remove_plugin"}},
    {"reloadPlugin", {"brack_reload_plugin"}},
    {"setPluginGuiVisible", {"brack_show_plugin_gui"}},
    {"showPluginGuiIn", {"brack_show_plugin_gui_in"}},
    {"getPluginState", {"brack_get_plugin_state"}},
    {"setPluginState", {"brack_set_plugin_state"}},
    {"setPluginName", {"brack_set_plugin_name"}},
    {"movePlugin", {"brack_move_plugin"}},
    {"moveMidiSource", {"brack_move_midi_source"}},
    {"addMidiSource", {"brack_add_midi_source"}},
    {"removeMidiSource", {"brack_remove_midi_source"}},
    {"reopenMidiSource", {"brack_reopen_midi_source"}},
    {"addMidiRoute", {"brack_connect_midi"}},
    {"removeMidiRoute", {"brack_disconnect_midi"}},
    {"setAudioRoutes", {"brack_clear_audio_routes", "brack_connect_audio"}},
    {"addAudioRoute", {"brack_connect_audio"}},
    {"removeAudioRoute", {"brack_disconnect_audio"}},
    {"setMasterGain", {"brack_set_master_gain"}},
    {"masterGain", {"brack_get_master_gain"}},
    {"sendMidiToPlugin", {"brack_send_midi", "brack_send_midi_at"}},
    {"sendMidiToSource", {"brack_send_midi_to_source", "brack_send_midi_to_source_at"}},
    {"nowNs", {"brack_now_ns"}},
    {"sendMidiToPluginAtTime", {"brack_send_midi_at_time"}},
    {"sendMidiToSourceAtTime", {"brack_send_midi_to_source_at_time"}},
    {"saveSessionJson", {"brack_save_session_json"}},
    {"loadSessionJson", {"brack_load_session_json"}},
    {"saveSessionFile", {"brack_save_session"}},
    {"loadSessionFile", {"brack_load_session"}},
    {"clear", {"brack_clear"}},
    {"snapshot", {"brack_get_status"}},
    {"cachedSnapshot", {"brack_get_status_cached"}},
    {"changeCount", {"brack_change_count"}},
    {"pollEvent", {"brack_poll_event"}},
    {"setEventNotify", {"brack_set_event_notify"}},
    {"scanPlugins", {"brack_scan_plugins"}},
};

const std::map<std::string, std::string> kNotOffered = {
    {"refreshSnapshot", "the cached status refreshes itself every ~50 ms"},
};

// C functions that are not about an engine method.
const std::set<std::string> kStandalone = {
    "brack_api_version",     "brack_version_string",       "brack_last_error",
    "brack_set_log_callback", "brack_engine_create",       "brack_engine_destroy",
    "brack_list_audio_devices",   "brack_list_midi_inputs",
    "brack_virtual_midi_available", "brack_virtual_midi_removal_hangs",
};

std::string readFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Names of the methods declared in the public part of class Engine.
std::set<std::string> engineMethods(const std::string& header) {
    std::set<std::string> names;
    const size_t cls = header.find("class Engine final");
    const size_t pub = header.find("public:", cls);
    const size_t priv = header.find("private:", pub);
    if (cls == std::string::npos || pub == std::string::npos || priv == std::string::npos) return names;
    std::istringstream body(header.substr(pub, priv - pub));
    const std::regex decl(R"(^\s+(?:static\s+)?[\w:<>,]+(?:<[^;]*>)?[\s*&]+(\w+)\()");
    for (std::string line; std::getline(body, line);) {
        const size_t text = line.find_first_not_of(' ');
        if (text == std::string::npos || line.compare(text, 2, "//") == 0) continue;
        std::smatch m;
        if (std::regex_search(line, m, decl) && m[1] != "Engine" && m[1] != "operator") names.insert(m[1]);
    }
    return names;
}

// The brack_* functions NativeMethods.cs declares ("internal static partial int brack_start(").
std::set<std::string> dotnetFunctions(const std::string& source) {
    std::set<std::string> names;
    const std::regex decl(R"(static\s+partial\s+[^;(]*?\b(brack_\w+)\s*\()");
    for (std::sregex_iterator it(source.begin(), source.end(), decl), end; it != end; ++it) names.insert((*it)[1]);
    return names;
}

std::set<std::string> cFunctions(const std::string& header) {
    std::set<std::string> names;
    const std::regex decl(R"(BRACK_API[^;(]*\b(brack_\w+)\s*\()");
    for (std::sregex_iterator it(header.begin(), header.end(), decl), end; it != end; ++it) names.insert((*it)[1]);
    return names;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: test_api_parity <engine.h> <brack.h> [NativeMethods.cs]\n");
        return 2;
    }
    const auto methods = engineMethods(readFile(argv[1]));
    const auto functions = cFunctions(readFile(argv[2]));
    std::printf("%zu engine methods, %zu C functions\n", methods.size(), functions.size());
    int failures = 0;
    if (methods.size() < 30 || functions.size() < 40) {
        std::fprintf(stderr, "could not read the headers\n");
        return 1;
    }

    for (auto& m : methods)
        if (!kOffered.count(m) && !kNotOffered.count(m)) {
            std::fprintf(stderr, "Engine::%s is not in the C API (list it in test_api_parity.cpp)\n", m.c_str());
            ++failures;
        }
    std::set<std::string> covered = kStandalone;
    for (auto& [method, fns] : kOffered) {
        if (!methods.count(method)) {
            std::fprintf(stderr, "Engine::%s is listed but not declared\n", method.c_str());
            ++failures;
        }
        for (auto& f : fns) {
            covered.insert(f);
            if (!functions.count(f)) {
                std::fprintf(stderr, "%s (for Engine::%s) is not declared in brack.h\n", f.c_str(), method.c_str());
                ++failures;
            }
        }
    }
    for (auto& f : functions)
        if (!covered.count(f)) {
            std::fprintf(stderr, "%s is not accounted for in test_api_parity.cpp\n", f.c_str());
            ++failures;
        }

    if (argc > 3) {
        const auto bound = dotnetFunctions(readFile(argv[3]));
        std::printf("%zu functions in the .NET binding\n", bound.size());
        for (auto& f : functions)
            if (!bound.count(f)) {
                std::fprintf(stderr, "%s is not in the .NET binding (NativeMethods.cs)\n", f.c_str());
                ++failures;
            }
        for (auto& f : bound)
            if (!functions.count(f)) {
                std::fprintf(stderr, "NativeMethods.cs declares %s, which brack.h does not\n", f.c_str());
                ++failures;
            }
    }

    std::puts(failures ? "FAILED" : "PASS");
    return failures ? 1 : 0;
}
