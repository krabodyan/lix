#include <cerrno>
#include <algorithm>
#include <string_view>
#include <vector>
#include <map>

#include <strings.h> // for strcasecmp

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>

#include "lix/libutil/archive.hh"
#include "lix/libutil/file-system.hh"
#include "lix/libutil/finally.hh"
#include "lix/libutil/serialise.hh"
#include "lix/libutil/config.hh"
#include "lix/libutil/logging.hh"
#include "lix/libutil/signals.hh"

namespace nix {

struct ArchiveSettings : Config
{
    #include "archive-settings.gen.inc"
};

static ArchiveSettings archiveSettings;

static GlobalConfig::Register rArchiveSettings(&archiveSettings);

PathFilter defaultPathFilter = [](const Path &) { return true; };


static WireFormatGenerator dumpContents(Path path, off_t size)
{
    AutoCloseFD fd{open(path.c_str(), O_RDONLY | O_CLOEXEC)};
    if (!fd) throw SysError("opening file '%1%'", path);

    std::vector<char> buf(65536);
    size_t left = size;

    while (left > 0) {
        auto n = std::min(left, buf.size());
        readFull(fd.get(), buf.data(), n);
        left -= n;
        co_yield std::span{buf.data(), n};
    }
}

static WireFormatGenerator dump(nar::File f)
{
    co_yield "(";
    co_yield "type";
    co_yield "regular";
    if (f.executable) {
        co_yield "executable";
        co_yield "";
    }
    co_yield "contents";
    co_yield f.size;
    co_yield std::move(f.contents);
    co_yield SerializingTransform::padding(f.size);
    co_yield ")";
}

static WireFormatGenerator dump(nar::Symlink s)
{
    co_yield "(";
    co_yield "type";
    co_yield "symlink";
    co_yield "target";
    co_yield s.target;
    co_yield ")";
}

static WireFormatGenerator dump(nar::Directory d)
{
    co_yield "(";
    co_yield "type";
    co_yield "directory";
    while (auto e = d.contents.next()) {
        co_yield std::visit(
            [&](auto & i) {
                return [](auto & name, auto & i) -> WireFormatGenerator {
                    co_yield "entry";
                    co_yield "(";
                    co_yield "name";
                    co_yield name;
                    co_yield "node";
                    co_yield dump(std::move(i));
                    co_yield ")";
                }(e->first, i);
            },
            e->second
        );
    }
    co_yield ")";
}

// list the given path under the given filter and return the oldest mtime.
// if returnUnhacked is true directory entries that appear to have had the
// nix case hack applied will be returned without the case hack suffix, if
// returnUnhacked is false directory entries will be returned as they have
// been read from disk. to produce a correct NAR from the results the case
// hack must be undone if configured unless returnUnhacked is set to true.
static nar::Entry list(Path path, time_t & mtime, PathFilter & filter, bool returnUnhacked)
{
    checkInterrupt();

    auto st = lstat(path);
    mtime = st.st_mtime;

    if (S_ISREG(st.st_mode)) {
        return nar::File{
            (st.st_mode & S_IXUSR) != 0,
            size_t(st.st_size),
            dumpContents(std::move(path), st.st_size)
        };
    } else if (S_ISDIR(st.st_mode)) {
        auto contents = [](Path path, time_t & mtime, PathFilter & filter, bool returnUnhacked
                        ) -> Generator<std::pair<const std::string &, nar::Entry>> {
            /* If we're on a case-insensitive system like macOS, undo
               the case hack applied by restorePath(). */
            std::map<std::string, std::string> unhacked;
            for (auto & i : readDirectory(path))
                if (archiveSettings.useCaseHack) {
                    std::string name(i.name);
                    size_t pos = i.name.find(caseHackSuffix);
                    if (pos != std::string::npos) {
                        debug("removing case hack suffix from '%1%'", path + "/" + i.name);
                        name.erase(pos);
                    }
                    if (!unhacked.emplace(name, i.name).second)
                        throw Error("file name collision in between '%1%' and '%2%'",
                            (path + "/" + unhacked[name]),
                            (path + "/" + i.name));
                } else
                    unhacked.emplace(i.name, i.name);

            for (auto & i : unhacked) {
                if (filter(path + "/" + i.first)) {
                    time_t tmp_mtime;
                    auto diskPath = path + "/" + i.second;
                    co_yield std::pair(
                        std::cref(returnUnhacked ? i.first : i.second),
                        list(diskPath, tmp_mtime, filter, returnUnhacked)
                    );
                    if (tmp_mtime > mtime) {
                        mtime = tmp_mtime;
                    }
                }
            }
        };

        return nar::Directory(contents(std::move(path), mtime, filter, returnUnhacked));
    } else if (S_ISLNK(st.st_mode)) {
        return nar::Symlink{readLink(path)};
    } else {
        throw Error("file '%1%' has an unsupported type", path);
    }
}

WireFormatGenerator dumpPathAndGetMtime(Path path, time_t & mtime)
{
    co_yield narVersionMagic1;
    co_yield std::visit(
        [](auto i) -> WireFormatGenerator { co_yield dump(std::move(i)); },
        list(path, mtime, defaultPathFilter, true)
    );
}

WireFormatGenerator dumpPath(Path path, PathFilter & filter)
{
    auto filtered = prepareDump(std::move(path), filter);
    co_yield filtered->dump();
}

WireFormatGenerator dumpPath(Path path)
{
    auto prepared = prepareDump(std::move(path));
    co_yield prepared->dump();
}


WireFormatGenerator dumpString(std::string_view s)
{
    co_yield narVersionMagic1;
    co_yield "(";
    co_yield "type";
    co_yield "regular";
    co_yield "contents";
    co_yield s;
    co_yield ")";
}

struct UnfilteredDump : PreparedDump
{
    using PreparedDump::PreparedDump;

