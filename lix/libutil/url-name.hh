#pragma once
///@file url-name.hh, for some hueristic-ish URL parsing.

#include <string>
#include <optional>

#include "lix/libutil/url.hh"

namespace nix {

/**
 * Try to extract a reasonably unique and meaningful, human-readable
 * name of a flake output from a parsed URL.
 * When nullopt is returned, the callsite should use information available
 * to it outside of the URL to determine a useful name.
 * This is a heuristic approach intended for user interfaces.
 * @return nullopt if the extracted name is not useful to identify a
 * flake output, for example because it is empty or "default".
 * Otherwise returns the extracted name.
 */
std::optional<std::string> getNameFromURL(ParsedURL const & url);

}
