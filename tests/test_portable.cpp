// The platform rules that are plain logic, tested here for every OS whatever this one is: where
// each plugin format keeps its binary, UTF-16 text (VST3's, on every platform), and how the GUI's
// list of plugins names a plugin of another architecture.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "plugin/library.h"
#include "plugin/plugin.h"
#include "plugin/plugin_files.h"
#include "util/common.h"

using namespace brack;
namespace fs = std::filesystem;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::fprintf(stderr, "CHECK failed: %s (line %d)\n", #cond, __LINE__); \
            ++g_failures;                                                          \
        }                                                                          \
    } while (0)

void touch(const fs::path& p) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << "x";
}

bool formatOf(const char* path, TargetOs os, PluginFormat expected) {
    PluginFormat f;
    return pluginFormatFromPath(path, f, os) && f == expected;
}

void formats() {
    CHECK(formatOf("a/Synth.clap", TargetOs::Windows, PluginFormat::Clap));
    CHECK(formatOf("a/Synth.VST3", TargetOs::Linux, PluginFormat::Vst3));
    CHECK(formatOf("a/Synth.dll", TargetOs::Windows, PluginFormat::Vst2));
    CHECK(formatOf("a/Synth.vst", TargetOs::MacOS, PluginFormat::Vst2));
    CHECK(formatOf("a/synth.so", TargetOs::Linux, PluginFormat::Vst2));
    PluginFormat f;
    CHECK(!pluginFormatFromPath("a/Synth.dll", f, TargetOs::Linux));
    CHECK(!pluginFormatFromPath("a/Synth.so", f, TargetOs::Windows));
    CHECK(!pluginFormatFromPath("a/Synth.vst", f, TargetOs::Windows));
    CHECK(pluginExtensionsText(TargetOs::Linux) == ".clap, .vst3 or .so");

    CHECK(canBeBundle(PluginFormat::Vst3, TargetOs::Windows) && canBeBundle(PluginFormat::Vst3, TargetOs::Linux));
    CHECK(!canBeBundle(PluginFormat::Clap, TargetOs::Windows) && !canBeBundle(PluginFormat::Vst2, TargetOs::Linux));
    CHECK(canBeBundle(PluginFormat::Clap, TargetOs::MacOS) && canBeBundle(PluginFormat::Vst2, TargetOs::MacOS));

    CHECK(vst3ArchFolders(TargetOs::Windows, "x64", "x64") == std::vector<std::string>{"x86_64-win"});
    CHECK(vst3ArchFolders(TargetOs::Windows, "x86", "x64") == std::vector<std::string>{"x86-win"});
    CHECK(vst3ArchFolders(TargetOs::Windows, "arm64", "arm64") == (std::vector<std::string>{"arm64x-win", "arm64-win"}));
    // An x64 build on an ARM64 PC loads ARM64EC and ARM64X code, natively, before x64 code.
    CHECK(vst3ArchFolders(TargetOs::Windows, "x64", "arm64") ==
          (std::vector<std::string>{"arm64ec-win", "arm64x-win", "x86_64-win"}));
    CHECK(vst3ArchFolders(TargetOs::Windows, "x86", "arm64") == std::vector<std::string>{"x86-win"});
    CHECK(vst3ArchFolders(TargetOs::Linux, "x64", "x64") == std::vector<std::string>{"x86_64-linux"});
    CHECK(vst3ArchFolders(TargetOs::Linux, "arm64", "arm64") == std::vector<std::string>{"aarch64-linux"});
    CHECK(vst3ArchFolders(TargetOs::Linux, "x64", "arm64") == std::vector<std::string>{"x86_64-linux"});
    CHECK(vst3ArchFolders(TargetOs::MacOS, "arm64", "arm64") == std::vector<std::string>{"MacOS"});
}