    WireFormatGenerator dump() const override
    {
        time_t ignored;
        auto all = list(rootPath, ignored, defaultPathFilter, true);
        co_yield narVersionMagic1;
        co_yield std::visit([](auto & i) { return nix::dump(std::move(i)); }, all);
    }
};

struct PrefilteredDump : PreparedDump
{
    struct File
    {
        bool executable;
        uint64_t size;
    };

    struct Symlink
    {
        Path target;
    };

    struct Directory;

    using Entry = std::variant<File, Symlink, Directory>;

    struct Directory
    {
        std::vector<std::pair<std::string, Entry>> contents;
    };

    Entry root;

    PrefilteredDump(Path path, PathFilter & filter) : PreparedDump(std::move(path))
    {
        time_t ignored;
        fillFrom(root, list(rootPath, ignored, filter, false));
    }

    static void fillFrom(Entry & target, nar::Entry e)
    {
        overloaded handlers{
            [&](nar::File & f) { target = File{f.executable, f.size}; },
            [&](nar::Symlink & s) { target = Symlink{std::move(s.target)}; },
            [&](nar::Directory & d) {
                Directory self;
                while (auto entry = d.contents.next()) {
                    fillFrom(
                        self.contents.emplace_back(std::move(entry->first), Entry{}).second,
                        std::move(entry->second)
                    );
                }
                target = std::move(self);
            },
        };
        std::visit(handlers, e);
    }

    static nar::Entry convert(Path path, const Entry & e)
    {
        overloaded handlers{
            [&](const File & f) -> nar::Entry {
                return nar::File{f.executable, f.size, dumpContents(std::move(path), f.size)};
            },
            [&](const Symlink & s) -> nar::Entry { return nar::Symlink{s.target}; },
            [&](const Directory & d) -> nar::Entry {
                return nar::Directory{
                    [](Path path, const Directory & d
                    ) -> Generator<std::pair<const std::string &, nar::Entry>> {
                        for (auto & [name, entry] : d.contents) {
                            std::string narName = archiveSettings.useCaseHack
                                ? name.substr(0, name.find(caseHackSuffix))
                                : name;
                            co_yield std::pair{
                                std::cref(narName), convert(path + "/" + name, entry)
                            };
                        }
                    }(std::move(path), d)
                };
            },
        };
        return std::visit(handlers, std::move(e));
    }

    WireFormatGenerator dump() const override
    {
        co_yield narVersionMagic1;
        co_yield std::visit(
            [](auto i) { return nix::dump(std::move(i)); }, convert(rootPath, root)
        );
    }
};

box_ptr<PreparedDump> prepareDump(Path path)
{
    return make_box_ptr<UnfilteredDump>(std::move(path));
}

box_ptr<PreparedDump> prepareDump(Path path, PathFilter & filter)
{
    return make_box_ptr<PrefilteredDump>(std::move(path), filter);
}

static SerialisationError badArchive(const std::string & s)
{
    return SerialisationError("bad archive: " + s);
}


struct CaseInsensitiveCompare
{
    bool operator() (const std::string & a, const std::string & b) const
    {
        return strcasecmp(a.c_str(), b.c_str()) < 0;
    }
};

namespace nar {

namespace {
struct Parser
{
    struct FileHeader
    {
        bool executable;
        uint64_t size;
    };
    struct Symlink
    {
        Path target;
    };
    struct Directory;
    struct WantBytes
    {
        size_t n;
    };

