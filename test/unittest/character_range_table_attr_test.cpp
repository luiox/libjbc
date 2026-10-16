/// @file        character_range_table_attr_test.cpp
/// @brief       CharacterRangeTable（javac -Xjcov 私有调试属性）typed 建模
///              单测：解码保真、行结构、半截行显式失败、trailing 保持、
///              u16 域宽闸门。规格：u2 行数 + 每行 {u2, u2, u4, u4, u2}。
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

/// CRT 体：1 行 {start=0, end=1, charStart=0x0c09, charEnd=0x0c17, flags=1}。
/// char 值取 JDK8 -Xjcov 实测形态：(line << 10) | column。
Bytes oneRowBody()
{
    return {0x00, 0x01,
            0x00, 0x00, 0x00, 0x01,   // start_pc=0, end_pc=1
            0x00, 0x00, 0x0c, 0x09,   // char_start
            0x00, 0x00, 0x0c, 0x17,   // char_end
            0x00, 0x01};              // flags = CRT_STATEMENT
}

}   // namespace

// ---- 保真：typed 解码 + 未触碰写回逐字节不动 ----

TEST(CrtAttrTest, DecodeOnlyWriteIsByteIdentical)
{
    const Bytes cls = classWithAttr("CharacterRangeTable", oneRowBody());
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    ASSERT_TRUE(cf->decodeAttr(cf->attrs[0]).is_ok());
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

TEST(CrtAttrTest, TypedKindAndRowStructure)
{
    const Bytes cls = classWithAttr("CharacterRangeTable", oneRowBody());
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    auto d = cf->decodeAttr(cf->attrs[0]);
    ASSERT_TRUE(d.is_ok());
    ASSERT_EQ(d.unwrap()->kind(), AttrKind::CharacterRangeTable);
    const auto& crt = static_cast<const CharacterRangeTableAttr&>(*cf->attrs[0].peek());
    ASSERT_EQ(crt.rows.size(), 1u);
    EXPECT_EQ(crt.rows[0].startPc, 0u);
    EXPECT_EQ(crt.rows[0].endPc, 1u);
    EXPECT_EQ(crt.rows[0].charStart, 0x0c09u);
    EXPECT_EQ(crt.rows[0].charEnd, 0x0c17u);
    EXPECT_EQ(crt.rows[0].flags, 1u);
}

// ---- 失败显式：半截行（声明行数与剩余字节不符）拒绝解码 ----

TEST(CrtAttrTest, TruncatedRowIsExplicitFailure)
{
    // 声明 2 行，只带 1 行 + 2 字节：解码在第二行中途触底。
    Bytes body = {0x00, 0x02};
    const Bytes row = oneRowBody();
    body.insert(body.end(), row.begin() + 2, row.end());   // 去掉行数前缀的完整行
    body.push_back(0x00);
    body.push_back(0x00);
    const Bytes cls = classWithAttr("CharacterRangeTable", body);
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    auto d = cf->decodeAttr(cf->attrs[0]);
    ASSERT_TRUE(d.is_err());
    EXPECT_EQ(d.unwrap_err(), ReadError::Truncated);
    // lazy 未触碰：copy-through 保真不受解码失败影响。
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

// ---- trailing（design.md §27.1）：声明 length 多出的尾部字节重编码原样写回 ----

TEST(CrtAttrTest, TrailingSurvivesReencode)
{
    Bytes body = oneRowBody();
    body.push_back(0xDE);
    body.push_back(0xAD);
    body.push_back(0xBE);
    body.push_back(0xEF);
    const Bytes cls = classWithAttr("CharacterRangeTable", body);
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    ASSERT_TRUE(cf->decodeAttr(cf->attrs[0]).is_ok());
    // 未修改往返（Decoded 态走 copy）：字节一致。
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
    // 置脏触发重编码：结构输出后 trailing 原样追加，字节仍一致。
    ASSERT_NE(cf->attrs[0].mutate(), nullptr);
    EXPECT_EQ(jbc::test::writeOk(*cf), cls) << "re-encode dropped trailing";
}

// ---- 编码闸门（design.md §27.2）：u2 编码列超域宽显式 Err，不静默截断 ----

TEST(CrtAttrTest, EncodersRejectOutOfRangeFields)
{
    // start_pc 超 u16。
    {
        auto cf = jbc::test::readOk(classWithAttr("CharacterRangeTable", oneRowBody()));
        ASSERT_NE(cf, nullptr);
        ASSERT_TRUE(cf->decodeAttr(cf->attrs[0]).is_ok());
        static_cast<CharacterRangeTableAttr*>(cf->attrs[0].mutate())->rows[0].startPc = 0x10000;
        const auto r = writeClass(*cf);
        ASSERT_TRUE(r.is_err());
        EXPECT_EQ(r.unwrap_err(), WriteError::Unencodable);
    }
    // flags 超 u16。
    {
        auto cf = jbc::test::readOk(classWithAttr("CharacterRangeTable", oneRowBody()));
        ASSERT_NE(cf, nullptr);
        ASSERT_TRUE(cf->decodeAttr(cf->attrs[0]).is_ok());
        static_cast<CharacterRangeTableAttr*>(cf->attrs[0].mutate())->rows[0].flags = 0x10000;
        const auto r = writeClass(*cf);
        ASSERT_TRUE(r.is_err());
        EXPECT_EQ(r.unwrap_err(), WriteError::Unencodable);
    }
}
