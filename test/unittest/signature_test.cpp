/// @file        signature_test.cpp
/// @brief       泛型签名系统测试：三形态表驱动解析（每产生式 ≥2 形态）、
///        roundtrip 逐字节恒等、畸形输入显式 Err（码+位置）、类名手术面。
///
/// 验收口径（checklist B1）：合法输入 parse→unparse 逐字节恒等；畸形输入
/// Err 带位置不静默；手术后重 parse 恒等、逆向 rename 还原原串。
#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <type_traits>

#include "jbc/jbc.hpp"

using namespace jbc::type;

namespace {

/// @brief 断言签名解析成功且 roundtrip 逐字节恒等，返回形态供后续断言。
///
/// 按值收 Result：树节点含 unique_ptr（SigTypeParam::class_bound 等），只移不
/// 拷——从 unwrap() move 出来后按 NRVO/move 返回。
template <typename T>
T expectOk(Result<T, SigError> r, std::string_view sig)
{
    EXPECT_TRUE(r.is_ok()) << sig << " -> "
                           << (r.is_err() ? to_string(std::move(r).unwrap_err().code)
                                          : "ok")
                           << " @" << (r.is_err() ? std::move(r).unwrap_err().offset : 0);
    if (r.is_err()) {
        return T{};
    }
    T value = std::move(r).unwrap();
    ca::str::Utf8String out;
    if constexpr (std::is_same_v<T, SigClassSignature>) {
        out = unparseClassSignature(value);
    }
    else if constexpr (std::is_same_v<T, SigMethodSignature>) {
        out = unparseMethodSignature(value);
    }
    else if constexpr (std::is_same_v<T, SigType>) {
        out = unparseFieldSignature(value);
    }
    else {
        out = unparseSignature(value);
    }
    EXPECT_EQ(std::string_view(out.c_str(), out.byte_length()), sig)
        << "roundtrip mismatch";
    return value;
}

/// @brief 断言解析失败且错误码/位置精确匹配。
void expectErr(std::string_view sig, SigErrorCode code, ca::u32 offset)
{
    auto r = parseSignature(sig);
    ASSERT_TRUE(r.is_err()) << sig << " unexpectedly parsed";
    const SigError e = std::move(r).unwrap_err();
    EXPECT_EQ(e.code, code) << sig << " -> " << to_string(e.code);
    EXPECT_EQ(e.offset, offset) << sig << " (" << to_string(e.code) << ")";
}

/// @brief 构造 n 个 '[' 的串。
std::string brackets(int n)
{
    return std::string(static_cast<ca::usize>(n), '[');
}

}   // namespace

// ---- 字段签名（FieldTypeSignature：类类型/类型变量/数组）----

TEST(SignatureTest, FieldClassTypes)
{
    auto pkg = expectOk(parseFieldSignature("Ljava/util/List;"), "Ljava/util/List;");
    EXPECT_EQ(pkg.kind, SigType::Kind::Class);
    EXPECT_EQ(pkg.class_type.package_prefix, "java/util/");
    ASSERT_EQ(pkg.class_type.segments.size(), 1u);
    EXPECT_EQ(pkg.class_type.segments[0].name, "List");
    EXPECT_TRUE(pkg.class_type.segments[0].type_args.empty());

    auto def = expectOk(parseFieldSignature("LFoo;"), "LFoo;");
    EXPECT_EQ(def.class_type.package_prefix, std::string_view{});
    EXPECT_EQ(def.class_type.segments[0].name, "Foo");
}

TEST(SignatureTest, FieldTypeVarsAndArrays)
{
    auto tv = expectOk(parseFieldSignature("TT;"), "TT;");
    EXPECT_EQ(tv.kind, SigType::Kind::TypeVar);
    EXPECT_EQ(tv.type_var_name, "T");
    EXPECT_EQ(tv.array_dims, 0u);

    auto arr_prim = expectOk(parseFieldSignature("[I"), "[I");
    EXPECT_EQ(arr_prim.kind, SigType::Kind::Primitive);
    EXPECT_EQ(arr_prim.primitive, BaseType::Int);
    EXPECT_EQ(arr_prim.array_dims, 1u);

    auto arr_cls = expectOk(parseFieldSignature("[[Ljava/lang/String;"),
                            "[[Ljava/lang/String;");
    EXPECT_EQ(arr_cls.kind, SigType::Kind::Class);
    EXPECT_EQ(arr_cls.class_type.segments[0].name, "String");
    EXPECT_EQ(arr_cls.array_dims, 2u);

    auto arr_tv = expectOk(parseFieldSignature("[[[TT;"), "[[[TT;");
    EXPECT_EQ(arr_tv.kind, SigType::Kind::TypeVar);
    EXPECT_EQ(arr_tv.array_dims, 3u);
}

