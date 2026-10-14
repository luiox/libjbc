/// @file        type_test.cpp
/// @brief       类型/描述符系统测试：字段/方法描述符解析、类名 span 与
///        手术改写、槽宽、内部名/二进制名换算。
#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "jbc/jbc.hpp"

using namespace jbc::type;

namespace {

/// @brief 断言字段类型解析失败且错误码匹配。
void expectFieldErr(std::string_view desc, DescError expect)
{
    auto r = parseFieldType(desc);
    ASSERT_TRUE(r.is_err()) << desc;
    EXPECT_EQ(std::move(r).unwrap_err(), expect) << desc;
}

/// @brief 断言方法描述符解析失败且错误码匹配。
void expectMethodErr(std::string_view desc, DescError expect)
{
    auto r = parseMethodDescriptor(desc);
    ASSERT_TRUE(r.is_err()) << desc;
    EXPECT_EQ(std::move(r).unwrap_err(), expect) << desc;
}

}   // namespace

TEST(TypeTest, ParsesPrimitives)
{
    const char* one = "BCDFIJSZ";
    for (const char c : std::string_view{one}) {
        auto r = parseFieldType(std::string_view{&c, 1});
        ASSERT_TRUE(r.is_ok()) << c;
        const TypeDesc t = std::move(r).unwrap();
        EXPECT_EQ(t.kind, TypeDesc::Kind::Primitive);
        EXPECT_EQ(t.array_dims, 0u);
    }
    auto j = parseFieldType("J");
    ASSERT_TRUE(j.is_ok());
    EXPECT_EQ(std::move(j).unwrap().primitive, BaseType::Long);
}

TEST(TypeTest, ParsesClassAndArrays)
{
    auto cls = parseFieldType("Ljava/lang/String;");
    ASSERT_TRUE(cls.is_ok());
    TypeDesc t = std::move(cls).unwrap();
    EXPECT_EQ(t.kind, TypeDesc::Kind::Class);
    EXPECT_EQ(t.class_name, "java/lang/String");
    EXPECT_EQ(t.array_dims, 0u);

    auto arr = parseFieldType("[[D");
    ASSERT_TRUE(arr.is_ok());
    t = std::move(arr).unwrap();
    EXPECT_EQ(t.kind, TypeDesc::Kind::Primitive);
    EXPECT_EQ(t.primitive, BaseType::Double);
    EXPECT_EQ(t.array_dims, 2u);

    auto objarr = parseFieldType("[[Lf/A;;");   // 多余 ';' → 尾随字符
    EXPECT_TRUE(objarr.is_err());
}

TEST(TypeTest, RejectsMalformedFieldTypes)
{
    expectFieldErr("", DescError::Empty);
    expectFieldErr("V", DescError::BadBaseType);        // V 不是字段类型
    expectFieldErr("X", DescError::BadBaseType);        // 未知名
    expectFieldErr("L;", DescError::BadClassName);      // 空类名
    expectFieldErr("La.b;", DescError::BadClassName);   // 点是二进制名残留
    expectFieldErr("La[b;", DescError::BadClassName);   // '[' 不得出现在类名内
    expectFieldErr("Lja/va", DescError::Truncated);     // 缺 ';'
    expectFieldErr("[", DescError::Truncated);          // 孤立 '['
    expectFieldErr("II", DescError::TrailingChars);     // 恰好一个类型
    expectFieldErr("[V", DescError::BadBaseType);       // 数组叶不得为 V
}

TEST(TypeTest, ParsesMethodDescriptors)
{
    auto none = parseMethodDescriptor("()V");
    ASSERT_TRUE(none.is_ok());
    MethodDesc m = std::move(none).unwrap();
    EXPECT_TRUE(m.params.empty());
    EXPECT_TRUE(m.ret_void);

    auto main_ = parseMethodDescriptor("([Ljava/lang/String;)V");
    ASSERT_TRUE(main_.is_ok());
    m = std::move(main_).unwrap();
    ASSERT_EQ(m.params.size(), 1u);
    EXPECT_EQ(m.params[0].kind, TypeDesc::Kind::Class);
    EXPECT_EQ(m.params[0].class_name, "java/lang/String");
    EXPECT_EQ(m.params[0].array_dims, 1u);
    EXPECT_TRUE(m.ret_void);

    auto mix = parseMethodDescriptor("(IJ[Ljava/lang/String;[[D)Ljava/lang/Object;");
    ASSERT_TRUE(mix.is_ok());
    m = std::move(mix).unwrap();
    ASSERT_EQ(m.params.size(), 4u);
    EXPECT_EQ(m.params[0].primitive, BaseType::Int);
    EXPECT_EQ(m.params[1].primitive, BaseType::Long);
    EXPECT_EQ(m.params[2].class_name, "java/lang/String");
    EXPECT_EQ(m.params[3].primitive, BaseType::Double);
    EXPECT_EQ(m.params[3].array_dims, 2u);
    EXPECT_FALSE(m.ret_void);
    EXPECT_EQ(m.ret.class_name, "java/lang/Object");
}

TEST(TypeTest, RejectsMalformedMethodDescriptors)
{
    expectMethodErr("", DescError::Empty);
    expectMethodErr("I)V", DescError::BadMethodShape);
    expectMethodErr("(I", DescError::Truncated);
    expectMethodErr("(", DescError::Truncated);
    expectMethodErr("(V)V", DescError::BadBaseType);    // V 不得作参数
    expectMethodErr("()II", DescError::TrailingChars);
    expectMethodErr("(I)Lx", DescError::Truncated);
}

