#pragma once
///@file

#include "lix/libexpr/attr-set.hh"
#include "lix/libexpr/eval-error.hh"
#include "lix/libexpr/gc-alloc.hh"
#include "lix/libutil/generator.hh"
#include "lix/libutil/types.hh"
#include "lix/libexpr/value.hh"
#include "lix/libexpr/nixexpr.hh"
#include "lix/libexpr/symbol-table.hh"
#include "lix/libutil/config.hh"
#include "lix/libutil/experimental-features.hh"
#include "lix/libexpr/search-path.hh"
#include "lix/libexpr/repl-exit-status.hh"
#include "lix/libutil/backed-string-view.hh"

#include <map>
#include <optional>
#include <unordered_map>
#include <functional>

namespace nix {

/**
 * We put a limit on primop arity because it lets us use a fixed size array on
 * the stack. 8 is already an impractical number of arguments. Use an attrset
 * argument for such overly complicated functions.
 */
constexpr size_t maxPrimOpArity = 8;

class Store;
class EvalState;
class StorePath;
struct SingleDerivedPath;
enum RepairFlag : bool;
struct MemoryInputAccessor;
namespace eval_cache {
    class EvalCache;
}

/**
 * Function that implements a primop.
 */
using PrimOpImpl = void(EvalState & state, PosIdx pos, Value ** args, Value & v);

/**
 * Info about a primitive operation, and its implementation
 */
struct PrimOp
{
    /**
     * Name of the primop. `__` prefix is treated specially.
     */
    std::string name;

    /**
     * Names of the parameters of a primop, for primops that take a
     * fixed number of arguments to be substituted for these parameters.
     */
    std::vector<std::string> args;

    /**
     * Aritiy of the primop.
     *
     * If `args` is not empty, this field will be computed from that
     * field instead, so it doesn't need to be manually set.
     */
    size_t arity = 0;

    /**
     * Optional free-form documentation about the primop.
     */
    const char * doc = nullptr;

    /**
     * Implementation of the primop.
     */
    std::function<PrimOpImpl> fun;

    /**
     * Optional experimental for this to be gated on.
     */
    std::optional<ExperimentalFeature> experimentalFeature;

    /**
     * Validity check to be performed by functions that introduce primops,
     * such as RegisterPrimOp() and Value::mkPrimOp().
     */
    void check();
};

std::ostream & operator<<(std::ostream & output, PrimOp & primOp);

/**
 * Info about a constant
 */
struct Constant
{
    /**
     * Optional type of the constant (known since it is a fixed value).
     *
     * @todo we should use an enum for this.
     */
    ValueType type = nThunk;

    /**
     * Optional free-form documentation about the constant.
     */
    const char * doc = nullptr;

    /**
     * Whether the constant is impure, and not available in pure mode.
     */
    bool impureOnly = false;
};

using ValMap = GcMap<std::string, Value *>;

struct Env
{
    Env * up;
    Value * values[0];
};

void printEnvBindings(const EvalState &es, const Expr & expr, const Env & env);
void printEnvBindings(const SymbolTable & st, const StaticEnv & se, const Env & env, int lvl = 0);

std::unique_ptr<ValMap> mapStaticEnvBindings(const SymbolTable & st, const StaticEnv & se, const Env & env);

void copyContext(const Value & v, NixStringContext & context);


std::string printValue(EvalState & state, Value & v);
std::ostream & operator << (std::ostream & os, const ValueType t);


/**
 * Initialise the evaluator (including Boehm GC, if applicable).
 */
void initLibExpr();


struct RegexCache;

std::shared_ptr<RegexCache> makeRegexCache();

struct DebugTrace {
    std::shared_ptr<Pos> pos;
    const Expr & expr;
    const Env & env;
    HintFmt hint;
    bool isError;
    std::shared_ptr<const DebugTrace> parent;
};

struct DebugState
{
private:
    std::weak_ptr<const DebugTrace> latestTrace;

public:
    std::function<ReplExitStatus(EvalState & es, ValMap const & extraEnv)> repl;
    bool stop = false;
    bool inDebugger = false;
    std::map<const Expr *, const std::shared_ptr<const StaticEnv>> exprEnvs;
    int trylevel = 0;