TEST(SignatureTest, FieldGenericArgsNested)
{
    // Map<String, List<Integer>>：嵌套泛型实参
    const std::string_view sig = "Ljava/util/Map<Ljava/lang/String;Ljava/util/List<"
                                 "Ljava/lang/Integer;>;>;";
    auto t = expectOk(parseFieldSignature(sig), sig);
    const SigClassSegment& map = t.class_type.segments[0];
    ASSERT_EQ(map.type_args.size(), 2u);
    EXPECT_EQ(map.type_args[0].kind, SigTypeArg::Kind::Invariant);
    EXPECT_EQ(map.type_args[0].bound->kind, SigType::Kind::Class);
    EXPECT_EQ(map.type_args[0].bound->class_type.segments[0].name, "String");
    const SigType& list = *map.type_args[1].bound;
    ASSERT_EQ(list.class_type.segments.size(), 1u);
    ASSERT_EQ(list.class_type.segments[0].type_args.size(), 1u);
    EXPECT_EQ(list.class_type.segments[0].type_args[0].bound->class_type.segments[0].name,
              "Integer");
}

// ---- 类型实参（通配符三态 + 不变）----

TEST(SignatureTest, WildcardArgumentForms)
{
    auto star = expectOk(parseFieldSignature("Ljava/util/List<*>;"),
                         "Ljava/util/List<*>;");
    EXPECT_EQ(star.class_type.segments[0].type_args[0].kind,
              SigTypeArg::Kind::Unbounded);
    EXPECT_FALSE(star.class_type.segments[0].type_args[0].hasBound());

    auto upper = expectOk(parseFieldSignature("Ljava/util/List<+Ljava/lang/Number;>;"),
                          "Ljava/util/List<+Ljava/lang/Number;>;");
    const SigTypeArg& up = upper.class_type.segments[0].type_args[0];
    EXPECT_EQ(up.kind, SigTypeArg::Kind::UpperBound);
    EXPECT_EQ(up.bound->class_type.segments[0].name, "Number");

    auto lower = expectOk(parseFieldSignature("Ljava/util/List<-TT;>;"),
                          "Ljava/util/List<-TT;>;");
    const SigTypeArg& low = lower.class_type.segments[0].type_args[0];
    EXPECT_EQ(low.kind, SigTypeArg::Kind::LowerBound);
    EXPECT_EQ(low.bound->kind, SigType::Kind::TypeVar);

    // 通配符 × 数组 × 嵌套泛型交叠（JVMS TypeArguments 无逗号分隔：`*[I*TV;`
    // 即三实参——上界数组、无界、不变类型变量）。
    auto mixed = expectOk(parseFieldSignature("Ljava/util/Map<+[I*TV;>;"),
                          "Ljava/util/Map<+[I*TV;>;");
    const auto& args = mixed.class_type.segments[0].type_args;
    ASSERT_EQ(args.size(), 3u);
    EXPECT_EQ(args[0].kind, SigTypeArg::Kind::UpperBound);
    EXPECT_EQ(args[0].bound->kind, SigType::Kind::Primitive);
    EXPECT_EQ(args[0].bound->array_dims, 1u);
    EXPECT_EQ(args[1].kind, SigTypeArg::Kind::Unbounded);
    EXPECT_FALSE(args[1].hasBound());
    EXPECT_EQ(args[2].kind, SigTypeArg::Kind::Invariant);
    EXPECT_EQ(args[2].bound->kind, SigType::Kind::TypeVar);
}

// ---- 类类型段链（内部类成员）----

TEST(SignatureTest, ClassTypeInnerSegments)
{
    auto one = expectOk(parseFieldSignature("Lpkg/Outer$Like;"), "Lpkg/Outer$Like;");
    EXPECT_EQ(one.class_type.segments[0].name, "Outer$Like");   // '$' 是普通标识符字符

    auto inner = expectOk(parseFieldSignature("Lpkg/Outer.Inner;"),
                          "Lpkg/Outer.Inner;");
    EXPECT_EQ(inner.class_type.package_prefix, "pkg/");
    ASSERT_EQ(inner.class_type.segments.size(), 2u);
    EXPECT_EQ(inner.class_type.segments[0].name, "Outer");
    EXPECT_EQ(inner.class_type.segments[1].name, "Inner");

    // 每段可带独立实参（JVMS ClassTypeSignatureSuffix × SimpleClassTypeSignature）
    auto args = expectOk(parseFieldSignature("Lp/O<TT;>.I<Ljava/lang/String;>.J;"),
                         "Lp/O<TT;>.I<Ljava/lang/String;>.J;");
    ASSERT_EQ(args.class_type.segments.size(), 3u);
    EXPECT_EQ(args.class_type.segments[0].type_args.size(), 1u);
    EXPECT_EQ(args.class_type.segments[1].type_args[0].bound->class_type.segments[0].name,
              "String");
    EXPECT_TRUE(args.class_type.segments[2].type_args.empty());
}

// ---- 类签名（ClassSignature）----

