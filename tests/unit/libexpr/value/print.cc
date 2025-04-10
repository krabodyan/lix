#include "tests/libexpr.hh"

#include "lix/libexpr/value.hh"
#include "lix/libexpr/print.hh"

namespace nix {

using namespace testing;

struct ValuePrintingTests : LibExprTest
{
    template<class... A>
    void test(Value v, std::string_view expected, A... args)
    {
        std::stringstream out;
        v.print(state, out, args...);
        ASSERT_EQ(out.str(), expected);
    }
};

TEST_F(ValuePrintingTests, tInt)
{
    Value vInt;
    vInt.mkInt(10);
    test(vInt, "10");
}

TEST_F(ValuePrintingTests, tBool)
{
    Value vBool;
    vBool.mkBool(true);
    test(vBool, "true");
}

TEST_F(ValuePrintingTests, tString)
{
    Value vString;
    vString.mkString("some-string");
    test(vString, "\"some-string\"");
}

TEST_F(ValuePrintingTests, tPath)
{
    Value vPath;
    vPath.mkString("/foo");
    test(vPath, "\"/foo\"");
}

TEST_F(ValuePrintingTests, tNull)
{
    Value vNull;
    vNull.mkNull();
    test(vNull, "null");
}

TEST_F(ValuePrintingTests, tAttrs)
{
    Value vOne;
    vOne.mkInt(1);

    Value vTwo;
    vTwo.mkInt(2);

    BindingsBuilder builder = evaluator.buildBindings(10);
    builder.insert(evaluator.symbols.create("one"), &vOne);
    builder.insert(evaluator.symbols.create("two"), &vTwo);

    Value vAttrs;
    vAttrs.mkAttrs(builder.finish());

    test(vAttrs, "{ one = 1; two = 2; }");
}

TEST_F(ValuePrintingTests, tList)
{
    Value vOne;
    vOne.mkInt(1);

    Value vTwo;
    vTwo.mkInt(2);

    Value vList = evaluator.mem.newList(5);
    vList.bigList.elems[0] = &vOne;
    vList.bigList.elems[1] = &vTwo;
    vList.bigList.size = 3;

    test(vList, "[ 1 2 «nullptr» ]");
}

TEST_F(ValuePrintingTests, vThunk)
{
    Value vThunk;
    ExprLiteral e(noPos, NewValueAs::integer, 0);
    vThunk.mkThunk(nullptr, e);

    test(vThunk, "«thunk»");
}

TEST_F(ValuePrintingTests, vApp)
{
    Value vApp;
    vApp.mkApp(nullptr, nullptr);

    test(vApp, "«thunk»");
}

TEST_F(ValuePrintingTests, vLambda)
{
    Env env {
        .up = nullptr,
        .values = { }
    };
    PosTable::Origin origin = evaluator.positions.addOrigin(std::monostate(), 1);
    auto posIdx = evaluator.positions.add(origin, 0);

    ExprLambda eLambda(posIdx, std::make_unique<AttrsPattern>(), std::make_unique<ExprLiteral>(noPos, NewValueAs::integer, 0));
    eLambda.pattern->name = createSymbol("a");

    Value vLambda;
    vLambda.mkLambda(&env, &eLambda);

    test(vLambda, "«lambda @ «none»:1:1»");

    eLambda.setName(createSymbol("puppy"));

    test(vLambda, "«lambda puppy @ «none»:1:1»");
}

TEST_F(ValuePrintingTests, vPrimOp)
{
    Value vPrimOp;
    PrimOp primOp{
        .name = "puppy"
    };
    vPrimOp.mkPrimOp(&primOp);

    test(vPrimOp, "«primop puppy»");
}

TEST_F(ValuePrintingTests, vPrimOpApp)
{
    PrimOp primOp{
        .name = "puppy"
    };
    Value vPrimOp;
    vPrimOp.mkPrimOp(&primOp);

    Value vPrimOpApp;
    vPrimOpApp.mkPrimOpApp(&vPrimOp, nullptr);

    test(vPrimOpApp, "«partially applied primop puppy»");
}

TEST_F(ValuePrintingTests, vExternal)
{
    struct MyExternal : ExternalValueBase
    {
    public:
        std::string showType() const override
        {
            return "";
        }
        std::string typeOf() const override
        {
            return "";
        }
        virtual std::ostream & print(std::ostream & str) const override
        {
            str << "testing-external!";
            return str;
        }
    } myExternal;
    Value vExternal;
    vExternal.mkExternal(&myExternal);

    test(vExternal, "testing-external!");
}

TEST_F(ValuePrintingTests, vFloat)
{
    Value vFloat;
    vFloat.mkFloat(2.0);

    test(vFloat, "2");
}

TEST_F(ValuePrintingTests, vBlackhole)
{
    Value vBlackhole;
    vBlackhole.mkBlackhole();
    test(vBlackhole, "«potential infinite recursion»");
}

TEST_F(ValuePrintingTests, depthAttrs)
{
    Value vZero;
    vZero.mkInt(0);

    Value vOne;
    vOne.mkInt(1);

    Value vTwo;
    vTwo.mkInt(2);

    BindingsBuilder builderEmpty = evaluator.buildBindings(0);
    Value vAttrsEmpty;
    vAttrsEmpty.mkAttrs(builderEmpty.finish());

    BindingsBuilder builderNested = evaluator.buildBindings(1);
    builderNested.insert(evaluator.symbols.create("zero"), &vZero);
    Value vAttrsNested;
    vAttrsNested.mkAttrs(builderNested.finish());

    BindingsBuilder builder = evaluator.buildBindings(10);
    builder.insert(evaluator.symbols.create("one"), &vOne);
    builder.insert(evaluator.symbols.create("two"), &vTwo);
    builder.insert(evaluator.symbols.create("empty"), &vAttrsEmpty);
    builder.insert(evaluator.symbols.create("nested"), &vAttrsNested);

    Value vAttrs;
    vAttrs.mkAttrs(builder.finish());

    BindingsBuilder builder2 = evaluator.buildBindings(10);
    builder2.insert(evaluator.symbols.create("one"), &vOne);
    builder2.insert(evaluator.symbols.create("two"), &vTwo);
    builder2.insert(evaluator.symbols.create("nested"), &vAttrs);

    Value vNested;
    vNested.mkAttrs(builder2.finish());

    test(vNested, "{ nested = { ... }; one = 1; two = 2; }", PrintOptions { .maxDepth = 1 });
    test(vNested, "{ nested = { empty = { }; nested = { ... }; one = 1; two = 2; }; one = 1; two = 2; }", PrintOptions { .maxDepth = 2 });
    test(vNested, "{ nested = { empty = { }; nested = { zero = 0; }; one = 1; two = 2; }; one = 1; two = 2; }", PrintOptions { .maxDepth = 3 });
    test(vNested, "{ nested = { empty = { }; nested = { zero = 0; }; one = 1; two = 2; }; one = 1; two = 2; }", PrintOptions { .maxDepth = 4 });
}

TEST_F(ValuePrintingTests, depthList)
{
    Value vOne;
    vOne.mkInt(1);

    Value vTwo;
    vTwo.mkInt(2);

    BindingsBuilder builder = evaluator.buildBindings(10);
    builder.insert(evaluator.symbols.create("one"), &vOne);
    builder.insert(evaluator.symbols.create("two"), &vTwo);

    Value vAttrs;
    vAttrs.mkAttrs(builder.finish());

    BindingsBuilder builder2 = evaluator.buildBindings(10);
    builder2.insert(evaluator.symbols.create("one"), &vOne);
    builder2.insert(evaluator.symbols.create("two"), &vTwo);
    builder2.insert(evaluator.symbols.create("nested"), &vAttrs);

    Value vNested;
    vNested.mkAttrs(builder2.finish());

    Value vList = evaluator.mem.newList(5);
    vList.bigList.elems[0] = &vOne;
    vList.bigList.elems[1] = &vTwo;
    vList.bigList.elems[2] = &vNested;
    vList.bigList.size = 3;

    test(vList, "[ 1 2 { ... } ]", PrintOptions { .maxDepth = 1 });
    test(vList, "[ 1 2 { nested = { ... }; one = 1; two = 2; } ]", PrintOptions { .maxDepth = 2 });
    test(vList, "[ 1 2 { nested = { one = 1; two = 2; }; one = 1; two = 2; } ]", PrintOptions { .maxDepth = 3 });
    test(vList, "[ 1 2 { nested = { one = 1; two = 2; }; one = 1; two = 2; } ]", PrintOptions { .maxDepth = 4 });
    test(vList, "[ 1 2 { nested = { one = 1; two = 2; }; one = 1; two = 2; } ]", PrintOptions { .maxDepth = 5 });
}

struct StringPrintingTests : LibExprTest
{
    template<class... A>
    void test(std::string_view literal, std::string_view expected, unsigned int maxLength, A... args)
    {
        Value v;
        v.mkString(literal);

        std::stringstream out;
        printValue(state, out, v, PrintOptions {
            .maxStringLength = maxLength
        });
        ASSERT_EQ(out.str(), expected);
    }
};

TEST_F(StringPrintingTests, maxLengthTruncation)
{
    test("abcdefghi", "\"abcdefghi\"", 10);
    test("abcdefghij", "\"abcdefghij\"", 10);
    test("abcdefghijk", "\"abcdefghij\" «1 byte elided»", 10);
    test("abcdefghijkl", "\"abcdefghij\" «2 bytes elided»", 10);
    test("abcdefghijklm", "\"abcdefghij\" «3 bytes elided»", 10);
}

// Check that printing an attrset shows 'important' attributes like `type`
// first, but only reorder the attrs when we have a maxAttrs budget.
TEST_F(ValuePrintingTests, attrsTypeFirst)
{
    Value vType;
    vType.mkString("puppy");

    Value vApple;
    vApple.mkString("apple");

    BindingsBuilder builder = evaluator.buildBindings(10);
    builder.insert(evaluator.symbols.create("type"), &vType);
    builder.insert(evaluator.symbols.create("apple"), &vApple);

    Value vAttrs;
    vAttrs.mkAttrs(builder.finish());

    test(vAttrs,
         "{ type = \"puppy\"; apple = \"apple\"; }",
         PrintOptions {
            .maxAttrs = 100
         });

    test(vAttrs,
         "{ apple = \"apple\"; type = \"puppy\"; }",
         PrintOptions { });
}

TEST_F(ValuePrintingTests, ansiColorsInt)
{
    Value v;
    v.mkInt(10);

    test(v,
         ANSI_CYAN "10" ANSI_NORMAL,
         PrintOptions {
             .ansiColors = true
         });
}

TEST_F(ValuePrintingTests, ansiColorsFloat)
{
    Value v;
    v.mkFloat(1.6);

    test(v,
         ANSI_CYAN "1.6" ANSI_NORMAL,
         PrintOptions {
             .ansiColors = true
         });
}

TEST_F(ValuePrintingTests, ansiColorsBool)
{
    Value v;
    v.mkBool(true);

    test(v,
         ANSI_CYAN "true" ANSI_NORMAL,
         PrintOptions {
             .ansiColors = true
         });
}

TEST_F(ValuePrintingTests, ansiColorsString)
{
    Value v;
    v.mkString("puppy");

    test(v,
         ANSI_MAGENTA "\"puppy\"" ANSI_NORMAL,
         PrintOptions {
             .ansiColors = true
        });
}

TEST_F(ValuePrintingTests, ansiColorsStringElided)
{
    Value v;
    v.mkString("puppy");

    test(v,
         ANSI_MAGENTA "\"pup\" " ANSI_FAINT "«2 bytes elided»" ANSI_NORMAL,
         PrintOptions {
             .ansiColors = true,
             .maxStringLength = 3
         });
}

TEST_F(ValuePrintingTests, ansiColorsPath)
{
    Value v;
    v.mkPath(CanonPath("puppy"));

    test(v,
         ANSI_GREEN "/puppy" ANSI_NORMAL,
         PrintOptions {
             .ansiColors = true
         });
}

TEST_F(ValuePrintingTests, ansiColorsNull)
{
    Value v;
    v.mkNull();

    test(v,
         ANSI_CYAN "null" ANSI_NORMAL,
         PrintOptions {
             .ansiColors = true
         });
}

TEST_F(ValuePrintingTests, ansiColorsAttrs)
{
    Value vOne;
    vOne.mkInt(1);

    Value vTwo;
    vTwo.mkInt(2);

    BindingsBuilder builder = evaluator.buildBindings(10);
    builder.insert(evaluator.symbols.create("one"), &vOne);
    builder.insert(evaluator.symbols.create("two"), &vTwo);

    Value vAttrs;
    vAttrs.mkAttrs(builder.finish());

    test(vAttrs,
         "{ one = " ANSI_CYAN "1" ANSI_NORMAL "; two = " ANSI_CYAN "2" ANSI_NORMAL "; }",
         PrintOptions {
             .ansiColors = true
         });
}

TEST_F(ValuePrintingTests, ansiColorsDerivation)
{
    Value vDerivation;
    vDerivation.mkString("derivation");

    BindingsBuilder builder = evaluator.buildBindings(10);
    builder.insert(evaluator.s.type, &vDerivation);

    Value vAttrs;
    vAttrs.mkAttrs(builder.finish());

    test(vAttrs,
         ANSI_GREEN "«derivation»" ANSI_NORMAL,
         PrintOptions {
             .ansiColors = true,
             .force = true,
             .derivationPaths = true
         });

    test(vAttrs,
         "{ type = " ANSI_MAGENTA "\"derivation\"" ANSI_NORMAL "; }",
         PrintOptions {
             .ansiColors = true,
             .force = true
         });
}

TEST_F(ValuePrintingTests, ansiColorsError)
{
    Value vError;
    auto & e = evaluator.parseExprFromString("{ a = throw \"uh oh!\"; }", {CanonPath::root});
    state.eval(e, vError);

    test(*vError.attrs->begin()->value,
         ANSI_RED
         "«error: uh oh!»"
         ANSI_NORMAL,
         PrintOptions {
             .ansiColors = true,
             .force = true,
         });
}

TEST_F(ValuePrintingTests, ansiColorsDerivationError)
{
    Value vAttrs;
    auto & e = evaluator.parseExprFromString(
        "{ type = \"derivation\"; drvPath = throw \"uh oh!\"; }", {CanonPath::root}
    );
    state.eval(e, vAttrs);

    test(vAttrs,
         "{ drvPath = "
         ANSI_RED
         "«error: uh oh!»"
         ANSI_NORMAL
         "; type = "
         ANSI_MAGENTA
         "\"derivation\""
         ANSI_NORMAL
         "; }",
         PrintOptions {
             .ansiColors = true,
             .force = true
         });

    test(vAttrs,
         ANSI_RED
         "«error: uh oh!»"
         ANSI_NORMAL,
         PrintOptions {
             .ansiColors = true,
             .force = true,
             .derivationPaths = true,
         });
}

TEST_F(ValuePrintingTests, ansiColorsAssert)
{
    auto & e = evaluator.parseExprFromString("{ a = assert false; 1; }", {CanonPath::root});
    Value v;
    state.eval(e, v);

    ASSERT_EQ(v.type(), nAttrs);
    test(*v.attrs->begin()->value,
         ANSI_RED "«error: assertion failed»" ANSI_NORMAL,
         PrintOptions {
             .ansiColors = true,
             .force = true
         });
}

TEST_F(ValuePrintingTests, ansiColorsList)
{
    Value vOne;
    vOne.mkInt(1);

    Value vTwo;
    vTwo.mkInt(2);

    Value vList = evaluator.mem.newList(5);
    vList.bigList.elems[0] = &vOne;
    vList.bigList.elems[1] = &vTwo;
    vList.bigList.size = 3;

    test(vList,
         "[ " ANSI_CYAN "1" ANSI_NORMAL " " ANSI_CYAN "2" ANSI_NORMAL " " ANSI_MAGENTA "«nullptr»" ANSI_NORMAL " ]",
         PrintOptions {
             .ansiColors = true
         });
}

TEST_F(ValuePrintingTests, ansiColorsLambda)
{
    Env env {
        .up = nullptr,
        .values = { }
    };
    PosTable::Origin origin = evaluator.positions.addOrigin(std::monostate(), 1);
    auto posIdx = evaluator.positions.add(origin, 0);

    ExprLambda eLambda(posIdx, std::make_unique<AttrsPattern>(), std::make_unique<ExprLiteral>(noPos, NewValueAs::integer, 0));
    eLambda.pattern->name = createSymbol("a");

    Value vLambda;
    vLambda.mkLambda(&env, &eLambda);

    test(vLambda,
         ANSI_BLUE "«lambda @ «none»:1:1»" ANSI_NORMAL,
         PrintOptions {
             .ansiColors = true,
             .force = true
         });

    eLambda.setName(createSymbol("puppy"));

    test(vLambda,
         ANSI_BLUE "«lambda puppy @ «none»:1:1»" ANSI_NORMAL,
         PrintOptions {
             .ansiColors = true,
             .force = true
         });
}

TEST_F(ValuePrintingTests, ansiColorsPrimOp)
{
    PrimOp primOp{
        .name = "puppy"
    };
    Value v;
    v.mkPrimOp(&primOp);

    test(v,
         ANSI_BLUE "«primop puppy»" ANSI_NORMAL,
         PrintOptions {
             .ansiColors = true
         });
}

TEST_F(ValuePrintingTests, ansiColorsPrimOpApp)
{
    PrimOp primOp{
        .name = "puppy"
    };
    Value vPrimOp;
    vPrimOp.mkPrimOp(&primOp);

    Value v;
    v.mkPrimOpApp(&vPrimOp, nullptr);

    test(v,
         ANSI_BLUE "«partially applied primop puppy»" ANSI_NORMAL,
         PrintOptions {
             .ansiColors = true
         });
}

TEST_F(ValuePrintingTests, ansiColorsThunk)
{
    Value v;
    ExprLiteral e(noPos, NewValueAs::integer, 0);
    v.mkThunk(nullptr, e);

    test(v,
         ANSI_MAGENTA "«thunk»" ANSI_NORMAL,
         PrintOptions {
             .ansiColors = true
         });
}

TEST_F(ValuePrintingTests, ansiColorsBlackhole)
{
    Value v;
    v.mkBlackhole();

    test(v,
         ANSI_RED "«potential infinite recursion»" ANSI_NORMAL,
         PrintOptions {
             .ansiColors = true
         });
}

TEST_F(ValuePrintingTests, ansiColorsAttrsRepeated)
{
    Value vZero;
    vZero.mkInt(0);

    BindingsBuilder innerBuilder = evaluator.buildBindings(1);
    innerBuilder.insert(evaluator.symbols.create("x"), &vZero);

    Value vInner;
    vInner.mkAttrs(innerBuilder.finish());

    BindingsBuilder builder = evaluator.buildBindings(10);
    builder.insert(evaluator.symbols.create("a"), &vInner);
    builder.insert(evaluator.symbols.create("b"), &vInner);

    Value vAttrs;
    vAttrs.mkAttrs(builder.finish());

    test(vAttrs,
         "{ a = { x = " ANSI_CYAN "0" ANSI_NORMAL "; }; b = " ANSI_MAGENTA "«repeated»" ANSI_NORMAL "; }",
         PrintOptions {
             .ansiColors = true
         });
}

TEST_F(ValuePrintingTests, ansiColorsListRepeated)
{
    Value vZero;
    vZero.mkInt(0);

    BindingsBuilder innerBuilder = evaluator.buildBindings(1);
    innerBuilder.insert(evaluator.symbols.create("x"), &vZero);

    Value vInner;
    vInner.mkAttrs(innerBuilder.finish());

    Value vList = evaluator.mem.newList(3);
    vList.bigList.elems[0] = &vInner;
    vList.bigList.elems[1] = &vInner;
    vList.bigList.size = 2;

    test(vList,
         "[ { x = " ANSI_CYAN "0" ANSI_NORMAL "; } " ANSI_MAGENTA "«repeated»" ANSI_NORMAL " ]",
         PrintOptions {
             .ansiColors = true
         });
}

TEST_F(ValuePrintingTests, listRepeated)
{
    Value vZero;
    vZero.mkInt(0);

    BindingsBuilder innerBuilder = evaluator.buildBindings(1);
    innerBuilder.insert(evaluator.symbols.create("x"), &vZero);

    Value vInner;
    vInner.mkAttrs(innerBuilder.finish());

    Value vList = evaluator.mem.newList(3);
    vList.bigList.elems[0] = &vInner;
    vList.bigList.elems[1] = &vInner;
    vList.bigList.size = 2;

    test(vList, "[ { x = 0; } «repeated» ]", PrintOptions { });
    test(vList,
         "[ { x = 0; } { x = 0; } ]",
         PrintOptions {
             .trackRepeated = false
         });
}

TEST_F(ValuePrintingTests, ansiColorsAttrsElided)
{
    Value vOne;
    vOne.mkInt(1);

    Value vTwo;
    vTwo.mkInt(2);

    BindingsBuilder builder = evaluator.buildBindings(10);
    builder.insert(evaluator.symbols.create("one"), &vOne);
    builder.insert(evaluator.symbols.create("two"), &vTwo);

    Value vAttrs;
    vAttrs.mkAttrs(builder.finish());

    test(vAttrs,
         "{ one = " ANSI_CYAN "1" ANSI_NORMAL "; " ANSI_FAINT "«1 attribute elided»" ANSI_NORMAL " }",
         PrintOptions {
             .ansiColors = true,
             .maxAttrs = 1
         });

    Value vThree;
    vThree.mkInt(3);

    builder.insert(evaluator.symbols.create("three"), &vThree);
    vAttrs.mkAttrs(builder.finish());

    test(vAttrs,
         "{ one = " ANSI_CYAN "1" ANSI_NORMAL "; " ANSI_FAINT "«2 attributes elided»" ANSI_NORMAL " }",
         PrintOptions {
             .ansiColors = true,
             .maxAttrs = 1
         });
}

TEST_F(ValuePrintingTests, ansiColorsListElided)
{
    Value vOne;
    vOne.mkInt(1);

    Value vTwo;
    vTwo.mkInt(2);

    Value vList = evaluator.mem.newList(4);
    vList.bigList.elems[0] = &vOne;
    vList.bigList.elems[1] = &vTwo;
    vList.bigList.size = 2;

    test(vList,
         "[ " ANSI_CYAN "1" ANSI_NORMAL " " ANSI_FAINT "«1 item elided»" ANSI_NORMAL " ]",
         PrintOptions {
             .ansiColors = true,
             .maxListItems = 1
         });

    Value vThree;
    vThree.mkInt(3);

    vList.bigList.elems[2] = &vThree;
    vList.bigList.size = 3;

    test(vList,
         "[ " ANSI_CYAN "1" ANSI_NORMAL " " ANSI_FAINT "«2 items elided»" ANSI_NORMAL " ]",
         PrintOptions {
             .ansiColors = true,
             .maxListItems = 1
         });
}

} // namespace nix
