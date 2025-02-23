#include "lix/libmain/crash-handler.hh"
#include "lix/libutil/fmt.hh"
#include "lix/libutil/logging.hh"

#include <boost/core/demangle.hpp>
#include <exception>

namespace nix {

namespace {

void onTerminate()
{
    logFatal("Lix crashed. This is a bug. We would appreciate if you report it along with what caused it at https://git.lix.systems/lix-project/lix/issues with the following information included:\n");
    try {
        std::exception_ptr eptr = std::current_exception();
        if (eptr) {
            std::rethrow_exception(eptr);
        } else {
            logFatal("std::terminate() called without exception");
        }
    } catch (const std::exception & ex) {
        logFatal(fmt("Exception: %s: %s", boost::core::demangle(typeid(ex).name()), ex.what()));
    } catch (...) {
        logFatal("Unknown exception! Spooky.");
    }

    logFatal("Stack trace:");
    logFatal(getStackTrace());

    std::abort();
}
}

void registerCrashHandler()
{
    // DO NOT use this for signals. Boost stacktrace is very much not
    // async-signal-safe, and in a world with ASLR, addr2line is pointless.
    //
    // If you want signals, set up a minidump system and do it out-of-process.
    std::set_terminate(onTerminate);
}
}