TEST(SignatureTest, ClassSignatureForms)
{
    auto plain = expectOk(parseClassSignature("Ljava/lang/Object;"),
                          "Ljava/lang/Object;");
    EXPECT_TRUE(plain.type_params.empty());
    EXPECT_EQ(plain.superclass.class_type.segments[0].name, "Object");
    EXPECT_TRUE(plain.super_interfaces.empty());

    auto ifaces = expectOk(
        parseClassSignature("Ljava/lang/Object;Ljava/io/Serializable;Ljava/lang/Cloneable;"),
        "Ljava/lang/Object;Ljava/io/Serializable;Ljava/lang/Cloneable;");
    ASSERT_EQ(ifaces.super_interfaces.size(), 2u);
    EXPECT_EQ(ifaces.super_interfaces[0].class_type.segments[0].name, "Serializable");

    auto gen = expectOk(
        parseClassSignature("<K:Ljava/lang/Object;V:Ljava/util/List<TV;>;>"
                            "Ljava/util/HashMap<TK;TV;>;Ljava/lang/Cloneable;"),
        "<K:Ljava/lang/Object;V:Ljava/util/List<TV;>;>Ljava/util/HashMap<TK;TV;>;"
        "Ljava/lang/Cloneable;");
    ASSERT_EQ(gen.type_params.size(), 2u);
    EXPECT_EQ(gen.type_params[0].name, "K");
    EXPECT_EQ(gen.type_params[1].name, "V");
    ASSERT_NE(gen.type_params[1].class_bound, nullptr);
    EXPECT_EQ(gen.type_params[1].class_bound->kind, SigType::Kind::Class);
    ASSERT_EQ(gen.superclass.class_type.segments[0].type_args.size(), 2u);
}

TEST(SignatureTest, ClassSignatureRecursiveBoundAndEmptyClassBound)
{
    // 递归类型边界：<T extends Comparable<T>>
    auto rec = expectOk(parseClassSignature("<T:Ljava/lang/Comparable<TT;>;>Ljava/lang/Object;"),
                        "<T:Ljava/lang/Comparable<TT;>;>Ljava/lang/Object;");
    ASSERT_EQ(rec.type_params.size(), 1u);
    const SigType& bound = *rec.type_params[0].class_bound;
    ASSERT_EQ(bound.class_type.segments[0].type_args.size(), 1u);
    EXPECT_EQ(bound.class_type.segments[0].type_args[0].bound->kind, SigType::Kind::TypeVar);
    EXPECT_EQ(bound.class_type.segments[0].type_args[0].bound->type_var_name, "T");

    // 类界缺席的 javac 形态（`E::`，只有接口界）
    auto e = expectOk(parseClassSignature("<E::Ljava/lang/Comparable<TE;>;>Ljava/lang/Object;"),
                      "<E::Ljava/lang/Comparable<TE;>;>Ljava/lang/Object;");
    ASSERT_EQ(e.type_params.size(), 1u);
    EXPECT_EQ(e.type_params[0].class_bound, nullptr);
    ASSERT_EQ(e.type_params[0].interface_bounds.size(), 1u);
    EXPECT_EQ(e.type_params[0].interface_bounds[0].class_type.segments[0].name,
              "Comparable");

    // 显式空类界 + 接口界多值 + 类型变量作界（`U::TT;:TV;`：类界缺席，
    // 接口界两个类型变量）。
    auto multi = expectOk(
        parseClassSignature("<T:>Ljava/lang/Object;"),
        "<T:>Ljava/lang/Object;");
    EXPECT_EQ(multi.type_params[0].class_bound, nullptr);
    EXPECT_TRUE(multi.type_params[0].interface_bounds.empty());

    auto tv_bound = expectOk(
        parseClassSignature("<U::TT;:TV;>Ljava/lang/Object;"),
        "<U::TT;:TV;>Ljava/lang/Object;");
    EXPECT_EQ(tv_bound.type_params[0].class_bound, nullptr);
    ASSERT_EQ(tv_bound.type_params[0].interface_bounds.size(), 2u);
    EXPECT_EQ(tv_bound.type_params[0].interface_bounds[0].kind, SigType::Kind::TypeVar);
    EXPECT_EQ(tv_bound.type_params[0].interface_bounds[1].type_var_name, "V");
}

// ---- 方法签名（MethodSignature）----