// Bundles laid out as each OS has them, in a temporary folder.
void binaries(const fs::path& dir) {
    const fs::path win = dir / "Win.vst3", lin = dir / "Lin.vst3", mac = dir / "Mac.vst3";
    touch(win / "Contents" / "x86-win" / "Win.vst3");
    touch(win / "Contents" / "x86_64-win" / "Win.vst3");
    touch(lin / "Contents" / "x86_64-linux" / "Lin.so");
    touch(lin / "Contents" / "aarch64-linux" / "Lin.so");
    touch(mac / "Contents" / "MacOS" / "Mac");

    CHECK(pluginBinaryPath(PluginFormat::Vst3, win, TargetOs::Windows, "x86") == win / "Contents" / "x86-win" / "Win.vst3");
    CHECK(pluginBinaryPath(PluginFormat::Vst3, lin, TargetOs::Linux, "arm64") ==
          lin / "Contents" / "aarch64-linux" / "Lin.so");
    CHECK(pluginBinaryPath(PluginFormat::Vst3, mac, TargetOs::MacOS, "arm64") == mac / "Contents" / "MacOS" / "Mac");
    // No binary for this architecture: where it would be.
    CHECK(pluginBinaryPath(PluginFormat::Vst3, lin, TargetOs::Linux, "x86") == lin / "Contents" / "i386-linux" / "Lin.so");
    // A plain file is its own binary.
    touch(dir / "Single.vst3");
    CHECK(pluginBinaryPath(PluginFormat::Vst3, dir / "Single.vst3", TargetOs::Windows, "x64") == dir / "Single.vst3");

    // macOS CLAP and VST2 bundles; elsewhere those are plain files.
    const fs::path clap = dir / "Synth.clap", vst = dir / "Synth.vst";
    touch(clap / "Contents" / "MacOS" / "Synth");
    touch(vst / "Contents" / "MacOS" / "Synth");
    CHECK(pluginBinaryPath(PluginFormat::Clap, clap, TargetOs::MacOS, "arm64") == clap / "Contents" / "MacOS" / "Synth");
    CHECK(pluginBinaryPath(PluginFormat::Vst2, vst, TargetOs::MacOS, "x64") == vst / "Contents" / "MacOS" / "Synth");
    CHECK(pluginBinaryPath(PluginFormat::Clap, clap, TargetOs::Linux, "x64") == clap);

    // The architecture a bundle runs as: ours if it has it, else another one it has.
    CHECK(vst3BundleArchitecture(win, TargetOs::Windows, "x86", "x64") == "x86");
    CHECK(vst3BundleArchitecture(win, TargetOs::Windows, "arm64", "arm64") == "x64");
    CHECK(vst3BundleArchitecture(lin, TargetOs::Linux, "arm64", "arm64") == "arm64");
    CHECK(vst3BundleArchitecture(lin, TargetOs::Linux, "x86", "x64") == "x64");
    CHECK(vst3BundleArchitecture(win, TargetOs::Linux, "x64", "x64").empty());  // Windows folders do not count on Linux
    CHECK(vst3BundleArchitecture(mac, TargetOs::MacOS, "arm64", "arm64") == "arm64");

    // ARM64EC and ARM64X binaries: an x64 build takes them on an ARM64 PC, where they run
    // natively in its x64 process, and cannot load them on an x64 PC. An arm64 build loads
    // ARM64X (it holds arm64 code too), and leaves ARM64EC to the x64 plugin host.
    const fs::path ec = dir / "Ec.vst3", hybrid = dir / "Hybrid.vst3";
    touch(ec / "Contents" / "arm64ec-win" / "Ec.vst3");
    touch(hybrid / "Contents" / "arm64x-win" / "Hybrid.vst3");
    touch(win / "Contents" / "arm64ec-win" / "Win.vst3");
    CHECK(pluginBinaryPath(PluginFormat::Vst3, win, TargetOs::Windows, "x64", "arm64") ==
          win / "Contents" / "arm64ec-win" / "Win.vst3");
    CHECK(pluginBinaryPath(PluginFormat::Vst3, win, TargetOs::Windows, "x64", "x64") ==
          win / "Contents" / "x86_64-win" / "Win.vst3");
    CHECK(pluginBinaryPath(PluginFormat::Vst3, hybrid, TargetOs::Windows, "x64", "arm64") ==
          hybrid / "Contents" / "arm64x-win" / "Hybrid.vst3");
    CHECK(vst3BundleArchitecture(ec, TargetOs::Windows, "arm64", "arm64") == "x64");
    CHECK(vst3BundleArchitecture(ec, TargetOs::Windows, "x64", "arm64") == "x64");
    CHECK(vst3BundleArchitecture(ec, TargetOs::Windows, "x64", "x64").empty());
    CHECK(vst3BundleArchitecture(hybrid, TargetOs::Windows, "arm64", "arm64") == "arm64");
    CHECK(vst3BundleArchitecture(hybrid, TargetOs::Windows, "x64", "arm64") == "x64");
    CHECK(vst3BundleArchitecture(hybrid, TargetOs::Windows, "x64", "x64") == "arm64");  // refused: it does not run there
    CHECK(vst3BundleArchitecture(win, TargetOs::Windows, "x64", "arm64") == "x64");
}

