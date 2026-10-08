#pragma once
#include <clap/clap.h>

#include <memory>
#include <string>
#include <vector>

#include "plugin/plugin.h"

namespace brack {

// A loaded .clap binary. Shared by every instance created from it; the module
// is deinitialised and unloaded when the last reference goes away.
//
// Every call into it is guarded: a crash in clap_entry.init, get_factory or listing its
// plugins marks the module broken for good (markModuleBroken()).
class ClapModule : public std::enable_shared_from_this<ClapModule> {
public:
    static std::shared_ptr<ClapModule> load(const std::string& pathUtf8, std::string& error);
    ~ClapModule();

    const std::string& path() const { return path_; }
    const clap_plugin_factory_t* factory() const { return factory_; }
    // The plugins in the file. False (with `error`) if the module crashed listing them.
    bool describe(std::vector<PluginDescription>& out, std::string& error) const;

private:
    ClapModule() = default;
    std::string path_;
    void* library_ = nullptr;
    const clap_plugin_entry_t* entry_ = nullptr;
    const clap_plugin_factory_t* factory_ = nullptr;
};

}  // namespace brack
