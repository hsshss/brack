#include "remote/host_protocol.h"

#include "plugin/plugin_files.h"
#include "util/common.h"

namespace brack::remote {

std::vector<uint8_t> encodeMessage(const Message& m) {
    std::vector<uint8_t> body = Message::to_cbor(m);
    const uint32_t n = (uint32_t)body.size();
    std::vector<uint8_t> frame{(uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), (uint8_t)(n >> 24)};
    frame.insert(frame.end(), body.begin(), body.end());
    return frame;
}

bool decodeMessage(const std::vector<uint8_t>& payload, Message& out) {
    out = Message::from_cbor(payload, true, false);
    return !out.is_discarded() && out.is_object();
}

Message toMessage(const PluginDescription& d) {
    return {{"path", d.path},       {"id", d.id},
            {"name", d.name},       {"vendor", d.vendor},
            {"version", d.version}, {"description", d.description},
            {"features", d.features}, {"instrument", d.instrument},
            {"architecture", d.architecture}};
}

PluginDescription descriptionFromMessage(const Message& m) {
    PluginDescription d;
    d.path = m.value("path", "");
    pluginFormatFromPath(pathFromUtf8(d.path), d.format);
    d.id = m.value("id", "");
    d.name = m.value("name", "");
    d.vendor = m.value("vendor", "");
    d.version = m.value("version", "");
    d.description = m.value("description", "");
    d.features = m.value("features", std::vector<std::string>{});
    d.instrument = m.value("instrument", false);
    d.architecture = m.value("architecture", "");
    return d;
}

Message portsToMessage(const std::vector<NotePortInfo>& notes, const std::vector<AudioPortInfo>& ins,
                       const std::vector<AudioPortInfo>& outs) {
    auto audio = [](const std::vector<AudioPortInfo>& ports) {
        Message a = Message::array();
        for (auto& p : ports)
            a.push_back({{"id", p.id}, {"name", p.name}, {"channels", p.channels}, {"main", p.isMain}});
        return a;
    };
    Message n = Message::array();
    for (auto& p : notes) n.push_back({{"id", p.id}, {"name", p.name}, {"dialects", p.dialects}});
    return {{"notes", n}, {"inputs", audio(ins)}, {"outputs", audio(outs)}};
}

void portsFromMessage(const Message& m, std::vector<NotePortInfo>& notes, std::vector<AudioPortInfo>& ins,
                      std::vector<AudioPortInfo>& outs) {
    auto audio = [](const Message& a, std::vector<AudioPortInfo>& ports) {
        ports.clear();
        for (auto& p : a)
            ports.push_back({p.value("id", 0u), p.value("name", ""), p.value("channels", 0u), p.value("main", false)});
    };
    notes.clear();
    for (auto& p : m.value("notes", Message::array()))
        notes.push_back({p.value("id", 0u), p.value("name", ""), p.value("dialects", 0u)});
    audio(m.value("inputs", Message::array()), ins);
    audio(m.value("outputs", Message::array()), outs);
}

}  // namespace brack::remote