TEST(SignatureTest, MethodSignatureForms)
{
    auto none = expectOk(parseMethodSignature("()V"), "()V");
    EXPECT_TRUE(none.params.empty());
    EXPECT_TRUE(none.ret_void);
    EXPECT_TRUE(none.throws_list.empty());

    auto main_ = expectOk(parseMethodSignature("([Ljava/lang/String;)V"),
                          "([Ljava/lang/String;)V");
    ASSERT_EQ(main_.params.size(), 1u);
    EXPECT_EQ(main_.params[0].array_dims, 1u);
    EXPECT_TRUE(main_.ret_void);

    // 裸基元在参数/返回位合法（TypeSignature = ReferenceType | BaseType）
    auto prims = expectOk(parseMethodSignature("(IJ[DZ)V"), "(IJ[DZ)V");
    ASSERT_EQ(prims.params.size(), 4u);
    EXPECT_EQ(prims.params[0].kind, SigType::Kind::Primitive);
    EXPECT_EQ(prims.params[2].array_dims, 1u);

    auto gen_ret = expectOk(
        parseMethodSignature("<U:Ljava/lang/Object;>(TU;)Ljava/util/List<TU;>;"),
        "<U:Ljava/lang/Object;>(TU;)Ljava/util/List<TU;>;");
    ASSERT_EQ(gen_ret.type_params.size(), 1u);
    ASSERT_FALSE(gen_ret.ret_void);
    EXPECT_EQ(gen_ret.ret.class_type.segments[0].name, "List");
    ASSERT_EQ(gen_ret.ret.class_type.segments[0].type_args.size(), 1u);
    EXPECT_EQ(gen_ret.ret.class_type.segments[0].type_args[0].bound->kind,
              SigType::Kind::TypeVar);

    auto throws = expectOk(
        parseMethodSignature("()V^Ljava/lang/IllegalStateException;^TEX;"),
        "()V^Ljava/lang/IllegalStateException;^TEX;");
    ASSERT_EQ(throws.throws_list.size(), 2u);
    EXPECT_EQ(throws.throws_list[0].kind, SigType::Kind::Class);
    EXPECT_EQ(throws.throws_list[1].kind, SigType::Kind::TypeVar);
    EXPECT_EQ(throws.throws_list[1].type_var_name, "EX");
}

// ---- roundtrip 恒等（含深嵌套/多通配/数组×泛型交叠）----

TEST(SignatureTest, RoundtripIdentityDeepAndWide)
{
    const std::string_view cases[] = {
        "Ljava/util/List<Ljava/util/List<Ljava/util/List<Ljava/lang/String;>;>;>;",
        "Ljava/util/Map<+Ljava/lang/Number;-Ljava/lang/Integer;*>;",
        "[[Ljava/util/Map<*[TT;+[Ljava/util/List<-[[I>;>;",
        "<A:TB;:TC;D:Ljava/lang/Object;>([[TA;[TD;)V",
        "Lp/A<TT;>.B<Lp/C<*>;>.D<[[I>;",
        "(TT;Ljava/util/List<+Ljava/util/Map<Ljava/lang/String;*>;>;[I)V^TE;^Ljava/lang/Exception;",
        "<T:Ljava/lang/Object;>(TT;)TT;^TT;",
        "Ljava/lang/Enum<TE;>;",
        "()Ljava/lang/invoke/MethodHandle;",
        "<X:Lp/B<TX;>.C;>(Lp/D<TX;>.E<[[TT;>;)V",
    };
    for (const std::string_view sig : cases) {
        auto r = parseSignature(sig);
        ASSERT_TRUE(r.is_ok()) << sig;
        const Signature tree = std::move(r).unwrap();
        const ca::str::Utf8String out = unparseSignature(tree);
        EXPECT_EQ(std::string_view(out.c_str(), out.byte_length()), sig)
            << "roundtrip mismatch";
        // 树形态分派自洽：再 parse 一次仍恒等（手术后重读恒等的合成版）。
        auto again = parseSignature(std::string_view(out.c_str(), out.byte_length()));
        ASSERT_TRUE(again.is_ok()) << sig;
        const ca::str::Utf8String out2 = unparseSignature(std::move(again).unwrap());
        EXPECT_EQ(std::string_view(out2.c_str(), out2.byte_length()), sig);
    }
}

// ---- 自动分派（parseSignature）----

TEST(SignatureTest, AutoFormDispatch)
{
    auto m = parseSignature("(I)V");
    ASSERT_TRUE(m.is_ok());
    EXPECT_EQ(std::move(m).unwrap().form, SignatureForm::Method);

    auto f = parseSignature("[I");
    ASSERT_TRUE(f.is_ok());
    EXPECT_EQ(std::move(f).unwrap().form, SignatureForm::Field);

    auto tv = parseSignature("TT;");
    ASSERT_TRUE(tv.is_ok());
    EXPECT_EQ(std::move(tv).unwrap().form, SignatureForm::Field);

    // 单个 L...; 语法上是类形态的严格子集：按自动分派规则归类形态（成文）。
    auto c = parseSignature("Ljava/util/List;");
    ASSERT_TRUE(c.is_ok());
    EXPECT_EQ(std::move(c).unwrap().form, SignatureForm::Class);

    auto iface = parseSignature("Ljava/lang/Object;Ljava/io/Serializable;");
    ASSERT_TRUE(iface.is_ok());
    EXPECT_EQ(std::move(iface).unwrap().form, SignatureForm::Class);

    auto tp = parseSignature("<T:Ljava/lang/Object;>Ljava/lang/Object;");
    ASSERT_TRUE(tp.is_ok());
    EXPECT_EQ(std::move(tp).unwrap().form, SignatureForm::Class);

    auto tpm = parseSignature("<T:Ljava/lang/Object;>(TT;)V");
    ASSERT_TRUE(tpm.is_ok());
    EXPECT_EQ(std::move(tpm).unwrap().form, SignatureForm::Method);
}