    explicit DebugState(std::function<ReplExitStatus(EvalState & es, ValMap const & extraEnv)> repl)
        : repl(repl)
    {
        assert(repl);
    }

    void runDebugRepl(
        EvalState & evalState, const EvalError * error, const Env & env, const Expr & expr
    );

    const std::shared_ptr<const StaticEnv> staticEnvFor(const Expr & expr) const
    {
        if (auto i = exprEnvs.find(&expr); i != exprEnvs.end()) {
            return i->second;
        }
        return nullptr;
    }

    class TraceFrame
    {
        friend struct DebugState;
        template<class T>
        friend class EvalErrorBuilder;

        // holds both the data for this frame *and* a deleter that pulls this frame
        // off the trace stack. EvalErrorBuilder uses this for withFrame fake trace
        // frames, and to avoid needing to see this class definition in its header.
        const std::shared_ptr<const DebugTrace> entry = nullptr;

        explicit TraceFrame(std::shared_ptr<const DebugTrace> entry): entry(std::move(entry)) {}

    public:
        TraceFrame(std::nullptr_t) {}
    };

    TraceFrame addTrace(DebugTrace t);

    /// Enumerates the debug frame stack, from the current frame to the root frame.
    /// All values are guaranteed to not be null, but must be pointers because C++.
    Generator<const DebugTrace *> traces()
    {
        for (auto current = latestTrace.lock(); current; current = current->parent) {
            co_yield current.get();
        }
    }
};

struct StaticSymbols
{
    const Symbol outPath, drvPath, type, meta, name, value, system, overrides, outputs, outputName,
        ignoreNulls, file, line, column, functor, toString, right, wrong, structuredAttrs,
        allowedReferences, allowedRequisites, disallowedReferences, disallowedRequisites, maxSize,
        maxClosureSize, builder, args, contentAddressed, impure, outputHash, outputHashAlgo,
        outputHashMode, recurseForDerivations, description, self, epsilon, startSet, operator_, key,
        path, prefix, outputSpecified;

    const Expr::AstSymbols exprSymbols;

    explicit StaticSymbols(SymbolTable & symbols);
};


class EvalState
{
public:
    SymbolTable symbols;
    PosTable positions;
    const StaticSymbols s;

    /**
     * If set, force copying files to the Nix store even if they
     * already exist there.
     */
    RepairFlag repair;

    /**
     * The allowed filesystem paths in restricted or pure evaluation
     * mode.
     */
    std::optional<PathSet> allowedPaths;

    const SourcePath derivationInternal;

    /**
     * Store used to materialise .drv files.
     */
    const ref<Store> store;

    /**
     * Store used to build stuff.
     */
    const ref<Store> buildStore;

    RootValue vCallFlake = nullptr;
    RootValue vImportedDrvToDerivation = nullptr;

    std::unique_ptr<DebugState> debug;

    template<class T, typename... Args>
    [[nodiscard, gnu::noinline]]
    EvalErrorBuilder<T> & error(const Args & ... args) {
        // `EvalErrorBuilder::debugThrow` performs the corresponding `delete`.
        return *new EvalErrorBuilder<T>(*this, args...);
    }

    /**
     * A cache for evaluation caches, so as to reuse the same root value if possible
     */
    std::map<const Hash, ref<eval_cache::EvalCache>> evalCaches;

private:

    /* Cache for calls to addToStore(); maps source paths to the store
       paths. */
    std::map<SourcePath, StorePath> srcToStore;

    /**
     * A cache from path names to parse trees.
     */
    using FileParseCache = GcMap<SourcePath, Expr *>;
    FileParseCache fileParseCache;

    /**
     * A cache from path names to values.
     */
    using FileEvalCache = GcMap<SourcePath, Value>;
    FileEvalCache fileEvalCache;

    SearchPath searchPath;

    std::map<std::string, std::optional<std::string>> searchPathResolved;

