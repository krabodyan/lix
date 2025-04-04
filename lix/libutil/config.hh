#pragma once
///@file

#include <cassert>
#include <map>
#include <set>

#include "lix/libutil/json-fwd.hh"
#include "lix/libutil/types.hh"
#include "lix/libutil/experimental-features.hh"
#include "lix/libutil/deprecated-features.hh"
#include "lix/libutil/apply-config-options.hh"

namespace nix {

/**
 * The Config class provides Lix runtime configurations.
 *
 * What is a Configuration?
 *   A collection of uniquely named Settings.
 *
 * What is a Setting?
 *   Each property that you can set in a configuration corresponds to a
 *   `Setting`. A setting records value and description of a property
 *   with a default and optional aliases.
 *
 * A valid configuration consists of settings that are registered to a
 * `Config` object instance:
 *
 *   Config config;
 *   Setting<std::string> systemSetting{&config, "x86_64-linux", "system", "the current system"};
 *
 * The above creates a `Config` object and registers a setting called "system"
 * via the variable `systemSetting` with it. The setting defaults to the string
 * "x86_64-linux", it's description is "the current system". All of the
 * registered settings can then be accessed as shown below:
 *
 *   std::map<std::string, Config::SettingInfo> settings;
 *   config.getSettings(settings);
 *   config["system"].description == "the current system"
 *   config["system"].value == "x86_64-linux"
 *
 *
 * The above retrieves all currently known settings from the `Config` object
 * and adds them to the `settings` map.
 */

class Args;
class AbstractSetting;

class AbstractConfig
{
protected:
    StringMap unknownSettings;

    AbstractConfig(StringMap initials = {});

public:

    /**
     * Sets the value referenced by `name` to `value`. Returns true if the
     * setting is known, false otherwise.
     */
    virtual bool set(const std::string & name, const std::string & value, const ApplyConfigOptions & options = {}) = 0;

    struct SettingInfo
    {
        std::string value;
        std::string description;
    };

    /**
     * Adds the currently known settings to the given result map `res`.
     * - res: map to store settings in
     * - overriddenOnly: when set to true only overridden settings will be added to `res`
     */
    virtual void getSettings(std::map<std::string, SettingInfo> & res, bool overriddenOnly = false) = 0;

    /**
     * Parses the configuration in `contents` and applies it
     * - contents: configuration contents to be parsed and applied
     * - path: location of the configuration file
     */
    void applyConfig(const std::string & contents, const ApplyConfigOptions & options = {});

    /**
     * Resets the `overridden` flag of all Settings
     */
    virtual void resetOverridden() = 0;

    /**
     * Outputs all settings to JSON
     * - out: JSONObject to write the configuration to
     */
    virtual JSON toJSON() = 0;

    /**
     * Converts settings to `Args` to be used on the command line interface
     * - args: args to write to
     * - category: category of the settings
     */
    virtual void convertToArgs(Args & args, const std::string & category) = 0;

    /**
     * Logs a warning for each unregistered setting
     */
    void warnUnknownSettings();

    /**
     * Re-applies all previously attempted changes to unknown settings
     */
    void reapplyUnknownSettings();
};

/**
 * A class to simplify providing configuration settings. The typical
 * use is to inherit Config and add Setting<T> members:
 *
 * class MyClass : private Config
 * {
 *   Setting<int> foo{this, 123, "foo", "the number of foos to use"};
 *   Setting<std::string> bar{this, "blabla", "bar", "the name of the bar"};
 *
 *   MyClass() : Config(readConfigFile("/etc/my-app.conf"))
 *   {
 *     std::cout << foo << "\n"; // will print 123 unless overridden
 *   }
 * };
 */
class Config : public AbstractConfig
{
    friend class AbstractSetting;

public:

    struct SettingData
    {
        bool isAlias;
        AbstractSetting * setting;
    };

    using Settings = std::map<std::string, SettingData>;

private:

    Settings _settings;

public:

    Config(StringMap initials = {});

    bool set(const std::string & name, const std::string & value, const ApplyConfigOptions & options = {}) override;

    void addSetting(AbstractSetting * setting);

    void getSettings(std::map<std::string, SettingInfo> & res, bool overriddenOnly = false) override;

    void resetOverridden() override;

    JSON toJSON() override;

    void convertToArgs(Args & args, const std::string & category) override;
};

class AbstractSetting
{
    friend class Config;

public:

    struct deprecated_t {
        explicit deprecated_t() = default;
    };

    const std::string name;
    const std::string description;
    const std::set<std::string> aliases;

    int created = 123;

    bool overridden = false;

    std::optional<ExperimentalFeature> experimentalFeature;

protected:

    AbstractSetting(
        const std::string & name,
        const std::string & description,
        const std::set<std::string> & aliases,
        std::optional<ExperimentalFeature> experimentalFeature = std::nullopt);

    virtual ~AbstractSetting();

    virtual void set(const std::string & value, bool append = false, const ApplyConfigOptions & options = {}) = 0;

    /**
     * Whether the type is appendable; i.e. whether the `append`
     * parameter to `set()` is allowed to be `true`.
     */
    virtual bool isAppendable() = 0;

    virtual std::string to_string() const = 0;

    JSON toJSON();

    virtual std::map<std::string, JSON> toJSONObject() const;

    virtual void convertToArg(Args & args, const std::string & category);

    bool isOverridden() const;

};

/**
 * A setting of type T.
 */
template<typename T>
class BaseSetting : public AbstractSetting
{
protected:

    T value;
    const T defaultValue;
    const bool documentDefault;
    const bool deprecated;

    /**
     * Parse the string into a `T`.
     *
     * Used by `set()`.
     */
    virtual T parse(const std::string & str, const ApplyConfigOptions & options) const;