// ---- binaries of every OS and architecture, built byte by byte ----

// A file's bytes, written at offsets; the file grows to fit.
struct Bytes {
    std::vector<uint8_t> b;
    void put(size_t pos, uint64_t v, int n, bool bigEndian = false) {
        if (b.size() < pos + n) b.resize(pos + n);
        for (int i = 0; i < n; ++i) b[pos + i] = (uint8_t)(v >> (8 * (bigEndian ? n - 1 - i : i)));
    }
    void text(size_t pos, const std::string& s) {
        if (b.size() < pos + s.size()) b.resize(pos + s.size());
        for (size_t i = 0; i < s.size(); ++i) b[pos + i] = (uint8_t)s[i];
    }
    void append(size_t pos, const Bytes& other) {
        if (b.size() < pos + other.b.size()) b.resize(pos + other.b.size());
        std::copy(other.b.begin(), other.b.end(), b.begin() + pos);
    }
    fs::path save(const fs::path& path) const {
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), (std::streamsize)b.size());
        return path;
    }
};

BinaryInfo inspect(const fs::path& path) {
    BinaryInfo bi;
    std::string error;
    CHECK(inspectBinary(path, bi, error));
    return bi;
}

// A minimal PE header for `machine`, with no exports.
Bytes pe(uint16_t machine) {
    Bytes f;
    f.text(0, "MZ");
    f.put(0x3C, 0x40, 4);
    f.text(0x40, std::string("PE\0\0", 4));
    f.put(0x44, machine, 2);
    f.put(0x44 + 16, 0, 2);  // no optional header
    return f;
}

// An ELF shared library for `machine` whose .dynsym holds VSTPluginMain (exported), and an
// undefined, a local and a hidden symbol (not exports).
Bytes elf(bool is64, uint16_t machine) {
    Bytes f;
    f.text(0, "\x7F" "ELF");
    f.put(4, is64 ? 2 : 1, 1);
    f.put(5, 1, 1);  // little-endian
    f.put(18, machine, 2);
    const std::string names = std::string("\0VSTPluginMain\0undefined\0local\0hidden\0", 38);
    const size_t strtab = 0x100, symtab = 0x200, shoff = 0x400, symSize = is64 ? 24 : 16, shSize = is64 ? 64 : 40;
    f.text(strtab, names);
    struct Sym { uint32_t name; uint8_t info, other; uint16_t shndx; };
    const Sym syms[] = {{0, 0, 0, 0}, {1, 0x12, 0, 5}, {15, 0x12, 0, 0}, {25, 0x02, 0, 5}, {31, 0x12, 2, 5}};
    for (size_t i = 0; i < 5; ++i) {
        const size_t s = symtab + i * symSize;
        f.put(s, syms[i].name, 4);
        f.put(s + (is64 ? 4 : 12), syms[i].info, 1);
        f.put(s + (is64 ? 5 : 13), syms[i].other, 1);
        f.put(s + (is64 ? 6 : 14), syms[i].shndx, 2);
    }
    auto section = [&](size_t i, uint32_t type, uint64_t offset, uint64_t size, uint32_t link, uint64_t entsize) {
        const size_t s = shoff + i * shSize;
        f.put(s + 4, type, 4);
        f.put(s + (is64 ? 24 : 16), offset, is64 ? 8 : 4);
        f.put(s + (is64 ? 32 : 20), size, is64 ? 8 : 4);
        f.put(s + (is64 ? 40 : 24), link, 4);
        f.put(s + (is64 ? 56 : 36), entsize, is64 ? 8 : 4);
    };
    section(0, 0, 0, 0, 0, 0);
    section(1, 11, symtab, 5 * symSize, 2, symSize);  // SHT_DYNSYM, strings in section 2
    section(2, 3, strtab, names.size(), 0, 0);         // SHT_STRTAB
    f.put(is64 ? 40 : 32, shoff, is64 ? 8 : 4);
    f.put(is64 ? 58 : 46, shSize, 2);
    f.put(is64 ? 60 : 48, 3, 2);
    return f;
}

