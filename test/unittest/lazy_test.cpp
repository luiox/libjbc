/// @file        lazy_test.cpp
/// @brief       lazy 语义：没碰就没解、解码幂等、改动必留痕、Code 内嵌套同样 lazy。
#include <gtest/gtest.h>

#include "jbc/jbc.hpp"
#include "test_class_bytes.hpp"

using namespace jbc::core;
using jbc::test::Bytes;
using jbc::test::ClassBuilder;
using jbc::test::makeRichClass;

/// lazy 的第一条承诺：解析一个类，一个属性体都不会被创建。
/// 这是「扫描几万个类」可行性的全部来源。
TEST(LazyTest, NothingIsDecodedUntilAsked)
{
    const Bytes bytes = makeRichClass();
    auto cf = jbc::test::readOk(bytes);
    ASSERT_NE(cf, nullptr);

    for (const AttrSlot& s : cf->attrs) {
        EXPECT_FALSE(s.decoded()) << "class attr decoded without being asked";
    }
    for (const MemberInfo& m : cf->methods) {
        for (const AttrSlot& s : m.attrs) {
            EXPECT_FALSE(s.decoded()) << "method attr decoded without being asked";
        }
    }
    for (const MemberInfo& m : cf->fields) {
        for (const AttrSlot& s : m.attrs) {
            EXPECT_FALSE(s.decoded()) << "field attr decoded without being asked";
        }
    }
}

/// 只解一个属性：其余仍保持未解码——lazy 是逐个的，不是全有全无。
TEST(LazyTest, DecodingOneAttrLeavesOthersLazy)
{
    const Bytes bytes = makeRichClass();
    auto cf = jbc::test::readOk(bytes);
    ASSERT_NE(cf, nullptr);

    const Result<CodeAttr*, ReadError> r = cf->decodeCode(cf->methods[0]);
    ASSERT_TRUE(r.is_ok());
    ASSERT_NE(r.unwrap(), nullptr);

    EXPECT_TRUE(cf->methods[0].attrs[0].decoded());   // Code 解了
    EXPECT_FALSE(cf->methods[0].attrs[1].decoded());  // 同方法的未知属性没解
    EXPECT_FALSE(cf->attrs[0].decoded());             // 类级属性没解
}

/// 解码幂等：重复调用返回同一对象，不重新解析、不新增分配。
TEST(LazyTest, DecodeIsIdempotent)
{
    const Bytes bytes = makeRichClass();
    auto cf = jbc::test::readOk(bytes);
    ASSERT_NE(cf, nullptr);

    const Result<CodeAttr*, ReadError> r1 = cf->decodeCode(cf->methods[0]);
    ASSERT_TRUE(r1.is_ok());
    CodeAttr* first = r1.unwrap();

    const Result<AttrBody*, ReadError> r2 = cf->decodeAttr(cf->methods[0].attrs[0]);
    ASSERT_TRUE(r2.is_ok());
    EXPECT_EQ(r2.unwrap(), first);

    const Result<CodeAttr*, ReadError> r3 = cf->decodeCode(cf->methods[0]);
    ASSERT_TRUE(r3.is_ok());
    EXPECT_EQ(r3.unwrap(), first);
}

/// 改动不可能被忘记：可变访问只有 mutate() 一个入口，它顺手置脏。
/// 旧库「改了但没标记 → 改动被静默丢弃」这类 bug 在此结构上写不出来。
TEST(LazyTest, MutateIsTheOnlyWayToChangeAndItMarksDirty)
{
    const Bytes bytes = makeRichClass();
    auto cf = jbc::test::readOk(bytes);
    ASSERT_NE(cf, nullptr);

    AttrSlot& slot = cf->methods[0].attrs[0];

    // 未解码时 mutate() 只能返回空——不存在「边解边改」的歧义状态。
    EXPECT_EQ(slot.mutate(), nullptr);

    ASSERT_TRUE(cf->decodeAttr(slot).is_ok());
    EXPECT_EQ(slot.state, AttrSlot::State::Decoded);
    EXPECT_FALSE(slot.needsReencode());

    // 只读访问不置脏：看过不算改过。
    EXPECT_NE(slot.peek(), nullptr);
    EXPECT_EQ(slot.state, AttrSlot::State::Decoded);

    // 走 mutate() 即置脏。
    ASSERT_NE(slot.mutate(), nullptr);
    EXPECT_EQ(slot.state, AttrSlot::State::Modified);
    EXPECT_TRUE(slot.needsReencode());
}