    using Response = std::variant<FileHeader, Symlink, Directory, WantBytes>;

    struct Directory
    {
        using Entry = std::pair<const Path &, Generator<Response>>;
        using Stream = Generator<std::variant<WantBytes, Entry>>;
        Stream content;
    };

    std::vector<char> & buffer;

    Generator<Response> parse()
    {
        // these macros purposely duplicate parts of the wire protocol,
        // but in such a way that doing it *wrong* will definitely make
        // tests fail. we could also duplicate them completely, but not
        // doing so ensures that we're the inverse of dump at all times
#define FETCH_U64()                                                       \
    ({                                                                    \
        co_yield WantBytes{8};                                            \
        StringSource src(std::string_view(buffer.data(), buffer.size())); \
        readNum<uint64_t>(src);                                           \
    })
#define READ_U64()            \
    ({                        \
        auto u = FETCH_U64(); \
        buffer.clear();       \
        u;                    \
    })
#define READ_STRING()                                                     \
    ({                                                                    \
        uint64_t len = FETCH_U64();                                       \
        co_yield WantBytes{len + (8 - len % 8) % 8};                      \
        StringSource src(std::string_view(buffer.data(), buffer.size())); \
        auto str = readString(src);                                       \
        buffer.clear();                                                   \
        std::move(str);                                                   \
    })
#define READ_PADDING(size)                                                    \
    do {                                                                      \
        if ((size) % 8) {                                                     \
            co_yield WantBytes{8 - (size) % 8};                               \
            StringSource src(std::string_view(buffer.data(), buffer.size())); \
            readPadding((size), src);                                         \
            buffer.clear();                                                   \
        }                                                                     \
    } while (0)
#define EXPECT(raw, kind)                              \
    do {                                               \
        auto s = READ_STRING();                        \
        if (s != (raw)) {                              \
            throw badArchive("expected " kind " tag"); \
        }                                              \
    } while (0)

        EXPECT("(", "open");
        EXPECT("type", "type");

        const auto t = READ_STRING();

        if (t == "regular") {
            auto contentsOrFlag = READ_STRING();
            const bool executable = contentsOrFlag == "executable";
            if (executable) {
                auto s = READ_STRING();
                if (s != "") {
                    throw badArchive("executable marker has non-empty value");
                }
                contentsOrFlag = READ_STRING();
            }
            if (contentsOrFlag == "contents") {
                const uint64_t size = READ_U64();
                co_yield FileHeader{executable, size};
                READ_PADDING(size);
            } else {
                throw badArchive("file without contents found");
            }
        } else if (t == "directory") {
            // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
            auto makeReader = [this](bool & completed) -> Directory::Stream {
                std::map<Path, int, CaseInsensitiveCompare> names;
                std::string prevName;

                while (1) {
                    {
                        const auto s = READ_STRING();
                        if (s == ")") {
                            break;
                        } else if (s != "entry") {
                            throw badArchive("expected entry tag");
                        }
                        EXPECT("(", "open");
                    }

                    EXPECT("name", "name");
                    auto name = READ_STRING();
                    if (name.empty() || name == "." || name == ".."
                        || name.find('/') != std::string::npos
                        || name.find((char) 0) != std::string::npos
                        // The case hack is a thing that only exists on the
                        // filesystem.
                        // Unpacking one appearing in a NAR is super
                        // sketchy because it will at minimum cause corruption at
                        // the time of repacking the NAR.
                        || name.find(caseHackSuffix) != std::string::npos)
                    {
                        throw Error("NAR contains invalid file name '%1%'", name);
                    }
                    if (name <= prevName) {
                        throw Error("NAR directory is not sorted");
                    }
                    prevName = name;
                    if (archiveSettings.useCaseHack) {
                        auto i = names.find(name);
                        if (i != names.end()) {
                            debug("case collision between '%1%' and '%2%'", i->first, name);
                            name += caseHackSuffix;
                            name += std::to_string(++i->second);
                        } else {
                            names[name] = 0;
                        }
                    }

                    EXPECT("node", "node");
                    co_yield Directory::Entry{name, parse()};
                    EXPECT(")", "close");
                }

                completed = true;
            };

            bool completed = false;
            co_yield Directory{makeReader(completed)};
            // directories may nest, so to drain a directory properly we'd have to add a Finally
            // argument to the generator to ensure that the draining code is always run. this is
            // usually not necessary, hard to follow, and rather error-prone on top of all that.
            assert(completed);
            // directories are terminated already, don't try to read another ")"
            co_return;
        } else if (t == "symlink") {
            EXPECT("target", "target");
            std::string target = READ_STRING();
            co_yield Symlink{target};
        } else {
            throw badArchive("unknown file type " + t);
        }

        EXPECT(")", "close");

#undef FETCH_U64
#undef READ_U64
#undef READ_STRING
#undef READ_PADDING
#undef EXPECT
    }
};

struct SyncParser
{
    Source & source;
    std::vector<char> buffer;

