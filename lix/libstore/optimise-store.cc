#include "lix/libstore/local-store.hh"
#include "lix/libstore/globals.hh"
#include "lix/libutil/async.hh"
#include "lix/libutil/result.hh"
#include "lix/libutil/signals.hh"
#include "lix/libutil/strings.hh"

#include <cstring>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#if __APPLE__
#include <regex>
#endif

namespace nix {


static void makeWritable(const Path & path)
{
    auto st = lstat(path);
    if (chmod(path.c_str(), st.st_mode | S_IWUSR) == -1)
        throw SysError("changing writability of '%1%'", path);
}


struct MakeReadOnly
{
    Path path;
    MakeReadOnly(const PathView path) : path(path) { }
    ~MakeReadOnly()
    {
        try {
            /* This will make the path read-only. */
            if (path != "") canonicaliseTimestampAndPermissions(path);
        } catch (...) {
            ignoreExceptionInDestructor();
        }
    }
};


LocalStore::InodeHash LocalStore::loadInodeHash()
{
    debug("loading hash inodes in memory");
    InodeHash inodeHash;

    AutoCloseDir dir(opendir(linksDir.c_str()));
    if (!dir) throw SysError("opening directory '%1%'", linksDir);

    struct dirent * dirent;
    while (errno = 0, dirent = readdir(dir.get())) { /* sic */
        checkInterrupt();
        // We don't care if we hit non-hash files, anything goes
        inodeHash.insert(dirent->d_ino);
    }
    if (errno) throw SysError("reading directory '%1%'", linksDir);

    printMsg(lvlTalkative, "loaded %1% hash inodes", inodeHash.size());

    return inodeHash;
}


Strings LocalStore::readDirectoryIgnoringInodes(const Path & path, const InodeHash & inodeHash)
{
    Strings names;

    AutoCloseDir dir(opendir(path.c_str()));
    if (!dir) throw SysError("opening directory '%1%'", path);

    struct dirent * dirent;
    while (errno = 0, dirent = readdir(dir.get())) { /* sic */
        checkInterrupt();

        if (inodeHash.count(dirent->d_ino)) {
            debug("'%1%' is already linked", dirent->d_name);
            continue;
        }

        std::string name = dirent->d_name;
        if (name == "." || name == "..") continue;
        names.push_back(name);
    }
    if (errno) throw SysError("reading directory '%1%'", path);

    return names;
}


void LocalStore::optimisePath_(Activity * act, OptimiseStats & stats,
    const Path & path, InodeHash & inodeHash, RepairFlag repair)
{
    checkInterrupt();

    auto st = lstat(path);

#if __APPLE__
    /* HFS/macOS has some undocumented security feature disabling hardlinking for
       special files within .app dirs. *.app/Contents/PkgInfo and
       *.app/Contents/Resources/\*.lproj seem to be the only paths affected. See
       https://github.com/NixOS/nix/issues/1443 for more discussion. */

    if (std::regex_search(path, std::regex("\\.app/Contents/.+$")))
    {
        debug("'%1%' is not allowed to be linked in macOS", path);
        return;
    }
#endif

    if (S_ISDIR(st.st_mode)) {
        Strings names = readDirectoryIgnoringInodes(path, inodeHash);
        for (auto & i : names)
            optimisePath_(act, stats, path + "/" + i, inodeHash, repair);
        return;
    }

    /* We can hard link regular files and maybe symlinks. */
    if (!S_ISREG(st.st_mode)
#if CAN_LINK_SYMLINK
        && !S_ISLNK(st.st_mode)
#endif
        ) return;

    /* Sometimes SNAFUs can cause files in the Nix store to be
       modified, in particular when running programs as root under
       NixOS (example: $fontconfig/var/cache being modified).  Skip
       those files.  FIXME: check the modification time. */
    if (S_ISREG(st.st_mode) && (st.st_mode & S_IWUSR)) {
        warn("skipping suspicious writable file '%1%'", path);
        return;
    }

    /* This can still happen on top-level files. */
    if (st.st_nlink > 1 && inodeHash.count(st.st_ino)) {
        debug("'%s' is already linked, with %d other file(s)", path, st.st_nlink - 2);
        return;
    }

    /* Hash the file.  Note that hashPath() returns the hash over the
       NAR serialisation, which includes the execute bit on the file.
       Thus, executable and non-executable files with the same
       contents *won't* be linked (which is good because otherwise the
       permissions would be screwed up).

       Also note that if `path' is a symlink, then we're hashing the
       contents of the symlink (i.e. the result of readlink()), not
       the contents of the target (which may not even exist). */
    Hash hash = hashPath(HashType::SHA256, path).first;
    debug("'%1%' has hash '%2%'", path, hash.to_string(Base::Base32, true));

    /* Check if this is a known hash. */
    Path linkPath = linksDir + "/" + hash.to_string(Base::Base32, false);
    auto stLinkOpt = maybeLstat(linkPath);

    /* Maybe delete the link, if it has been corrupted. */
    if (stLinkOpt) {
        if (st.st_size != stLinkOpt->st_size
            || (repair && hash != hashPath(HashType::SHA256, linkPath).first))
        {
            // XXX: Consider overwriting linkPath with our valid version.
            warn("removing corrupted link '%s'", linkPath);
            warn("There may be more corrupted paths."
                 "\nYou should run `nix-store --verify --check-contents --repair` to fix them all");
            if (unlink(linkPath.c_str()) == -1 && errno != ENOENT)
                throw SysError("cannot unlink '%1%'", linkPath);
            stLinkOpt.reset();
        }
    }

    if (!stLinkOpt) {
        /* Nope, create a hard link in the links directory. */
        if (link(path.c_str(), linkPath.c_str()) == 0) {
            inodeHash.insert(st.st_ino);
            return;
        }

        switch (errno) {
        case EEXIST:
            /* Fall through if another process created ‘linkPath’ before
               we did. */
            stLinkOpt = lstat(linkPath);
            break;

        case ENOSPC:
            /* On ext4, that probably means the directory index is
               full.  When that happens, it's fine to ignore it: we
               just effectively disable deduplication of this
               file.  */
            printInfo("cannot link '%s' to '%s': %s", linkPath, path, strerror(errno));
            return;

        default:
            throw SysError("cannot link '%1%' to '%2%'", linkPath, path);
        }
    }

    /* Yes!  We've seen a file with the same contents.  Replace the
       current file with a hard link to that file. */
    if (st.st_ino == stLinkOpt->st_ino) {
        debug("'%1%' is already linked to '%2%'", path, linkPath);
        return;
    }

    printMsg(lvlTalkative, "linking '%1%' to '%2%'", path, linkPath);

    /* Make the containing directory writable, but only if it's not
       the store itself (we don't want or need to mess with its
       permissions). */
    const Path dirOfPath(dirOf(path));
    bool mustToggle = dirOfPath != config().realStoreDir.get();
    if (mustToggle) makeWritable(dirOfPath);

    /* When we're done, make the directory read-only again and reset
       its timestamp back to 0. */
    MakeReadOnly makeReadOnly(mustToggle ? dirOfPath : "");

    Path tempLink = makeTempPath(config().realStoreDir, "/.tmp-link");
    unlink(tempLink.c_str()); // just in case; ignore errors

    if (link(linkPath.c_str(), tempLink.c_str()) == -1) {
        if (errno == EMLINK) {
            /* Too many links to the same file (>= 32000 on most file
               systems).  This is likely to happen with empty files.
               Just shrug and ignore. */
            if (st.st_size)
                printInfo("'%1%' has maximum number of links", linkPath);
            return;
        }
        throw SysError("cannot link '%1%' to '%2%'", tempLink, linkPath);
    }

    /* Atomically replace the old file with the new hard link. */
    try {
        renameFile(tempLink, path);
    } catch (SysError & e) {
        if (unlink(tempLink.c_str()) == -1)
            printError("unable to unlink '%1%': %2%", tempLink, strerror(errno));
        if (errno == EMLINK) {
            /* Some filesystems generate too many links on the rename,
               rather than on the original link.  (Probably it
               temporarily increases the st_nlink field before
               decreasing it again.) */
            debug("'%s' has reached maximum number of links", linkPath);
            return;
        }
        throw;
    }

    stats.filesLinked++;
    stats.bytesFreed += st.st_size;
    stats.blocksFreed += st.st_blocks;

    if (act)
        act->result(resFileLinked, st.st_size, st.st_blocks);
}


kj::Promise<Result<void>> LocalStore::optimiseStore(OptimiseStats & stats)
try {
    Activity act(*logger, actOptimiseStore);

    auto paths = TRY_AWAIT(queryAllValidPaths());
    InodeHash inodeHash = loadInodeHash();

    act.progress(0, paths.size());

    uint64_t done = 0;

    for (auto & i : paths) {
        TRY_AWAIT(addTempRoot(i));
        if (!TRY_AWAIT(isValidPath(i))) continue; /* path was GC'ed, probably */
        {
            Activity act(*logger, lvlTalkative, actUnknown, fmt("optimising path '%s'", printStorePath(i)));
            optimisePath_(
                &act,
                stats,
                config().realStoreDir + "/" + std::string(i.to_string()),
                inodeHash,
                NoRepair
            );
        }
        done++;
        act.progress(done, paths.size());
    }
    co_return result::success();
} catch (...) {
    co_return result::current_exception();
}

kj::Promise<Result<void>> LocalStore::optimiseStore()
try {
    OptimiseStats stats;

    TRY_AWAIT(optimiseStore(stats));

    printInfo("%s freed by hard-linking %d files",
        showBytes(stats.bytesFreed),
        stats.filesLinked);
    co_return result::success();
} catch (...) {
    co_return result::current_exception();
}

void LocalStore::optimisePath(const Path & path, RepairFlag repair)
{
    OptimiseStats stats;
    InodeHash inodeHash;

    if (settings.autoOptimiseStore) optimisePath_(nullptr, stats, path, inodeHash, repair);
}


}
