#include <regex>

#include <gtest/gtest.h>

#include "lix/libstore/worker-protocol.hh"
#include "lix/libstore/worker-protocol-impl.hh"
#include "lix/libstore/derived-path.hh"
#include "lix/libstore/build-result.hh"
#include "protocol.hh"
#include "tests/characterization.hh"

namespace nix {

const char workerProtoDir[] = "worker-protocol";

struct WorkerProtoTest : VersionedProtoTest<WorkerProto, workerProtoDir>
{
    /**
     * For serializers that don't care about the minimum version, we
     * have to use the minimum supported to not throw an assert.
     */
    WorkerProto::Version defaultVersion = MIN_SUPPORTED_WORKER_PROTO_VERSION;
};


VERSIONED_CHARACTERIZATION_TEST(
    WorkerProtoTest,
    string,
    "string",
    defaultVersion,
    (std::tuple<std::string, std::string, std::string, std::string, std::string> {
        "",
        "hi",
        "white rabbit",
        "大白兔",
        "oh no \0\0\0 what was that!",
    }))

VERSIONED_CHARACTERIZATION_TEST(
    WorkerProtoTest,
    storePath,
    "store-path",
    defaultVersion,
    (std::tuple<StorePath, StorePath> {
        StorePath { "g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-foo" },
        StorePath { "g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-foo-bar" },
    }))

VERSIONED_CHARACTERIZATION_TEST(
    WorkerProtoTest,
    contentAddress,
    "content-address",
    defaultVersion,
    (std::tuple<ContentAddress, ContentAddress, ContentAddress> {
        ContentAddress {
            .method = TextIngestionMethod {},
            .hash = hashString(HashType::SHA256, "Derive(...)"),
        },
        ContentAddress {
            .method = FileIngestionMethod::Flat,
            .hash = hashString(HashType::SHA1, "blob blob..."),
        },
        ContentAddress {
            .method = FileIngestionMethod::Recursive,
            .hash = hashString(HashType::SHA256, "(...)"),
        },
    }))

VERSIONED_CHARACTERIZATION_TEST(
    WorkerProtoTest,
    derivedPath_1_29,
    "derived-path-1.29",
    1 << 8 | 29,
    (std::tuple<DerivedPath, DerivedPath, DerivedPath> {
        DerivedPath::Opaque {
            .path = StorePath { "g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-foo" },
        },
        DerivedPath::Built {
            .drvPath = makeConstantStorePathRef(StorePath {
                "g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-bar.drv",
            }),
            .outputs = OutputsSpec::All { },
        },
        DerivedPath::Built {
            .drvPath = makeConstantStorePathRef(StorePath {
                "g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-bar.drv",
            }),
            .outputs = OutputsSpec::Names { "x", "y" },
        },
    }))

VERSIONED_CHARACTERIZATION_TEST(
    WorkerProtoTest,
    derivedPath_1_30,
    "derived-path-1.30",
    1 << 8 | 30,
    (std::tuple<DerivedPath, DerivedPath, DerivedPath, DerivedPath> {
        DerivedPath::Opaque {
            .path = StorePath { "g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-foo" },
        },
        DerivedPath::Opaque {
            .path = StorePath { "g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-foo.drv" },
        },
        DerivedPath::Built {
            .drvPath = makeConstantStorePathRef(StorePath {
                "g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-bar.drv",
            }),
            .outputs = OutputsSpec::All { },
        },
        DerivedPath::Built {
            .drvPath = makeConstantStorePathRef(StorePath {
                "g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-bar.drv",
            }),
            .outputs = OutputsSpec::Names { "x", "y" },
        },
    }))

VERSIONED_CHARACTERIZATION_TEST(
    WorkerProtoTest,
    drvOutput,
    "drv-output",
    defaultVersion,
    (std::tuple<DrvOutput, DrvOutput> {
        {
            .drvHash = Hash::parseSRI("sha256-FePFYIlMuycIXPZbWi7LGEiMmZSX9FMbaQenWBzm1Sc="),
            .outputName = "baz",
        },
        DrvOutput {
            .drvHash = Hash::parseSRI("sha256-b4afnqKCO9oWXgYHb9DeQ2berSwOjS27rSd9TxXDc/U="),
            .outputName = "quux",
        },
    }))

VERSIONED_CHARACTERIZATION_TEST(
    WorkerProtoTest,
    realisation,
    "realisation",
    defaultVersion,
    (std::tuple<Realisation, Realisation> {
        Realisation {
            .id = DrvOutput {
                .drvHash = Hash::parseSRI("sha256-FePFYIlMuycIXPZbWi7LGEiMmZSX9FMbaQenWBzm1Sc="),
                .outputName = "baz",
            },
            .outPath = StorePath { "g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-foo" },
            .signatures = { "asdf", "qwer" },
        },
        Realisation {
            .id = {
                .drvHash = Hash::parseSRI("sha256-FePFYIlMuycIXPZbWi7LGEiMmZSX9FMbaQenWBzm1Sc="),
                .outputName = "baz",
            },
            .outPath = StorePath { "g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-foo" },
            .signatures = { "asdf", "qwer" },
            .dependentRealisations = {
                {
                    DrvOutput {
                        .drvHash = Hash::parseSRI("sha256-b4afnqKCO9oWXgYHb9DeQ2berSwOjS27rSd9TxXDc/U="),
                        .outputName = "quux",
                    },
                    StorePath { "g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-foo" },
                },
            },
        },
    }))

VERSIONED_CHARACTERIZATION_TEST(
    WorkerProtoTest,
    buildResult_1_27,
    "build-result-1.27",
    1 << 8 | 27,
    ({
        using namespace std::literals::chrono_literals;
        std::tuple<BuildResult, BuildResult, BuildResult> t {
            BuildResult {
                .status = BuildResult::OutputRejected,
                .errorMsg = "no idea why",
            },
            BuildResult {
                .status = BuildResult::NotDeterministic,
                .errorMsg = "no idea why",
            },
            BuildResult {
                .status = BuildResult::Built,
            },
        };
        t;
    }))

VERSIONED_CHARACTERIZATION_TEST(
    WorkerProtoTest,
    buildResult_1_28,
    "build-result-1.28",
    1 << 8 | 28,
    ({
        using namespace std::literals::chrono_literals;
        std::tuple<BuildResult, BuildResult, BuildResult> t {
            BuildResult {
                .status = BuildResult::OutputRejected,
                .errorMsg = "no idea why",
            },
            BuildResult {
                .status = BuildResult::NotDeterministic,
                .errorMsg = "no idea why",
            },
            BuildResult {
                .status = BuildResult::Built,
                .builtOutputs = {
                    {
                        "foo",
                        {
                            .id = DrvOutput {
                                .drvHash = Hash::parseSRI("sha256-b4afnqKCO9oWXgYHb9DeQ2berSwOjS27rSd9TxXDc/U="),
                                .outputName = "foo",
                            },
                            .outPath = StorePath { "g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-foo" },
                        },
                    },
                    {
                        "bar",
                        {
                            .id = DrvOutput {
                                .drvHash = Hash::parseSRI("sha256-b4afnqKCO9oWXgYHb9DeQ2berSwOjS27rSd9TxXDc/U="),
                                .outputName = "bar",
                            },
                            .outPath = StorePath { "g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-bar" },
                        },
                    },
                },
            },
        };
        t;
    }))

VERSIONED_CHARACTERIZATION_TEST(
    WorkerProtoTest,
    buildResult_1_29,
    "build-result-1.29",
    1 << 8 | 29,
    ({
        using namespace std::literals::chrono_literals;
        std::tuple<BuildResult, BuildResult, BuildResult> t {
            BuildResult {
                .status = BuildResult::OutputRejected,
                .errorMsg = "no idea why",
            },
            BuildResult {
                .status = BuildResult::NotDeterministic,
                .errorMsg = "no idea why",
                .timesBuilt = 3,
                .isNonDeterministic = true,
                .startTime = 30,
                .stopTime = 50,
            },
            BuildResult {
                .status = BuildResult::Built,
                .timesBuilt = 1,
                .builtOutputs = {
                    {
                        "foo",
                        {
                            .id = DrvOutput {
                                .drvHash = Hash::parseSRI("sha256-b4afnqKCO9oWXgYHb9DeQ2berSwOjS27rSd9TxXDc/U="),
                                .outputName = "foo",
                            },
                            .outPath = StorePath { "g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-foo" },
                        },
                    },
                    {
                        "bar",
                        {
                            .id = DrvOutput {
                                .drvHash = Hash::parseSRI("sha256-b4afnqKCO9oWXgYHb9DeQ2berSwOjS27rSd9TxXDc/U="),
                                .outputName = "bar",
                            },
                            .outPath = StorePath { "g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-bar" },
                        },
                    },
                },
                .startTime = 30,
                .stopTime = 50,
#if 0
                // These fields are not yet serialized.
                // FIXME Include in next version of protocol or document
                // why they are skipped.
                .cpuUser = std::chrono::milliseconds(500s),
                .cpuSystem = std::chrono::milliseconds(604s),
#endif
            },
        };
        t;
    }))

VERSIONED_CHARACTERIZATION_TEST(
    WorkerProtoTest,
    keyedBuildResult_1_29,
    "keyed-build-result-1.29",
    1 << 8 | 29,
    ({
        using namespace std::literals::chrono_literals;
        std::tuple<KeyedBuildResult, KeyedBuildResult/*, KeyedBuildResult*/> t {
            KeyedBuildResult {
                {
                    .status = KeyedBuildResult::OutputRejected,
                    .errorMsg = "no idea why",
                },
                /* .path = */ DerivedPath::Opaque {
                    StorePath { "g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-xxx" },
                },
            },
            KeyedBuildResult {
                {
                    .status = KeyedBuildResult::NotDeterministic,
                    .errorMsg = "no idea why",
                    .timesBuilt = 3,
                    .isNonDeterministic = true,
                    .startTime = 30,
                    .stopTime = 50,
                },
                /* .path = */ DerivedPath::Built {
                    .drvPath = makeConstantStorePathRef(StorePath {
                        "g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-bar.drv",
                    }),
                    .outputs = OutputsSpec::Names { "out" },
                },
            },
        };
        t;
    }))

VERSIONED_CHARACTERIZATION_TEST(
    WorkerProtoTest,
    unkeyedValidPathInfo,
    "unkeyed-valid-path-info",
    defaultVersion,
    (std::tuple<UnkeyedValidPathInfo, UnkeyedValidPathInfo> {
        ({
            UnkeyedValidPathInfo info {
                Hash::parseSRI("sha256-FePFYIlMuycIXPZbWi7LGEiMmZSX9FMbaQenWBzm1Sc="),
            };
            info.registrationTime = 23423;
            info.narSize = 34878;
            info;
        }),
        ({
            UnkeyedValidPathInfo info {
                Hash::parseSRI("sha256-FePFYIlMuycIXPZbWi7LGEiMmZSX9FMbaQenWBzm1Sc="),
            };
            info.deriver = StorePath {
                "g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-bar.drv",
            };
            info.references = {
                StorePath {
                    "g1w7hyyyy1w7hy3qg1w7hy3qgqqqqy3q-foo.drv",
                },
            };
            info.registrationTime = 23423;
            info.narSize = 34878;
            info;
        }),
    }))

VERSIONED_CHARACTERIZATION_TEST(
    WorkerProtoTest,
    validPathInfo,
    "valid-path-info",
    defaultVersion,
    (std::tuple<ValidPathInfo, ValidPathInfo, ValidPathInfo> {
        ({
            ValidPathInfo info {
                StorePath {
                    "g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-bar",
                },
                UnkeyedValidPathInfo {
                    Hash::parseSRI("sha256-FePFYIlMuycIXPZbWi7LGEiMmZSX9FMbaQenWBzm1Sc="),
                },
            };
            info.registrationTime = 23423;
            info.narSize = 34878;
            info.ultimate = true;
            info;
        }),
        ({
            ValidPathInfo info {
                StorePath {
                    "g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-bar",
                },
                UnkeyedValidPathInfo {
                    Hash::parseSRI("sha256-FePFYIlMuycIXPZbWi7LGEiMmZSX9FMbaQenWBzm1Sc="),
                },
            };
            info.deriver = StorePath {
                "g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-bar.drv",
            };
            info.references = {
                // other reference
                StorePath {
                    "g1w7hyyyy1w7hy3qg1w7hy3qgqqqqy3q-foo",
                },
                // self reference
                StorePath {
                    "g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-bar",
                },
            };
            info.registrationTime = 23423;
            info.narSize = 34878;
            info.sigs = {
                "fake-sig-1",
                "fake-sig-2",
            },
            info;
        }),
        ({
            ValidPathInfo info {
                *LibStoreTest::store,
                "foo",
                FixedOutputInfo {
                    .method = FileIngestionMethod::Recursive,
                    .hash = hashString(HashType::SHA256, "(...)"),
                    .references = {
                        .others = {
                            StorePath {
                                "g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-bar",
                            },
                        },
                        .self = true,
                    },
                },
                Hash::parseSRI("sha256-FePFYIlMuycIXPZbWi7LGEiMmZSX9FMbaQenWBzm1Sc="),
            };
            info.registrationTime = 23423;
            info.narSize = 34878;
            info;
        }),
    }))

VERSIONED_CHARACTERIZATION_TEST(
    WorkerProtoTest,
    optionalTrustedFlag,
    "optional-trusted-flag",
    defaultVersion,
    (std::tuple<std::optional<TrustedFlag>, std::optional<TrustedFlag>, std::optional<TrustedFlag>> {
        std::nullopt,
        std::optional { Trusted },
        std::optional { NotTrusted },
    }))

VERSIONED_CHARACTERIZATION_TEST(
    WorkerProtoTest,
    vector,
    "vector",
    defaultVersion,
    (std::tuple<std::vector<std::string>, std::vector<std::string>, std::vector<std::string>, std::vector<std::vector<std::string>>> {
        { },
        { "" },
        { "", "foo", "bar" },
        { {}, { "" }, { "", "1", "2" } },
    }))

VERSIONED_CHARACTERIZATION_TEST(
    WorkerProtoTest,
    set,
    "set",
    defaultVersion,
    (std::tuple<std::set<std::string>, std::set<std::string>, std::set<std::string>, std::set<std::set<std::string>>> {
        { },
        { "" },
        { "", "foo", "bar" },
        { {}, { "" }, { "", "1", "2" } },
    }))

VERSIONED_CHARACTERIZATION_TEST(
    WorkerProtoTest,
    optionalStorePath,
    "optional-store-path",
    defaultVersion,
    (std::tuple<std::optional<StorePath>, std::optional<StorePath>> {
        std::nullopt,
        std::optional {
            StorePath { "g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-foo-bar" },
        },
    }))

VERSIONED_CHARACTERIZATION_TEST(
    WorkerProtoTest,
    optionalContentAddress,
    "optional-content-address",
    defaultVersion,
    (std::tuple<std::optional<ContentAddress>, std::optional<ContentAddress>> {
        std::nullopt,
        std::optional {
            ContentAddress {
                .method = FileIngestionMethod::Flat,
                .hash = hashString(HashType::SHA1, "blob blob..."),
            },
        },
    }))

}
