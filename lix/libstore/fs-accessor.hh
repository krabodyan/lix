#pragma once
///@file

#include "lix/libutil/result.hh"
#include "lix/libutil/types.hh"
#include <kj/async.h>

namespace nix {

/**
 * An abstract class for accessing a filesystem-like structure, such
 * as a (possibly remote) Nix store or the contents of a NAR file.
 */
class FSAccessor
{
public:
    enum Type { tMissing, tRegular, tSymlink, tDirectory };

    struct Stat
    {
        Type type = tMissing;
        /**
         * regular files only
         */
        uint64_t fileSize = 0;
        /**
         * regular files only
         */
        bool isExecutable = false; // regular files only
        /**
         * regular files only
         */
        uint64_t narOffset = 0; // regular files only
    };

    virtual ~FSAccessor() { }

    virtual kj::Promise<Result<Stat>> stat(const Path & path) = 0;

    virtual kj::Promise<Result<StringSet>> readDirectory(const Path & path) = 0;

    /**
     * Read a file inside the store.
     *
     * If `requireValidPath` is set to `true` (the default), the path must be
     * inside a valid store path, otherwise it just needs to be physically
     * present (but not necessarily properly registered)
     */
    virtual kj::Promise<Result<std::string>>
    readFile(const Path & path, bool requireValidPath = true) = 0;

    virtual kj::Promise<Result<std::string>> readLink(const Path & path) = 0;
};

}
