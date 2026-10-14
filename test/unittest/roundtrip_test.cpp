/// @file        roundtrip_test.cpp
/// @brief       保真写回：未触碰即原样，触碰才重编码。
///
/// 这是 libjbc core 的核心验收。旧库的 roundtrip 是「重编码一遍希望它一样」，
/// 这里是「没碰过的东西根本不经过编码器」——判据从「编码器正确」降级为
/// 「copy 正确」，后者几乎不可能错。
#include <gtest/gtest.h>

#include "jbc/jbc.hpp"
#include "test_class_bytes.hpp"

using namespace jbc::core;
using jbc::test::Bytes;
using jbc::test::makeRichClass;

/// 什么都没碰 → 输出与输入逐字节相同（含未知属性、异常表、嵌套属性）。
TEST(RoundtripTest, UntouchedClassIsByteIdentical)
{
    const Bytes bytes = makeRichClass();
    auto cf = jbc::test::readOk(bytes);
    ASSERT_NE(cf, nullptr);

    const Bytes out = jbc::test::writeOk(*cf);
    EXPECT_EQ(out, bytes);
}

/// 解码全部属性但不修改 → 仍然字节相同。
/// 证明「看过」不等于「改过」：解码不置脏，写回依旧走 copy。
TEST(RoundtripTest, DecodingEverythingKeepsBytesIdentical)
{
    const Bytes bytes = makeRichClass();
    auto cf = jbc::test::readOk(bytes);
    ASSERT_NE(cf, nullptr);

    ASSERT_TRUE(decodeAllAttrs(*cf).is_ok());

    // 全部属性都有 body 了，但没有一个被置脏。
    EXPECT_TRUE(cf->attrs[0].decoded());
    EXPECT_EQ(cf->attrs[0].state, AttrSlot::State::Decoded);
    EXPECT_FALSE(cf->attrs[0].needsReencode());

    const Bytes out = jbc::test::writeOk(*cf);
    EXPECT_EQ(out, bytes);
}

/// 未知属性必须原样往返——它们是 hostile 样本 roundtrip 断裂的主要来源。
TEST(RoundtripTest, UnknownAttributesSurviveByteForByte)
{
    const Bytes bytes = makeRichClass();
    auto cf = jbc::test::readOk(bytes);
    ASSERT_NE(cf, nullptr);

    AttrSlot* weird = cf->findClassAttr("WeirdCustomAttr");
    ASSERT_NE(weird, nullptr);
    ASSERT_EQ(weird->data.length, 2u);

    const ByteView before = cf->source().spanOrEmpty(weird->data);
    ASSERT_EQ(before.size(), 2u);
    EXPECT_EQ(before[0], 0xAA);
    EXPECT_EQ(before[1], 0xBB);

    const Bytes out = jbc::test::writeOk(*cf);
    EXPECT_EQ(out, bytes);
}

/// 修改 Code 的 maxStack → 只有该属性被重编码，其余照旧 copy。
/// 重解析输出验证：改动生效，且结构仍然合法。
TEST(RoundtripTest, ModifiedCodeIsReencodedAndOthersArePreserved)
{
    const Bytes bytes = makeRichClass();
    auto cf = jbc::test::readOk(bytes);
    ASSERT_NE(cf, nullptr);

    const Result<CodeAttr*, ReadError> rc = cf->decodeCode(cf->methods[0]);
    ASSERT_TRUE(rc.is_ok());
    CodeAttr* code = rc.unwrap();
    ASSERT_NE(code, nullptr);

    // mutate() 是唯一的修改入口，它顺手置脏。
    CodeAttr* mutableCode = static_cast<CodeAttr*>(cf->methods[0].attrs[0].mutate());
    ASSERT_NE(mutableCode, nullptr);
    mutableCode->maxStack = 7;
    EXPECT_EQ(cf->methods[0].attrs[0].state, AttrSlot::State::Modified);

    const Bytes out = jbc::test::writeOk(*cf);
    EXPECT_NE(out, bytes);  // 确实变了

    // 重解析：改动生效，其余未动
    auto again = jbc::test::readOk(out);
    ASSERT_NE(again, nullptr);
    const Result<CodeAttr*, ReadError> rc2 = again->decodeCode(again->methods[0]);
    ASSERT_TRUE(rc2.is_ok());
    ASSERT_NE(rc2.unwrap(), nullptr);
    EXPECT_EQ(rc2.unwrap()->maxStack, 7);
    EXPECT_EQ(rc2.unwrap()->maxLocals, 1);
    // 同方法的另一个（未知）属性原样保留
    EXPECT_EQ(again->utf8(again->methods[0].attrs[1].nameCp), "WeirdCustomAttr");
    EXPECT_EQ(again->methods[0].attrs[1].data.length, 3u);
}

/// 追加 CP 条目后写回：新条目必须出现在输出里，旧索引一个都不许动。
TEST(RoundtripTest, AppendedConstantPoolSurvivesRoundtrip)
{
    const Bytes bytes = makeRichClass();
    auto cf = jbc::test::readOk(bytes);
    ASSERT_NE(cf, nullptr);

    const ca::u32 nameIdx = cf->cp.addUtf8("p/Added");
    const ca::u32 clsIdx  = cf->cp.addClass(static_cast<ca::u16>(nameIdx));
    (void)clsIdx;

    const Bytes out = jbc::test::writeOk(*cf);
    EXPECT_NE(out, bytes);

    auto again = jbc::test::readOk(out);
    ASSERT_NE(again, nullptr);
    EXPECT_EQ(again->utf8(nameIdx), "p/Added");
    // 旧索引内容不变（只追加的最直接证据）
    EXPECT_EQ(again->thisName(), "p/T");
    EXPECT_EQ(again->superName(), "java/lang/Object");
}