// A 64-bit Mach-O image for `cputype` whose symbol table holds _<exported> (exported), and an
// undefined and a local symbol (not exports).
Bytes machO(uint32_t cputype, const std::string& exported) {
    Bytes f;
    f.put(0, 0xFEEDFACF, 4);
    f.put(4, cputype, 4);
    f.put(16, 1, 4);  // one load command
    const std::string names = std::string("\0_", 2) + exported + std::string("\0_undef\0_local\0", 15);
    const size_t symoff = 0x80, stroff = 0x100;
    f.put(32, 0x2, 4);  // LC_SYMTAB
    f.put(36, 24, 4);
    f.put(40, symoff, 4);
    f.put(44, 3, 4);
    f.put(48, stroff, 4);
    f.put(52, names.size(), 4);
    const uint32_t strx[] = {1, (uint32_t)exported.size() + 3, (uint32_t)exported.size() + 10};
    const uint8_t type[] = {0x0F, 0x01, 0x0E};  // N_SECT | N_EXT, N_UNDF | N_EXT, N_SECT
    for (size_t i = 0; i < 3; ++i) {
        f.put(symoff + i * 16, strx[i], 4);
        f.put(symoff + i * 16 + 4, type[i], 1);
        f.put(symoff + i * 16 + 5, type[i] & 0x0E ? 1 : 0, 1);
    }
    f.text(stroff, names);
    return f;
}

// A stripped 64-bit arm64 Mach-O image: no symbol table, only the export trie dyld reads
// (LC_DYLD_EXPORTS_TRIE), holding _VSTPluginMain and, below it, _VSTPluginMain2.
Bytes strippedMachO() {
    Bytes f;
    f.put(0, 0xFEEDFACF, 4);
    f.put(4, 0x0100000C, 4);
    f.put(16, 1, 4);
    f.put(32, 0x80000033, 4);
    f.put(36, 16, 4);
    const size_t trie = 0x80;
    // Root (offset 0): no symbol ends here; one child, "_VSTPluginMain", at 18. That node: a symbol
    // (terminal info: flags, address); one child, "2", at 25, also a symbol, with no children.
    Bytes t;
    t.put(0, 0, 1);
    t.put(1, 1, 1);
    t.text(2, std::string("_VSTPluginMain\0", 15));
    t.b.insert(t.b.end(), {18, 2, 0, 0x10, 1, '2', 0, 25, 2, 0, 0x20, 0});
    f.put(40, trie, 4);
    f.put(44, t.b.size(), 4);
    f.append(trie, t);
    return f;
}