    /**
     * Cache used by checkSourcePath().
     */
    std::unordered_map<Path, SourcePath> resolvedPaths;

    /**
     * Cache used by prim_match().
     */
    std::shared_ptr<RegexCache> regexCache;

#if HAVE_BOEHMGC
    /**
     * Allocation cache for GC'd Value objects.
     */
    std::shared_ptr<void *> valueAllocCache;

    /**
     * Allocation cache for size-1 Env objects.
     */
    std::shared_ptr<void *> env1AllocCache;
#endif

public:

    EvalState(
        const SearchPath & _searchPath,
        ref<Store> store,
        std::shared_ptr<Store> buildStore = nullptr);
    ~EvalState();

    /**
     * Return a `SourcePath` that refers to `path` in the root
     * filesystem.
     */
    SourcePath rootPath(CanonPath path);

    /**
     * Allow access to a path.
     */
    void allowPath(const Path & path);

    /**
     * Allow access to a store path. Note that this gets remapped to
     * the real store path if `store` is a chroot store.
     */
    void allowPath(const StorePath & storePath);

    /**
     * Allow access to a store path and return it as a string.
     */
    void allowAndSetStorePathString(const StorePath & storePath, Value & v);

    /**
     * Check whether access to a path is allowed and throw an error if
     * not. Otherwise return the canonicalised path.
     */
    SourcePath checkSourcePath(const SourcePath & path);

    void checkURI(const std::string & uri);

    /**
     * When using a diverted store and 'path' is in the Nix store, map
     * 'path' to the diverted location (e.g. /nix/store/foo is mapped
     * to /home/alice/my-nix/nix/store/foo). However, this is only
     * done if the context is not empty, since otherwise we're
     * probably trying to read from the actual /nix/store. This is
     * intended to distinguish between import-from-derivation and
     * sources stored in the actual /nix/store.
     */
    Path toRealPath(const Path & path, const NixStringContext & context);

    /**
     * Parse a Nix expression from the specified file.
     */
    Expr & parseExprFromFile(const SourcePath & path);
    Expr & parseExprFromFile(const SourcePath & path, std::shared_ptr<StaticEnv> & staticEnv);

    /**
     * Parse a Nix expression from the specified string.
     */
    Expr & parseExprFromString(
        std::string s,
        const SourcePath & basePath,
        std::shared_ptr<StaticEnv> & staticEnv,
        const FeatureSettings & xpSettings = featureSettings
    );
    Expr & parseExprFromString(
        std::string s,
        const SourcePath & basePath,
        const FeatureSettings & xpSettings = featureSettings
    );

    Expr & parseStdin();

    /**
     * Evaluate an expression read from the given file to normal
     * form. Optionally enforce that the top-level expression is
     * trivial (i.e. doesn't require arbitrary computation).
     */
    void evalFile(const SourcePath & path, Value & v, bool mustBeTrivial = false);

    /**
     * Like `evalFile`, but with an already parsed expression.
     */
    void cacheFile(
        const SourcePath & path,
        const SourcePath & resolvedPath,
        Expr * e,
        Value & v,
        bool mustBeTrivial = false);

    void resetFileCache();

    /**
     * Look up a file in the search path.
     */
    SourcePath findFile(const std::string_view path);
    SourcePath findFile(const SearchPath & searchPath, const std::string_view path, const PosIdx pos = noPos);

    /**
     * Try to resolve a search path value (not the optinal key part)
     *
     * If the specified search path element is a URI, download it.
     *
     * If it is not found, return `std::nullopt`
     */
    std::optional<std::string> resolveSearchPathPath(const SearchPath::Path & path);

    /**
     * Evaluate an expression to normal form
     *
     * @param [out] v The resulting is stored here.
     */
    void eval(Expr & e, Value & v);

    /**
     * Evaluation the expression, then verify that it has the expected
     * type.
     */
    inline bool evalBool(Env & env, Expr & e);
    inline bool evalBool(Env & env, Expr & e, const PosIdx pos, std::string_view errorCtx);
    inline void evalAttrs(Env & env, Expr & e, Value & v, const PosIdx pos, std::string_view errorCtx);

