#include "lix/libcmd/command.hh"
#include "lix/libutil/hash.hh"
#include "lix/libstore/content-address.hh"
#include "lix/libcmd/legacy.hh"
#include "lix/libmain/shared.hh"
#include "lix/libutil/references.hh"
#include "lix/libutil/archive.hh"
#include "hash.hh"

namespace nix {

struct CmdHashBase : Command
{
    FileIngestionMethod mode;
    Base base = Base::SRI;
    bool truncate = false;
    HashType ht = HashType::SHA256;
    std::vector<std::string> paths;
    std::optional<std::string> modulus;

    CmdHashBase(FileIngestionMethod mode) : mode(mode)
    {
        addFlag({
            .longName = "sri",
            .description = "Print the hash in SRI format.",
            .handler = {&base, Base::SRI},
        });

        addFlag({
            .longName = "base64",
            .description = "Print the hash in base-64 format.",
            .handler = {&base, Base::Base64},
        });

        addFlag({
            .longName = "base32",
            .description = "Print the hash in base-32 (Nix-specific) format.",
            .handler = {&base, Base::Base32},
        });

        addFlag({
            .longName = "base16",
            .description = "Print the hash in base-16 format.",
            .handler = {&base, Base::Base16},
        });

        addFlag(Flag::mkHashTypeFlag("type", &ht));

        #if 0
        addFlag({
            .longName = "modulo",
            .description = "Compute the hash modulo the specified string.",
            .labels = {"modulus"},
            .handler = {&modulus},
        });
        #endif\

        expectArgs({
            .label = "paths",
            .handler = {&paths},
            .completer = completePath
        });
    }

    std::string description() override
    {
        switch (mode) {
        case FileIngestionMethod::Flat:
            return  "print cryptographic hash of a regular file";
        case FileIngestionMethod::Recursive:
            return "print cryptographic hash of the NAR serialisation of a path";
        default:
            assert(false);
        };
    }

    void run() override
    {
        for (auto path : paths) {
            auto source = [&] () -> GeneratorSource {
                switch (mode) {
                case FileIngestionMethod::Flat:
                    return GeneratorSource(readFileSource(path));
                case FileIngestionMethod::Recursive:
                    return GeneratorSource(dumpPath(path));
                }
                assert(false);
            }();

            Hash h = modulus
                ? computeHashModulo(ht, *modulus, source).first
                : hashSource(ht, source).first;
            if (truncate && h.hashSize > 20) h = compressHash(h, 20);
            logger->cout(h.to_string(base, base == Base::SRI));
        }
    }
};

struct CmdToBase : Command
{
    Base base;
    std::optional<HashType> ht;
    std::vector<std::string> args;

    CmdToBase(Base base) : base(base)
    {
        addFlag(Flag::mkHashTypeOptFlag("type", &ht));
        expectArgs("strings", &args);
    }

    std::string description() override
    {
        return fmt("convert a hash to %s representation",
            base == Base::Base16 ? "base-16" :
            base == Base::Base32 ? "base-32" :
            base == Base::Base64 ? "base-64" :
            "SRI");
    }

    void run() override
    {
        for (auto s : args)
            logger->cout(Hash::parseAny(s, ht).to_string(base, base == Base::SRI));
    }
};

struct CmdHash : MultiCommand
{
    CmdHash()
        : MultiCommand({
            {"file",
             [](auto & aio) {
                 return make_ref<MixAio<CmdHashBase>>(aio, FileIngestionMethod::Flat);
                 ;
             }},
            {"path",
             [](auto & aio) {
                 return make_ref<MixAio<CmdHashBase>>(aio, FileIngestionMethod::Recursive);
             }},
            {"to-base16",
             [](auto & aio) { return make_ref<MixAio<CmdToBase>>(aio, Base::Base16); }},
            {"to-base32",
             [](auto & aio) { return make_ref<MixAio<CmdToBase>>(aio, Base::Base32); }},
            {"to-base64",
             [](auto & aio) { return make_ref<MixAio<CmdToBase>>(aio, Base::Base64); }},
            {"to-sri", [](auto & aio) { return make_ref<MixAio<CmdToBase>>(aio, Base::SRI); }},
        })
    {
    }

    std::string description() override
    {
        return "compute and convert cryptographic hashes";
    }

    Category category() override { return catUtility; }

    void run() override
    {
        if (!command)
            throw UsageError("'nix hash' requires a sub-command.");
        command->second->run();
    }
};

void registerNixHash()
{
    registerCommand<CmdHash>("hash");
}

/* Legacy nix-hash command. */
static int compatNixHash(AsyncIoRoot & aio, std::string programName, Strings argv)
{
    std::optional<HashType> ht;
    bool flat = false;
    Base base = Base::Base16;
    bool truncate = false;
    enum { opHash, opTo } op = opHash;
    std::vector<std::string> ss;

    LegacyArgs(aio, programName, [&](Strings::iterator & arg, const Strings::iterator & end) {
        if (*arg == "--help")
            showManPage("nix-hash");
        else if (*arg == "--version")
            printVersion("nix-hash");
        else if (*arg == "--flat") flat = true;
        else if (*arg == "--base16") base = Base::Base16;
        else if (*arg == "--base32") base = Base::Base32;
        else if (*arg == "--base64") base = Base::Base64;
        else if (*arg == "--sri") base = Base::SRI;
        else if (*arg == "--truncate") truncate = true;
        else if (*arg == "--type") {
            std::string s = getArg(*arg, arg, end);
            ht = parseHashType(s);
        }
        else if (*arg == "--to-base16") {
            op = opTo;
            base = Base::Base16;
        }
        else if (*arg == "--to-base32") {
            op = opTo;
            base = Base::Base32;
        }
        else if (*arg == "--to-base64") {
            op = opTo;
            base = Base::Base64;
        }
        else if (*arg == "--to-sri") {
            op = opTo;
            base = Base::SRI;
        }
        else if (*arg != "" && arg->at(0) == '-')
            return false;
        else
            ss.push_back(*arg);
        return true;
    }).parseCmdline(argv);

    if (op == opHash) {
        MixAio<CmdHashBase> cmd(aio, flat ? FileIngestionMethod::Flat : FileIngestionMethod::Recursive);
        if (!ht.has_value()) ht = HashType::MD5;
        cmd.ht = ht.value();
        cmd.base = base;
        cmd.truncate = truncate;
        cmd.paths = ss;
        cmd.run();
    }

    else {
        MixAio<CmdToBase> cmd(aio, base);
        cmd.args = ss;
        if (ht.has_value()) cmd.ht = ht;
        cmd.run();
    }

    return 0;
}

void registerLegacyNixHash() {
    LegacyCommandRegistry::add("nix-hash", compatNixHash);
}

}
