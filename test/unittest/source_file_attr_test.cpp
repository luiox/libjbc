/// @file        source_file_attr_test.cpp
/// @brief       SourceFile（JVMS 4.7.10）typed 建模单测：分派接线、解码保真、
///              typed 读写面、半截体显式失败、trailing 保持、u16 域宽闸门。
///
/// SourceFile 无任何下游 Raw 消费者，是 B3 四件中唯一已接 decodeAttrBody
/// typed 分派的属性（design.md §32.9）。
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "jbc/jbc.hpp"
#include "test_class_bytes.hpp"

using namespace jbc::core;
using jbc::test::Bytes;
using jbc::test::ClassBuilder;

namespace {

/// 一个类级属性 name=content 的最小类。
Bytes classWithAttr(const std::string& name, const Bytes& content)
{
    ClassBuilder cb;
    const ca::u16 thisIdx  = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 attrIdx  = cb.utf8(name);
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setVersion(52);
    cb.addClassAttr(attrIdx, content);
    return cb.build();
}

}   // namespace

// ---- 分派接线：decodeAttr 产出 typed 体；未触碰写回逐字节不动 ----

TEST(SourceFileAttrTest, WiredDispatchProducesTypedBody)
{
    // CP 分配顺序：p/T=1, Object=2, T.java=3, SourceFile=4。
    ClassBuilder cb;
    const ca::u16 thisIdx  = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 srcIdx   = cb.utf8("T.java");
    const ca::u16 attrIdx  = cb.utf8("SourceFile");
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setVersion(52);
    cb.addClassAttr(attrIdx, {0x00, static_cast<ca::u8>(srcIdx)});
    const Bytes cls = cb.build();

    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    auto d = cf->decodeAttr(cf->attrs[0]);
    ASSERT_TRUE(d.is_ok());
    ASSERT_EQ(d.unwrap()->kind(), AttrKind::SourceFile);
    const auto* sf = static_cast<const SourceFileAttr*>(cf->attrs[0].peek());
    ASSERT_NE(sf, nullptr);
    EXPECT_EQ(sf->sourceFileCp, srcIdx);
    EXPECT_EQ(cf->cp.utf8Of(sf->sourceFileCp), "T.java");
    // 未触碰（Decoded 态写回走 copy）：字节一致。
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

// ---- typed 读写面：mutate 改值 → 重编码 → 重读 typed 视图一致 ----

TEST(SourceFileAttrTest, TypedValueSwapSurvivesRoundtrip)
{
    // CP 分配顺序：p/T=1, Object=2, A.java=3, B.java=4, SourceFile=5。
    ClassBuilder cb;
    const ca::u16 thisIdx  = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 aIdx     = cb.utf8("A.java");
    const ca::u16 bIdx     = cb.utf8("B.java");
    const ca::u16 attrIdx  = cb.utf8("SourceFile");
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setVersion(52);
    cb.addClassAttr(attrIdx, {0x00, static_cast<ca::u8>(aIdx)});
    const Bytes cls = cb.build();

    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    ASSERT_TRUE(cf->decodeAttr(cf->attrs[0]).is_ok());
    static_cast<SourceFileAttr*>(cf->attrs[0].mutate())->sourceFileCp = bIdx;

    const Bytes out = jbc::test::writeOk(*cf);
    auto cf2 = jbc::test::readOk(out);
    ASSERT_NE(cf2, nullptr);
    ASSERT_TRUE(cf2->decodeAttr(cf2->attrs[0]).is_ok());
    const auto* sf = static_cast<const SourceFileAttr*>(cf2->attrs[0].peek());
    ASSERT_NE(sf, nullptr);
    EXPECT_EQ(sf->sourceFileCp, bIdx);
    EXPECT_EQ(cf2->cp.utf8Of(sf->sourceFileCp), "B.java");
}

// ---- 半截体：decode 显式 Err；lazy 未触碰写回不受影响 ----

TEST(SourceFileAttrTest, TruncatedBodyIsExplicitFailure)
{
    const Bytes cls = classWithAttr("SourceFile", {0x00});
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    auto d = cf->decodeAttr(cf->attrs[0]);
    ASSERT_TRUE(d.is_err());
    EXPECT_EQ(d.unwrap_err(), ReadError::Truncated);
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

// ---- trailing（design.md §27.1）：声明 length 多出的尾部字节重编码原样写回 ----

TEST(SourceFileAttrTest, TrailingSurvivesReencode)
{
    Bytes body = {0x00, 0x03, 0xDE, 0xAD, 0xBE, 0xEF};
    const Bytes cls = classWithAttr("SourceFile", body);
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    ASSERT_TRUE(cf->decodeAttr(cf->attrs[0]).is_ok());
    // 未修改往返（Decoded 态走 copy）：字节一致。
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
    // 置脏触发重编码：结构输出后 trailing 原样追加，字节仍一致。
    ASSERT_NE(cf->attrs[0].mutate(), nullptr);
    EXPECT_EQ(jbc::test::writeOk(*cf), cls) << "re-encode dropped trailing";
    // trailing 落在体上，可观察。
    const auto* sf = static_cast<const SourceFileAttr*>(cf->attrs[0].peek());
    ASSERT_NE(sf, nullptr);
    EXPECT_EQ(sf->trailing.size(), 4u);
}

// ---- 编码闸门（design.md §27.2）：u2 编码列超域宽显式 Err，不静默截断 ----

TEST(SourceFileAttrTest, EncoderRejectsOutOfRangeIndex)
{
    auto cf = jbc::test::readOk(classWithAttr("SourceFile", {0x00, 0x03}));
    ASSERT_NE(cf, nullptr);
    ASSERT_TRUE(cf->decodeAttr(cf->attrs[0]).is_ok());
    static_cast<SourceFileAttr*>(cf->attrs[0].mutate())->sourceFileCp = 0x10000;
    const auto r = writeClass(*cf);
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), WriteError::Unencodable);
}