    /**
     * If `v` is a thunk, enter it and overwrite `v` with the result
     * of the evaluation of the thunk.  If `v` is a delayed function
     * application, call the function and overwrite `v` with the
     * result.  Otherwise, this is a no-op.
     */
    inline void forceValue(Value & v, const PosIdx pos);

    void tryFixupBlackHolePos(Value & v, PosIdx pos);

    /**
     * Force a value, then recursively force list elements and
     * attributes.
     */
    void forceValueDeep(Value & v);

    /**
     * Force `v`, and then verify that it has the expected type.
     */
    NixInt forceInt(Value & v, const PosIdx pos, std::string_view errorCtx);
    NixFloat forceFloat(Value & v, const PosIdx pos, std::string_view errorCtx);
    bool forceBool(Value & v, const PosIdx pos, std::string_view errorCtx);

    void forceAttrs(Value & v, const PosIdx pos, std::string_view errorCtx);

    template <typename Callable>
    inline void forceAttrs(Value & v, Callable getPos, std::string_view errorCtx);

    inline void forceList(Value & v, const PosIdx pos, std::string_view errorCtx);
    /**
     * @param v either lambda or primop
     */
    void forceFunction(Value & v, const PosIdx pos, std::string_view errorCtx);
    std::string_view forceString(Value & v, const PosIdx pos, std::string_view errorCtx);
    std::string_view forceString(Value & v, NixStringContext & context, const PosIdx pos, std::string_view errorCtx);
    std::string_view forceStringNoCtx(Value & v, const PosIdx pos, std::string_view errorCtx);

    template<typename... Args>
    [[gnu::noinline]]
    void addErrorTrace(Error & e, const Args & ... formatArgs) const;
    template<typename... Args>
    [[gnu::noinline]]
    void addErrorTrace(Error & e, const PosIdx pos, const Args & ... formatArgs) const;

public:
    /**
     * @return true iff the value `v` denotes a derivation (i.e. a
     * set with attribute `type = "derivation"`).
     */
    bool isDerivation(Value & v);

    std::optional<std::string> tryAttrsToString(const PosIdx pos, Value & v,
        NixStringContext & context, bool coerceMore = false, bool copyToStore = true);

    /**
     * String coercion.
     *
     * Converts strings, paths and derivations to a
     * string.  If `coerceMore` is set, also converts nulls, integers,
     * booleans and lists to a string.  If `copyToStore` is set,
     * referenced paths are copied to the Nix store as a side effect.
     */
    BackedStringView coerceToString(const PosIdx pos, Value & v, NixStringContext & context,
        std::string_view errorCtx,
        bool coerceMore = false, bool copyToStore = true,
        bool canonicalizePath = true);

    StorePath copyPathToStore(NixStringContext & context, const SourcePath & path);

    /**
     * Path coercion.
     *
     * Converts strings, paths and derivations to a
     * path.  The result is guaranteed to be a canonicalised, absolute
     * path.  Nothing is copied to the store.
     */
    SourcePath coerceToPath(const PosIdx pos, Value & v, NixStringContext & context, std::string_view errorCtx);

    /**
     * Like coerceToPath, but the result must be a store path.
     */
    StorePath coerceToStorePath(const PosIdx pos, Value & v, NixStringContext & context, std::string_view errorCtx);

    /**
     * Part of `coerceToSingleDerivedPath()` without any store IO which is exposed for unit testing only.
     */
    std::pair<SingleDerivedPath, std::string_view> coerceToSingleDerivedPathUnchecked(const PosIdx pos, Value & v, std::string_view errorCtx);

    /**
     * Coerce to `SingleDerivedPath`.
     *
     * Must be a string which is either a literal store path or a
     * "placeholder (see `DownstreamPlaceholder`).
     *
     * Even more importantly, the string context must be exactly one
     * element, which is either a `NixStringContextElem::Opaque` or
     * `NixStringContextElem::Built`. (`NixStringContextEleme::DrvDeep`
     * is not permitted).
     *
     * The string is parsed based on the context --- the context is the
     * source of truth, and ultimately tells us what we want, and then
     * we ensure the string corresponds to it.
     */
    SingleDerivedPath coerceToSingleDerivedPath(const PosIdx pos, Value & v, std::string_view errorCtx);

public:

    /**
     * The base environment, containing the builtin functions and
     * values.
     */
    Env & baseEnv;

    /**
     * The same, but used during parsing to resolve variables.
     */
    std::shared_ptr<StaticEnv> staticBaseEnv; // !!! should be private

    /**
     * Name and documentation about every constant.
     *
     * Constants from primops are hard to crawl, and their docs will go
     * here too.
     */
    std::vector<std::pair<std::string, Constant>> constantInfos;

private:

    unsigned int baseEnvDispl = 0;

    void createBaseEnv();

    Value * addConstant(const std::string & name, const Value & v, Constant info);

    void addConstant(const std::string & name, Value * v, Constant info);

    Value * addPrimOp(PrimOp && primOp);

public:

    Value & getBuiltin(const std::string & name);

    struct Doc
    {
        Pos pos;
        std::optional<std::string> name;
        size_t arity;
        std::vector<std::string> args;
        /**
         * Unlike the other `doc` fields in this file, this one should never be
         * `null`.
         */
        const char * doc;
    };

    std::optional<Doc> getDoc(Value & v);

private:

    inline Value * lookupVar(Env * env, const ExprVar & var, bool noEval);

    friend struct ExprVar;
    friend struct ExprAttrs;
    friend struct ExprLet;

    Expr * parse(
        char * text,
        size_t length,
        Pos::Origin origin,
        const SourcePath & basePath,
        std::shared_ptr<StaticEnv> & staticEnv,
        const FeatureSettings & xpSettings = featureSettings);

    /**
     * Current Nix call stack depth, used with `max-call-depth` setting to throw stack overflow hopefully before we run out of system stack.
     */
    size_t callDepth = 0;

public:

    /**
     * Do a deep equality test between two values.  That is, list
     * elements and attributes are compared recursively.
     */
    bool eqValues(Value & v1, Value & v2, const PosIdx pos, std::string_view errorCtx);

    bool isFunctor(Value & fun);

    // FIXME: use std::span
    void callFunction(Value & fun, size_t nrArgs, Value * * args, Value & vRes, const PosIdx pos);

    void callFunction(Value & fun, Value & arg, Value & vRes, const PosIdx pos)
    {
        Value * args[] = {&arg};
        callFunction(fun, 1, args, vRes, pos);
    }

    /**
     * Automatically call a function for which each argument has a
     * default value or has a binding in the `args` map.
     */
    void autoCallFunction(Bindings & args, Value & fun, Value & res);

    /**
     * Allocation primitives.
     */
    inline Value * allocValue();
    inline Env & allocEnv(size_t size);

    Bindings * allocBindings(size_t capacity);

    BindingsBuilder buildBindings(size_t capacity)
    {
        return BindingsBuilder(*this, allocBindings(capacity));
    }

    void mkList(Value & v, size_t length);
    void mkThunk_(Value & v, Expr & expr);
    void mkPos(Value & v, PosIdx pos);

    /**
     * Create a string representing a store path.
     *
     * The string is the printed store path with a context containing a
     * single `NixStringContextElem::Opaque` element of that store path.
     */
    void mkStorePathString(const StorePath & storePath, Value & v);

    /**
     * Create a string representing a `SingleDerivedPath::Built`.
     *
     * The string is the printed store path with a context containing a
     * single `NixStringContextElem::Built` element of the drv path and
     * output name.
     *
     * @param value Value we are settings
     *
     * @param b the drv whose output we are making a string for, and the
     * output
     *
     * @param optStaticOutputPath Optional output path for that string.
     * Must be passed if and only if output store object is
     * input-addressed or fixed output. Will be printed to form string
     * if passed, otherwise a placeholder will be used (see
     * `DownstreamPlaceholder`).
     *
     * @param xpSettings Stop-gap to avoid globals during unit tests.
     */
    void mkOutputString(
        Value & value,
        const SingleDerivedPath::Built & b,
        std::optional<StorePath> optStaticOutputPath,
        const ExperimentalFeatureSettings & xpSettings = experimentalFeatureSettings);

