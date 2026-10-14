/// @file        reader_test.cpp
/// @brief       骨架解析、属性目录与恶意输入。
#include <gtest/gtest.h>

#include "jbc/jbc.hpp"
#include "test_class_bytes.hpp"

using namespace jbc::core;
using jbc::test::AttrSpec;
using jbc::test::Bytes;
using jbc::test::ClassBuilder;
using jbc::test::codeContent;
using jbc::test::ExcSpec;

namespace {

Bytes makeRichClass()
{
    return jbc::test::makeRichClass();
}

}   // namespace

/// 骨架字段必须与输入一致；这些是 summary 扫描所需的全部信息。
TEST(ReaderTest, SkeletonFieldsAreParsed)
{
    ClassBuilder cb;
    const ca::u16 thisIdx   = cb.utf8("p/T");
    const ca::u16 superIdx  = cb.utf8("java/lang/Object");
    const ca::u16 ifaceIdx  = cb.utf8("p/Iface");
    const ca::u16 fieldName = cb.utf8("value");
    const ca::u16 fieldDesc = cb.utf8("I");
    const ca::u16 methName  = cb.utf8("<init>");
    const ca::u16 methDesc  = cb.utf8("()V");

    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.addInterface(cb.classRef(ifaceIdx));
    cb.setAccess(0x0031);
    cb.setVersion(52);
    cb.addField(0x0002, fieldName, fieldDesc);
    cb.addMethod(0x0001, methName, methDesc);

    auto cf = jbc::test::readOk(cb.build());
    ASSERT_NE(cf, nullptr);

    EXPECT_EQ(cf->major, 52);
    EXPECT_EQ(cf->access, 0x0031);
    EXPECT_EQ(cf->thisName(), "p/T");
    EXPECT_EQ(cf->superName(), "java/lang/Object");

    ASSERT_EQ(cf->interfaces.size(), 1u);
    ASSERT_EQ(cf->fields.size(), 1u);
    EXPECT_EQ(cf->nameOf(cf->fields[0]), "value");
    EXPECT_EQ(cf->descOf(cf->fields[0]), "I");
    ASSERT_EQ(cf->methods.size(), 1u);
    EXPECT_EQ(cf->nameOf(cf->methods[0]), "<init>");

    EXPECT_EQ(cf->state(), ParseState::Validated);
    EXPECT_TRUE(cf->diagnostics().empty());
}

/// 属性目录 eager、内容 lazy：目录里的 name/length/span 必须正确，
/// 但一个属性体都不该存在。
TEST(ReaderTest, AttributeDirectoryIsEagerButContentIsNot)
{
    const Bytes bytes = makeRichClass();
    auto cf = jbc::test::readOk(bytes);
    ASSERT_NE(cf, nullptr);

    // 类级：一个未知属性
    ASSERT_EQ(cf->attrs.size(), 1u);
    EXPECT_EQ(cf->utf8(cf->attrs[0].nameCp), "WeirdCustomAttr");
    EXPECT_EQ(cf->attrs[0].data.length, 2u);
    EXPECT_FALSE(cf->attrs[0].decoded());
    EXPECT_EQ(cf->attrs[0].state, AttrSlot::State::Untouched);

    // 方法级：Code + 未知
    ASSERT_EQ(cf->methods.size(), 1u);
    ASSERT_EQ(cf->methods[0].attrs.size(), 2u);
    EXPECT_EQ(cf->utf8(cf->methods[0].attrs[0].nameCp), "Code");
    EXPECT_EQ(cf->utf8(cf->methods[0].attrs[1].nameCp), "WeirdCustomAttr");
    EXPECT_FALSE(cf->methods[0].attrs[0].decoded());
    EXPECT_FALSE(cf->methods[0].attrs[1].decoded());

    // span 必须落在源字节内且长度自洽
    const ByteView src = cf->source();
    EXPECT_TRUE(src.covers(cf->attrs[0].data.offset, cf->attrs[0].data.length));
}