/// Code 属性内的嵌套属性同样是 lazy：解 Code 不等于解 StackMapTable。
TEST(LazyTest, NestedCodeAttributesAreLazyToo)
{
    const Bytes bytes = makeRichClass();
    auto cf = jbc::test::readOk(bytes);
    ASSERT_NE(cf, nullptr);

    const Result<CodeAttr*, ReadError> r = cf->decodeCode(cf->methods[0]);
    ASSERT_TRUE(r.is_ok());
    CodeAttr* code = r.unwrap();
    ASSERT_NE(code, nullptr);

    // Code 的定长部分解出来了
    EXPECT_EQ(code->maxStack, 2);
    EXPECT_EQ(code->maxLocals, 1);
    ASSERT_EQ(code->code.size(), 5u);
    EXPECT_EQ(code->code[0], 0x2a);
    ASSERT_EQ(code->excTable.size(), 1u);
    EXPECT_EQ(code->excTable[0].endPc, 5u);

    // 嵌套属性只建了目录，内容未解
    ASSERT_EQ(code->attrs.size(), 1u);
    EXPECT_EQ(cf->utf8(code->attrs[0].nameCp), "WeirdCustomAttr");
    EXPECT_FALSE(code->attrs[0].decoded());

    // 显式解嵌套属性 → 得到 RawAttr 字节副本
    const Result<AttrBody*, ReadError> rn = cf->decodeAttr(code->attrs[0]);
    ASSERT_TRUE(rn.is_ok());
    const AttrBody* body = rn.unwrap();
    ASSERT_NE(body, nullptr);
    ASSERT_TRUE(ca::core::tag_cast::isa<RawAttr>(body));
    const std::vector<ca::u8>& raw = ca::core::tag_cast::cast<RawAttr>(body)->bytes();
    ASSERT_EQ(raw.size(), 4u);
    EXPECT_EQ(raw[0], 0xDE);
    EXPECT_EQ(raw[3], 0xEF);
}

/// 未建模属性解码为 RawAttr，字节一个不少——不猜语义，也不丢。
TEST(LazyTest, UnknownAttrDecodesToRawBytes)
{
    const Bytes bytes = makeRichClass();
    auto cf = jbc::test::readOk(bytes);
    ASSERT_NE(cf, nullptr);

    const Result<AttrBody*, ReadError> r = cf->decodeAttr(cf->attrs[0]);
    ASSERT_TRUE(r.is_ok());
    const AttrBody* body = r.unwrap();
    ASSERT_NE(body, nullptr);

    ASSERT_TRUE(ca::core::tag_cast::isa<RawAttr>(body));
    const std::vector<ca::u8>& raw = ca::core::tag_cast::cast<RawAttr>(body)->bytes();
    ASSERT_EQ(raw.size(), 2u);
    EXPECT_EQ(raw[0], 0xAA);
    EXPECT_EQ(raw[1], 0xBB);
}

/// 没有源字节就不能 lazy 解码——这不是隐藏的降级，是明确的失败。
TEST(LazyTest, DecodeRequiresSourceBytes)
{
    const Bytes bytes = makeRichClass();

    ReadOptions opt;
    opt.preserveSourceBytes = false;
    auto cf = jbc::test::readOk(bytes, opt);
    ASSERT_NE(cf, nullptr);
    ASSERT_FALSE(cf->hasSource());

    const Result<AttrBody*, ReadError> r = cf->decodeAttr(cf->attrs[0]);
    EXPECT_TRUE(r.is_err());
}

/// 没有 Code 属性的方法：decodeCode 返回空而不是报错（不存在 ≠ 畸形）。
TEST(LazyTest, MethodWithoutCodeReturnsNull)
{
    ClassBuilder cb;
    const ca::u16 thisIdx   = cb.utf8("p/T");
    const ca::u16 abstractM = cb.utf8("run");
    const ca::u16 voidIdx   = cb.utf8("()V");
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(cb.utf8("java/lang/Object")));
    cb.addMethod(0x0401, abstractM, voidIdx);  // abstract，无 Code

    auto cf = jbc::test::readOk(cb.build());
    ASSERT_NE(cf, nullptr);
    ASSERT_EQ(cf->methods.size(), 1u);

    const Result<CodeAttr*, ReadError> r = cf->decodeCode(cf->methods[0]);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.unwrap(), nullptr);
}
