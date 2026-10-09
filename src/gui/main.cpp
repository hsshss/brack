// brack standalone GUI (Dear ImGui + GLFW + OpenGL 3).
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <future>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "engine.h"
#include "file_dialog.h"
#include "midi/midi_input.h"
#include "plugin/plugin_files.h"
#include "remote/plugin_host.h"
#include "settings.h"
#include "theme.h"
#include "util/common.h"
#include "widgets.h"

using namespace brack;
using namespace brack::ui;

namespace {

// How long the settings (the rack included) wait after the last change before they are
// written, so that a crash taking brack down loses only the last few seconds of work.
constexpr auto kAutosaveDelay = std::chrono::seconds(3);
// The longest changes go unsaved while they keep coming (a knob being turned, a plugin
// reporting its parameters as it plays).
constexpr auto kAutosaveMaxDelay = std::chrono::seconds(30);

// A file inside a VST3 bundle stands for the bundle: a dialog that picks files cannot pick the
// bundle's folder, and on Linux the binary in it is a .so.
std::filesystem::path bundleOf(const std::filesystem::path& file) {
    std::error_code ec;
    for (auto p = file.parent_path(); p.has_relative_path(); p = p.parent_path())
        if (pathToUtf8(p.extension()) == ".vst3" && std::filesystem::is_directory(p, ec)) return p;
    return file;
}

struct LogLine {
    LogLevel level;
    std::string text;
};

enum class View { Rack, Routing, Settings, Log };
struct ViewEntry {
    View view;
    const char* label;
    Icon icon;
};
// The side menu, top to bottom.
constexpr ViewEntry kViews[] = {
    {View::Rack, "Rack", Icon::Rack},
    {View::Routing, "Routing", Icon::Routing},
    {View::Settings, "Audio settings", Icon::Sliders},
    {View::Log, "Log", Icon::Log},
};

class App {
public:
    // configDir: where the settings and the plugin cache are kept.
    App(const GuiSettings& settings, std::filesystem::path configDir) : configDir_(std::move(configDir)) {
        setLogSink([this](LogLevel l, const std::string& m) {
            std::lock_guard lock(logMutex_);
            log_.push_back({l, m});
            while (log_.size() > 1000) log_.pop_front();
        });
        refreshDevices();
        virtualAvailable_ = virtualMidiAvailable(virtualReason_);
        virtualRemovalHangs_ = virtualAvailable_ && virtualMidiRemovalHangsService();
        extraDirs_ = settings.scanDirs;
        if (MidiSourceKind k; parseMidiSourceKind(settings.newSourceKind, k)) newKind_ = kindIndex(k);
        seenGuiState_ = guiState(settings);
        startScan();
        editCfg_ = engine_.config();
        worker_ = std::thread([this] { runWorker(); });
    }
    ~App() {
        stopWorker();
        if (scanThread_.joinable()) scanThread_.join();
        setLogSink(nullptr);
    }

    void openSession(const std::string& path) {
        runAsync("Loading session", [this, path] {
            std::string err;
            if (!engine_.loadSessionFile(path, err)) {
                error(err);
                return;
            }
            onUi([this, path, cfg = engine_.config()] {
                sessionPath_ = path;
                editCfg_ = cfg;
            });
            if (!engine_.startDevice(err)) error("audio: " + err);
        });
    }

    // Brings back the rack as it was when brack last exited, and the session file it came from.
    // The path comes back only with the rack: Save must never write a rack that did not come
    // from that file over it.
    void restoreSession(const std::string& json, const std::string& path) {
        runAsync("Restoring the previous session", [this, json, path] {
            std::string err;
            if (!engine_.loadSessionJson(json, err)) {
                keepSavedRack_ = true;
                error("could not restore the previous session: " + err);
                return;
            }
            onUi([this, path, cfg = engine_.config()] {
                sessionPath_ = path;
                editCfg_ = cfg;
            });
            if (!engine_.startDevice(err)) error("audio: " + err);
        });
    }

    // Engine work still queued or running; the window stays up until it is done.
    bool busy() const { return pending_.load() > 0; }

    // Every frame (`placement`: the current window geometry). Writes the settings kAutosaveDelay
    // after the last change, or kAutosaveMaxDelay after the first unsaved one. A change is anything
    // the engine counts (plugins, routing, configuration, a plugin reporting a state change) or
    // GUI state (folders, window placement).
    void autosave(const GuiSettings& placement) {
        const auto now = std::chrono::steady_clock::now();
        const uint64_t engineChanges = engine_.changeCount();
        std::string gui = guiState(placement);
        if (engineChanges != seenEngineChanges_ || gui != seenGuiState_) {
            seenEngineChanges_ = engineChanges;
            seenGuiState_ = std::move(gui);
            if (!unsaved_) firstUnsaved_ = now;
            lastChange_ = now;
            unsaved_ = true;
        }
        if (!unsaved_ || autosaving_) return;
        if (now - lastChange_ < kAutosaveDelay && now - firstUnsaved_ < kAutosaveMaxDelay) return;
        unsaved_ = false;
        autosaving_ = true;
        GuiSettings s = placement;  // with the rack as loaded, kept when keepSavedRack_
        s.scanDirs = extraDirs_;
        s.newSourceKind = midiSourceKindName(kNewKinds[newKind_]);
        // On the worker, in order with the engine work queued before it (a restore that fails
        // has set keepSavedRack_ by then). A crashed plugin is not asked: it contributes the
        // state it last saved.
        runAsync("Saving settings", [this, s, path = sessionPath_]() mutable {
            if (!keepSavedRack_) {
                s.sessionPath = path;
                s.session = engine_.saveSessionJson();
            }
            std::string err;
            const bool ok = saveGuiSettings(configDir_, s, err);
            if (!ok) logWarn("autosave: " + err);
            onUi([this, ok] {
                autosaving_ = false;
                if (ok) return;
                // Try again kAutosaveDelay from now, not on every frame.
                firstUnsaved_ = lastChange_ = std::chrono::steady_clock::now();
                unsaved_ = true;
            });
        });
    }

    // Only once the worker is idle (see busy()).
    void storeSettings(GuiSettings& s) {
        s.scanDirs = extraDirs_;
        s.newSourceKind = midiSourceKindName(kNewKinds[newKind_]);
        if (keepSavedRack_) return;
        s.sessionPath = sessionPath_;
        s.session = engine_.saveSessionJson();
    }

