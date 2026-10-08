#include "clap/clap_module.h"

#include <map>
#include <mutex>

#include "plugin/library.h"
#include "plugin/plugin_files.h"
#include "util/common.h"

namespace brack {

namespace {
std::mutex g_moduleMutex;
std::map<std::string, std::weak_ptr<ClapModule>> g_modules;
}  // namespace

std::shared_ptr<ClapModule> ClapModule::load(const std::string& pathUtf8, std::string& error) {
    std::lock_guard lock(g_moduleMutex);
    std::string key = pathToUtf8(std::filesystem::absolute(pathFromUtf8(pathUtf8)).lexically_normal());
    if (moduleBroken(key, error)) return nullptr;
    if (auto it = g_modules.find(key); it != g_modules.end())
        if (auto m = it->second.lock()) return m;

    std::shared_ptr<ClapModule> m(new ClapModule());
    m->path_ = key;
    m->library_ = openLibrary(pathToUtf8(pluginBinaryPath(PluginFormat::Clap, pathFromUtf8(key))), error);
    if (!m->library_) return nullptr;
    m->entry_ = static_cast<const clap_plugin_entry_t*>(librarySymbol(m->library_, "clap_entry"));
    if (!m->entry_) {
        error = "not a CLAP plugin (no clap_entry)";
        return nullptr;
    }
    if (!clap_version_is_compatible(m->entry_->clap_version)) {
        error = "incompatible CLAP version";
        m->entry_ = nullptr;
        return nullptr;
    }
    // Guarded call by call, so that the lock above is always released normally.
    GuardFault fault;
    bool initialised = false;
    if (!guardedCall([&] { initialised = m->entry_->init(key.c_str()); }, fault)) {
        error = faultReport("clap_entry.init", fault);
        markModuleBroken(key, error, m);
        return nullptr;
    }
    if (!initialised) {
        error = "clap_entry.init failed";
        m->entry_ = nullptr;
        return nullptr;
    }
    if (!guardedCall([&] {
            m->factory_ = static_cast<const clap_plugin_factory_t*>(m->entry_->get_factory(CLAP_PLUGIN_FACTORY_ID));
        }, fault)) {
        error = faultReport("clap_entry.get_factory", fault);
        markModuleBroken(key, error, m);
        return nullptr;
    }
    if (!m->factory_) {
        error = "plugin has no plugin factory";
        return nullptr;
    }
    g_modules[key] = m;
    return m;
}

ClapModule::~ClapModule() {
    if (entry_) {
        GuardFault fault;
        if (!guardedCall([&] { entry_->deinit(); }, fault)) {
            // Too late to keep this object, but the library stays loaded and is not used again.
            const std::string report = faultReport("clap_entry.deinit", fault);
            logWarn(path_ + ": " + report + "; it stays loaded");
            markModuleBroken(path_, report, nullptr);
            return;
        }
    }
    if (library_) closeLibrary(library_);
}

bool ClapModule::describe(std::vector<PluginDescription>& out, std::string& error) const {
    out.clear();
    if (!factory_) return true;
    GuardFault fault;
    std::vector<PluginDescription> result;
    const bool ok = guardedCall([&] {
        uint32_t n = factory_->get_plugin_count(factory_);
        for (uint32_t i = 0; i < n; ++i) {
            const clap_plugin_descriptor_t* d = factory_->get_plugin_descriptor(factory_, i);
            if (!d || !d->id) continue;
            PluginDescription p;
            p.format = PluginFormat::Clap;
            p.path = path_;
            p.id = d->id;
            p.name = d->name ? d->name : d->id;
            p.vendor = d->vendor ? d->vendor : "";
            p.version = d->version ? d->version : "";
            p.description = d->description ? d->description : "";
            if (d->features)
                for (const char* const* f = d->features; *f; ++f) p.features.emplace_back(*f);
            for (auto& f : p.features)
                if (f == CLAP_PLUGIN_FEATURE_INSTRUMENT) p.instrument = true;
            result.push_back(std::move(p));
        }
    }, fault);
    if (!ok) {
        error = faultReport("listing its plugins", fault);
        markModuleBroken(path_, error, std::const_pointer_cast<ClapModule>(shared_from_this()));
        return false;
    }
    out = std::move(result);
    return true;
}

}  // namespace brack