/// 不保留源字节就无法 copy-through：必须显式失败，而不是静默改成重编码。
TEST(RoundtripTest, WithoutSourceBytesWriteRejectsPreserve)
{
    const Bytes bytes = makeRichClass();

    ReadOptions opt;
    opt.preserveSourceBytes = false;
    auto cf = jbc::test::readOk(bytes, opt);
    ASSERT_NE(cf, nullptr);
    EXPECT_FALSE(cf->hasSource());

    const Result<Bytes, WriteError> r = writeClass(*cf);
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), WriteError::MissingSource);
}

/// 改过但本版本没有编码器的属性 → 拒绝写出，绝不产出半成品 class。
/// （v0.1 只有 Code 与 Raw 可编码；把 Code 的 body 换成不可编码形态来触发。）
TEST(RoundtripTest, UnencodableModificationIsRejected)
{
    const Bytes bytes = makeRichClass();
    auto cf = jbc::test::readOk(bytes);
    ASSERT_NE(cf, nullptr);

    // 手动构造一个「已修改但无 body」的非法状态，模拟编码器缺失。
    cf->methods[0].attrs[1].body  = nullptr;
    cf->methods[0].attrs[1].state = AttrSlot::State::Modified;

    const Result<Bytes, WriteError> r = writeClass(*cf);
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), WriteError::Unencodable);
}

/// Code 内嵌套属性编码失败必须向上传播，不能写出错误的 Code length。
TEST(RoundtripTest, NestedUnencodableModificationIsRejected)
{
    const Bytes bytes = makeRichClass();
    auto cf = jbc::test::readOk(bytes);
    ASSERT_NE(cf, nullptr);

    const Result<CodeAttr*, ReadError> rc = cf->decodeCode(cf->methods[0]);
    ASSERT_TRUE(rc.is_ok());
    ASSERT_NE(rc.unwrap(), nullptr);
    CodeAttr* code = rc.unwrap();
    ASSERT_EQ(code->attrs.size(), 1u);

    code->attrs[0].body.reset();
    code->attrs[0].state = AttrSlot::State::Modified;

    const Result<Bytes, WriteError> r = writeClass(*cf);
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), WriteError::Unencodable);
}

/// code_length 是 u2 约束；扩大模型中的 code 数组后必须报错，不能静默截断。
TEST(RoundtripTest, OversizedCodeIsRejected)
{
    const Bytes bytes = makeRichClass();
    auto cf = jbc::test::readOk(bytes);
    ASSERT_NE(cf, nullptr);

    const Result<CodeAttr*, ReadError> rc = cf->decodeCode(cf->methods[0]);
    ASSERT_TRUE(rc.is_ok());
    ASSERT_NE(rc.unwrap(), nullptr);
    rc.unwrap()->code.resize(0x10000u, 0);
    ASSERT_NE(cf->methods[0].attrs[0].mutate(), nullptr);

    const Result<Bytes, WriteError> r = writeClass(*cf);
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), WriteError::Unencodable);
}

/// 类属性表之后的尾部垃圾（design.md §27.1 trailing 同族）：未修改往返必须
/// 字节一致——静默丢尾即保真违约。
TEST(RoundtripTest, ClassTrailingBytesRoundtripUntouched)
{
    Bytes bytes = makeRichClass();
    const Bytes tail = {0xCA, 0xFE, 0xBA, 0xBE};
    bytes.insert(bytes.end(), tail.begin(), tail.end());

    auto cf = jbc::test::readOk(bytes);
    ASSERT_NE(cf, nullptr);
    ASSERT_EQ(cf->trailing.size(), 4u);
    EXPECT_EQ(cf->trailing, tail);

    // 全解码后仍未改：copy 路径，含尾部逐字节一致。
    ASSERT_TRUE(decodeAllAttrs(*cf).is_ok());
    const Bytes out = jbc::test::writeOk(*cf);
    EXPECT_EQ(out, bytes);
}

/// 改一个属性触发重编码后，类级 trailing 仍原样附在输出末尾。
TEST(RoundtripTest, ClassTrailingBytesSurviveAttributeModification)
{
    Bytes bytes = makeRichClass();
    const Bytes tail = {0x01, 0x02, 0x03, 0x04};
    bytes.insert(bytes.end(), tail.begin(), tail.end());

    auto cf = jbc::test::readOk(bytes);
    ASSERT_NE(cf, nullptr);

    const Result<CodeAttr*, ReadError> rc = cf->decodeCode(cf->methods[0]);
    ASSERT_TRUE(rc.is_ok());
    ASSERT_NE(rc.unwrap(), nullptr);
    CodeAttr* mutableCode = static_cast<CodeAttr*>(cf->methods[0].attrs[0].mutate());
    ASSERT_NE(mutableCode, nullptr);
    mutableCode->maxStack = 9;

    const Bytes out = jbc::test::writeOk(*cf);
    EXPECT_NE(out, bytes);   // 改动确实生效
    ASSERT_GT(out.size(), tail.size());
    EXPECT_TRUE(ByteView{out}.subOrEmpty(out.size() - tail.size(), tail.size())
                    .equals(ByteView{tail}))
        << "class trailing lost after re-encode";
}
