// Binary headers read from disk, on any OS: PE (Windows), ELF (Linux), Mach-O (macOS, thin or
// universal). Nothing is loaded, so none of the plugin's code runs.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "plugin/library.h"
#include "util/common.h"

namespace brack {

namespace {

enum class Format { Pe, Elf, MachO };

#if defined(_WIN32)
constexpr Format kOwnFormat = Format::Pe;
#elif defined(__APPLE__)
constexpr Format kOwnFormat = Format::MachO;
#else
constexpr Format kOwnFormat = Format::Elf;
#endif

// Bounds-checked reads at file offsets; a read past the end yields 0 and sets `ok` false.
class Reader {
public:
    explicit Reader(const std::filesystem::path& path) : f_(path, std::ios::binary) {
        if (f_) {
            f_.seekg(0, std::ios::end);
            size_ = (uint64_t)f_.tellg();
        }
    }
    bool open() const { return (bool)f_; }
    uint64_t size() const { return size_; }
    bool ok = true;

    bool bytes(uint64_t pos, void* dst, size_t n) {
        if (pos > size_ || n > size_ - pos) {
            ok = false;
            std::memset(dst, 0, n);
            return false;
        }
        f_.seekg((std::streamoff)pos);
        if (!f_.read(static_cast<char*>(dst), (std::streamsize)n)) {
            f_.clear();
            ok = false;
            return false;
        }
        return true;
    }
    uint64_t le(uint64_t pos, int n) {
        uint8_t b[8] = {};
        bytes(pos, b, (size_t)n);
        uint64_t v = 0;
        for (int i = n - 1; i >= 0; --i) v = v << 8 | b[i];
        return v;
    }
    uint64_t be(uint64_t pos, int n) {
        uint8_t b[8] = {};
        bytes(pos, b, (size_t)n);
        uint64_t v = 0;
        for (int i = 0; i < n; ++i) v = v << 8 | b[i];
        return v;
    }
    // A NUL-terminated string of at most `max` bytes at `pos`.
    std::string cstr(uint64_t pos, size_t max = 256) {
        if (pos >= size_) return {};
        std::string s((size_t)std::min<uint64_t>(max, size_ - pos), '\0');
        if (!bytes(pos, s.data(), s.size())) return {};
        s.resize(strnlen(s.data(), s.size()));
        return s;
    }

private:
    std::ifstream f_;
    uint64_t size_ = 0;
};

std::string unknownMachine(const char* format, uint64_t machine) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%s machine 0x%llx", format, (unsigned long long)machine);
    return buf;
}

// ---- PE ----

std::string peArchitecture(uint64_t machine) {
    switch (machine) {
        case 0x8664: return "x64";  // also ARM64EC, which an x64 process loads
        case 0x014C: return "x86";
        case 0xAA64: return "arm64";  // also ARM64X
        default: return unknownMachine("PE", machine);
    }
}

bool readPe(Reader& r, BinaryInfo& out, std::string& error) {
    const uint64_t nt = r.le(0x3C, 4);
    if (r.le(nt, 4) != 0x00004550) {  // "PE\0\0"
        error = "not a Windows binary";
        return false;
    }
    const uint64_t fh = nt + 4;
    out.architectures = {peArchitecture(r.le(fh, 2))};
    const uint64_t sections = r.le(fh + 2, 2), optSize = r.le(fh + 16, 2), opt = fh + 20;
    const uint64_t magic = r.le(opt, 2);
    if (magic != 0x10B && magic != 0x20B) return true;  // no optional header to find exports in
    const uint64_t dirs = opt + (magic == 0x20B ? 112 : 96);
    if (r.le(opt + (magic == 0x20B ? 108 : 92), 4) == 0 || optSize < dirs - opt + 8) return true;
    const uint64_t exportRva = r.le(dirs, 4);
    if (!exportRva) return true;

    const uint64_t table = opt + optSize;
    auto offsetOf = [&](uint64_t rva, uint64_t& off) {
        for (uint64_t i = 0; i < sections; ++i) {
            const uint64_t s = table + i * 40;
            const uint64_t va = r.le(s + 12, 4), size = std::max(r.le(s + 8, 4), r.le(s + 16, 4));
            if (rva >= va && rva < va + size) {
                off = r.le(s + 20, 4) + (rva - va);
                return true;
            }
        }
        return false;
    };
    uint64_t ed = 0, names = 0;
    if (!offsetOf(exportRva, ed) || !offsetOf(r.le(ed + 32, 4), names)) return true;
    const uint64_t count = std::min<uint64_t>(r.le(ed + 24, 4), 1 << 16);
    for (uint64_t i = 0; i < count && r.ok; ++i) {
        uint64_t name = 0;
        if (offsetOf(r.le(names + i * 4, 4), name)) out.exports.push_back(r.cstr(name));
    }
    return true;
}

// ---- ELF ----

std::string elfArchitecture(uint64_t machine) {
    switch (machine) {
        case 62: return "x64";     // EM_X86_64
        case 3: return "x86";      // EM_386
        case 183: return "arm64";  // EM_AARCH64
        default: return unknownMachine("ELF", machine);
    }
}

bool readElf(Reader& r, BinaryInfo& out, std::string& error) {
    uint8_t ident[16];
    r.bytes(0, ident, sizeof ident);
    const bool is64 = ident[4] == 2;
    if ((ident[4] != 1 && !is64) || ident[5] != 1) {
        error = "not a little-endian ELF binary";
        return false;
    }
    out.architectures = {elfArchitecture(r.le(18, 2))};

    // Exports: defined global (or weak) symbols of default or protected visibility in .dynsym.
    const uint64_t shoff = is64 ? r.le(40, 8) : r.le(32, 4);
    const uint64_t shentsize = r.le(is64 ? 58 : 46, 2), shnum = std::min<uint64_t>(r.le(is64 ? 60 : 48, 2), 1 << 12);
    auto section = [&](uint64_t i, uint64_t& type, uint64_t& offset, uint64_t& size, uint64_t& link, uint64_t& entsize) {
        const uint64_t s = shoff + i * shentsize;
        type = r.le(s + 4, 4);
        offset = is64 ? r.le(s + 24, 8) : r.le(s + 16, 4);
        size = is64 ? r.le(s + 32, 8) : r.le(s + 20, 4);
        link = is64 ? r.le(s + 40, 4) : r.le(s + 24, 4);
        entsize = is64 ? r.le(s + 56, 8) : r.le(s + 36, 4);
    };
    for (uint64_t i = 0; i < shnum && shoff && r.ok; ++i) {
        uint64_t type, offset, size, link, entsize;
        section(i, type, offset, size, link, entsize);
        if (type != 11 || !entsize) continue;  // SHT_DYNSYM
        uint64_t strType, strOffset, strSize, strLink, strEntsize;
        section(link, strType, strOffset, strSize, strLink, strEntsize);
        const uint64_t count = std::min<uint64_t>(size / entsize, 1 << 16);
        for (uint64_t k = 1; k < count && r.ok; ++k) {
            const uint64_t sym = offset + k * entsize;
            const uint64_t name = r.le(sym, 4);
            const uint64_t info = r.le(sym + (is64 ? 4 : 12), 1), other = r.le(sym + (is64 ? 5 : 13), 1);
            const uint64_t shndx = r.le(sym + (is64 ? 6 : 14), 2);
            const uint64_t bind = info >> 4, visibility = other & 3;
            if (shndx == 0 || (bind != 1 && bind != 2) || (visibility != 0 && visibility != 3)) continue;
            if (name < strSize) out.exports.push_back(r.cstr(strOffset + name));
        }
    }
    return true;
}

// ---- Mach-O ----

std::string machOArchitecture(uint64_t cputype) {
    switch (cputype) {
        case 0x01000007: return "x64";    // CPU_TYPE_X86_64
        case 0x00000007: return "x86";    // CPU_TYPE_X86
        case 0x0100000C: return "arm64";  // CPU_TYPE_ARM64
        default: return unknownMachine("Mach-O", cputype);
    }
}

// Exports in the trie dyld looks symbols up in (`trie`, from LC_DYLD_INFO or LC_DYLD_EXPORTS_TRIE):
// each node has its terminal info (size first; 0 when no symbol ends there), then its children,
// each an edge label and the child node's offset.
void readExportTrie(const std::vector<uint8_t>& trie, BinaryInfo& out) {
    auto uleb = [&](size_t& pos) {
        uint64_t v = 0;
        for (int shift = 0; pos < trie.size() && shift < 64; shift += 7) {
            const uint8_t b = trie[pos++];
            v |= (uint64_t)(b & 0x7F) << shift;
            if (!(b & 0x80)) break;
        }
        return v;
    };
    std::vector<std::pair<size_t, std::string>> pending{{0, ""}};
    for (size_t visited = 0; !pending.empty() && visited < (1 << 16); ++visited) {
        auto [pos, name] = std::move(pending.back());
        pending.pop_back();
        const uint64_t terminal = uleb(pos);
        if (terminal && !name.empty()) out.exports.push_back(name[0] == '_' ? name.substr(1) : name);
        if (terminal > trie.size() - pos) continue;
        pos += (size_t)terminal;
        if (pos >= trie.size()) continue;
        for (int children = trie[pos++]; children > 0 && pos < trie.size(); --children) {
            const size_t label = pos;
            while (pos < trie.size() && trie[pos]) ++pos;
            std::string childName = name + std::string(trie.begin() + (std::ptrdiff_t)label, trie.begin() + (std::ptrdiff_t)pos);
            ++pos;
            const uint64_t child = uleb(pos);
            if (child && child < trie.size()) pending.emplace_back((size_t)child, std::move(childName));
        }
    }
}

// The exports of the thin Mach-O image at `base`, without the leading underscore C names get:
// from its export trie, which stripping leaves, else (binaries for macOS before 10.6) external
// symbols defined in a section of its symbol table.
void readMachOExports(Reader& r, uint64_t base, BinaryInfo& out) {
    const uint64_t magic = r.le(base, 4);
    const bool is64 = magic == 0xFEEDFACF;
    const uint64_t ncmds = std::min<uint64_t>(r.le(base + 16, 4), 1 << 12);
    uint64_t cmd = base + (is64 ? 32 : 28);
    uint64_t symtab = 0, trieOffset = 0, trieSize = 0;
    for (uint64_t i = 0; i < ncmds && r.ok; ++i) {
        const uint64_t type = r.le(cmd, 4), size = r.le(cmd + 4, 4);
        if (type == 0x2) symtab = cmd;                     // LC_SYMTAB
        if (type == 0x22 || type == 0x80000022) {          // LC_DYLD_INFO(_ONLY)
            trieOffset = r.le(cmd + 40, 4);
            trieSize = r.le(cmd + 44, 4);
        } else if (type == 0x80000033) {                   // LC_DYLD_EXPORTS_TRIE
            trieOffset = r.le(cmd + 8, 4);
            trieSize = r.le(cmd + 12, 4);
        }
        if (size < 8) break;
        cmd += size;
    }
    if (trieSize) {
        std::vector<uint8_t> trie((size_t)std::min<uint64_t>(trieSize, 1 << 24));
        if (r.bytes(base + trieOffset, trie.data(), trie.size())) {
            readExportTrie(trie, out);
            return;
        }
    }
    if (!symtab) return;
    const uint64_t symoff = base + r.le(symtab + 8, 4), nsyms = std::min<uint64_t>(r.le(symtab + 12, 4), 1 << 20);
    const uint64_t stroff = base + r.le(symtab + 16, 4), strsize = r.le(symtab + 20, 4);
    const uint64_t entry = is64 ? 16 : 12;
    for (uint64_t k = 0; k < nsyms && r.ok; ++k) {
        const uint64_t sym = symoff + k * entry;
        const uint64_t strx = r.le(sym, 4), ntype = r.le(sym + 4, 1);
        if ((ntype & 0xE0) || !(ntype & 0x01) || (ntype & 0x0E) != 0x0E || strx >= strsize) continue;
        std::string name = r.cstr(stroff + strx);
        if (!name.empty() && name[0] == '_') name.erase(0, 1);
        out.exports.push_back(std::move(name));
    }
}

bool readMachO(Reader& r, BinaryInfo& out, std::string& error) {
    const uint64_t magic = r.be(0, 4);
    if (magic == 0xCAFEBABE || magic == 0xCAFEBABF) {  // universal: one image per architecture
        const bool fat64 = magic == 0xCAFEBABF;
        const uint64_t n = std::min<uint64_t>(r.be(4, 4), 64);
        uint64_t exportsFrom = 0;
        for (uint64_t i = 0; i < n && r.ok; ++i) {
            const uint64_t a = 8 + i * (fat64 ? 32 : 20);
            const std::string arch = machOArchitecture(r.be(a, 4));
            const uint64_t offset = fat64 ? r.be(a + 8, 8) : r.be(a + 8, 4);
            out.architectures.push_back(arch);
            if (!exportsFrom || arch == buildArchitecture()) exportsFrom = offset;
        }
        if (out.architectures.empty()) {
            error = "an empty universal binary";
            return false;
        }
        readMachOExports(r, exportsFrom, out);
        return true;
    }
    out.architectures = {machOArchitecture(r.le(4, 4))};
    readMachOExports(r, 0, out);
    (void)error;
    return true;
}

}  // namespace

