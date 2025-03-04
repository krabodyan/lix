#include "lix/libcmd/command.hh"
#include "lix/libcmd/cmd-profiles.hh"
#include "lix/libmain/shared.hh"
#include "lix/libstore/store-api.hh"
#include "lix/libmain/common-args.hh"
#include "lix/libstore/names.hh"
#include "lix/libutil/result.hh"

#include <regex>

namespace nix {

struct Info
{
    std::string outputName;
};

// name -> version -> store paths
typedef std::map<std::string, std::map<std::string, std::map<StorePath, Info>>> GroupedPaths;

static kj::Promise<Result<GroupedPaths>>
getClosureInfo(ref<Store> store, const StorePath & toplevel)
try {
    StorePathSet closure;
    TRY_AWAIT(store->computeFSClosure({toplevel}, closure));

    GroupedPaths groupedPaths;

    for (auto const & path : closure) {
        /* Strip the output name. Unfortunately this is ambiguous (we
           can't distinguish between output names like "bin" and
           version suffixes like "unstable"). */
        static std::regex regex("(.*)-([a-z]+|lib32|lib64)");
        std::cmatch match;
        std::string name{path.name()};
        std::string_view const origName = path.name();
        std::string outputName;

        if (std::regex_match(origName.begin(), origName.end(), match, regex)) {
            name = match[1];
            outputName = match[2];
        }

        DrvName drvName(name);
        groupedPaths[drvName.name][drvName.version].emplace(path, Info { .outputName = outputName });
    }

    co_return groupedPaths;
} catch (...) {
    co_return result::current_exception();
}

kj::Promise<Result<void>> printClosureDiff(
    ref<Store> store,
    const StorePath & beforePath,
    const StorePath & afterPath,
    std::string_view indent)
try {
    auto beforeClosure = TRY_AWAIT(getClosureInfo(store, beforePath));
    auto afterClosure = TRY_AWAIT(getClosureInfo(store, afterPath));

    std::set<std::string> allNames;
    for (auto & [name, _] : beforeClosure) allNames.insert(name);
    for (auto & [name, _] : afterClosure) allNames.insert(name);

    for (auto & name : allNames) {
        auto & beforeVersions = beforeClosure[name];
        auto & afterVersions = afterClosure[name];

        // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
        auto totalSize = [&](const std::map<std::string, std::map<StorePath, Info>> & versions
                         ) -> kj::Promise<Result<uint64_t>> {
            try {
                uint64_t sum = 0;
                for (auto & [_, paths] : versions)
                    for (auto & [path, _] : paths)
                        sum += TRY_AWAIT(store->queryPathInfo(path))->narSize;
                co_return sum;
            } catch (...) {
                co_return result::current_exception();
            }
        };

        auto beforeSize = TRY_AWAIT(totalSize(beforeVersions));
        auto afterSize = TRY_AWAIT(totalSize(afterVersions));
        auto sizeDelta = (int64_t) afterSize - (int64_t) beforeSize;
        auto showDelta = std::abs(sizeDelta) >= 8 * 1024;

        std::set<std::string> removed, unchanged;
        for (auto & [version, _] : beforeVersions)
            if (!afterVersions.count(version)) removed.insert(version); else unchanged.insert(version);

        std::set<std::string> added;
        for (auto & [version, _] : afterVersions)
            if (!beforeVersions.count(version)) added.insert(version);

        if (showDelta || !removed.empty() || !added.empty()) {
            std::vector<std::string> items;
            if (!removed.empty() || !added.empty())
                items.push_back(fmt("%s → %s", showVersions(removed), showVersions(added)));
            if (showDelta)
                items.push_back(fmt("%s%+.1f KiB" ANSI_NORMAL, sizeDelta > 0 ? ANSI_RED : ANSI_GREEN, sizeDelta / 1024.0));
            logger->cout("%s%s: %s", indent, name, concatStringsSep(", ", items));
        }
    }
    co_return result::success();
} catch (...) {
    co_return result::current_exception();
}

}

using namespace nix;

struct CmdDiffClosures : SourceExprCommand, MixOperateOnOptions
{
    std::string _before, _after;

    CmdDiffClosures()
    {
        expectArg("before", &_before);
        expectArg("after", &_after);
    }

    std::string description() override
    {
        return "show what packages and versions were added and removed between two closures";
    }

    std::string doc() override
    {
        return
          #include "diff-closures.md"
          ;
    }

    void run(ref<Store> store) override
    {
        auto state = getEvaluator()->begin(aio());
        auto before = parseInstallable(*state, store, _before);
        auto beforePath = Installable::toStorePath(*state, getEvalStore(), store, Realise::Outputs, operateOn, before);
        auto after = parseInstallable(*state, store, _after);
        auto afterPath = Installable::toStorePath(*state, getEvalStore(), store, Realise::Outputs, operateOn, after);
        aio().blockOn(printClosureDiff(store, beforePath, afterPath, ""));
    }
};

static auto rCmdDiffClosures = registerCommand2<CmdDiffClosures>({"store", "diff-closures"});