    /**
     * Append or overwrite `value` with `newValue`.
     *
     * Some types to do not support appending in which case `append`
     * should never be passed. The default handles this case.
     *
     * @param append Whether to append or overwrite.
     */
    virtual void appendOrSet(T newValue, bool append, const ApplyConfigOptions & options);

public:

    BaseSetting(const T & def,
        const bool documentDefault,
        const std::string & name,
        const std::string & description,
        const std::set<std::string> & aliases = {},
        std::optional<ExperimentalFeature> experimentalFeature = std::nullopt,
        bool deprecated = false)
        : AbstractSetting(name, description, aliases, experimentalFeature)
        , value(def)
        , defaultValue(def)
        , documentDefault(documentDefault)
        , deprecated(deprecated)
    { }

    operator const T &() const { return value; }
    const T & get() const { return value; }
    template<typename U>
    bool operator ==(const U & v2) const { return value == v2; }
    template<typename U>
    bool operator !=(const U & v2) const { return value != v2; }
    template<typename U>
    void setDefault(const U & v) { if (!overridden) value = v; }

    /**
     * Require any experimental feature the setting depends on
     *
     * Uses `parse()` to get the value from `str`, and `appendOrSet()`
     * to set it.
     */
    void set(const std::string & str, bool append = false, const ApplyConfigOptions & options = {}) override final;

    void override(const T & v);

    /**
     * C++ trick; This is template-specialized to compile-time indicate whether
     * the type is appendable.
     */
    struct trait;

    /**
     * Always defined based on the C++ magic
     * with `trait` above.
     */
    bool isAppendable() override final;

    std::string to_string() const override;

    void convertToArg(Args & args, const std::string & category) override;

    std::map<std::string, JSON> toJSONObject() const override;
};

template<typename T>
std::ostream & operator <<(std::ostream & str, const BaseSetting<T> & opt)
{
    return str << static_cast<const T &>(opt);
}

template<typename T>
bool operator ==(const T & v1, const BaseSetting<T> & v2) { return v1 == static_cast<const T &>(v2); }

template<typename T>
class Setting : public BaseSetting<T>
{
public:
    Setting(Config * options,
        const T & def,
        const std::string & name,
        const std::string & description,
        const std::set<std::string> & aliases = {},
        const bool documentDefault = true,
        std::optional<ExperimentalFeature> experimentalFeature = std::nullopt,
        bool deprecated = false)
        : BaseSetting<T>(def, documentDefault, name, description, aliases, std::move(experimentalFeature), deprecated)
    {
        options->addSetting(this);
    }
};

/**
 * A special setting for Paths.
 * These are automatically canonicalised (e.g. "/foo//bar/" becomes "/foo/bar").
 * The empty string is not permitted when a path is required.
 */
template<typename T>
class PathsSetting : public BaseSetting<T>
{
public:
    PathsSetting(Config * options,
        const T & def,
        const std::string & name,
        const std::string & description,
        const std::set<std::string> & aliases = {},
        const bool documentDefault = true,
        std::optional<ExperimentalFeature> experimentalFeature = std::nullopt,
        bool deprecated = false)
        : BaseSetting<T>(def, documentDefault, name, description, aliases, std::move(experimentalFeature), deprecated)
    {
        options->addSetting(this);
    }

    T parse(const std::string & str, const ApplyConfigOptions & options) const override;
};


struct GlobalConfig : public AbstractConfig
{
    typedef std::vector<Config*> ConfigRegistrations;
    static ConfigRegistrations * configRegistrations;

    bool set(const std::string & name, const std::string & value, const ApplyConfigOptions & options = {}) override;

    void getSettings(std::map<std::string, SettingInfo> & res, bool overriddenOnly = false) override;

    void resetOverridden() override;

    JSON toJSON() override;

    /**
     * Outputs all settings in a key-value pair format suitable to be used as
     * `nix.conf`
     */
    std::string toKeyValue(bool overriddenOnly = false);

    void convertToArgs(Args & args, const std::string & category) override;

    struct Register
    {
        Register(Config * config);
    };
};

extern GlobalConfig globalConfig;


struct FeatureSettings : Config {
    #include "feature-settings.gen.inc"

    /**
     * Check whether the given experimental feature is enabled.
     */
    bool isEnabled(const ExperimentalFeature &) const;

    /**
     * Require an experimental feature be enabled, throwing an error if it is
     * not.
     */
    void require(const ExperimentalFeature &) const;

    /**
     * `std::nullopt` pointer means no feature, which means there is nothing that could be
     * disabled, and so the function returns true in that case.
     */
    bool isEnabled(const std::optional<ExperimentalFeature> &) const;

    /**
     * `std::nullopt` pointer means no feature, which means there is nothing that could be
     * disabled, and so the function does nothing in that case.
     */
    void require(const std::optional<ExperimentalFeature> &) const;

    /**
     * Check whether the given deprecated feature is enabled.
     */
    bool isEnabled(const DeprecatedFeature &) const;

    /**
     * Require an deprecated feature be enabled, throwing an error if it is
     * not.
     */
    void require(const DeprecatedFeature &) const;

    /**
     * `std::nullopt` pointer means no feature, which means there is nothing that could be
     * disabled, and so the function returns true in that case.
     */
    bool isEnabled(const std::optional<DeprecatedFeature> &) const;

    /**
     * `std::nullopt` pointer means no feature, which means there is nothing that could be
     * disabled, and so the function does nothing in that case.
     */
    void require(const std::optional<DeprecatedFeature> &) const;
};

// FIXME: don't use a global variable.
extern FeatureSettings& featureSettings;

// Aliases to `featureSettings` for not having to change the name in the code everywhere
using ExperimentalFeatureSettings = FeatureSettings;
extern FeatureSettings experimentalFeatureSettings;
}