    void frame() {
        runUiTasks();
        // Never wait for the host thread here: it may be busy loading a plugin.
        snap_ = engine_.cachedSnapshot();
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("brack", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
        header();
        transport();
        banners();
        nav();
        ImGui::SameLine(0, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(24), px(24)));
        ImGui::BeginChild("view", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleVar();
        switch (view_) {
            case View::Rack: rackView(); break;
            case View::Routing: routingView(); break;
            case View::Settings: settingsView(); break;
            case View::Log: logView(); break;
        }
        ImGui::EndChild();
        addDialog();
        fileDialogFrame();
        ImGui::End();
    }

    bool fileDialogOpen() const { return dialog_.valid(); }

    std::string title() const { return "Brack - " + sessionName(); }

private:
    std::string sessionName() const {
        return sessionPath_.empty() ? std::string("untitled") : pathToUtf8(pathFromUtf8(sessionPath_).filename());
    }

    // What the GUI itself saves, as one comparable string (see autosave()).
    std::string guiState(const GuiSettings& placement) const {
        char window[96];
        std::snprintf(window, sizeof window, "%d,%d,%d,%d,%d", placement.x, placement.y, placement.width,
                      placement.height, placement.maximized ? 1 : 0);
        return extraDirs_ + '\n' + std::to_string(newKind_) + '\n' + sessionPath_ + '\n' + window;
    }

    // ---- file dialogs (file_dialog.h): the GUI draws on while one is open

    // `then` gets the path chosen in `dialog`, on the UI thread; nothing when it was cancelled.
    void choose(std::future<std::string> dialog, std::function<void(const std::string&)> then) {
        dialog_ = std::move(dialog);
        chosen_ = std::move(then);
    }

    // Brack's own controls wait behind a modal note while the dialog is open.
    void fileDialogFrame() {
        if (!dialog_.valid()) return;
        if (dialog_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            const std::string path = dialog_.get();
            const auto then = std::move(chosen_);
            chosen_ = {};
            if (!path.empty()) then(path);
            return;
        }
        ImGui::OpenPopup("File dialog");
        if (ImGui::BeginPopupModal("File dialog", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
            ImGui::TextUnformatted("Waiting for the file dialog...");
            ImGui::EndPopup();
        }
    }

    // Any thread.
    void error(const std::string& e) {
        logError(e);
        onUi([this, e] { lastError_ = e; });
    }

    // ---- engine work off the UI thread ----
    // Plugin loading runs on the engine's host thread (plugins want init and activate on their
    // main thread) and can take seconds. Engine calls are therefore made from this worker,
    // in order, and the UI reads cachedSnapshot(); results come back through onUi().

    void runAsync(std::string label, std::function<void()> fn) {
        pending_.fetch_add(1);
        {
            std::lock_guard lock(workMutex_);
            work_.push_back({std::move(label), std::move(fn)});
        }
        workCv_.notify_one();
    }

    void runWorker() {
        for (;;) {
            std::pair<std::string, std::function<void()>> job;
            {
                std::unique_lock lock(workMutex_);
                workCv_.wait(lock, [this] { return workStop_ || !work_.empty(); });
                if (work_.empty()) return;  // stopping, and nothing left to do
                job = std::move(work_.front());
                work_.pop_front();
                busyLabel_ = job.first;
            }
            try {
                job.second();
            } catch (const std::exception& ex) {
                error(ex.what());
            }
            engine_.refreshSnapshot();  // the UI sees the result on its next frame
            pending_.fetch_sub(1);
        }
    }

    void stopWorker() {
        {
            std::lock_guard lock(workMutex_);
            workStop_ = true;
        }
        workCv_.notify_one();
        if (worker_.joinable()) worker_.join();
    }

    std::string busyLabel() {
        std::lock_guard lock(workMutex_);
        return busyLabel_;
    }

    // Any thread: runs fn on the UI thread at the start of the next frame.
    void onUi(std::function<void()> fn) {
        std::lock_guard lock(uiMutex_);
        uiTasks_.push_back(std::move(fn));
    }

    void runUiTasks() {
        std::vector<std::function<void()>> tasks;
        {
            std::lock_guard lock(uiMutex_);
            tasks.swap(uiTasks_);
        }
        for (auto& t : tasks) t();
    }

    void refreshDevices() {
        devices_ = listAudioOutputDevices();
        midiInputs_ = listHardwareMidiInputs();
    }

    void startScan() {
        if (scanning_) return;
        if (scanThread_.joinable()) scanThread_.join();
        scanning_ = true;
        scanThread_ = std::thread([this, dirs = splitDirs(extraDirs_)] {
            // The DLL's scan (brack_scan_plugins with a cache). Unchanged files come from the cache:
            // VST2 plugins are instantiated to be described.
            const auto cachePath = configDir_.empty() ? std::string() : pathToUtf8(configDir_ / "plugin-cache.json");
            auto found = Engine::scanPlugins(dirs, cachePath);
            // brack hosts instruments: effects stay out of the list ("From file..." still loads anything).
            std::erase_if(found, [](const PluginDescription& d) { return !d.isInstrument(); });
            std::sort(found.begin(), found.end(), [](auto& a, auto& b) { return a.name < b.name; });
            std::lock_guard lock(scanMutex_);
            scanned_ = std::move(found);
            scanning_ = false;
        });
    }

    static std::vector<std::string> splitDirs(const std::string& s) {
        std::vector<std::string> out;
        size_t start = 0;
        while (start < s.size()) {
            size_t end = s.find(';', start);
            if (end == std::string::npos) end = s.size();
            if (end > start) out.push_back(s.substr(start, end - start));
            start = end + 1;
        }
        return out;
    }

    // ---- header, transport, banners, side menu ----

    void header() {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, col::header);
        ImGui::BeginChild("header", ImVec2(0, px(44)), 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleColor();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 wp = ImGui::GetWindowPos();
        const float ww = ImGui::GetWindowWidth();
        dl->AddLine(ImVec2(wp.x, wp.y + px(44) - 1), ImVec2(wp.x + ww, wp.y + px(44) - 1), col::line);
        ImGui::SetCursorScreenPos(ImVec2(wp.x + px(20), wp.y));
        Row row(44, ww - px(40));

        const ImVec2 logo = row.next(px(22), px(22));
        auto box = [&](float x, float y, float w, float h, float r, ImU32 c) {
            dl->AddRectFilled(ImVec2(logo.x + px(x), logo.y + px(y)), ImVec2(logo.x + px(x + w), logo.y + px(y + h)), c, px(r));
        };
        box(0, 0, 22, 22, 5.5f, col::header);
        dl->AddRect(logo, ImVec2(logo.x + px(22), logo.y + px(22)), col::borderStrong, px(5.5f));
        box(3.5f, 4.5f, 15, 5.5f, 1.5f, rgb(0x44444C));
        box(3.5f, 12, 15, 5.5f, 1.5f, rgb(0x44444C));
        box(5, 6, 5.5f, 2.75f, 1, col::midi);
        box(5, 13.5f, 5.5f, 2.75f, 1, col::audio);
        row.gap(10);
        drawText(row.next(textWidth(fonts.bold, 13, "Brack"), px(13)), fonts.bold, 13, col::text, "Brack");
        row.gap(16);

        const float sw = px(10 + 6 + 14 + 10) + textWidth(fonts.sans, 13, "Session");
        const ImVec2 sp = row.next(sw, px(30));
        ImGui::SetCursorScreenPos(sp);
        if (ImGui::InvisibleButton("##session", ImVec2(sw, px(30)))) ImGui::OpenPopup("session");
        if (ImGui::IsItemHovered() || ImGui::IsPopupOpen("session"))
            dl->AddRectFilled(sp, ImVec2(sp.x + sw, sp.y + px(30)), col::card, px(6));
        drawText(ImVec2(sp.x + px(10), sp.y + px(8.5f)), fonts.sans, 13, col::text2, "Session");
        drawIcon(dl, Icon::ChevronDown, ImVec2(sp.x + sw - px(24), sp.y + px(8)), px(14), col::text2);
        ImGui::SetNextWindowPos(ImVec2(sp.x, sp.y + px(34)));
        if (ImGui::BeginPopup("session")) {
            if (ImGui::MenuItem("New")) {
                runAsync("Clearing the rack", [this] { engine_.clear(); });
                sessionPath_.clear();
            }
            if (ImGui::MenuItem("Open..."))
                choose(openFileDialog("Brack session (*.json)", "*.json"), [this](const std::string& p) { openSession(p); });
            if (ImGui::MenuItem("Save", nullptr, false, !sessionPath_.empty())) save(sessionPath_);
            if (ImGui::MenuItem("Save as..."))
                choose(saveFileDialog("Brack session (*.json)", "*.json", "json"), [this](const std::string& p) { save(p); });
            ImGui::EndPopup();
        }
        row.gap(16);
        const std::string name = sessionName();
        drawText(row.next(textWidth(fonts.sans, 13, name), px(13)), fonts.sans, 13, col::text, name);

        if (busy()) {
            const std::string label = busyLabel() + "...";
            const float w = px(10 + 14 + 8 + 10) + textWidth(fonts.sans, 12, label);
            const ImVec2 p = row.nextRight(w, px(26));
            dl->AddRectFilled(p, ImVec2(p.x + w, p.y + px(26)), col::card, px(13));
            dl->AddRect(p, ImVec2(p.x + w, p.y + px(26)), col::border, px(13));
            const ImVec2 c(p.x + px(17), p.y + px(13));
            const float a = float(ImGui::GetTime()) * 2 * std::numbers::pi_v<float>;
            dl->AddCircle(c, px(4.7f), col::borderStrong, 0, px(1.75f));
            dl->PathArcTo(c, px(4.7f), a, a + std::numbers::pi_v<float> / 2);
            dl->PathStroke(col::text, 0, px(1.75f));
            drawText(ImVec2(p.x + px(32), p.y + px(7)), fonts.sans, 12, col::text2, label);
        }
        row.end();
        ImGui::EndChild();
        spaceBelow(0);
    }

    void save(const std::string& path) {
        runAsync("Saving", [this, path] {
            std::string err;
            if (engine_.saveSessionFile(path, err)) {
                onUi([this, path] { sessionPath_ = path; });
                logInfo("saved " + path);
            } else {
                error(err);
            }
        });
    }

    void transport() {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, col::transport);
        ImGui::BeginChild("transport", ImVec2(0, px(100)), 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleColor();
        const ImVec2 wp = ImGui::GetWindowPos();
        const float ww = ImGui::GetWindowWidth();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(wp.x, wp.y + px(100) - 1), ImVec2(wp.x + ww, wp.y + px(100) - 1), col::line);
        ImGui::SetCursorScreenPos(ImVec2(wp.x + px(20), wp.y + px(12)));

        const bool running = snap_.mode != EngineMode::Stopped;
        Row row(44, ww - px(40));
        const ButtonLook power = running ? ButtonLook{col::runningBg, col::runningBorder, col::audio} : ButtonLook{col::raised, col::borderStrong, col::text};
        const char* label = running ? "Running###audio" : "Start audio###audio";
        row.place(buttonWidth(label, power, 44, Icon::Power), px(44));
        if (button(label, power, 44, Icon::Power)) {
            runAsync(running ? "Stopping audio" : "Starting audio", [this, running] {
                std::string err;
                if (running) engine_.stop();
                else if (!engine_.startDevice(err)) error("audio: " + err);
            });
        }
        if (running) ImGui::SetItemTooltip("Stop audio");
        row.gap(24);

        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * (running ? 1.0f : 0.6f));
        drawText(row.next(textWidth(fonts.bold, 11, "MASTER"), px(11)), fonts.bold, 11, col::text3, "MASTER");
        row.gap(12);
        row.place(px(220), px(28));
        const float db = masterFader(px(220));
        row.gap(12);
        char value[32];
        if (db <= kMasterMinDb) std::snprintf(value, sizeof value, "-inf dB");
        else std::snprintf(value, sizeof value, "%.1f dB", db);
        const ImVec2 vp = row.next(px(56), px(12));
        drawText(ImVec2(vp.x + px(56) - textWidth(fonts.mono, 12, value), vp.y), fonts.mono, 12, col::text, value);
        ImGui::PopStyleVar();

        if (running && !snap_.outputPeaks.empty()) {
            row.gap(24);
            const float w = std::min(px(260), row.remaining());
            meters(row.next(w, px(44)), w);
        }
        row.end(10);

        Row status(22, ww - px(40));
        auto item = [&](const std::string& s, ImFont* f, ImU32 c) {
            drawText(status.next(textWidth(f, 12, s), px(12)), f, 12, c, s);
            status.gap(16);
        };
        if (running) {
            item(snap_.deviceName, fonts.sans, col::text);
            item("out " + khz(snap_.outputSampleRate) + " · " + std::to_string(snap_.outputChannels) + " ch", fonts.mono, col::text2);
            if (snap_.resampling)
                item("plugins " + khz(snap_.processSampleRate) + " · SRC " + resamplerQualityName(snap_.config.resamplerQuality) + " · " +
                         std::to_string((int)std::lround(snap_.resamplerLatencyMs)) + " ms",
                     fonts.mono, col::text2);
            char cpu[32];
            std::snprintf(cpu, sizeof cpu, "CPU %.1f%%", snap_.cpuLoad * 100.0f);
            item(cpu, fonts.mono, col::text2);
        } else {
            item("Audio stopped", fonts.sans, col::text3);
        }
        status.end();
        ImGui::EndChild();
        spaceBelow(0);
    }

    static std::string khz(uint32_t hz) {
        char s[32];
        std::snprintf(s, sizeof s, "%g kHz", hz / 1000.0);
        return s;
    }

    static constexpr float kMasterMinDb = -60.0f;

    // Master volume in dB; the left end mutes. It applies at once (no worker round trip),
    // and the snapshot reads it live, so the fader never lags behind. Returns the dB shown.
    float masterFader(float width) {
        const float gain = snap_.masterGain;
        float db = gain > 0.0f ? std::max(20.0f * std::log10(gain), kMasterMinDb) : kMasterMinDb;
        if (dbFader("##master", db, kMasterMinDb, 12.0f, width, 16, col::text)) {
            const float set = db <= kMasterMinDb ? 0.0f : std::pow(10.0f, db / 20.0f);
            engine_.setMasterGain(set);
            snap_.masterGain = set;
        }
        return db;
    }