// ---- 畸形输入：显式 Err（码 + 位置）----

TEST(SignatureTest, MalformedInputsExplicitErrors)
{
    // 空与形态
    expectErr("", SigErrorCode::Empty, 0);
    expectErr("X", SigErrorCode::BadSignatureShape, 0);
    expectErr("I)V", SigErrorCode::BadSignatureShape, 0);
    expectErr("V", SigErrorCode::BadSignatureShape, 0);
    // 截断
    expectErr("L", SigErrorCode::Truncated, 1);
    expectErr("Lcom", SigErrorCode::Truncated, 4);
    expectErr("Lcom/A", SigErrorCode::Truncated, 6);
    expectErr("TT", SigErrorCode::Truncated, 2);
    expectErr("[", SigErrorCode::Truncated, 1);
    expectErr("(I", SigErrorCode::Truncated, 2);
    expectErr("()L", SigErrorCode::Truncated, 3);
    expectErr("()TT", SigErrorCode::Truncated, 4);
    expectErr("Ljava/util/List<", SigErrorCode::Truncated, 16);
    // 尾随
    expectErr("Ljava/lang/Object;X", SigErrorCode::TrailingChars, 18);
    expectErr("TT;X", SigErrorCode::TrailingChars, 3);
    expectErr("()VX", SigErrorCode::TrailingChars, 3);
    // 标识符
    expectErr("L;", SigErrorCode::BadIdentifier, 1);
    expectErr("L/A;", SigErrorCode::BadIdentifier, 1);
    expectErr("Lcom//A;", SigErrorCode::BadIdentifier, 5);
    expectErr("LA..B;", SigErrorCode::BadIdentifier, 3);
    expectErr("T;", SigErrorCode::BadIdentifier, 1);
    // 类型参数表
    expectErr("<>Ljava/lang/Object;", SigErrorCode::BadTypeParams, 1);
    expectErr("<T>Ljava/lang/Object;", SigErrorCode::BadTypeParams, 2);
    expectErr("<:Ljava/lang/Object;>Ljava/lang/Object;", SigErrorCode::BadIdentifier, 1);
    // 界
    expectErr("<T:(I)V>", SigErrorCode::BadBound, 3);
    expectErr("<E::>Ljava/lang/Object;", SigErrorCode::BadBound, 4);
    // 类型实参表
    expectErr("Ljava/util/List<>;", SigErrorCode::BadTypeArg, 16);
    expectErr("Ljava/util/List<;", SigErrorCode::BadBaseType, 16);
    // 裸基元/未知字符在纯引用位
    expectErr("Ljava/util/List<I>;", SigErrorCode::BadPrimitive, 16);
    expectErr("(X)V", SigErrorCode::BadBaseType, 1);
    expectErr("()X", SigErrorCode::BadBaseType, 2);
    // 类类型
    expectErr("LA<..>;", SigErrorCode::BadBaseType, 3);
    expectErr("LA<TT;>X;", SigErrorCode::BadClassType, 7);
    // 方法形态
    expectErr("()V^X", SigErrorCode::BadThrows, 4);
    expectErr("()V^", SigErrorCode::BadThrows, 4);
}

TEST(SignatureTest, NestingLimits)
{
    // 数组维数 255 合法、256 拒绝（对齐描述符层/classfile 上限）。
    const std::string ok_arr = brackets(255) + "I";
    auto ok = parseFieldSignature(ok_arr);
    ASSERT_TRUE(ok.is_ok());
    EXPECT_EQ(std::move(ok).unwrap().array_dims, 255u);

    const std::string bad_arr = brackets(256) + "I";
    auto bad = parseFieldSignature(bad_arr);
    ASSERT_TRUE(bad.is_err());
    EXPECT_EQ(std::move(bad).unwrap_err().code, SigErrorCode::NestingTooDeep);

    // 类型实参嵌套 255 层合法（上限对齐数组维数的 255）；256 层拒绝（防栈炸弹）。
    auto buildNested = [](int levels) {
        std::string s;
        for (int i = 0; i < levels; ++i) {
            s += "LA<";
        }
        s += "TT;";
        for (int i = 0; i < levels; ++i) {
            s += ">;";
        }
        return s;
    };
    auto deep_ok = parseFieldSignature(buildNested(255));
    ASSERT_TRUE(deep_ok.is_ok());
    auto deep_bad = parseFieldSignature(buildNested(256));
    ASSERT_TRUE(deep_bad.is_err());
    EXPECT_EQ(std::move(deep_bad).unwrap_err().code, SigErrorCode::NestingTooDeep);
}

// ---- 手术面：span 坐标 ----