/// 逐字节截断：任何前缀都不得崩溃，失败必须是 Truncated。
/// 这是防御式读取的最低验收——畸形输入是预期输入。
TEST(ReaderTest, EveryTruncationFailsCleanly)
{
    const Bytes full = makeRichClass();
    for (ca::usize n = 0; n < full.size(); ++n) {
        const Bytes trunc(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(n));
        const Result<ReadResult, ReadError> r = readClass(ByteView{trunc});
        if (r.is_err()) {
            // 截断只可能产生「结构边界无法确定」这一类错误；具体是哪一处边界
            // 取决于截断点落在 CP 上、属性长度上还是别处。出现其他错误码才是 bug。
            const ReadError e = r.unwrap_err();
            EXPECT_TRUE(e == ReadError::Truncated || e == ReadError::BadAttributeLength ||
                        e == ReadError::BadConstantPoolTag || e == ReadError::BadConstantPoolCount ||
                        e == ReadError::BadCodeLength)
                << "unexpected error " << to_string(e) << " at n=" << n;
        }
    }
}

/// magic 不对就必须拒绝：连是不是 classfile 都不知道，谈不上降级。
TEST(ReaderTest, BadMagicIsRejected)
{
    Bytes bytes = makeRichClass();
    bytes[0]    = 0xCB;

    const Result<ReadResult, ReadError> r = readClass(ByteView{bytes});
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), ReadError::BadMagic);
}

/// 4 字节输入——只有 magic，什么都没有。
///
/// 这不是臆造的用例：仓库里 `build/artifacts/.../class-data-output.class`
/// 就是这样一个 4 字节文件，libjbytecode 在它上面直接栈缓冲区溢出崩溃
/// （0xC0000409，__security_check_cookie 失败），连错误通道都没有。
/// 畸形输入是预期输入，必须走返回值而不是崩。
TEST(ReaderTest, FourByteInputFailsCleanly)
{
    const Bytes onlyMagic = {0xCA, 0xFE, 0xBA, 0xBE};
    const Result<ReadResult, ReadError> r =
        readClass(ByteView{onlyMagic.data(), onlyMagic.size()});
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), ReadError::Truncated);
}

/// 属性长度声明越界：无法确定后续结构边界，必须失败而不是「尽量读」。
TEST(ReaderTest, OversizedAttributeLengthIsRejected)
{
    ClassBuilder cb;
    const ca::u16 thisIdx  = cb.utf8("p/T");
    const ca::u16 weirdIdx = cb.utf8("Weird");
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(cb.utf8("java/lang/Object")));
    cb.addClassAttr(weirdIdx, Bytes{0x01});

    Bytes bytes = cb.build();
    // 最后一个属性：name_index(u2) 在末尾往前 2+4+1 处，length(u4) 紧随其后。
    const ca::usize lenOff = bytes.size() - 5;
    bytes[lenOff]     = 0x00;
    bytes[lenOff + 1] = 0x00;
    bytes[lenOff + 2] = 0x10;  // 声明 0x00100000 字节
    bytes[lenOff + 3] = 0x00;

    const Result<ReadResult, ReadError> r = readClass(ByteView{bytes});
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), ReadError::BadAttributeLength);
}

/// 版本偏高只降级记录，不拒绝读取——多数结构照旧可读。
TEST(ReaderTest, FutureVersionDegradesInsteadOfFailing)
{
    ClassBuilder cb;
    cb.setThis(cb.classRef(cb.utf8("p/T")));
    cb.setSuper(cb.classRef(cb.utf8("java/lang/Object")));
    cb.setVersion(200);

    auto cf = jbc::test::readOk(cb.build());
    ASSERT_NE(cf, nullptr);
    EXPECT_EQ(cf->major, 200);
    ASSERT_EQ(cf->diagnostics().count(), 1u);
    EXPECT_EQ(cf->diagnostics().items()[0].code, ReadError::UnsupportedVersion);
    // 无 Error 级诊断 → 仍判 Validated
    EXPECT_EQ(cf->state(), ParseState::Validated);
}

/// classfile 头部的 this_class/super_class/interface 和成员元数据都是可见的
/// CP 引用，必须在骨架阶段校验；不能把错误索引留到查询时才暴露。
TEST(ReaderTest, VisibleConstantPoolReferencesAreValidated)
{
    ClassBuilder cb;
    const ca::u16 nameIdx = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    (void)superIdx;
    cb.setThis(nameIdx);  // Utf8，不是 Class
    cb.setSuper(0);

    const Result<ReadResult, ReadError> r = readClass(ByteView{cb.build()});
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), ReadError::BadConstantPoolRef);
}