    // One bar per output channel (up to 8) in a box 44 tall; numbered while there is room.
    void meters(ImVec2 p, float w) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const size_t n = std::min<size_t>(snap_.outputPeaks.size(), 8);
        const float pitch = std::min(px(10), px(44) / n), bar = std::max(1.0f, std::round(pitch * 0.6f));
        const bool numbered = n <= 4;
        const float x0 = p.x + (numbered ? px(18) : 0), top = p.y + (px(44) - pitch * n) * 0.5f;
        for (size_t c = 0; c < n; ++c) {
            const float y = std::round(top + c * pitch + (pitch - bar) * 0.5f);
            if (numbered)
                drawText(ImVec2(p.x, y + (bar - px(10)) * 0.5f), fonts.mono, 10, col::text3, std::to_string(c + 1));
            const float db = 20.0f * std::log10(std::max(snap_.outputPeaks[c], 1e-6f));
            const float v = std::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f);
            dl->AddRectFilled(ImVec2(x0, y), ImVec2(p.x + w, y + bar), col::raised, bar * 0.5f);
            if (v > 0)
                dl->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + (p.x + w - x0) * v, y + bar), db > -0.5f ? col::clip : col::audio, bar * 0.5f);
        }
    }

    void banners() {
        auto band = [](const char* id, ImU32 bg, ImU32 line) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(20), px(10)));
            ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
            const ImVec2 wp = ImGui::GetWindowPos();
            const float y = wp.y + ImGui::GetWindowHeight() - 1;
            ImGui::GetWindowDrawList()->AddLine(ImVec2(wp.x, y), ImVec2(wp.x + ImGui::GetWindowWidth(), y), line);
        };
        auto endBand = [] {
            ImGui::EndChild();
            spaceBelow(0);
        };
        if (!lastError_.empty()) {
            band("error", col::errorBg, col::errorBorder);
            const float w = ImGui::GetContentRegionAvail().x, wrap = w - px(16 + 12 + 12 + 28);
            const float th = fonts.sans->CalcTextSizeA(px(13), FLT_MAX, wrap, lastError_.c_str()).y;
            Row row(std::max(28.0f, th / px(1)), w);
            drawIcon(ImGui::GetWindowDrawList(), Icon::Warning, row.next(px(16), px(16)), px(16), col::error);
            row.gap(12);
            const ImVec2 tp = row.next(wrap, th);
            ImGui::GetWindowDrawList()->AddText(fonts.sans, px(13), tp, col::error, lastError_.c_str(), nullptr, wrap);
            row.placeRight(px(28), px(28));
            if (button("##dismiss", ButtonLook{0, 0, col::error}, 28, Icon::Close)) lastError_.clear();
            ImGui::SetItemTooltip("Dismiss");
            row.end();
            endBand();
        }
        if (snap_.deviceStalled) {
            band("stalled", col::warnBg, col::warnBorder);
            Row row(22);
            drawIcon(ImGui::GetWindowDrawList(), Icon::Warning, row.next(px(16), px(16)), px(16), col::warn);
            row.gap(12);
            const char* msg = "Output device disconnected. Plugins keep running on an internal clock until it returns.";
            drawText(row.next(textWidth(fonts.sans, 13, msg), px(13)), fonts.sans, 13, col::warn, msg);
            row.gap(12);
            row.place(textWidth(fonts.sans, 13, "Audio settings"), px(13));
            ImGui::PushStyleColor(ImGuiCol_TextLink, col::warn);
            if (ImGui::TextLink("Audio settings")) view_ = View::Settings;
            ImGui::PopStyleColor();
            row.end();
            endBand();
        }
    }

    void nav() {
        ImGui::BeginChild("nav", ImVec2(px(176), 0), 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 wp = ImGui::GetWindowPos();
        dl->AddLine(ImVec2(wp.x + px(176) - 1, wp.y), ImVec2(wp.x + px(176) - 1, wp.y + ImGui::GetWindowHeight()), col::line);
        ImGui::SetCursorScreenPos(ImVec2(wp.x + px(10), wp.y + px(16)));
        for (const ViewEntry& v : kViews) {
            Row row(40, px(156));
            const ImVec2 p = row.next(px(156), px(40));
            ImGui::SetCursorScreenPos(p);
            if (ImGui::InvisibleButton(v.label, ImVec2(px(156), px(40)))) view_ = v.view;
            const bool on = view_ == v.view;
            if (on || ImGui::IsItemHovered())
                dl->AddRectFilled(p, ImVec2(p.x + px(156), p.y + px(40)), on ? col::raised : col::card, px(8));
            const ImU32 c = on ? col::text : col::text2;
            drawIcon(dl, v.icon, ImVec2(p.x + px(12), p.y + px(11)), px(18), c);
            drawText(ImVec2(p.x + px(40), p.y + px(13.5f)), fonts.sans, 13, c, v.label);
            row.end(2);
        }
        ImGui::Dummy(ImVec2(0, 0));
        ImGui::EndChild();
    }

    // The view's title, and its subtitle; the caller may place buttons at the right, then end() the row.
    static Row viewTitle(const char* title, const char* subtitle = nullptr) {
        Row row(subtitle ? 52.0f : 36.0f);
        drawText(ImVec2(row.left(), row.top() + (subtitle ? 0 : (row.height() - px(22)) * 0.5f)), fonts.bold, 22, col::text, title);
        if (subtitle) drawText(ImVec2(row.left(), row.top() + px(33)), fonts.sans, 13, col::text3, subtitle);
        return row;
    }

    // ---- rack ----

    void rackView() {
        Row title = viewTitle("Rack");
        title.placeRight(buttonWidth("Add instrument", look::primary, 36, Icon::Plus), px(36));
        if (button("Add instrument", look::primary, 36, Icon::Plus)) openAdd_ = true;
        title.gapRight(12);
        title.placeRight(buttonWidth("From file...", look::outline, 36, Icon::File), px(36));
        if (button("From file...", look::outline, 36, Icon::File)) chooseFile();
        title.end(24);

        sectionHeader("INSTRUMENTS", col::text, "Drag to reorder · double-click a name to rename, a volume for 0 dB");
        if (snap_.plugins.empty()) {
            Row row(26);
            const char* none = "No instruments yet. Add one with Add instrument.";
            drawText(row.next(textWidth(fonts.sans, 13, none), px(13)), fonts.sans, 13, col::text3, none);
            row.end(10);
        }
        for (size_t row = 0; row < snap_.plugins.size(); ++row) pluginCard(row);
        ImGui::Dummy(ImVec2(0, 0));
        spaceBelow(14);
        sectionHeader("MIDI INPUTS", col::midi);
        sourcesCard();
    }

    // "L" / "R" for a stereo port, else the channel's number; prefixed with the port's number
    // when the plugin has more than one output port.
    static std::string channelLabel(const EngineSnapshot::Plugin& p, uint32_t port, uint32_t ch) {
        std::string s = p.audioOutputs.size() > 1 ? std::to_string(port + 1) + ":" : "";
        return s + (p.audioOutputs[port].channels == 2 ? (ch == 0 ? "L" : "R") : std::to_string(ch + 1));
    }

    std::vector<AudioRoute> routesOf(const std::string& plugin) const {
        std::vector<AudioRoute> mine;
        for (auto& r : snap_.audioRoutes)
            if (r.plugin == plugin) mine.push_back(r);
        return mine;
    }

    static std::vector<AudioRoute>::iterator findRoute(std::vector<AudioRoute>& routes, uint32_t port, uint32_t ch) {
        return std::find_if(routes.begin(), routes.end(), [&](auto& r) { return r.port == port && r.channel == ch; });
    }

    void startRename(const EngineSnapshot::Plugin& p) {
        renaming_ = p.id;
        renameBuf_ = displayName(p);
        renameFocus_ = true;
    }

    void pluginCard(size_t index) {
        const auto& p = snap_.plugins[index];
        ImGui::PushID(p.id.c_str());
        beginCard("card", 0, p.crashed ? col::errorBg : col::card, p.crashed ? col::errorBorder : col::border, ImVec2(8, 8));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const std::string shown = displayName(p);
        const float x0 = ImGui::GetCursorScreenPos().x, width = ImGui::GetContentRegionAvail().x - px(6);
        const bool canReload = (p.crashed && p.separateProcess) || !p.loaded;

        Row line(32, width);
        if (renaming_ != p.id) {
            // The whole line is the handle; the buttons on it stay clickable.
            ImGui::SetNextItemAllowOverlap();
            ImGui::InvisibleButton("##line", ImVec2(width, line.height()));
            if (ImGui::IsItemHovered()) {
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) startRename(p);
                ImGui::SetTooltip("id: %s\n%s %s\n%s\n(drag to reorder, double-click to rename)", p.id.c_str(),
                                  pluginFormatLabel(p.format), p.pluginId.c_str(), p.path.c_str());
            }
            std::string from;
            if (dragReorder("BRACK_PLUGIN", p.id, shown, from))
                runAsync("Reordering", [this, from, index] {
                    std::string err;
                    if (!engine_.movePlugin(from, index, err)) error(err);
                });
        }
        const ImVec2 grip = line.next(px(20), px(32));
        drawIcon(dl, Icon::Grip, ImVec2(grip.x - px(2), grip.y + px(4)), px(24), col::faint);
        line.gap(10);
        if (renaming_ == p.id) {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(px(8), px(7)));
            line.place(px(240), ImGui::GetFrameHeight());
            if (renameFocus_) {
                ImGui::SetKeyboardFocusHere();
                renameFocus_ = false;
            }
            ImGui::SetNextItemWidth(px(240));
            const bool enter = ImGui::InputText("##rename", &renameBuf_, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            ImGui::PopStyleVar();
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                renaming_.clear();
            } else if (enter || ImGui::IsItemDeactivated()) {
                if (renameBuf_ != shown)
                    runAsync("Renaming " + shown, [this, id = p.id, name = renameBuf_] {
                        std::string err;
                        if (!engine_.setPluginName(id, name, err)) error(err);
                    });
                renaming_.clear();
            }
        } else {
            drawText(line.next(textWidth(fonts.bold, 15, shown), px(15)), fonts.bold, 15, col::text, shown);
        }
        line.gap(10);
        const char* format = pluginFormatLabel(p.format);
        ImVec2 sz = badgeSize(format, fonts.mono, 10, 5);
        badge(line.next(sz.x, sz.y), format, fonts.mono, 10, col::control, 0, col::text2, 5);
        if (!p.architecture.empty()) {
            line.gap(8);
            sz = badgeSize(p.architecture, fonts.mono, 10, 5);
            const ImVec2 a = line.next(sz.x, sz.y);
            badge(a, p.architecture, fonts.mono, 10, 0, col::borderTag, col::text2, 5);
            if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(a, ImVec2(a.x + sz.x, a.y + sz.y))) {
                if (p.architecture == buildArchitecture())
                    ImGui::SetTooltip("Built for %s, as Brack is.", p.architecture.c_str());
                else
                    ImGui::SetTooltip("Built for %s: runs in %s.", p.architecture.c_str(),
                                      pathToUtf8(pluginHostExecutable(p.architecture).filename()).c_str());
            }
        }
        line.gap(14);

        line.placeRight(px(32), px(32));
        if (button("##more", look::ghost, 32, Icon::More)) ImGui::OpenPopup("more");
        if (ImGui::BeginPopup("more")) {
            if (ImGui::MenuItem("Rename")) startRename(p);
            if (canReload && ImGui::MenuItem("Reload")) reload(p);
            if (ImGui::MenuItem("Remove")) {
                runAsync("Removing " + p.id, [this, id = p.id] {
                    std::string err;
                    if (!engine_.removePlugin(id, err)) error(err);
                });
            }
            ImGui::EndPopup();
        }
        line.gapRight(10);
        if (canReload) {
            const ButtonLook& l = p.crashed ? look::danger : look::dangerOutline;
            line.placeRight(buttonWidth("Reload", l, 32, Icon::Reload, 12), px(32));
            if (button("Reload", l, 32, Icon::Reload, 12)) reload(p);
        } else if (p.hasGui && !p.crashed) {
            line.placeRight(buttonWidth("Editor", look::filled, 32, Icon::Editor, 12), px(32));
            if (button("Editor", look::filled, 32, Icon::Editor, 12))
                runAsync("Opening the editor", [this, id = p.id] {
                    std::string err;
                    if (!engine_.setPluginGuiVisible(id, true, err)) error(err);
                });
        }
        line.gapRight(10);

        // The status, clipped to the room left between the badges and the buttons.
        const ImVec2 s0(line.left(), line.top()), s1(line.left() + std::max(0.0f, line.remaining()), line.top() + line.height());
        ImGui::PushClipRect(s0, s1, true);
        const bool statusHovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(s0, s1);
        if (p.crashed) {
            drawIcon(dl, Icon::Warning, line.next(px(13), px(13)), px(13), col::error);
            line.gap(6);
            const char* what = p.separateProcess ? "Crashed — silent until reloaded" : "Crashed — silent until Brack restarts";
            drawText(line.next(textWidth(fonts.sans, 12, what), px(12)), fonts.sans, 12, col::error, what);
            if (statusHovered)
                ImGui::SetTooltip("%s\n%s", p.status.c_str(),
                                  p.separateProcess ? "Silent until it is loaded again (Reload)."
                                                    : "No longer called and silent. Save the session and restart Brack to use it again.");
        } else if (!p.loaded || p.failed) {
            const std::string what = p.failed ? "process error" : p.status;
            drawText(line.next(textWidth(fonts.sans, 12, what), px(12)), fonts.sans, 12, col::error, what);
            if (statusHovered) ImGui::SetTooltip("%s", p.status.c_str());
        } else {
            const ImVec2 d = line.next(px(6), px(6));
            dl->AddCircleFilled(ImVec2(d.x + px(3), d.y + px(3)), px(3), tint(p.active ? col::audio : col::faint));
            line.gap(6);
            const std::string what = std::string(p.active ? "Active" : "Idle") + (p.latency ? " · latency " + std::to_string(p.latency) : "");
            drawText(line.next(textWidth(fonts.sans, 12, what), px(12)), fonts.sans, 12, col::text2, what);
        }
        ImGui::PopClipRect();
        line.end(6);

        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * (p.crashed ? 0.55f : 1.0f));
        const float bodyX = x0 + px(30), bodyW = width - px(30);
        auto bodyRow = [&](float maxWidth) {
            ImGui::SetCursorScreenPos(ImVec2(bodyX, ImGui::GetCursorScreenPos().y));
            return Row(26, std::min(bodyW, maxWidth));
        };
        auto head = [&](Row& row, const char* label) {
            drawText(row.next(px(72), px(11)), fonts.bold, 11, col::text3, label);
            row.gap(8);
        };
        auto none = [&](Row& row) { drawText(row.next(px(40), px(12)), fonts.sans, 12, col::text3, "None"); };

        Row midi = bodyRow(bodyW);
        head(midi, "MIDI FROM");
        bool any = false;
        for (auto& s : snap_.sources) {
            const bool routed = std::any_of(snap_.midiRoutes.begin(), snap_.midiRoutes.end(),
                                            [&](const MidiRoute& r) { return r.source == s.id && r.plugin == p.id; });
            if (!routed) continue;
            const std::string name = sourceName(s);
            const float w = px(8 + 6 + 6 + 8) + textWidth(fonts.sans, 12, name);
            if (w > midi.remaining()) break;
            const ImVec2 a = midi.next(w, px(22));
            dl->AddRectFilled(a, ImVec2(a.x + w, a.y + px(22)), tint(col::midiPill), px(11));
            dl->AddCircleFilled(ImVec2(a.x + px(11), a.y + px(11)), px(3), tint(receiving(s) ? col::midi : col::borderStrong));
            drawText(ImVec2(a.x + px(20), a.y + px(5)), fonts.sans, 12, col::midiText, name);
            midi.gap(8);
            any = true;
        }
        if (!any) none(midi);
        midi.end(2);

        std::vector<AudioRoute> mine = routesOf(p.id);
        bool changed = false, first = true;
        for (uint32_t port = 0; port < p.audioOutputs.size(); ++port) {
            for (uint32_t ch = 0; ch < p.audioOutputs[port].channels; ++ch) {
                const std::string key = p.id + "/" + std::to_string(port) + "/" + std::to_string(ch);
                ImGui::PushID(key.c_str());
                Row row = bodyRow(px(480));
                head(row, first ? "AUDIO OUT" : "");
                first = false;
                auto it = findRoute(mine, port, ch);
                const bool routed = it != mine.end();
                const std::string label = channelLabel(p, port, ch) + " → " + (routed ? std::to_string(it->output + 1) : "off");
                const float lw = std::max(px(48), textWidth(fonts.mono, 12, label));
                const ImVec2 lp = row.next(lw, px(12));
                drawText(lp, fonts.mono, 12, routed ? col::audioText : col::text3, label);
                if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(lp, ImVec2(lp.x + lw, lp.y + px(12))))
                    ImGui::SetTooltip("%s, channel %u", p.audioOutputs[port].name.c_str(), ch + 1);
                row.gap(8);
                if (routed) {
                    // While dragging, and until the snapshot catches up (a frame or more later),
                    // show the edited value: the stale one would pull the thumb back.
                    const float snapDb = 20.0f * std::log10(std::max(it->gain, 1e-4f));
                    float db = key == gainKey_ ? gainDb_ : snapDb;
                    const ImVec2 vp = row.nextRight(px(56), px(12));
                    row.gapRight(8);
                    const float fw = row.remaining();
                    row.place(fw, px(26));
                    const bool moved = dbFader("##gain", db, -60.0f, 12.0f, fw, 14, col::faint);
                    if (moved) {
                        it->gain = std::pow(10.0f, db / 20.0f);
                        changed = true;
                    }
                    if (moved || ImGui::IsItemActive()) {
                        gainKey_ = key;
                        gainDb_ = db;
                        gainEditTime_ = ImGui::GetTime();
                    } else if (key == gainKey_ && (std::fabs(snapDb - gainDb_) < 0.05f || ImGui::GetTime() - gainEditTime_ > 0.5)) {
                        gainKey_.clear();
                    }
                    char value[32];
                    std::snprintf(value, sizeof value, "%.1f dB", db);
                    drawText(ImVec2(vp.x + px(56) - textWidth(fonts.mono, 12, value), vp.y), fonts.mono, 12, col::text2, value);
                }
                row.end(2);
                ImGui::PopID();
            }
        }
        if (first) {
            Row row = bodyRow(px(480));
            head(row, "AUDIO OUT");
            none(row);
            row.end(2);
        }
        if (changed) {
            runAsync("Routing", [this, id = p.id, mine] {
                std::string err;
                if (!engine_.setAudioRoutes(id, mine, err)) error(err);
            });
        }
        ImGui::PopStyleVar();
        ImGui::Dummy(ImVec2(0, px(2)));
        endCard();
        spaceBelow(10);
        ImGui::PopID();
    }

    void reload(const EngineSnapshot::Plugin& p) {
        runAsync("Loading " + p.id + " again", [this, id = p.id] {
            std::string err;
            if (!engine_.reloadPlugin(id, err)) error(err);
        });
    }

    void chooseFile() {
        const std::string vst2 = std::string("*") + vst2Extension();
        choose(openFileDialog("Plugin (*.clap, *.vst3, VST2 " + vst2 + ")", "*.clap;*.vst3;" + vst2), [this](const std::string& p) {
            PluginConfig pc;
            pc.path = pathToUtf8(bundleOf(pathFromUtf8(p)));
            addPlugin(pc);
        });
    }

    static std::string displayName(const EngineSnapshot::Plugin& p) { return p.name.empty() ? p.id : p.name; }
    // API sources have no port; their id is what callers address them by.
    static std::string sourceName(const EngineSnapshot::Source& s) { return s.name.empty() ? s.id : s.name; }

    // Whether the source has had messages in the last 0.15 s: its dots light up while it does.
    bool receiving(const EngineSnapshot::Source& s) {
        auto& last = activity_[s.id];
        if (s.messages != last.first) last = {s.messages, ImGui::GetTime()};
        return ImGui::GetTime() - last.second < 0.15;
    }
    // Call right after a list row's item: makes it a drag source (carrying `id`) and a
    // drop target. Returns true, with the dragged id in `from`, when another row of the
    // same `type` was dropped here; the caller then moves it to this row's index.
    static bool dragReorder(const char* type, const std::string& id, const std::string& label, std::string& from) {
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(type, id.c_str(), id.size() + 1);
            ImGui::TextUnformatted(label.c_str());
            ImGui::EndDragDropSource();
        }
        bool dropped = false;
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(type)) {
                from = static_cast<const char*>(payload->Data);
                dropped = from != id;
            }
            ImGui::EndDragDropTarget();
        }
        return dropped;
    }

    void addPlugin(const PluginConfig& pc) {
        std::string label = "Loading " + pathToUtf8(pathFromUtf8(pc.path).filename());
        runAsync(label, [this, pc] {
            std::string err;
            if (engine_.addPlugin(pc, true, err).empty()) error(err);
        });
    }

    void sourcesCard() {
        beginCard("sources", 0, col::card, col::border, ImVec2(0, 0));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float w = ImGui::GetContentRegionAvail().x;
        for (size_t index = 0; index < snap_.sources.size(); ++index) {
            auto& s = snap_.sources[index];
            ImGui::PushID(s.id.c_str());
            Row row(48, w);
            const ImVec2 r0(row.left(), row.top()), r1(row.left() + w, row.top() + row.height());
            if (!s.ok) dl->AddRectFilled(r0, r1, col::errorBg);
            dl->AddLine(ImVec2(r0.x, r1.y - 1), ImVec2(r1.x, r1.y - 1), col::line);
            // The whole row is the handle; the buttons on it stay clickable.
            ImGui::SetNextItemAllowOverlap();
            ImGui::InvisibleButton("##row", ImVec2(w, row.height()));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("id: %s\n%llu messages\n(drag to reorder)", s.id.c_str(), (unsigned long long)s.messages);
            std::string from;
            if (dragReorder("BRACK_SOURCE", s.id, sourceName(s), from))
                runAsync("Reordering", [this, from, index] {
                    std::string err;
                    if (!engine_.moveMidiSource(from, index, err)) error(err);
                });
            row.gap(14);
            row.gapRight(14);

            const char* tag = s.kind == MidiSourceKind::Virtual ? "VIRTUAL" : s.kind == MidiSourceKind::Hardware ? "DEVICE" : "API";
            const ImVec2 tsz = badgeSize(tag, fonts.bold, 10, 6);
            const ImVec2 tp = row.next(px(64), tsz.y);
            badge(tp, tag, fonts.bold, 10, 0, s.ok ? col::borderTag : col::errorTag, s.ok ? col::text2 : col::error, 6);
            row.gap(10);

            row.placeRight(px(28), px(28));
            if (button("##remove", ButtonLook{0, 0, col::text3}, 28, Icon::Close)) {
                runAsync("Removing " + s.id, [this, id = s.id] {
                    std::string err;
                    if (!engine_.removeMidiSource(id, err)) error(err);
                });
            }
            ImGui::SetItemTooltip("Remove");
            row.gapRight(10);
            if (!s.ok && s.kind != MidiSourceKind::Api) {
                row.placeRight(buttonWidth("Retry", look::dangerOutline, 28, Icon::None, 12), px(28));
                if (button("Retry", look::dangerOutline, 28, Icon::None, 12)) {
                    runAsync("Reopening " + s.id, [this, id = s.id] {
                        std::string err;
                        if (!engine_.reopenMidiSource(id, err)) error(err);
                    });
                }
                row.gapRight(10);
            }
            const ImVec2 led = row.nextRight(px(8), px(8));
            dl->AddCircleFilled(ImVec2(led.x + px(4), led.y + px(4)), px(4), receiving(s) ? col::midi : col::borderStrong);
            row.gapRight(10);

            const ImVec2 n0(row.left(), row.top());
            ImGui::PushClipRect(n0, ImVec2(n0.x + std::max(0.0f, row.remaining()), n0.y + row.height()), true);
            const std::string name = sourceName(s);
            ImFont* nf = s.kind == MidiSourceKind::Api ? fonts.mono : fonts.sans;
            const float ns = s.kind == MidiSourceKind::Api ? 12.0f : 13.0f;
            drawText(row.next(textWidth(nf, ns, name), px(ns)), nf, ns, col::text, name);
            if (!s.ok) {
                row.gap(10);
                drawText(row.next(textWidth(fonts.sans, 12, s.status), px(12)), fonts.sans, 12, col::error, s.status);
            }
            ImGui::PopClipRect();
            row.end();
            ImGui::PopID();
        }

        const MidiSourceKind kind = kNewKinds[newKind_];
        bool canAdd = kind != MidiSourceKind::Hardware || newInput_ < (int)midiInputs_.size();
        if (kind == MidiSourceKind::Virtual && !virtualAvailable_) canAdd = false;
        Row add(54, w);
        add.gap(14);
        add.gapRight(14);
        static const char* const kinds[] = {"Device", "Virtual port", "API"};  // as kNewKinds
        add.place(segmentedWidth(kinds), px(34));
        segmented("##kind", newKind_, kinds);
        add.gap(8);
        add.placeRight(buttonWidth("Add input", look::filled, 34), px(34));
        ImGui::BeginDisabled(!canAdd);
        if (button("Add input", look::filled, 34)) {
            MidiSourceConfig sc;
            sc.kind = kind;
            if (kind == MidiSourceKind::Virtual) sc.name = newVirtualName_;
            else if (kind == MidiSourceKind::Hardware) sc.name = midiInputs_[newInput_];
            else sc.id = newApiId_;
            runAsync(sc.kind == MidiSourceKind::Virtual ? "Creating virtual port" : "Opening MIDI input", [this, sc] {
                std::string err;
                if (engine_.addMidiSource(sc, err).empty()) error(err);
            });
        }
        ImGui::EndDisabled();
        add.gapRight(8);
        if (kind == MidiSourceKind::Hardware) {
            add.placeRight(px(34), px(34));
            if (button("##refresh", look::outlineMuted, 34, Icon::Reload)) refreshDevices();
            ImGui::SetItemTooltip("Refresh ports");
            add.gapRight(8);
        }
        const float fw = add.remaining();
        add.place(fw, ImGui::GetFrameHeight());
        ImGui::SetNextItemWidth(fw);
        if (kind == MidiSourceKind::Virtual) {
            ImGui::InputTextWithHint("##vname", "virtual port name", &newVirtualName_);
        } else if (kind == MidiSourceKind::Hardware) {
            std::string preview = newInput_ < (int)midiInputs_.size() ? midiInputs_[newInput_] : "(none)";
            if (ImGui::BeginCombo("##hw", preview.c_str())) {
                for (int i = 0; i < (int)midiInputs_.size(); ++i)
                    if (ImGui::Selectable(midiInputs_[i].c_str(), i == newInput_)) newInput_ = i;
                ImGui::EndCombo();
            }
        } else {
            ImGui::InputTextWithHint("##apiid", "source id", &newApiId_);
        }
        add.end();

        std::string warning;
        if (kind == MidiSourceKind::Virtual && !virtualAvailable_) warning = "Virtual ports unavailable: " + virtualReason_;
        if (kind == MidiSourceKind::Virtual && virtualRemovalHangs_)
            warning = "Warning: this Windows build has a MIDI service bug (microsoft/MIDI#1047). Removing a virtual port, or "
                      "quitting Brack while one exists, makes Windows MIDI Services stop responding until Windows is "
                      "restarted. Fixed by the late-November 2026 Windows update.";
        if (!warning.empty()) {
            const ImVec2 p(ImGui::GetCursorScreenPos().x + px(14), ImGui::GetCursorScreenPos().y);
            const float wrap = w - px(28);
            const float h = fonts.sans->CalcTextSizeA(px(12), FLT_MAX, wrap, warning.c_str()).y;
            dl->AddText(fonts.sans, px(12), p, col::warn, warning.c_str(), nullptr, wrap);
            ImGui::Dummy(ImVec2(w, h + px(12)));
        }
        endCard();
    }

    // ---- routing ----

    void routingView() {
        viewTitle("Routing", "Every connection in one place. The rack shows the same routes per instrument.").end(28);
        sectionHeader("MIDI → INSTRUMENTS", col::midi, "Click a cell to connect");
        midiMatrix();
        spaceBelow(28);
        sectionHeader("INSTRUMENTS → OUTPUTS", col::audio, "Volume is set per instrument in the rack");
        audioCards();
    }

    void midiMatrix() {
        struct MatrixRow {
            std::string plugin, label;
            uint16_t port;
            bool crashed;
        };
        std::vector<MatrixRow> rows;
        for (auto& p : snap_.plugins)
            for (uint16_t i = 0; i < p.notePorts.size(); ++i)
                rows.push_back(
                    {p.id, p.notePorts.size() > 1 ? displayName(p) + " / " + p.notePorts[i].name : displayName(p), i, p.crashed});
        if (rows.empty() || snap_.sources.empty()) {
            text("Add MIDI inputs and instruments with note inputs to route them.", fonts.sans, 13, col::text3);
            return;
        }
        int ncols = (int)std::min<size_t>(snap_.sources.size() + 1, 63);
        float labels = textWidth(fonts.sans, 13, "instrument /");
        for (int c = 0; c + 1 < ncols; ++c) labels = std::max(labels, textWidth(fonts.sans, 13, sourceName(snap_.sources[c])));
        const float rowH = px(40), cell = px(24);
        const float height = std::min(px(420), labels + px(16) + rows.size() * (rowH + 1));
        beginCard("matrix", 0, col::card, col::border, ImVec2(0, 0));
        if (ImGui::BeginTable("matrix", ncols,
                              ImGuiTableFlags_BordersInner | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_HighlightHoveredColumn,
                              ImVec2(0, height))) {
            ImGui::TableSetupScrollFreeze(1, 1);  // the instrument column and the header row
            // Every header is vertical: input columns stay narrow, and the corner needs no second
            // header row. No backslash in the corner label: Japanese fonts draw a yen sign.
            ImGui::TableSetupColumn("instrument /\ninput", ImGuiTableColumnFlags_AngledHeader);
            // Column ids follow the position ("##<index>"), not the label: ImGui reconciles columns
            // by id and would carry a moved column's display order along, undoing a reorder.
            for (int c = 0; c + 1 < ncols; ++c)
                ImGui::TableSetupColumn((sourceName(snap_.sources[c]) + "##" + std::to_string(c)).c_str(),
                                        ImGuiTableColumnFlags_AngledHeader | ImGuiTableColumnFlags_WidthFixed, cell);
            // ImGui measures the angle from vertical: 0 turns the labels a full 90 degrees.
            ImGui::PushStyleVar(ImGuiStyleVar_TableAngledHeadersAngle, 0.0f);
            ImGui::TableAngledHeadersRow();
            ImGui::PopStyleVar();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            for (auto& row : rows) {
                ImGui::TableNextRow(0, rowH);
                ImGui::TableNextColumn();
                ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + px(8), ImGui::GetCursorPosY() + (rowH - px(13)) * 0.5f - ImGui::GetStyle().CellPadding.y));
                text(row.label, fonts.sans, 13, row.crashed ? col::text3 : col::text);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("id: %s%s", row.plugin.c_str(), row.crashed ? "\n(crashed)" : "");
                for (int c = 0; c + 1 < ncols; ++c) {
                    ImGui::TableNextColumn();
                    const auto& s = snap_.sources[c];
                    MidiRoute r{s.id, row.plugin, row.port};
                    const bool on = std::find(snap_.midiRoutes.begin(), snap_.midiRoutes.end(), r) != snap_.midiRoutes.end();
                    ImGui::PushID((s.id + "|" + row.plugin + "|" + std::to_string(row.port)).c_str());
                    const ImVec2 p(ImGui::GetCursorScreenPos().x + (ImGui::GetContentRegionAvail().x - cell) * 0.5f,
                                   ImGui::GetCursorScreenPos().y + (rowH - cell) * 0.5f - ImGui::GetStyle().CellPadding.y);
                    ImGui::SetCursorScreenPos(p);
                    if (ImGui::InvisibleButton("##r", ImVec2(cell, cell))) {
                        runAsync("Routing", [this, r, on] {
                            std::string err;
                            if (on ? !engine_.removeMidiRoute(r) : !engine_.addMidiRoute(r, err)) error(err);
                        });
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s → %s", sourceName(s).c_str(), row.label.c_str());
                    const ImVec2 q(p.x + cell, p.y + cell);
                    if (on) {
                        dl->AddRectFilled(p, q, col::midi, px(5));
                        drawIcon(dl, Icon::Check, ImVec2(p.x + px(5), p.y + px(5)), px(14), col::onMidi);
                    } else {
                        if (ImGui::IsItemHovered()) dl->AddRectFilled(p, q, col::raised, px(5));
                        dl->AddRect(p, q, col::borderTag, px(5));
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
        endCard();
    }

    void audioCards() {
        if (snap_.plugins.empty()) {
            text("Add an instrument to route its outputs.", fonts.sans, 13, col::text3);
            return;
        }
        const uint32_t outs = snap_.outputChannels ? snap_.outputChannels : std::max<uint32_t>(snap_.config.audio.channels, 2);
        std::vector<std::string> names{"Off"};
        for (uint32_t o = 0; o < outs; ++o) names.push_back("Out " + std::to_string(o + 1));
        std::vector<const char*> labels;
        for (auto& n : names) labels.push_back(n.c_str());
        const float gap = px(12), w = std::floor((ImGui::GetContentRegionAvail().x - gap) * 0.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
        for (size_t i = 0; i < snap_.plugins.size(); ++i) {
            if (i % 2) ImGui::SameLine();
            audioCard(snap_.plugins[i], labels, w);
        }
        ImGui::PopStyleVar();
    }

    // `outputs`: "Off", then one label per output channel.
    void audioCard(const EngineSnapshot::Plugin& p, std::span<const char* const> outputs, float width) {
        ImGui::PushID(p.id.c_str());
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * (p.crashed ? 0.6f : 1.0f));
        beginCard("outputs", width, col::card, col::border, ImVec2(0, 0));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float w = ImGui::GetContentRegionAvail().x;
        Row title(40, w);
        title.gap(14);
        const std::string name = displayName(p);
        drawText(title.next(textWidth(fonts.bold, 13, name), px(13)), fonts.bold, 13, col::text, name);
        title.gap(8);
        std::string note = p.audioOutputs.size() == 1 ? p.audioOutputs[0].name : "";
        if (p.crashed) note += note.empty() ? "crashed" : " · crashed";
        drawText(title.next(textWidth(fonts.sans, 12, note), px(12)), fonts.sans, 12, col::text3, note);
        const float ly = title.top() + px(40) - 1, lx = ImGui::GetWindowPos().x;
        dl->AddLine(ImVec2(lx, ly), ImVec2(lx + ImGui::GetWindowWidth(), ly), tint(col::line));
        title.end(4);

        float labelW = px(16);
        for (uint32_t port = 0; port < p.audioOutputs.size(); ++port)
            for (uint32_t ch = 0; ch < p.audioOutputs[port].channels; ++ch)
                labelW = std::max(labelW, textWidth(fonts.mono, 12, channelLabel(p, port, ch)));
        std::vector<AudioRoute> mine = routesOf(p.id);
        bool changed = false;
        for (uint32_t port = 0; port < p.audioOutputs.size(); ++port) {
            for (uint32_t ch = 0; ch < p.audioOutputs[port].channels; ++ch) {
                ImGui::PushID(int(port * 1000 + ch));
                Row row(44, w);
                row.gap(14);
                row.gapRight(14);
                drawText(row.next(labelW, px(12)), fonts.mono, 12, col::text2, channelLabel(p, port, ch));
                row.gap(12);
                auto it = findRoute(mine, port, ch);
                int sel = it == mine.end() ? 0 : (int)it->output + 1;
                const int was = sel;
                if (segmentedWidth(outputs) <= row.remaining()) {
                    row.place(segmentedWidth(outputs), px(34));
                    segmented("##out", sel, outputs, sel == 0 ? col::controlOn : col::audio, sel == 0 ? col::text : col::onAudio);
                } else {  // too many outputs to show side by side
                    const float cw = std::min(row.remaining(), px(140));
                    row.place(cw, ImGui::GetFrameHeight());
                    ImGui::SetNextItemWidth(cw);
                    if (ImGui::BeginCombo("##out", outputs[sel])) {
                        for (int o = 0; o < (int)outputs.size(); ++o)
                            if (ImGui::Selectable(outputs[o], sel == o)) sel = o;
                        ImGui::EndCombo();
                    }
                }
                if (sel != was) {
                    if (sel == 0) std::erase_if(mine, [&](auto& r) { return r.port == port && r.channel == ch; });
                    else if (it != mine.end()) it->output = uint32_t(sel - 1);
                    else mine.push_back({p.id, port, ch, uint32_t(sel - 1), 1.0f});
                    changed = true;
                }
                row.end();
                ImGui::PopID();
            }
        }
        if (p.audioOutputs.empty()) {
            Row row(44, w);
            row.gap(14);
            drawText(row.next(px(100), px(12)), fonts.sans, 12, col::text3, "No audio outputs");
            row.end();
        }
        if (changed) {
            runAsync("Routing", [this, id = p.id, mine] {
                std::string err;
                if (!engine_.setAudioRoutes(id, mine, err)) error(err);
            });
        }
        ImGui::Dummy(ImVec2(0, px(4)));
        endCard();
        ImGui::PopStyleVar();
        ImGui::PopID();
    }

    // ---- settings ----

    static int differences(const EngineConfig& a, const EngineConfig& b) {
        return (a.audio.deviceName != b.audio.deviceName) + (a.audio.sampleRate != b.audio.sampleRate) +
               (a.audio.channels != b.audio.channels) + (a.audio.bufferFrames != b.audio.bufferFrames) +
               (a.audio.exclusive != b.audio.exclusive) + (a.processSampleRate != b.processSampleRate) +
               (a.blockSize != b.blockSize) + (a.resamplerQuality != b.resamplerQuality) + (a.pluginsInProcess != b.pluginsInProcess) +
               (a.loadPluginsSerially != b.loadPluginsSerially);
    }

    // A settings card's title line; the caller may place buttons at the right, then end() it.
    static Row cardTitle(const char* title, float width) {
        Row row(48, width);
        const float y = row.top() + px(48) - 1;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(row.left(), y), ImVec2(row.left() + width, y), tint(col::line));
        row.gap(20);
        row.gapRight(12);
        drawText(row.next(textWidth(fonts.bold, 14, title), px(14)), fonts.bold, 14, col::text, title);
        return row;
    }

    void settingsView() {
        const float width = std::min(ImGui::GetContentRegionAvail().x, px(860));
        viewTitle("Audio settings", "Changes wait here until you apply them.").end(20);
        auto& a = editCfg_.audio;
        const EngineConfig& now = snap_.config;
        float x0 = 0;
        auto bodyRow = [&](float h) {
            ImGui::SetCursorScreenPos(ImVec2(x0 + px(20), ImGui::GetCursorScreenPos().y));
            return Row(h, width - px(40));
        };
        auto label = [&](Row& row, const char* s) { drawText(row.next(px(216), px(13)), fonts.sans, 13, col::text2, s); };
        auto note = [&](Row& row, const char* s) { drawText(row.next(textWidth(fonts.sans, 12, s), px(12)), fonts.sans, 12, col::text3, s); };
        // An edited setting's control is outlined in amber until it is applied.
        auto edited = [](bool differs) { ImGui::PushStyleColor(ImGuiCol_Border, differs ? col::midi : col::borderStrong); };
        auto uintInput = [](const char* id, uint32_t& v, uint32_t lo, uint32_t hi) {
            int iv = (int)v;
            ImGui::SetNextItemWidth(px(96));
            ImGui::PushFont(fonts.mono, 13);
            if (ImGui::InputInt(id, &iv, 0, 0)) v = (uint32_t)std::clamp(iv, (int)lo, (int)hi);
            ImGui::PopFont();
        };
        auto rateCombo = [](const char* id, uint32_t& v, const char* zeroLabel) {
            static const uint32_t rates[] = {44100, 48000, 88200, 96000, 176400, 192000};
            std::string preview = v == 0 ? zeroLabel : std::to_string(v) + " Hz";
            ImGui::SetNextItemWidth(px(200));
            if (ImGui::BeginCombo(id, preview.c_str())) {
                if (ImGui::Selectable(zeroLabel, v == 0)) v = 0;
                for (auto r : rates)
                    if (ImGui::Selectable((std::to_string(r) + " Hz").c_str(), v == r)) v = r;
                ImGui::EndCombo();
            }
        };

        beginCard("device", width, col::card, col::border, ImVec2(0, 0), 12);
        x0 = ImGui::GetCursorScreenPos().x;
        cardTitle("Output device", width).end(18);
        {
            Row row = bodyRow(36);
            label(row, "Device");
            row.placeRight(px(36), px(36));
            if (button("##refresh", look::outlineMuted, 36, Icon::Reload)) refreshDevices();
            ImGui::SetItemTooltip("Refresh devices");
            row.gapRight(8);
            const float cw = row.remaining();
            row.place(cw, ImGui::GetFrameHeight());
            ImGui::SetNextItemWidth(cw);
            edited(a.deviceName != now.audio.deviceName);
            std::string preview = a.deviceName.empty() ? "(system default)" : a.deviceName;
            if (ImGui::BeginCombo("##device", preview.c_str())) {
                if (ImGui::Selectable("(system default)", a.deviceName.empty())) a.deviceName.clear();
                for (auto& d : devices_)
                    if (ImGui::Selectable(d.name.c_str(), a.deviceName == d.name)) a.deviceName = d.name;
                ImGui::EndCombo();
            }
            ImGui::PopStyleColor();
            row.end(18);
        }
#ifdef _WIN32
        {
            Row row = bodyRow(24);
            label(row, "WASAPI exclusive mode");
            row.place(px(40), px(24));
            toggleSwitch("##exclusive", a.exclusive);
            row.gap(12);
            note(row, "Brack takes the device for itself and picks its rate. Windows only.");
            row.end(18);
        }
        {
            Row row = bodyRow(36);
            label(row, "Device sample rate");
            row.place(px(200), ImGui::GetFrameHeight());
            ImGui::BeginDisabled(!a.exclusive);
            edited(a.sampleRate != now.audio.sampleRate);
            rateCombo("##devrate", a.sampleRate, "Device default");
            ImGui::PopStyleColor();
            ImGui::EndDisabled();
            if (!a.exclusive) {
                row.gap(12);
                note(row, "Shared mode runs at the device mix rate.");
            }
            row.end(18);
        }
#else
        {
            Row row = bodyRow(20);
            label(row, "Device sample rate");
            note(row, "The device runs at the sound server's rate.");
            row.end(18);
        }
#endif
        {
            char ms[48] = "frames";
            const uint32_t rate = snap_.outputSampleRate ? snap_.outputSampleRate : a.sampleRate;
            if (rate) std::snprintf(ms, sizeof ms, "frames · %.1f ms", a.bufferFrames * 1000.0 / rate);
            const float second = px(96 + 8 + 32) + textWidth(fonts.sans, 12, ms);
            Row labels = bodyRow(20);
            drawText(labels.next(second, px(13)), fonts.sans, 13, col::text2, "Buffer");
            drawText(labels.next(px(200), px(13)), fonts.sans, 13, col::text2, "Output channels");
            labels.end(6);
            Row row = bodyRow(36);
            row.place(px(96), ImGui::GetFrameHeight());
            edited(a.bufferFrames != now.audio.bufferFrames);
            uintInput("##buffer", a.bufferFrames, 16, 8192);
            ImGui::PopStyleColor();
            row.gap(8);
            note(row, ms);
            row.gap(32);
            row.place(px(96), ImGui::GetFrameHeight());
            edited(a.channels != now.audio.channels);
            uintInput("##channels", a.channels, 1, 32);
            ImGui::PopStyleColor();
            row.end(20);
        }
        endCard();
        spaceBelow(20);

        beginCard("processing", width, col::card, col::border, ImVec2(0, 0), 12);
        x0 = ImGui::GetCursorScreenPos().x;
        cardTitle("Plugin processing", width).end(18);
        {
            Row labels = bodyRow(20);
            drawText(labels.next(px(232), px(13)), fonts.sans, 13, col::text2, "Processing sample rate");
            drawText(labels.next(px(200), px(13)), fonts.sans, 13, col::text2, "Block size");
            labels.end(6);
            Row row = bodyRow(36);
            row.place(px(200), ImGui::GetFrameHeight());
            edited(editCfg_.processSampleRate != now.processSampleRate);
            rateCombo("##prate", editCfg_.processSampleRate, "Same as output");
            ImGui::PopStyleColor();
            row.gap(32);
            row.place(px(96), ImGui::GetFrameHeight());
            edited(editCfg_.blockSize != now.blockSize);
            uintInput("##block", editCfg_.blockSize, 16, 8192);
            ImGui::PopStyleColor();
            row.gap(8);
            char ms[48] = "frames";
            // Plugins run at the processing rate; "same as output" is the device's rate.
            const uint32_t outRate = snap_.outputSampleRate ? snap_.outputSampleRate : editCfg_.audio.sampleRate;
            const uint32_t rate = editCfg_.processSampleRate ? editCfg_.processSampleRate : outRate;
            if (rate) std::snprintf(ms, sizeof ms, "frames · %.1f ms", editCfg_.blockSize * 1000.0 / rate);
            note(row, ms);
            row.end(18);
        }
        {
            Row head = bodyRow(20);
            label(head, "Resampler quality");
            head.end(8);
            Row row = bodyRow(50);
            static const char* const names[] = {"Standard", "High", "Ultra"};  // ResamplerQuality order
            static const char* const depth[] = {"136 dB", "180 dB", "207 dB"};
            ImDrawList* dl = ImGui::GetWindowDrawList();
            for (int i = 0; i < 3; ++i) {
                const ImVec2 p = row.next(px(120), px(50));
                ImGui::SetCursorScreenPos(p);
                if (ImGui::InvisibleButton(names[i], ImVec2(px(120), px(50)))) editCfg_.resamplerQuality = ResamplerQuality(i);
                const bool on = (int)editCfg_.resamplerQuality == i;
                const ImVec2 q(p.x + px(120), p.y + px(50));
                if (on) dl->AddRectFilled(p, q, col::control, px(8));
                const bool pending = on && editCfg_.resamplerQuality != now.resamplerQuality;
                dl->AddRect(p, q, pending ? col::midi : on ? col::text : col::borderStrong, px(8));
                drawText(ImVec2(p.x + px(14), p.y + px(9)), fonts.sans, 13, on || ImGui::IsItemHovered() ? col::text : col::text2, names[i]);
                drawText(ImVec2(p.x + px(14), p.y + px(28)), fonts.mono, 11, col::text3, depth[i]);
                row.gap(8);
            }
            row.end(8);
            char latency[160] = "Used when the processing rate differs from the device rate. Linear phase, r8brain.";
            if (snap_.resampling)
                std::snprintf(latency, sizeof latency, "Used when the processing rate differs from the device rate. Linear phase, r8brain. Current latency %.2f ms.",
                              snap_.resamplerLatencyMs);
            Row hint = bodyRow(18);
            note(hint, latency);
            hint.end(18);
        }
        {
            const char* why = "Off: each plugin runs in a process of its own, so one that crashes or hangs stops only itself "
                              "and can be reloaded. Plugins for the other architecture always run on their own.";
            const float wrap = std::min(px(560), width - px(40 + 32 + 40 + 12));
            const float th = fonts.sans->CalcTextSizeA(px(12), FLT_MAX, wrap, why).y;
            const float h = px(14 + 18 + 4 + 14) + th;
            Row box = bodyRow(h / px(1));
            const ImVec2 p(box.left(), box.top()), q(box.left() + width - px(40), box.top() + h);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p, q, col::bg, px(10));
            dl->AddRect(p, q, editCfg_.pluginsInProcess != now.pluginsInProcess ? col::midi : col::line, px(10));
            ImGui::SetCursorScreenPos(ImVec2(p.x + px(16), p.y + px(14)));
            toggleSwitch("##inprocess", editCfg_.pluginsInProcess);
            drawText(ImVec2(p.x + px(16 + 40 + 12), p.y + px(15)), fonts.sans, 13, col::text, "Run plugins inside Brack");
            dl->AddText(fonts.sans, px(12), ImVec2(p.x + px(16 + 40 + 12), p.y + px(36)), col::text3, why, nullptr, wrap);
            box.end(20);
        }
        {
            const char* why = "On: a session's plugins load, and start, one after another. Slower to open, for plugins "
                              "that fail when two of them start together.";
            const float wrap = std::min(px(560), width - px(40 + 32 + 40 + 12));
            const float th = fonts.sans->CalcTextSizeA(px(12), FLT_MAX, wrap, why).y;
            const float h = px(14 + 18 + 4 + 14) + th;
            Row box = bodyRow(h / px(1));
            const ImVec2 p(box.left(), box.top()), q(box.left() + width - px(40), box.top() + h);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p, q, col::bg, px(10));
            dl->AddRect(p, q, editCfg_.loadPluginsSerially != now.loadPluginsSerially ? col::midi : col::line, px(10));
            ImGui::SetCursorScreenPos(ImVec2(p.x + px(16), p.y + px(14)));
            toggleSwitch("##serially", editCfg_.loadPluginsSerially);
            drawText(ImVec2(p.x + px(16 + 40 + 12), p.y + px(15)), fonts.sans, 13, col::text, "Load plugins one at a time");
            dl->AddText(fonts.sans, px(12), ImVec2(p.x + px(16 + 40 + 12), p.y + px(36)), col::text3, why, nullptr, wrap);
            box.end(20);
        }
        endCard();
        spaceBelow(20);

        if (const int n = differences(editCfg_, now)) {
            beginCard("apply", width, col::pendingBg, col::warnBorder, ImVec2(0, 0), 12);
            Row row(64, width);
            row.gap(20);
            row.gapRight(16);
            const ImVec2 d = row.next(px(8), px(8));
            ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(d.x + px(4), d.y + px(4)), px(4), col::midi);
            row.gap(16);
            const std::string what = std::to_string(n) + (n == 1 ? " change" : " changes") + " not applied";
            const ImVec2 t = row.next(px(320), px(36));
            drawText(t, fonts.sans, 13, col::warn, what);
            drawText(ImVec2(t.x, t.y + px(20)), fonts.sans, 12, col::pendingNote, "Applying restarts audio and reloads every plugin.");
            row.placeRight(buttonWidth("Apply", look::amber, 36), px(36));
            if (button("Apply", look::amber, 36)) {
                runAsync("Applying audio settings", [this, cfg = editCfg_] {
                    std::string err;
                    if (!engine_.setConfig(cfg, err)) error(err);
                });
            }
            row.gapRight(12);
            row.placeRight(buttonWidth("Revert", look::amberOutline, 36), px(36));
            if (button("Revert", look::amberOutline, 36)) editCfg_ = now;
            row.end();
            endCard();
            spaceBelow(20);
        }

        beginCard("folders", width, col::card, col::border, ImVec2(0, 0), 12);
        x0 = ImGui::GetCursorScreenPos().x;
        {
            Row title = cardTitle("Plugin folders", width);
            const char* rescan = scanning_ ? "Scanning###rescan" : "Rescan###rescan";
            title.placeRight(buttonWidth(rescan, look::outline, 32, Icon::Reload, 12), px(32));
            ImGui::BeginDisabled(scanning_);
            if (button(rescan, look::outline, 32, Icon::Reload, 12)) startScan();
            ImGui::EndDisabled();
            title.end();
        }
        std::vector<std::string> dirs = splitDirs(extraDirs_);
        for (size_t i = 0; i < dirs.size(); ++i) {
            ImGui::PushID(int(i));
            Row row(52, width);
            const float y = row.top() + px(52) - 1;
            ImGui::GetWindowDrawList()->AddLine(ImVec2(row.left(), y), ImVec2(row.left() + width, y), col::line);
            row.gap(20);
            row.gapRight(12);
            drawIcon(ImGui::GetWindowDrawList(), Icon::Folder, row.next(px(16), px(16)), px(16), col::text3);
            row.gap(12);
            row.placeRight(px(32), px(32));
            const bool remove = button("##remove", look::ghost, 32, Icon::Close);
            ImGui::SetItemTooltip("Remove");
            row.gapRight(12);
            const float tw = std::max(0.0f, row.remaining());
            const ImVec2 t = row.next(tw, px(12));
            const std::string shown = shortenPath(dirs[i], fonts.mono, 12, tw);
            drawText(t, fonts.mono, 12, col::text, shown);
            if (shown != dirs[i] && ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(t, ImVec2(t.x + tw, t.y + px(16))))
                ImGui::SetTooltip("%s", dirs[i].c_str());
            row.end();
            ImGui::PopID();
            if (remove) {
                dirs.erase(dirs.begin() + i);
                extraDirs_ = joinDirs(dirs);
                break;
            }
        }
        {
            ImGui::SetCursorScreenPos(ImVec2(x0, ImGui::GetCursorScreenPos().y + px(12)));
            Row row(34, width);
            row.gap(20);
            row.gapRight(12);
            row.placeRight(buttonWidth("Add", look::filled, 34, Icon::Plus), px(34));
            std::string dir = newDir_;
            dir.erase(0, dir.find_first_not_of(" \t"));
            dir.erase(dir.find_last_not_of(" \t") + 1);
            ImGui::BeginDisabled(dir.empty());
            const bool addDir = button("Add", look::filled, 34, Icon::Plus);
            ImGui::EndDisabled();
            row.gapRight(8);
            const float iw = row.remaining();
            row.place(iw, ImGui::GetFrameHeight());
            ImGui::SetNextItemWidth(iw);
            const bool enter = ImGui::InputTextWithHint("##newdir", "Folder path", &newDir_, ImGuiInputTextFlags_EnterReturnsTrue);
            if ((addDir || enter) && !dir.empty()) {
                dirs.push_back(dir);
                extraDirs_ = joinDirs(dirs);
                newDir_.clear();
            }
            row.end(12);
        }
        {
            Row row(18, width);
            row.gap(20);
            note(row, "Searched as well as the standard CLAP, VST3 and VST2 locations, CLAP_PATH, VST3_PATH and VST_PATH.");
            row.end(16);
        }
        endCard();
        spaceBelow(16);
        {
            Row row(18, width);
            note(row, "The rack and these settings are saved to");
            row.gap(4);
            const std::string path = pathToUtf8(guiSettingsPath(configDir_));
            const float pw = std::max(0.0f, row.remaining());
            const ImVec2 p = row.next(pw, px(12));
            drawText(p, fonts.mono, 12, col::text2, shortenPath(path, fonts.mono, 12, pw));
            if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(p, ImVec2(p.x + pw, p.y + px(16)))) ImGui::SetTooltip("%s", path.c_str());
            row.end();
        }
    }

    static std::string joinDirs(const std::vector<std::string>& dirs) {
        std::string s;
        for (auto& d : dirs) s += (s.empty() ? "" : ";") + d;
        return s;
    }

    // ---- add instrument ----

    void addDialog() {
        if (openAdd_) {
            ImGui::OpenPopup("Add instrument");
            openAdd_ = false;
        }
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        const float w = std::min(px(680), vp->WorkSize.x - px(32));
        const float listH = std::clamp(vp->WorkSize.y - px(96 + 48 + 216), px(120), px(420));
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + (vp->WorkSize.x - w) * 0.5f, vp->WorkPos.y + px(96)));
        ImGui::SetNextWindowSize(ImVec2(w, px(216) + listH));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, px(14));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        const bool open = ImGui::BeginPopupModal("Add instrument", nullptr,
                                                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);
        ImGui::PopStyleVar(3);
        if (!open) return;
        bool rescan = false;
        {
            std::lock_guard lock(scanMutex_);
            addDialogBody(w, listH, rescan);
        }
        if (rescan) startScan();
        ImGui::EndPopup();
    }

    void addDialogBody(float w, float listH, bool& rescan) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 wp = ImGui::GetWindowPos();
        bool close = ImGui::IsKeyPressed(ImGuiKey_Escape);
        ImGui::SetCursorScreenPos(ImVec2(wp.x + px(20), wp.y + px(16)));
        Row head(32, w - px(36));
        drawText(head.next(textWidth(fonts.bold, 16, "Add instrument"), px(16)), fonts.bold, 16, col::text, "Add instrument");
        head.placeRight(px(32), px(32));
        if (button("##close", look::ghost, 32, Icon::Close)) close = true;
        head.end(14);

        ImGui::SetCursorScreenPos(ImVec2(wp.x + px(20), ImGui::GetCursorScreenPos().y));
        Row search(42, w - px(40));
        const ImVec2 s0(search.left(), search.top()), s1(search.left() + w - px(40), search.top() + px(42));
        dl->AddRectFilled(s0, s1, col::bg, px(9));
        dl->AddRect(s0, s1, col::text, px(9));
        search.gap(12);
        drawIcon(dl, Icon::Search, search.next(px(16), px(16)), px(16), col::text2);
        search.gap(10);
        search.gapRight(12);
        ImGui::PushFont(nullptr, 14);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        const float iw = search.remaining();
        search.place(iw, ImGui::GetFrameHeight());
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(iw);
        ImGui::InputTextWithHint("##search", "Search by name or vendor", &addQuery_);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::PopFont();
        search.end(12);

        static const char* const formats[] = {"All", "CLAP", "VST3", "VST2"};
        static const PluginFormat formatOf[] = {PluginFormat::Clap, PluginFormat::Clap, PluginFormat::Vst3, PluginFormat::Vst2};
        ImGui::SetCursorScreenPos(ImVec2(wp.x + px(20), ImGui::GetCursorScreenPos().y));
        Row pills(28, w - px(40));
        for (int i = 0; i < 4; ++i) {
            const float pw = px(24) + textWidth(fonts.sans, 12, formats[i]);
            const ImVec2 p = pills.next(pw, px(28));
            ImGui::SetCursorScreenPos(p);
            if (ImGui::InvisibleButton(formats[i], ImVec2(pw, px(28)))) addFormat_ = i;
            const bool on = addFormat_ == i;
            const ImVec2 q(p.x + pw, p.y + px(28));
            if (on) dl->AddRectFilled(p, q, col::text, px(14));
            else dl->AddRect(p, q, ImGui::IsItemHovered() ? col::borderTag : col::borderStrong, px(14));
            drawText(ImVec2(p.x + px(12), p.y + px(8)), fonts.sans, 12, on ? col::onLight : col::text2, formats[i]);
            pills.gap(6);
        }
        pills.end(12);

        std::string query = addQuery_;
        for (char& c : query) c = (char)std::tolower((unsigned char)c);
        auto contains = [&](std::string s) {
            for (char& c : s) c = (char)std::tolower((unsigned char)c);
            return s.find(query) != std::string::npos;
        };
        std::vector<const PluginDescription*> shown;
        for (auto& d : scanned_)
            if ((addFormat_ == 0 || d.format == formatOf[addFormat_]) && (query.empty() || contains(d.name) || contains(d.vendor)))
                shown.push_back(&d);
        auto keyOf = [](const PluginDescription& d) { return d.path + '\n' + d.id; };
        const PluginDescription* picked = nullptr;
        for (auto* d : shown)
            if (keyOf(*d) == addPick_) picked = d;
        const PluginDescription* add = nullptr;

        const float listTop = ImGui::GetCursorScreenPos().y;
        dl->AddLine(ImVec2(wp.x, listTop), ImVec2(wp.x + w, listTop), col::line);
        ImGui::SetCursorScreenPos(ImVec2(wp.x, listTop + 1));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(6), px(6)));
        ImGui::BeginChild("list", ImVec2(w, listH - 1), ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleVar();
        ImDrawList* ldl = ImGui::GetWindowDrawList();
        const float rw = ImGui::GetContentRegionAvail().x;
        for (size_t i = 0; i < shown.size(); ++i) {
            const PluginDescription& d = *shown[i];
            const bool on = &d == picked;
            const float rh = px(10 + 19 + 2 + 17 + 10) + (on ? px(17) : 0);
            const ImVec2 p = ImGui::GetCursorScreenPos(), q(p.x + rw, p.y + rh);
            ImGui::PushID(int(i));
            if (ImGui::InvisibleButton("##plugin", ImVec2(rw, rh))) addPick_ = keyOf(d);
            if (ImGui::IsItemHovered()) {
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) add = &d;
                ImGui::SetTooltip("%s\n%s", d.id.c_str(), d.path.c_str());
            }
            ImGui::PopID();
            if (on) {
                ldl->AddRectFilled(p, q, col::control, px(8));
                ldl->AddRect(p, q, col::borderTag, px(8));
            } else if (ImGui::IsItemHovered()) {
                ldl->AddRectFilled(p, q, col::raised, px(8));
            }
            float right = q.x - px(14);
            auto rightBadge = [&](const std::string& s, ImU32 bg, ImU32 border) {
                const ImVec2 sz = badgeSize(s, fonts.mono, 10, 5);
                right -= sz.x;
                badge(ImVec2(right, p.y + px(12)), s, fonts.mono, 10, bg, border, col::text2, 5);
                right -= px(10);
            };
            rightBadge(pluginFormatLabel(d.format), col::control, 0);
            if (!d.architecture.empty()) rightBadge(d.architecture, 0, col::borderTag);
            ImGui::PushClipRect(p, ImVec2(right, q.y), true);
            drawText(ImVec2(p.x + px(14), p.y + px(10)), fonts.sans, 14, col::text, d.name);
            ImGui::PopClipRect();
            drawText(ImVec2(p.x + px(14), p.y + px(31)), fonts.sans, 12, col::text3, d.vendor);
            if (on) drawText(ImVec2(p.x + px(14), p.y + px(50)), fonts.mono, 11, col::text3, shortenPath(d.path, fonts.mono, 11, q.x - p.x - px(28)));
        }
        if (shown.empty()) {
            const char* empty = scanned_.empty() && scanning_ ? "Scanning for instruments..."
                                                              : "No instrument matches. Effects and unlisted plugins load with From file...";
            const ImVec2 p = ImGui::GetCursorScreenPos();
            drawText(ImVec2(p.x + (rw - textWidth(fonts.sans, 13, empty)) * 0.5f, p.y + px(32)), fonts.sans, 13, col::text3, empty);
            ImGui::Dummy(ImVec2(rw, px(80)));
        }
        ImGui::EndChild();

        const float footTop = listTop + listH;
        dl->AddRectFilled(ImVec2(wp.x + 1, footTop), ImVec2(wp.x + w - 1, wp.y + px(216) + listH - 1), col::transport, px(13),
                          ImDrawFlags_RoundCornersBottom);
        dl->AddLine(ImVec2(wp.x, footTop), ImVec2(wp.x + w, footTop), col::line);
        ImGui::SetCursorScreenPos(ImVec2(wp.x + px(20), footTop));
        Row foot(60, w - px(36));
        foot.place(buttonWidth("From file...", look::outline, 36, Icon::File), px(36));
        if (button("From file...", look::outline, 36, Icon::File)) {
            close = true;
            chooseFile();
        }
        foot.gap(8);
        foot.place(px(36), px(36));
        ImGui::BeginDisabled(scanning_);
        if (button("##rescan", look::outlineMuted, 36, Icon::Reload)) rescan = true;
        ImGui::EndDisabled();
        ImGui::SetItemTooltip(scanning_ ? "Scanning..." : "Rescan plugins");
        foot.gap(12);
        const std::string count = scanning_ ? std::string("Scanning...")
                                            : std::to_string(shown.size()) + " of " + std::to_string(scanned_.size()) + " instruments";
        drawText(foot.next(textWidth(fonts.sans, 12, count), px(12)), fonts.sans, 12, col::text3, count);
        foot.placeRight(buttonWidth("Add", look::primary, 36), px(36));
        ImGui::BeginDisabled(!picked);
        if (button("Add", look::primary, 36)) add = picked;
        ImGui::EndDisabled();
        foot.gapRight(8);
        foot.placeRight(buttonWidth("Cancel", look::ghost, 36), px(36));
        if (button("Cancel", look::ghost, 36)) close = true;
        foot.end();

        if (add) {
            PluginConfig pc;
            pc.path = add->path;
            pc.pluginId = add->id;
            addPlugin(pc);
            close = true;
        }
        if (close) ImGui::CloseCurrentPopup();
    }

    // ---- log ----

    void logView() {
        Row title = viewTitle("Log", "The latest 1000 lines. Follows new lines while scrolled to the bottom.");
        const bool copied = ImGui::GetTime() - copiedAt_ < 1.5;
        const char* copy = copied ? "Copied###copy" : "Copy all###copy";
        const Icon icon = copied ? Icon::Check : Icon::Copy;
        const ImVec2 bp = title.nextRight(buttonWidth(copy, look::outline, 36, icon), title.height());
        ImGui::SetCursorScreenPos(ImVec2(bp.x, bp.y + title.height() - px(36)));
        if (button(copy, look::outline, 36, icon)) {
            std::string all;
            {
                std::lock_guard lock(logMutex_);
                for (auto& l : log_) all += std::string(levelLook(l.level).tag) + ' ' + l.text + '\n';
            }
            ImGui::SetClipboardText(all.c_str());
            copiedAt_ = ImGui::GetTime();
        }
        title.end(16);

        ImGui::PushStyleColor(ImGuiCol_ChildBg, col::header);
        ImGui::PushStyleColor(ImGuiCol_Border, col::border);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, px(10));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, px(8)));
        ImGui::BeginChild("log", ImVec2(0, std::max(px(120), ImGui::GetContentRegionAvail().y)),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float w = ImGui::GetContentRegionAvail().x, textX = px(14 + 40 + 12), wrap = w - textX - px(14);
        {
            std::lock_guard lock(logMutex_);
            for (auto& l : log_) {
                const LevelLook lk = levelLook(l.level);
                const float h = fonts.mono->CalcTextSizeA(px(12), FLT_MAX, wrap, l.text.c_str()).y + px(6);
                const ImVec2 p = ImGui::GetCursorScreenPos();
                if (ImGui::IsRectVisible(p, ImVec2(p.x + w, p.y + h))) {
                    if (lk.row) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), lk.row);
                    drawText(ImVec2(p.x + px(14), p.y + px(3)), fonts.mono, 12, lk.tagColor, lk.tag);
                    dl->AddText(fonts.mono, px(12), ImVec2(p.x + textX, p.y + px(3)), lk.textColor, l.text.c_str(), nullptr, wrap);
                }
                ImGui::Dummy(ImVec2(w, h));
            }
        }
        ImGui::PopStyleVar();
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }

    struct LevelLook {
        const char* tag;
        ImU32 tagColor, textColor, row;
    };
    static LevelLook levelLook(LogLevel l) {
        switch (l) {
            case LogLevel::Error: return {"ERROR", col::error, col::error, col::logErrorRow};
            case LogLevel::Warning: return {"WARN", col::logWarnTag, col::warn, col::logWarnRow};
            case LogLevel::Info: return {"INFO", col::logInfoTag, col::text2, 0};
            case LogLevel::Debug: break;
        }
        return {"DEBUG", col::logInfoTag, col::text3, 0};
    }

    Engine engine_;
    EngineSnapshot snap_;
    EngineConfig editCfg_;
    std::string sessionPath_, lastError_;
    const std::filesystem::path configDir_;
    // Set by the worker when the saved rack cannot be restored: settings saved from then on keep
    // that rack and its session file as they were loaded, so it is not lost.
    std::atomic<bool> keepSavedRack_{false};
    std::vector<AudioDeviceInfo> devices_;
    std::vector<std::string> midiInputs_;
    bool virtualAvailable_ = false, virtualRemovalHangs_ = false;
    std::string virtualReason_;
    View view_ = View::Rack;

    std::mutex scanMutex_;
    std::vector<PluginDescription> scanned_;
    std::atomic<bool> scanning_{false};
    std::thread scanThread_;
    std::string extraDirs_, newDir_;
    std::string renaming_, renameBuf_;  // plugin being renamed (id), and its edit buffer
    bool renameFocus_ = false;

    // The "Add instrument" dialog
    bool openAdd_ = false;
    std::string addQuery_;
    int addFormat_ = 0;    // "All", then one format
    std::string addPick_;  // the instrument picked: its path and id

    // The "add MIDI input" kinds in segment order; newKind_ indexes this.
    static constexpr MidiSourceKind kNewKinds[] = {MidiSourceKind::Hardware, MidiSourceKind::Virtual, MidiSourceKind::Api};
    static int kindIndex(MidiSourceKind k) { return int(std::find(std::begin(kNewKinds), std::end(kNewKinds), k) - std::begin(kNewKinds)); }
    int newKind_ = 0, newInput_ = 0;
    std::string newVirtualName_ = "Brack", newApiId_ = "api";
    std::string gainKey_;  // route gain fader being dragged ("plugin/port/channel")
    float gainDb_ = 0;
    double gainEditTime_ = 0;
    std::map<std::string, std::pair<uint64_t, double>> activity_;
    double copiedAt_ = -10;  // when the log was copied

    std::mutex logMutex_;
    std::deque<LogLine> log_;

    std::thread worker_;
    std::mutex workMutex_;
    std::condition_variable workCv_;
    std::deque<std::pair<std::string, std::function<void()>>> work_;
    bool workStop_ = false;
    std::string busyLabel_;
    std::future<std::string> dialog_;  // a file dialog open (UI thread)
    std::function<void(const std::string&)> chosen_;
    std::atomic<int> pending_{0};
    std::mutex uiMutex_;

    // autosave() (UI thread)
    uint64_t seenEngineChanges_ = 0;
    std::string seenGuiState_;
    std::chrono::steady_clock::time_point lastChange_{};
    std::chrono::steady_clock::time_point firstUnsaved_{};  // the first change since the last save
    bool unsaved_ = false;
    bool autosaving_ = false;
    std::vector<std::function<void()>> uiTasks_;
};