    Generator<Entry> parse()
    {
        Parser parser{buffer};
        auto stream = parser.parse();
        co_yield parse(stream);
    }

    void feed(size_t n)
    {
        checkInterrupt();

        auto end = buffer.size();
        buffer.resize(end + n);
        source(buffer.data() + end, n);
    }

    Generator<std::pair<const std::string &, Entry>>
    readDir(Parser::Directory::Stream stream)
    {
        while (auto e = stream.next()) {
            if (auto want = std::get_if<Parser::WantBytes>(&*e)) {
                feed(want->n);
            } else if (auto entry = std::get_if<Parser::Directory::Entry>(&*e)) {
                auto parsed = parse(entry->second);
                while (auto e = parsed.next()) {
                    co_yield std::pair(std::cref(entry->first), std::move(*e));
                }
            } else {
                assert(false && "expected parser response in dir");
            }
        }
    }

    Generator<Entry> parse(Generator<Parser::Response> & stream)
    {
        while (auto i = stream.next()) {
            if (auto want = std::get_if<Parser::WantBytes>(&*i)) {
                feed(want->n);
            } else if (auto f = std::get_if<Parser::FileHeader>(&*i)) {
                auto makeReader = [](Source & source, uint64_t & left) -> Generator<Bytes> {
                    std::array<char, 65536> buf;

                    while (left) {
                        checkInterrupt();
                        auto n = size_t(std::min<uint64_t>(buf.size(), left));
                        source(buf.data(), n);
                        co_yield std::span{buf.data(), n};
                        left -= n;
                    }
                };
                auto left = f->size;
                co_yield File{f->executable, f->size, makeReader(source, left)};
                // we could drain the remainder of the file, but coroutines being interruptible
                // at any time makes this difficult. for files this is not that hard, but being
                // consistent with directories is more important than handling the simple case.
                assert(left == 0);
            } else if (auto sl = std::get_if<Parser::Symlink>(&*i)) {
                co_yield Symlink{std::move(sl->target)};
            } else if (auto dir = std::get_if<Parser::Directory>(&*i)) {
                co_yield Directory{readDir(std::move(dir->content))};
            } else {
                assert(false && "unhandled parser response");
            }
        }
    }
};
}

Generator<Entry> parse(Source & source)
{
    std::string version;
    try {
        version = readString(source, narVersionMagic1.size());
    } catch (SerialisationError & e) {
        /* This generally means the integer at the start couldn't be
           decoded.  Ignore and throw the exception below. */
    }
    if (version != narVersionMagic1)
        throw badArchive("input doesn't look like a Nix archive");
    SyncParser p{source};
    co_yield p.parse();
}

}

static void restore(NARParseVisitor & sink, nar::Entry entry, const Path & path)
{
    return std::visit(
        overloaded{
            [&](nar::File f) {
                auto handle = sink.createRegularFile(path, f.size, f.executable);
                while (auto block = f.contents.next()) {
                    handle->receiveContents(std::string_view{block->data(), block->size()});
                }
                handle->close();
            },
            [&](nar::Symlink sl) { sink.createSymlink(path, sl.target); },
            [&](nar::Directory d) {
                auto dir = sink.createDirectory(path);
                while (auto entry = d.contents.next()) {
                    restore(*dir, std::move(entry->second), entry->first);
                }
            },
        },
        std::move(entry)
    );
}

void parseDump(NARParseVisitor & sink, Source & source)
{
    auto nar = nar::parse(source);
    while (auto entry = nar.next()) {
        restore(sink, std::move(*entry), "");
    }
}

/*
 * Note [NAR restoration security]:
 * It's *critical* that NAR restoration will never overwrite anything even if
 * duplicate filenames are passed in. It is inevitable that not all NARs are
 * fit to actually successfully restore to the target filesystem; errors may
 * occur due to collisions, and this *must* cause the NAR to be rejected.
 *
 * Although the filenames are blocked from being *the same bytes* by a higher
 * layer, filesystems have other ideas on every platform:
 * - The store may be on a case-insensitive filesystem like APFS, ext4 with
 *   casefold directories, zfs with casesensitivity=insensitive
 * - The store may be on a Unicode normalizing (or normalization-insensitive)
 *   filesystem like APFS (where files are looked up by
 *   hash(normalize(fname))), HFS+ (where file names are always normalized to
 *   approximately NFD), or zfs with normalization=formC, etc.
 *
 * It is impossible to know the version of Unicode being used by the underlying
 * filesystem, thus it is *impossible* to stop these collisions.
 *
 * Overwriting files as a result of invalid NARs will cause a security bug like
 * CppNix's CVE-2024-45593 (GHSA-h4vv-h3jq-v493)
 */

/**
 * This code restores NARs from disk.
 *
 * See Note [NAR restoration security] for security invariants in this procedure.
 *
 */
struct NARRestoreVisitor : NARParseVisitor
{
    Path dstPath;

private:
    struct MyFileHandle : public FileHandle
    {
        AutoCloseFD fd;