TEST(SignatureTest, ClassNameSpansInOrder)
{
    // <T:Ljava/lang/Bound;>([Ljava/util/List<TT;>;Lpkg/Outer<TT;>.Inner;)Ljava/util/A<Lp/B;>;
    //  0        4          19 21  24            38    43 44        54      60     65 67  79   84
    const std::string_view sig = "<T:Ljava/lang/Bound;>([Ljava/util/List<TT;>;"
                                 "Lpkg/Outer<TT;>.Inner;)Ljava/util/A<Lp/B;>;";
    auto r = classNameSpans(sig, SignatureForm::Method);
    ASSERT_TRUE(r.is_ok());
    const auto spans = std::move(r).unwrap();
    // 字节序：界类 → 参数 List → 参数 Outer（首段全名）+ Inner（后缀段名）→
    // 返回 A → 实参 B。
    ASSERT_EQ(spans.size(), 6u);
    EXPECT_EQ(spans[0].kind, SigClassNameSpan::Kind::InternalName);
    EXPECT_EQ(sig.substr(spans[0].offset, spans[0].length), "java/lang/Bound");
    EXPECT_EQ(spans[0].offset, 4u);
    EXPECT_EQ(sig.substr(spans[1].offset, spans[1].length), "java/util/List");
    EXPECT_EQ(spans[1].offset, 24u);
    EXPECT_EQ(spans[2].kind, SigClassNameSpan::Kind::InternalName);
    EXPECT_EQ(sig.substr(spans[2].offset, spans[2].length), "pkg/Outer");
    EXPECT_EQ(spans[2].offset, 45u);
    EXPECT_EQ(spans[3].kind, SigClassNameSpan::Kind::SimpleName);
    EXPECT_EQ(sig.substr(spans[3].offset, spans[3].length), "Inner");
    EXPECT_EQ(spans[3].offset, 60u);
    EXPECT_EQ(sig.substr(spans[4].offset, spans[4].length), "java/util/A");
    EXPECT_EQ(spans[4].offset, 68u);
    EXPECT_EQ(sig.substr(spans[5].offset, spans[5].length), "p/B");
    EXPECT_EQ(spans[5].offset, 81u);

    // 语法非法即 Err，不产出半份数据。
    auto bad = classNameSpans("Ljava/util/List<", SignatureForm::Field);
    ASSERT_TRUE(bad.is_err());
    EXPECT_EQ(std::move(bad).unwrap_err().code, SigErrorCode::Truncated);
}

TEST(SignatureTest, ClassTypeSpansWithInnerSegments)
{
    // Lp/O<TT;>.I<LX;>.J;
    //  0  3    9  12     16 18
    const std::string_view sig = "Lp/O<TT;>.I<LX;>.J;";
    auto r = classNameSpans(sig, SignatureForm::Field);
    ASSERT_TRUE(r.is_ok());
    const auto spans = std::move(r).unwrap();
    ASSERT_EQ(spans.size(), 4u);
    EXPECT_EQ(spans[0].kind, SigClassNameSpan::Kind::InternalName);
    EXPECT_EQ(sig.substr(spans[0].offset, spans[0].length), "p/O");
    EXPECT_EQ(spans[0].offset, 1u);
    EXPECT_EQ(spans[1].kind, SigClassNameSpan::Kind::SimpleName);
    EXPECT_EQ(sig.substr(spans[1].offset, spans[1].length), "I");
    EXPECT_EQ(spans[1].offset, 10u);
    // 段实参内的下级类名按字节序排在两段之间。
    EXPECT_EQ(spans[2].kind, SigClassNameSpan::Kind::InternalName);
    EXPECT_EQ(sig.substr(spans[2].offset, spans[2].length), "X");
    EXPECT_EQ(spans[2].offset, 13u);
    EXPECT_EQ(spans[3].kind, SigClassNameSpan::Kind::SimpleName);
    EXPECT_EQ(sig.substr(spans[3].offset, spans[3].length), "J");
    EXPECT_EQ(spans[3].offset, 17u);
}