// True when enough of the rectangle lies on some monitor's work area to grab
// the title bar (monitors may have been removed since the settings were saved).
bool isOnScreen(int x, int y, int w, int h) {
    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    for (int i = 0; i < count; ++i) {
        int mx, my, mw, mh;
        glfwGetMonitorWorkarea(monitors[i], &mx, &my, &mw, &mh);
        int ix = std::min(x + w, mx + mw) - std::max(x, mx);
        int iy = std::min(y + h, my + mh) - std::max(y, my);
        if (ix >= 100 && iy >= 50 && y >= my - 10) return true;
    }
    return false;
}

#if !defined(_WIN32) && !defined(__APPLE__)
// X11 takes the window's icon as pixels. On Windows GLFW loads brack.exe's GLFW_ICON resource,
// and macOS shows the bundle's icon.
#include "icon/window_icon.inc"
#endif

}  // namespace

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
namespace {
void showStartupError(const std::string& message) {
    MessageBoxW(nullptr, widen(message).c_str(), L"Brack", MB_ICONERROR);
}
}  // namespace
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(narrow(wargv[i]));
    LocalFree(wargv);
#else
namespace {
void showStartupError(const std::string& message) { std::fprintf(stderr, "brack: %s\n", message.c_str()); }
}  // namespace
int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
#endif
    // brack [--config FOLDER] [SESSION.json]
    std::optional<std::string> configArg;
    std::string sessionArg;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--config") {
            if (i + 1 == args.size() || args[i + 1].empty()) {
                showStartupError("--config needs a folder");
                return 1;
            }
            configArg = args[++i];  // the last one wins
        } else if (sessionArg.empty()) {
            sessionArg = args[i];
        }
    }
    // A folder that cannot be used stops the start rather than falling back to the usual one,
    // whose rack this run would then save over.
    std::filesystem::path configDir = configDirectory();
    if (configArg) {
        std::error_code ec;
        configDir = std::filesystem::absolute(pathFromUtf8(*configArg), ec);
        if (ec) {
            showStartupError("--config " + *configArg + ": " + ec.message());
            return 1;
        }
    }

    GuiSettings settings;
    std::string settingsError;
    loadGuiSettings(configDir, settings, settingsError);  // reported in the log once the app is up

    // Before the window there is no log to show, and a Windows GUI program has no console.
    auto cannotStart = [](const char* what) {
        const char* why = nullptr;
        glfwGetError(&why);
        showStartupError(std::string(what) + (why ? std::string(": ") + why : std::string()));
        return 1;
    };
    if (!glfwInit()) return cannotStart("cannot start GLFW");