        MyFileHandle(AutoCloseFD && fd, uint64_t size, bool executable) : FileHandle(), fd(std::move(fd))
        {
            if (executable) {
                makeExecutable();
            }

            maybePreallocateContents(size);
        }

        void makeExecutable()
        {
            struct stat st;
            if (fstat(fd.get(), &st) == -1)
                throw SysError("fstat");
            if (fchmod(fd.get(), st.st_mode | (S_IXUSR | S_IXGRP | S_IXOTH)) == -1)
                throw SysError("fchmod");
        }

        void maybePreallocateContents(uint64_t len)
        {
            if (!archiveSettings.preallocateContents)
                return;

#if HAVE_POSIX_FALLOCATE
            if (len) {
                errno = posix_fallocate(fd.get(), 0, len);
                /* Note that EINVAL may indicate that the underlying
                   filesystem doesn't support preallocation (e.g. on
                   OpenSolaris).  Since preallocation is just an
                   optimisation, ignore it. */
                if (errno && errno != EINVAL && errno != EOPNOTSUPP && errno != ENOSYS)
                    throw SysError("preallocating file of %1% bytes", len);
            }
#endif
        }

        ~MyFileHandle() = default;

        virtual void close() override
        {
            /* Call close explicitly to make sure the error is checked */
            fd.close();
        }

        void receiveContents(std::string_view data) override
        {
            writeFull(fd.get(), data);
        }
    };

public:
    NARRestoreVisitor(Path dstPath): dstPath(std::move(dstPath)) {}

    box_ptr<NARParseVisitor> createDirectory(const std::string & name) override
    {
        Path p = dstPath + name;
        if (mkdir(p.c_str(), 0777) == -1)
            throw SysError("creating directory '%1%'", p);
        return make_box_ptr<NARRestoreVisitor>(p + "/");
    };

    box_ptr<FileHandle> createRegularFile(const std::string & name, uint64_t size, bool executable) override
    {
        Path p = dstPath + name;
        AutoCloseFD fd = AutoCloseFD{open(p.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0666)};
        if (!fd) throw SysError("creating file '%1%'", p);

        return make_box_ptr<MyFileHandle>(std::move(fd), size, executable);
    }

    void createSymlink(const std::string & name, const std::string & target) override
    {
        Path p = dstPath + name;
        nix::createSymlink(target, p);
    }
};


void restorePath(const Path & path, Source & source)
{
    NARRestoreVisitor sink(path);
    parseDump(sink, source);
}


WireFormatGenerator copyNAR(Source & source)
{
    // FIXME: if 'source' is the output of dumpPath() followed by EOF,
    // we should just forward all data directly without parsing.

    auto items = nar::parse(source);
    co_yield narVersionMagic1;
    while (auto e = items.next()) {
        co_yield std::visit([](auto & i) { return dump(std::move(i)); }, *e);
    }
}

}