TEST(SignatureTest, ReplaceClassNameSpanSemantics)
{
    const std::string_view sig = "Ljava/util/Map<TK;[Ljava/util/List;>;";
    auto spans = classNameSpans(sig, SignatureForm::Field);
    ASSERT_TRUE(spans.is_ok());
    auto list = std::move(spans).unwrap();
    ASSERT_EQ(list.size(), 2u);

    auto first = replaceClassNameSpan(sig, list[0], "x/HashMap");
    ASSERT_TRUE(first.is_ok());
    ca::str::Utf8String out = std::move(first).unwrap();
    EXPECT_EQ(std::string_view(out.c_str(), out.byte_length()),
              "Lx/HashMap<TK;[Ljava/util/List;>;");

    auto second = replaceClassNameSpan(sig, list[1], "x/ArrayList");
    ASSERT_TRUE(second.is_ok());
    out = std::move(second).unwrap();
    EXPECT_EQ(std::string_view(out.c_str(), out.byte_length()),
              "Ljava/util/Map<TK;[Lx/ArrayList;>;");

    // 改写结果可重新解析（手术后重 parse 恒等的单点版）。
    auto reparsed = parseFieldSignature(std::string_view{out.c_str(), out.byte_length()});
    EXPECT_TRUE(reparsed.is_ok());

    // 新名按 span 形态校验：内部类段（SimpleName）禁 '/'；非法即 Err 不产出半截串。
    const std::string_view inner_sig = "Lp/O.I;";
    auto inner_spans = classNameSpans(inner_sig, SignatureForm::Field);
    ASSERT_TRUE(inner_spans.is_ok());
    auto inner_list = std::move(inner_spans).unwrap();
    ASSERT_EQ(inner_list.size(), 2u);
    EXPECT_EQ(inner_list[1].kind, SigClassNameSpan::Kind::SimpleName);
    EXPECT_TRUE(replaceClassNameSpan(inner_sig, inner_list[1], "x/Name").is_err());
    EXPECT_TRUE(replaceClassNameSpan(sig, list[0], "").is_err());
    const SigClassNameSpan out_of_range{SigClassNameSpan::Kind::InternalName, 100u, 4u};
    EXPECT_TRUE(replaceClassNameSpan(sig, out_of_range, "x/Y").is_err());
}

// ---- 手术面：rename 便捷 API ----

TEST(SignatureTest, RenameTopLevelClassAcrossPositions)
{
    const std::string_view sig = "<T:Ldemo/Expr;>Ljava/lang/Object;Ldemo/Expr;"
                                 "Lother/Keeper<Ldemo/Expr;>;";
    auto r = renameClassInSignature(sig, SignatureForm::Class, "demo/Expr", "renamed/Expr2");
    ASSERT_TRUE(r.is_ok());
    const SigRenameResult res = std::move(r).unwrap();
    EXPECT_EQ(res.hits, 3u);   // 界 + 接口 + 实参
    EXPECT_EQ(std::string_view(res.signature.c_str(), res.signature.byte_length()),
              "<T:Lrenamed/Expr2;>Ljava/lang/Object;Lrenamed/Expr2;"
              "Lother/Keeper<Lrenamed/Expr2;>;");
    // 手术后重 parse 恒等；逆向 rename 还原原串（改写可逆、其余字节零漂移）。
    const std::string_view out =
        std::string_view{res.signature.c_str(), res.signature.byte_length()};
    auto reparsed = parseClassSignature(out);
    ASSERT_TRUE(reparsed.is_ok());
    auto back = renameClassInSignature(out, SignatureForm::Class, "renamed/Expr2",
                                       "demo/Expr");
    ASSERT_TRUE(back.is_ok());
    EXPECT_STREQ(std::move(back).unwrap().signature.c_str(), sig.data());
}

TEST(SignatureTest, RenameInnerClassByInternalName)
{
    // 形态一（javac 常规产物）：内部名以段名内嵌 '$' 出现，单段覆盖两部件。
    const std::string_view dollar_sig = "Ljava/util/Function<Ljbcx/Outer$Inner;>;";
    auto r1 = renameClassInSignature(dollar_sig, SignatureForm::Field,
                                     "jbcx/Outer$Inner", "x/New$Renamed");
    ASSERT_TRUE(r1.is_ok());
    const SigRenameResult res1 = std::move(r1).unwrap();
    EXPECT_EQ(res1.hits, 1u);
    EXPECT_EQ(std::string_view(res1.signature.c_str(), res1.signature.byte_length()),
              "Ljava/util/Function<Lx/New$Renamed;>;");

    // 形态二（JVMS 段链）：'.' 后缀段按 '$' 合成匹配，段链与实参原样保留。
    const std::string_view chain_sig = "Lpkg/Outer<TT;>.Inner;";
    auto r2 = renameClassInSignature(chain_sig, SignatureForm::Field,
                                     "pkg/Outer$Inner", "np/New$Renamed");
    ASSERT_TRUE(r2.is_ok());
    const SigRenameResult res2 = std::move(r2).unwrap();
    EXPECT_EQ(res2.hits, 1u);
    EXPECT_EQ(std::string_view(res2.signature.c_str(), res2.signature.byte_length()),
              "Lnp/New<TT;>.Renamed;");

    // 跨形态：扁平段链（单段名）把嵌套名改成顶级名——new 内部名按 '$'
    // 合成写满本段（javac 通行非规范 $ 形态）；真嵌套段链不做结构重排，
    // 部件越界即不命中（原串零改写）。
    auto flat = renameClassInSignature(dollar_sig, SignatureForm::Field,
                                       "jbcx/Outer$Inner", "np/Flat");
    ASSERT_TRUE(flat.is_ok());
    const SigRenameResult flatRes = std::move(flat).unwrap();
    EXPECT_EQ(flatRes.hits, 1u);
    EXPECT_EQ(std::string_view(flatRes.signature.c_str(), flatRes.signature.byte_length()),
              "Ljava/util/Function<Lnp/Flat;>;");

    // 真嵌套段链跨形态 → 不命中跳过（hits==0 原串，非 Err）。
    auto bad = renameClassInSignature(chain_sig, SignatureForm::Field,
                                      "pkg/Outer$Inner", "np/Flat");
    ASSERT_TRUE(bad.is_ok());
    const SigRenameResult badRes = std::move(bad).unwrap();
    EXPECT_EQ(badRes.hits, 0u);
    EXPECT_EQ(std::string_view(badRes.signature.c_str(), badRes.signature.byte_length()),
              chain_sig);
}