#ifdef __APPLE__
    // macOS has OpenGL 3.2 and later only as the core profile.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    const char* glsl = "#version 150";
#else
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    const char* glsl = "#version 130";
#endif
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);  // window size in logical pixels on high-DPI screens
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);          // shown after the saved placement is applied
    GLFWwindow* window = glfwCreateWindow(1200, 760, "Brack", nullptr, nullptr);
    if (!window) {
        cannotStart("cannot open an OpenGL 3 window");
        glfwTerminate();
        return 1;
    }
#if !defined(_WIN32) && !defined(__APPLE__)
    glfwSetWindowIcon(window, static_cast<int>(std::size(kWindowIcons)), kWindowIcons);
#endif
    float sx = 1;
#ifndef __APPLE__  // macOS sizes windows in points, and Dear ImGui draws at the backing scale itself
    glfwGetWindowContentScale(window, &sx, nullptr);
#endif
    // The layout is fixed and does not wrap: narrower, the transport and the rack would clip.
    glfwSetWindowSizeLimits(window, (int)std::lround(880 * sx), GLFW_DONT_CARE, GLFW_DONT_CARE, GLFW_DONT_CARE);
    if (settings.hasWindow) {
        glfwSetWindowSize(window, settings.width, settings.height);
        if (isOnScreen(settings.x, settings.y, settings.width, settings.height))
            glfwSetWindowPos(window, settings.x, settings.y);
    }
    glfwShowWindow(window);
    setFileDialogOwner(window);
    if (settings.hasWindow && settings.maximized) glfwMaximizeWindow(window);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    setupStyle(sx);
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl);

    {
        App app(settings, configDir);
        if (!settingsError.empty()) logWarn("settings: " + settingsError);
        if (!sessionArg.empty()) app.openSession(sessionArg);
        else if (!settings.session.empty()) app.restoreSession(settings.session, settings.sessionPath);
        std::string shownTitle;
        // A close request waits for queued engine work (a plugin still loading, say), and for a
        // file dialog to be closed, drawing meanwhile, so the settings saved below include it.
        while (!glfwWindowShouldClose(window) || app.busy() || app.fileDialogOpen()) {
            glfwWaitEventsTimeout(1.0 / 30.0);  // redraw at least 30x/s for meters and activity
            // Remember the normal (not maximised / minimised) placement for next time.
            if (!glfwGetWindowAttrib(window, GLFW_MAXIMIZED) && !glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
                glfwGetWindowPos(window, &settings.x, &settings.y);
                glfwGetWindowSize(window, &settings.width, &settings.height);
                settings.hasWindow = settings.width > 0 && settings.height > 0;
            }
            settings.maximized = glfwGetWindowAttrib(window, GLFW_MAXIMIZED) != 0;
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            app.frame();
            if (!glfwWindowShouldClose(window)) app.autosave(settings);  // closing saves anyway, below
            ImGui::Render();
            int w, h;
            glfwGetFramebufferSize(window, &w, &h);
            glViewport(0, 0, w, h);
            const ImVec4 bg = ImGui::ColorConvertU32ToFloat4(col::bg);
            glClearColor(bg.x, bg.y, bg.z, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(window);
            if (app.title() != shownTitle) {
                shownTitle = app.title();
                glfwSetWindowTitle(window, shownTitle.c_str());
            }
        }
        settings.maximized = glfwGetWindowAttrib(window, GLFW_MAXIMIZED) != 0;
        app.storeSettings(settings);  // while plugins are still alive, so their state is included
        std::string err;
        if (!saveGuiSettings(configDir, settings, err)) logError("settings: " + err);
    }  // engine shuts down cleanly here (virtual ports, plugins)

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