std::string BinaryInfo::architecture() const {
    if (std::find(architectures.begin(), architectures.end(), buildArchitecture()) != architectures.end())
        return buildArchitecture();
    return architectures.empty() ? std::string() : architectures.front();
}

bool BinaryInfo::hasExport(const char* name) const {
    return std::find(exports.begin(), exports.end(), name) != exports.end();
}

bool inspectBinary(const std::filesystem::path& path, BinaryInfo& out, std::string& error) {
    out = {};
    Reader r(path);
    if (!r.open()) {
        error = "cannot open file";
        return false;
    }
    Format format;
    const uint64_t le32 = r.le(0, 4), be32 = r.be(0, 4);
    if ((le32 & 0xFFFF) == 0x5A4D) format = Format::Pe;  // "MZ"
    else if (be32 == 0x7F454C46) format = Format::Elf;   // "\x7F" "ELF"
    else if (le32 == 0xFEEDFACF || le32 == 0xFEEDFACE || be32 == 0xCAFEBABE || be32 == 0xCAFEBABF) format = Format::MachO;
    else {
        error = "not a binary Brack knows (PE, ELF or Mach-O)";
        return false;
    }
    const bool read = format == Format::Pe ? readPe(r, out, error) : format == Format::Elf ? readElf(r, out, error)
                                                                                         : readMachO(r, out, error);
    if (!read) return false;
    out.loadable = format == kOwnFormat && out.architecture() == buildArchitecture();
    return true;
}

}  // namespace brack