    /**
     * Create a string representing a `SingleDerivedPath`.
     *
     * A combination of `mkStorePathString` and `mkOutputString`.
     */
    void mkSingleDerivedPathString(
        const SingleDerivedPath & p,
        Value & v);

    void concatLists(Value & v, size_t nrLists, Value * * lists, const PosIdx pos, std::string_view errorCtx);

    /**
     * Print statistics, if enabled.
     *
     * Performs a full memory GC before printing the statistics, so that the
     * GC statistics are more accurate.
     */
    void maybePrintStats();

    /**
     * Print statistics, unconditionally, cheaply, without performing a GC first.
     */
    void printStatistics();

    /**
     * Perform a full memory garbage collection - not incremental.
     *
     * @return true if Nix was built with GC and a GC was performed, false if not.
     *              The return value is currently not thread safe - just the return value.
     */
    bool fullGC();

    /**
     * Realise the given context, and return a mapping from the placeholders
     * used to construct the associated value to their final store path
     */
    [[nodiscard]] StringMap realiseContext(const NixStringContext & context);

private:

    /**
     * Like `mkOutputString` but just creates a raw string, not an
     * string Value, which would also have a string context.
     */
    std::string mkOutputStringRaw(
        const SingleDerivedPath::Built & b,
        std::optional<StorePath> optStaticOutputPath,
        const ExperimentalFeatureSettings & xpSettings = experimentalFeatureSettings);

    /**
     * Like `mkSingleDerivedPathStringRaw` but just creates a raw string
     * Value, which would also have a string context.
     */
    std::string mkSingleDerivedPathStringRaw(
        const SingleDerivedPath & p);

    unsigned long nrEnvs = 0;
    unsigned long nrValuesInEnvs = 0;
    unsigned long nrValues = 0;
    unsigned long nrListElems = 0;
    unsigned long nrLookups = 0;
    unsigned long nrAttrsets = 0;
    unsigned long nrAttrsInAttrsets = 0;
    unsigned long nrAvoided = 0;
    unsigned long nrOpUpdates = 0;
    unsigned long nrOpUpdateValuesCopied = 0;
    unsigned long nrListConcats = 0;
    unsigned long nrPrimOpCalls = 0;
    unsigned long nrFunctionCalls = 0;

    bool countCalls;

    using PrimOpCalls = std::map<std::string, size_t>;
    PrimOpCalls primOpCalls;

    using FunctionCalls = std::map<ExprLambda *, size_t>;
    FunctionCalls functionCalls;

    void incrFunctionCall(ExprLambda * fun);

    using AttrSelects = std::map<PosIdx, size_t>;
    AttrSelects attrSelects;

    friend struct ExprOpUpdate;
    friend struct ExprOpConcatLists;
    friend struct ExprVar;
    friend struct ExprString;
    friend struct ExprInt;
    friend struct ExprFloat;
    friend struct ExprPath;
    friend struct ExprSelect;
    friend void prim_getAttr(EvalState & state, const PosIdx pos, Value * * args, Value & v);
    friend void prim_match(EvalState & state, const PosIdx pos, Value * * args, Value & v);
    friend void prim_split(EvalState & state, const PosIdx pos, Value * * args, Value & v);

    friend struct Value;
};

/**
 * @return A string representing the type of the value `v`.
 *
 * @param withArticle Whether to begin with an english article, e.g. "an
 * integer" vs "integer".
 */
std::string_view showType(ValueType type, bool withArticle = true);
std::string showType(const Value & v);

/**
 * If `path` refers to a directory, then append "/default.nix".
 */
SourcePath resolveExprPath(SourcePath path);

static constexpr std::string_view corepkgsPrefix{"/__corepkgs__/"};


}

#include "lix/libexpr/eval-inline.hh" // IWYU pragma: keep