void binaryHeaders(const fs::path& dir) {
    fs::create_directories(dir);
    // PE: the three Windows architectures.
    CHECK(inspect(pe(0x8664).save(dir / "x64.dll")).architectures == std::vector<std::string>{"x64"});
    CHECK(inspect(pe(0x014C).save(dir / "x86.dll")).architectures == std::vector<std::string>{"x86"});
    CHECK(inspect(pe(0xAA64).save(dir / "arm64.dll")).architectures == std::vector<std::string>{"arm64"});

    // ELF: the three Linux architectures, 32 and 64-bit; only real exports count.
    struct { bool is64; uint16_t machine; const char* arch; } elfs[] = {{true, 62, "x64"}, {false, 3, "x86"}, {true, 183, "arm64"}};
    for (auto& e : elfs) {
        const BinaryInfo bi = inspect(elf(e.is64, e.machine).save(dir / (std::string(e.arch) + ".so")));
        CHECK(bi.architectures == std::vector<std::string>{e.arch});
        CHECK(bi.exports == std::vector<std::string>{"VSTPluginMain"});
        CHECK(bi.architecture() == e.arch);
    }
    CHECK(inspect(elf(false, 40).save(dir / "arm32.so")).architecture() == "ELF machine 0x28");

    // Mach-O, thin.
    const BinaryInfo thin = inspect(machO(0x0100000C, "VSTPluginMain").save(dir / "arm64.dylib"));
    CHECK(thin.architectures == std::vector<std::string>{"arm64"} && thin.exports == std::vector<std::string>{"VSTPluginMain"});
    // 32-bit x86 (no exports).
    Bytes i386;
    i386.put(0, 0xFEEDFACE, 4);
    i386.put(4, 7, 4);
    i386.put(24, 0, 4);
    CHECK(inspect(i386.save(dir / "x86.dylib")).architectures == std::vector<std::string>{"x86"});
    // Stripped: the exports come from the export trie.
    CHECK(inspect(strippedMachO().save(dir / "stripped.dylib")).exports == (std::vector<std::string>{"VSTPluginMain", "VSTPluginMain2"}));

    // Mach-O, universal (x64 and arm64): exports from the image Brack would load.
    Bytes fat;
    fat.put(0, 0xCAFEBABE, 4, true);
    fat.put(4, 2, 4, true);
    const Bytes slices[] = {machO(0x01000007, "X64Only"), machO(0x0100000C, "Arm64Only")};
    for (size_t i = 0; i < 2; ++i) {
        const size_t a = 8 + i * 20, offset = 0x1000 * (i + 1);
        fat.put(a, i == 0 ? 0x01000007 : 0x0100000C, 4, true);
        fat.put(a + 8, offset, 4, true);
        fat.put(a + 12, slices[i].b.size(), 4, true);
        fat.append(offset, slices[i]);
    }
    const BinaryInfo universal = inspect(fat.save(dir / "universal.dylib"));
    CHECK(universal.architectures == (std::vector<std::string>{"x64", "arm64"}));
    const bool ours = std::string(buildArchitecture()) == "arm64";
    CHECK(universal.architecture() == (ours ? "arm64" : "x64"));
    CHECK(universal.exports == std::vector<std::string>{ours ? "Arm64Only" : "X64Only"});

    // Only a binary of this OS's own format can be loaded here.
    const fs::path own = dir / ("own." + std::string(kCurrentOs == TargetOs::Windows ? "dll" : "so"));
    const uint16_t peMachine = std::string(buildArchitecture()) == "x64" ? 0x8664 : std::string(buildArchitecture()) == "x86" ? 0x014C : 0xAA64;
    CHECK(inspect(pe(peMachine).save(own)).loadable == (kCurrentOs == TargetOs::Windows));
    CHECK(!inspect(dir / "universal.dylib").loadable || kCurrentOs == TargetOs::MacOS);

    BinaryInfo bi;
    std::string error;
    std::ofstream(dir / "text.dll") << "not a binary";
    CHECK(!inspectBinary(dir / "text.dll", bi, error) && !error.empty());
}

void utf16() {
    const std::string text = "Synth \xE3\x82\xB7\xE3\x83\xB3\xE3\x82\xBB \xF0\x9F\x8E\xB9";  // "Synth シンセ 🎹"
    const std::u16string wide = utf8ToUtf16(text);
    CHECK(wide.size() == 12);  // the keyboard is a surrogate pair
    CHECK(wide[6] == 0x30B7 && wide[10] == 0xD83C && wide[11] == 0xDFB9);
    CHECK(utf16ToUtf8(wide) == text);
    CHECK(utf16ToUtf8(u"a\xD800" u"b") == "a\xEF\xBF\xBD" "b");  // a lone surrogate
    CHECK(utf8ToUtf16("a\xC0\x80" "b") == u"a\xFFFD\xFFFD" u"b");  // an overlong encoding
    CHECK(utf8ToUtf16("\xE3\x82") == u"\xFFFD\xFFFD");              // cut short
    CHECK(utf8ToUtf16("").empty() && utf16ToUtf8(u"").empty());
}

void labels() {
    PluginDescription d;
    d.name = "synth";
    d.format = PluginFormat::Vst3;
    d.architecture = buildArchitecture();
    CHECK(pluginLabel(d) == "synth [VST3]");
    for (const char* other : {"x64", "x86", "arm64"}) {
        if (other == std::string(buildArchitecture())) continue;
        d.architecture = other;
        CHECK(pluginLabel(d) == "synth [VST3, " + std::string(other) + "]");
    }
}

}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / ("brack_test_portable-" + std::string(buildArchitecture()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    formats();
    binaries(dir);
    binaryHeaders(dir / "headers");
    utf16();
    labels();
    fs::remove_all(dir, ec);
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("all checks passed");
    return 0;
}