TEST(SignatureTest, RenameNoHitAndBadNames)
{
    const std::string_view sig = "Ljava/util/List<Ljava/lang/String;>;";
    auto miss = renameClassInSignature(sig, SignatureForm::Field, "demo/Absent",
                                       "demo/New");
    ASSERT_TRUE(miss.is_ok());
    const SigRenameResult res = std::move(miss).unwrap();
    EXPECT_EQ(res.hits, 0u);
    EXPECT_STREQ(res.signature.c_str(), "Ljava/util/List<Ljava/lang/String;>;");

    // 前缀污染不误伤：demo/Expr 不是 demo/ExprExtra。
    const std::string_view tricky = "Ldemo/ExprExtra;";
    auto no_prefix = renameClassInSignature(tricky, SignatureForm::Field, "demo/Expr",
                                            "x/Y");
    ASSERT_TRUE(no_prefix.is_ok());
    EXPECT_EQ(std::move(no_prefix).unwrap().hits, 0u);

    EXPECT_TRUE(renameClassInSignature(sig, SignatureForm::Field, "", "x/Y").is_err());
    EXPECT_TRUE(renameClassInSignature(sig, SignatureForm::Field, "a/B", "").is_err());
    EXPECT_TRUE(renameClassInSignature(sig, SignatureForm::Field, "a/B;", "x/Y").is_err());
    EXPECT_TRUE(renameClassInSignature(sig, SignatureForm::Field, "a/B", "x;Y").is_err());
    // 尾 '$' / 连 '$'（kotlin 空段真名）字符集合法——不再 Err：本扁平
    // 签名无命中返回 hits==0；真嵌套 '.' 段链对带空 $ 段的规则名不可达
    // （按不命中降级，见跨形态口径）。
    auto dollarTail = renameClassInSignature(sig, SignatureForm::Field, "a/B$", "x/Y$Z");
    ASSERT_TRUE(dollarTail.is_ok());
    EXPECT_EQ(std::move(dollarTail).unwrap().hits, 0u);
}

TEST(SignatureTest, RenamePreservesUnrelatedBytes)
{
    // 通配符/数组/实参结构与空格外字节全部保留（含标识符里的非 ASCII 字节）。
    const std::string_view sig = "Ljava/util/Map<+[Ldemo/A;-TT;*Ljava/lang/I=\"x\";>;";
    auto r = renameClassInSignature(sig, SignatureForm::Field, "demo/A", "n/B");
    ASSERT_TRUE(r.is_ok());
    const SigRenameResult res = std::move(r).unwrap();
    EXPECT_EQ(res.hits, 1u);
    EXPECT_EQ(std::string_view(res.signature.c_str(), res.signature.byte_length()),
              "Ljava/util/Map<+[Ln/B;-TT;*Ljava/lang/I=\"x\";>;");
}

TEST(SignatureTest, InternalNameComposition)
{
    auto inner = parseFieldSignature("Lp/O<TT;>.I<LX;>.K;");
    ASSERT_TRUE(inner.is_ok());
    const ca::str::Utf8String name =
        internalNameOf(std::move(inner).unwrap().class_type);
    EXPECT_STREQ(name.c_str(), "p/O$I$K");

    auto top = parseFieldSignature("Lpkg/Outer;");
    ASSERT_TRUE(top.is_ok());
    EXPECT_STREQ(internalNameOf(std::move(top).unwrap().class_type).c_str(), "pkg/Outer");
}

TEST(SignatureTest, NameValidityHelpers)
{
    EXPECT_FALSE(isValidSigIdentifier("a/b"));   // '/' 保留（JVMS Identifier 禁用，包分隔走 InternalName 面）
    EXPECT_TRUE(isValidSigIdentifier("A$B"));
    EXPECT_FALSE(isValidSigIdentifier(""));
    EXPECT_FALSE(isValidSigIdentifier("a.b"));
    EXPECT_FALSE(isValidSigIdentifier("a;b"));
    EXPECT_TRUE(isValidSigInternalName("p/q/R"));
    EXPECT_FALSE(isValidSigInternalName("p<q"));
    EXPECT_TRUE(isValidSigSimpleName("R$1"));
    EXPECT_FALSE(isValidSigSimpleName("p/R"));
}