TEST(TypeTest, ClassSpansOnMethodDescriptor)
{
    const std::string_view desc = "(Lcom/a/B;IJ[[Lcom/c/D;)V";
    auto r = classNameSpans(desc);
    ASSERT_TRUE(r.is_ok());
    const auto spans = std::move(r).unwrap();
    ASSERT_EQ(spans.size(), 2u);
    EXPECT_EQ(spans[0].offset, 2u);
    EXPECT_EQ(spans[0].length, 7u);
    EXPECT_EQ(desc.substr(spans[0].offset, spans[0].length), "com/a/B");
    EXPECT_EQ(desc.substr(spans[1].offset, spans[1].length), "com/c/D");
}

TEST(TypeTest, ClassSpansOnFieldDescriptorAndErrors)
{
    const std::string_view field = "[[Lz/W;";
    auto r = classNameSpans(field);
    ASSERT_TRUE(r.is_ok());
    const auto spans = std::move(r).unwrap();
    ASSERT_EQ(spans.size(), 1u);
    EXPECT_EQ(field.substr(spans[0].offset, spans[0].length), "z/W");

    auto plain = classNameSpans("Lx/Y;");
    ASSERT_TRUE(plain.is_ok());
    const auto one = std::move(plain).unwrap();
    ASSERT_EQ(one.size(), 1u);
    EXPECT_EQ(one[0].offset, 1u);

    // 语法非法即 Err，不产出半份 span。
    auto bad = classNameSpans("(La/B;V");
    EXPECT_TRUE(bad.is_err());
}

TEST(TypeTest, ReplaceClassNameRewritesOnlyTargetSpan)
{
    const std::string_view desc = "(Lcom/a/B;[Lcom/a/B;)V";
    auto spans = classNameSpans(desc);
    ASSERT_TRUE(spans.is_ok());
    auto list = std::move(spans).unwrap();
    ASSERT_EQ(list.size(), 2u);

    auto first = replaceClassName(desc, list[0], "x/Y");
    ASSERT_TRUE(first.is_ok());
    ca::str::Utf8String out = std::move(first).unwrap();
    EXPECT_EQ(std::string_view(out.c_str(), out.byte_length()), "(Lx/Y;[Lcom/a/B;)V");

    auto second = replaceClassName(desc, list[1], "x/Y");
    ASSERT_TRUE(second.is_ok());
    out = std::move(second).unwrap();
    EXPECT_EQ(std::string_view(out.c_str(), out.byte_length()), "(Lcom/a/B;[Lx/Y;)V");

    // 改写结果可重新解析，span 数不变。
    auto reparsed = parseMethodDescriptor(std::string_view{out.c_str(), out.byte_length()});
    EXPECT_TRUE(reparsed.is_ok());
}

TEST(TypeTest, ReplaceClassNameRejectsBadInput)
{
    const std::string_view desc = "(Lcom/a/B;)V";
    auto spans = classNameSpans(desc);
    ASSERT_TRUE(spans.is_ok());
    auto list = std::move(spans).unwrap();

    EXPECT_TRUE(replaceClassName(desc, list[0], "x.y").is_err());     // 二进制名
    EXPECT_TRUE(replaceClassName(desc, list[0], "").is_err());        // 空名
    const ClassNameSpan out_of_range{100u, 4u};
    EXPECT_TRUE(replaceClassName(desc, out_of_range, "x/Y").is_err());
}

TEST(TypeTest, SlotWidths)
{
    auto w = parseFieldType("J");
    ASSERT_TRUE(w.is_ok());
    EXPECT_EQ(slotWidth(std::move(w).unwrap()), 2u);
    auto d = parseFieldType("D");
    ASSERT_TRUE(d.is_ok());
    EXPECT_EQ(slotWidth(std::move(d).unwrap()), 2u);
    auto i = parseFieldType("I");
    ASSERT_TRUE(i.is_ok());
    EXPECT_EQ(slotWidth(std::move(i).unwrap()), 1u);
    auto a = parseFieldType("[J");   // 数组是引用：一槽
    ASSERT_TRUE(a.is_ok());
    EXPECT_EQ(slotWidth(std::move(a).unwrap()), 1u);

    auto m = parseMethodDescriptor("(JIDLjava/lang/String;)V");
    ASSERT_TRUE(m.is_ok());
    EXPECT_EQ(paramSlotCount(std::move(m).unwrap()), 6u);   // 2+1+2+1

    auto main_ = parseMethodDescriptor("([Ljava/lang/String;)V");
    ASSERT_TRUE(main_.is_ok());
    EXPECT_EQ(paramSlotCount(std::move(main_).unwrap()), 1u);
}

TEST(TypeTest, InternalBinaryNameConversions)
{
    EXPECT_TRUE(isValidInternalName("a/b/C"));
    EXPECT_TRUE(isValidInternalName("C"));
    EXPECT_FALSE(isValidInternalName(""));
    EXPECT_FALSE(isValidInternalName("a.b.C"));
    EXPECT_FALSE(isValidInternalName("a[b"));
    EXPECT_FALSE(isValidInternalName("a;b"));

    auto bin = internalToBinary("a/b/C");
    EXPECT_STREQ(bin.c_str(), "a.b.C");
    auto in = binaryToInternal("a.b.C");
    EXPECT_STREQ(in.c_str(), "a/b/C");
}
