#pragma once
///@file

#include <limits>
#include <string>

#include "lix/libstore/store-api.hh"
#include "lix/libstore/gc-store.hh"
#include "lix/libstore/log-store.hh"


namespace nix {


class Pipe;
class Pid;
struct FdSink;
struct FdSource;
template<typename T> class Pool;

struct RemoteStoreConfig : virtual StoreConfig
{
    using StoreConfig::StoreConfig;

    const Setting<int> maxConnections{this, 1, "max-connections",
        "Maximum number of concurrent connections to the Nix daemon."};

    const Setting<unsigned int> maxConnectionAge{this,
        std::numeric_limits<unsigned int>::max(),
        "max-connection-age",
        "Maximum age of a connection before it is closed."};
};

/**
 * \todo RemoteStore is a misnomer - should be something like
 * DaemonStore.
 */
class RemoteStore : public virtual Store,
    public virtual GcStore,
    public virtual LogStore
{
public:

    RemoteStore(const RemoteStoreConfig & config);

    RemoteStoreConfig & config() override = 0;
    const RemoteStoreConfig & config() const override = 0;

    /* Implementations of abstract store API methods. */

    bool isValidPathUncached(const StorePath & path) override;

    StorePathSet queryValidPaths(const StorePathSet & paths,
        SubstituteFlag maybeSubstitute = NoSubstitute) override;

    StorePathSet queryAllValidPaths() override;

    std::shared_ptr<const ValidPathInfo> queryPathInfoUncached(const StorePath & path) override;

    void queryReferrers(const StorePath & path, StorePathSet & referrers) override;

    StorePathSet queryValidDerivers(const StorePath & path) override;

    StorePathSet queryDerivationOutputs(const StorePath & path) override;

    std::map<std::string, std::optional<StorePath>> queryPartialDerivationOutputMap(const StorePath & path, Store * evalStore = nullptr) override;
    std::optional<StorePath> queryPathFromHashPart(const std::string & hashPart) override;

    kj::Promise<Result<StorePathSet>> querySubstitutablePaths(const StorePathSet & paths) override;

    kj::Promise<Result<void>> querySubstitutablePathInfos(const StorePathCAMap & paths,
        SubstitutablePathInfos & infos) override;

    /**
     * Add a content-addressable store path. `dump` will be drained.
     */
    ref<const ValidPathInfo> addCAToStore(
        Source & dump,
        std::string_view name,
        ContentAddressMethod caMethod,
        HashType hashType,
        const StorePathSet & references,
        RepairFlag repair);

    /**
     * Add a content-addressable store path. Does not support references. `dump` will be drained.
     */
    kj::Promise<Result<StorePath>> addToStoreFromDump(Source & dump, std::string_view name,
        FileIngestionMethod method = FileIngestionMethod::Recursive, HashType hashAlgo = HashType::SHA256, RepairFlag repair = NoRepair, const StorePathSet & references = StorePathSet()) override;

    kj::Promise<Result<void>> addToStore(const ValidPathInfo & info, Source & nar,
        RepairFlag repair, CheckSigsFlag checkSigs) override;

    kj::Promise<Result<void>> addMultipleToStore(
        Source & source,
        RepairFlag repair,
        CheckSigsFlag checkSigs) override;

    kj::Promise<Result<void>> addMultipleToStore(
        PathsSource & pathsToCopy,
        Activity & act,
        RepairFlag repair,
        CheckSigsFlag checkSigs) override;

    kj::Promise<Result<StorePath>> addTextToStore(
        std::string_view name,
        std::string_view s,
        const StorePathSet & references,
        RepairFlag repair) override;

    void registerDrvOutput(const Realisation & info) override;

    std::shared_ptr<const Realisation> queryRealisationUncached(const DrvOutput &) override;

    kj ::Promise<Result<void>> buildPaths(
        const std::vector<DerivedPath> & paths,
        BuildMode buildMode,
        std::shared_ptr<Store> evalStore
    ) override;

    kj::Promise<Result<std::vector<KeyedBuildResult>>> buildPathsWithResults(
        const std::vector<DerivedPath> & paths,
        BuildMode buildMode,
        std::shared_ptr<Store> evalStore) override;

    kj ::Promise<Result<BuildResult>> buildDerivation(
        const StorePath & drvPath, const BasicDerivation & drv, BuildMode buildMode
    ) override;

    kj::Promise<Result<void>> ensurePath(const StorePath & path) override;

    void addTempRoot(const StorePath & path) override;

    Roots findRoots(bool censor) override;

    void collectGarbage(const GCOptions & options, GCResults & results) override;

    kj::Promise<Result<void>> optimiseStore() override;

    kj::Promise<Result<bool>> verifyStore(bool checkContents, RepairFlag repair) override;

    /**
     * The default instance would schedule the work on the client side, but
     * for consistency with `buildPaths` and `buildDerivation` it should happen
     * on the remote side.
     *
     * We make this fail for now so we can add implement this properly later
     * without it being a breaking change.
     */
    kj::Promise<Result<void>> repairPath(const StorePath & path) override
    try { unsupported("repairPath"); } catch (...) { return {result::current_exception()}; }

    void addSignatures(const StorePath & storePath, const StringSet & sigs) override;

    void queryMissing(const std::vector<DerivedPath> & targets,
        StorePathSet & willBuild, StorePathSet & willSubstitute, StorePathSet & unknown,
        uint64_t & downloadSize, uint64_t & narSize) override;

    void addBuildLog(const StorePath & drvPath, std::string_view log) override;

    std::optional<std::string> getVersion() override;

    void connect() override;

    unsigned int getProtocol() override;

    std::optional<TrustedFlag> isTrustedClient() override;

    struct Connection;

    ref<Connection> openConnectionWrapper();

protected:

    virtual ref<Connection> openConnection() = 0;

    void initConnection(Connection & conn);

    ref<Pool<Connection>> connections;

    virtual void setOptions(Connection & conn);

    void setOptions() override;

    struct ConnectionHandle;

    ConnectionHandle getConnection();

    friend struct ConnectionHandle;

    virtual ref<FSAccessor> getFSAccessor() override;

    virtual box_ptr<Source> narFromPath(const StorePath & path) override;

private:

    std::atomic_bool failed{false};

    kj::Promise<Result<void>> copyDrvsFromEvalStore(
        const std::vector<DerivedPath> & paths,
        std::shared_ptr<Store> evalStore);
};

}
