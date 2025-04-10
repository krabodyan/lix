#pragma once
/**
 * @file
 *
 * Template implementations (as opposed to mere declarations).
 *
 * This file is an exmample of the "impl.hh" pattern. See the
 * contributing guide.
 *
 * One only needs to include this when one is declaring a
 * `BaseClass<CustomType>` setting, or as derived class of such an
 * instantiation.
 */

#include "lix/libutil/args.hh"
#include "lix/libutil/config.hh"
#include "lix/libutil/logging.hh"
#include "lix/libutil/strings.hh"

namespace nix {

template<> struct BaseSetting<Strings>::trait
{
    static constexpr bool appendable = true;
};
template<> struct BaseSetting<StringSet>::trait
{
    static constexpr bool appendable = true;
};
template<> struct BaseSetting<StringMap>::trait
{
    static constexpr bool appendable = true;
};
template<> struct BaseSetting<ExperimentalFeatures>::trait
{
    static constexpr bool appendable = true;
};
template<> struct BaseSetting<DeprecatedFeatures>::trait
{
    static constexpr bool appendable = true;
};

template<typename T>
struct BaseSetting<T>::trait
{
    static constexpr bool appendable = false;
};

template<typename T>
bool BaseSetting<T>::isAppendable()
{
    return trait::appendable;
}

template<> void BaseSetting<Strings>::appendOrSet(Strings newValue, bool append, const ApplyConfigOptions & options);
template<> void BaseSetting<StringSet>::appendOrSet(StringSet newValue, bool append, const ApplyConfigOptions & options);
template<> void BaseSetting<StringMap>::appendOrSet(StringMap newValue, bool append, const ApplyConfigOptions & options);
template<> void BaseSetting<ExperimentalFeatures>::appendOrSet(ExperimentalFeatures newValue, bool append, const ApplyConfigOptions & options);
template<> void BaseSetting<DeprecatedFeatures>::appendOrSet(DeprecatedFeatures newValue, bool append, const ApplyConfigOptions & options);

template<typename T>
void BaseSetting<T>::appendOrSet(T newValue, bool append, const ApplyConfigOptions & options)
{
    static_assert(
        !trait::appendable,
        "using default `appendOrSet` implementation with an appendable type");
    assert(!append);
    value = std::move(newValue);
}

template<typename T>
void BaseSetting<T>::set(const std::string & str, bool append, const ApplyConfigOptions & options)
{
    if (experimentalFeatureSettings.isEnabled(experimentalFeature)) {
        auto parsed = parse(str, options);
        if (deprecated && (append || parsed != value)) {
            warn("deprecated setting '%s' found (set to '%s')", name, str);
        }
        overridden = true;
        appendOrSet(std::move(parsed), append, options);
    } else {
        assert(experimentalFeature);
        warn("Ignoring setting '%s' because experimental feature '%s' is not enabled",
            name,
            showExperimentalFeature(*experimentalFeature));
    }
}

template<typename T>
void BaseSetting<T>::override(const T & v)
{
    overridden = true;
    value = v;
}

template<> void BaseSetting<bool>::convertToArg(Args & args, const std::string & category);

template<typename T>
void BaseSetting<T>::convertToArg(Args & args, const std::string & category)
{
    args.addFlag({
        .longName = name,
        .description = fmt("Set the `%s` setting.", name),
        .category = category,
        .labels = {"value"},
        .handler = {[this](std::string s) { set(s); }},
        .experimentalFeature = experimentalFeature,
    });

    if (isAppendable())
        args.addFlag({
            .longName = "extra-" + name,
            .description = fmt("Append to the `%s` setting.", name),
            .category = category,
            .labels = {"value"},
            .handler = {[this](std::string s) { set(s, true); }},
            .experimentalFeature = experimentalFeature,
        });
}

#define DECLARE_CONFIG_SERIALISER(TY) \
    template<> TY BaseSetting< TY >::parse(const std::string & str, const ApplyConfigOptions & options) const; \
    template<> std::string BaseSetting< TY >::to_string() const;

DECLARE_CONFIG_SERIALISER(std::string)
DECLARE_CONFIG_SERIALISER(std::optional<std::string>)
DECLARE_CONFIG_SERIALISER(std::optional<uint16_t>)
DECLARE_CONFIG_SERIALISER(bool)
DECLARE_CONFIG_SERIALISER(Strings)
DECLARE_CONFIG_SERIALISER(StringSet)
DECLARE_CONFIG_SERIALISER(StringMap)
DECLARE_CONFIG_SERIALISER(ExperimentalFeatures)
DECLARE_CONFIG_SERIALISER(DeprecatedFeatures)

template<typename T>
T BaseSetting<T>::parse(const std::string & str, const ApplyConfigOptions & options) const
{
    static_assert(std::is_integral<T>::value, "Integer required.");

    if (auto n = string2Int<T>(str))
        return *n;
    else
        throw UsageError("setting '%s' has invalid value '%s'", name, str);
}

template<typename T>
std::string BaseSetting<T>::to_string() const
{
    static_assert(std::is_integral<T>::value, "Integer required.");

    return std::to_string(value);
}

}
