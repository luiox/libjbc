/// @file        exceptions_attr_test.cpp
/// @brief       Exceptions（JVMS 4.7.5）typed 建模单测：分派接线钉门、typed
///              解码/编码、半截体显式失败、trailing 保持、u16 域宽闸门，
///              以及「typed 读写与既有 Raw 手术口径一致」的对拍。
///
/// 分派已接线（design.md §32.9）：decodeAttrBody 对 "Exceptions" 返回
/// typed 体（B3 接线批与消费方迁移同批落地）；Raw 手术 oracle 从
/// 槽源 span 直接取体字节，是旧栈 remapExceptionsAttr 字节手术的测试
/// 内镜像（口径对齐用，非第二套手术）。
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "jbc/jbc.hpp"
#include "test_class_bytes.hpp"

using namespace jbc::core;
using jbc::test::Bytes;
using jbc::test::ClassBuilder;

namespace {

/// Exceptions 体：u2 count + u2 class_index*。
Bytes excBody(const std::vector<ca::u16>& classCps)
{
    Bytes b;
    jbc::test::detail::putU16(b, static_cast<ca::u16>(classCps.size()));
    for (const ca::u16 cp : classCps) {
        jbc::test::detail::putU16(b, cp);
    }
    return b;
}

/// 带方法级 Exceptions 的类。CP 分配：p/T=1, Object=2, OldA=3, OldB=4,
/// m=5, ()V=6, Exceptions=7; Class 条目 8=OldA(3), 9=OldB(4)。
Bytes classWithExceptions(const Bytes& content)
{
    ClassBuilder cb;
    const ca::u16 thisIdx   = cb.utf8("p/T");
    const ca::u16 superIdx  = cb.utf8("java/lang/Object");
    const ca::u16 oldAIdx   = cb.utf8("OldA");
    const ca::u16 oldBIdx   = cb.utf8("OldB");
    const ca::u16 mNameIdx  = cb.utf8("m");
    const ca::u16 mDescIdx  = cb.utf8("()V");
    const ca::u16 attrIdx   = cb.utf8("Exceptions");
    [[maybe_unused]] const ca::u16 classA = cb.classRef(oldAIdx);
    [[maybe_unused]] const ca::u16 classB = cb.classRef(oldBIdx);
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setVersion(52);
    cb.addMethod(0x0001, mNameIdx, mDescIdx,
                 {jbc::test::AttrSpec{attrIdx, content}});
    return cb.build();
}

/// 方法属性槽定位（唯一方法 + 名字匹配）。
AttrSlot* findMethodSlot(ClassFile& cf, std::string_view name)
{
    for (AttrSlot& slot : cf.methods[0].attrs) {
        if (cf.cp.utf8Equals(slot.nameCp, name)) {
            return &slot;
        }
    }
    return nullptr;
}

/// 扫描式 Utf8 驻留（与上层 remapper 的 findOrAddUtf8 同口径）。
ca::u32 internUtf8(ClassFile& cf, std::string_view text)
{
    const ConstantPool& cp = cf.cp;
    for (ca::u32 i = 1; i < cp.size(); ++i) {
        if (cp.isUtf8(i) && cp.utf8Equals(i, text)) {
            return i;
        }
    }
    return cf.cp.addUtf8(text);
}

/// 扫描式 Class 条目驻留（与上层 ClassSession::internClass 同口径）。
ca::u32 internClass(ClassFile& cf, std::string_view internalName)
{
    const ConstantPool& cp = cf.cp;
    for (ca::u32 i = 1; i < cp.size(); ++i) {
        if (cp.valid(i) && cp.tagOf(i) == CpTag::Class) {
            const ca::u32 nameCp = cp.at(i).a;
            if (cp.isUtf8(nameCp) && cp.utf8Equals(nameCp, internalName)) {
                return i;
            }
        }
    }
    return cf.cp.addClass(internUtf8(cf, internalName));
}

/// typed 手术路径：typed 解码 → 逐条目类名重映射（命中才动）→ makeNew
/// 置 New → 写回。语义镜像旧栈 remapExceptionsAttr。
Bytes typedSurgery(const Bytes& cls)
{
    auto cf = jbc::test::readOk(cls);
    EXPECT_NE(cf, nullptr);
    AttrSlot* slot = findMethodSlot(*cf, "Exceptions");
    EXPECT_NE(slot, nullptr);
    auto d = decodeExceptionsBody(*cf, *slot);
    EXPECT_TRUE(d.is_ok());
    auto body = std::move(d).unwrap();
    auto& exc = static_cast<ExceptionsAttr&>(*body);

    for (ca::u32& cp : exc.exceptionCps) {
        const std::string_view name = cf->cp.utf8Of(cf->cp.at(cp).a);
        if (name.rfind("Old", 0) == 0) {
            cp = internClass(*cf, "New" + std::string{name.substr(3)});
        }
    }
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(body));
    return jbc::test::writeOk(*cf);
}

/// Raw 手术 oracle：旧栈 remapExceptionsAttr 字节手术的测试内逐字节
/// 镜像（口径对齐用，非第二套手术——生产手术已迁 typed）。体字节从槽源
/// span 直接取，不依赖 decodeAttr 的分派形态。
Bytes rawSurgeryOracle(const Bytes& cls)
{
    auto cf = jbc::test::readOk(cls);
    if (cf == nullptr) {
        ADD_FAILURE() << "readOk failed";
        return {};
    }
    AttrSlot* slot = findMethodSlot(*cf, "Exceptions");
    if (slot == nullptr) {
        ADD_FAILURE() << "Exceptions slot missing";
        return {};
    }
    const ByteView data = cf->source().spanOrEmpty(slot->data);
    if (data.size() != slot->data.length || data.size() < 2) {
        ADD_FAILURE() << "Exceptions raw body unavailable";
        return {};
    }

    auto rdU16 = [&data, at = ca::usize{0}]() mutable -> ca::u32 {
        if (at + 2 > data.size()) {
            return 0;
        }
        const ca::u32 v = (static_cast<ca::u32>(data[at]) << 8) | data[at + 1];
        at += 2;
        return v;
    };

    Bytes out;
    jbc::test::detail::putU16(out, static_cast<ca::u16>(rdU16()));   // count
    const ca::u32 n = (static_cast<ca::u32>(data[0]) << 8) | data[1];
    for (ca::u32 i = 0; i < n; ++i) {
        const ca::u32 classCp = rdU16();
        const std::string_view name = cf->cp.utf8Of(cf->cp.at(classCp).a);
        if (name.rfind("Old", 0) == 0) {
            jbc::test::detail::putU16(
                out, static_cast<ca::u16>(internClass(*cf, "New" + std::string{name.substr(3)})));
        }
        else {
            jbc::test::detail::putU16(out, static_cast<ca::u16>(classCp));
        }
    }
    *slot = AttrSlot::makeNew(
        slot->nameCp, std::make_unique<RawAttr>(Bytes{out.begin(), out.end()}));
    return jbc::test::writeOk(*cf);
}

}   // namespace

// ---- 分派接线钉门：decodeAttrBody 对 Exceptions 返回 typed 体 ----
//
// B3 接线批翻转分派（与消费方迁移同批，design.md §32.9）：本用例
// 在挂账期钉 Raw 形、接线时翻红对账，现钉 typed 形。

TEST(ExceptionsAttrTest, TypedDispatchWiredAfterConsumerMigration)
{
    const Bytes cls = classWithExceptions(excBody({8, 9}));
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    auto d = cf->decodeAttr(cf->methods[0].attrs[0]);
    ASSERT_TRUE(d.is_ok());
    EXPECT_EQ(d.unwrap()->kind(), AttrKind::Exceptions);
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

// ---- typed 解码：条目表逐项命中 ----

TEST(ExceptionsAttrTest, TypedBodyDecodeMatchesBytes)
{
    const Bytes cls = classWithExceptions(excBody({8, 9}));
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findMethodSlot(*cf, "Exceptions");
    ASSERT_NE(slot, nullptr);
    auto d = decodeExceptionsBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto body = std::move(d).unwrap();
    ASSERT_EQ(body->kind(), AttrKind::Exceptions);
    // 直读入口不触碰槽状态（lazy 语义保持）。
    EXPECT_FALSE(slot->decoded());
    const auto& exc = static_cast<const ExceptionsAttr&>(*body);
    ASSERT_EQ(exc.exceptionCps.size(), 2u);
    EXPECT_EQ(exc.exceptionCps[0], 8u);
    EXPECT_EQ(exc.exceptionCps[1], 9u);
    EXPECT_EQ(cf->cp.utf8Of(cf->cp.at(exc.exceptionCps[0]).a), "OldA");
    EXPECT_EQ(cf->cp.utf8Of(cf->cp.at(exc.exceptionCps[1]).a), "OldB");
    EXPECT_TRUE(exc.trailing.empty());
}

TEST(ExceptionsAttrTest, EmptyTableRoundtrips)
{
    const Bytes cls = classWithExceptions(excBody({}));
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findMethodSlot(*cf, "Exceptions");
    ASSERT_NE(slot, nullptr);
    auto d = decodeExceptionsBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto body = std::move(d).unwrap();
    const auto& exc = static_cast<const ExceptionsAttr&>(*body);
    EXPECT_TRUE(exc.exceptionCps.empty());
    // makeNew 重编码字节一致（空表形态无损）。
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(body));
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

// ---- 半截体：条目数声明与剩余字节不符，显式 Err ----

TEST(ExceptionsAttrTest, TruncatedTableIsExplicitFailure)
{
    // 声明 2 条，只带 1 条 + 1 字节。
    Bytes half = excBody({8, 9});
    half.resize(2 + 2 + 1);
    const Bytes cls = classWithExceptions(half);
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findMethodSlot(*cf, "Exceptions");
    ASSERT_NE(slot, nullptr);
    auto d = decodeExceptionsBody(*cf, *slot);
    ASSERT_TRUE(d.is_err());
    EXPECT_EQ(d.unwrap_err(), ReadError::Truncated);
    // 解码失败不影响 lazy copy-through。
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

// ---- typed 编码：makeNew 置 New 重编码，字节与原 Raw 内容一致 ----

TEST(ExceptionsAttrTest, TypedEncodeIsCanonical)
{
    const Bytes cls = classWithExceptions(excBody({8, 9}));
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findMethodSlot(*cf, "Exceptions");
    ASSERT_NE(slot, nullptr);
    auto d = decodeExceptionsBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(d).unwrap());
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

// ---- trailing（design.md §27.1）：多余尾部字节保留 + 重编码原样写回 ----

TEST(ExceptionsAttrTest, TrailingSurvivesReencode)
{
    Bytes content = excBody({8, 9});
    content.push_back(0xDE);
    content.push_back(0xAD);
    const Bytes cls = classWithExceptions(content);
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findMethodSlot(*cf, "Exceptions");
    ASSERT_NE(slot, nullptr);
    auto d = decodeExceptionsBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto body = std::move(d).unwrap();
    ASSERT_EQ(static_cast<const ExceptionsAttr&>(*body).trailing.size(), 2u);
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(body));
    EXPECT_EQ(jbc::test::writeOk(*cf), cls) << "re-encode dropped trailing";
}

// ---- 编码闸门（design.md §27.2）：u2 编码列超域宽显式 Err ----

TEST(ExceptionsAttrTest, EncoderRejectsOutOfRangeFields)
{
    // 单条目超 u16。
    {
        const Bytes cls = classWithExceptions(excBody({8}));
        auto cf = jbc::test::readOk(cls);
        ASSERT_NE(cf, nullptr);
        AttrSlot* slot = findMethodSlot(*cf, "Exceptions");
        ASSERT_NE(slot, nullptr);
        auto d = decodeExceptionsBody(*cf, *slot);
        ASSERT_TRUE(d.is_ok());
        auto body = std::move(d).unwrap();
        static_cast<ExceptionsAttr&>(*body).exceptionCps[0] = 0x10000;
        *slot = AttrSlot::makeNew(slot->nameCp, std::move(body));
        const auto r = writeClass(*cf);
        ASSERT_TRUE(r.is_err());
        EXPECT_EQ(r.unwrap_err(), WriteError::Unencodable);
    }
    // 条目数超 u16 声明域（表长构造为 65536 条在上界拒绝）。
    {
        auto cf = jbc::test::readOk(classWithExceptions(excBody({8})));
        ASSERT_NE(cf, nullptr);
        AttrSlot* slot = findMethodSlot(*cf, "Exceptions");
        ASSERT_NE(slot, nullptr);
        auto d = decodeExceptionsBody(*cf, *slot);
        ASSERT_TRUE(d.is_ok());
        auto body = std::move(d).unwrap();
        static_cast<ExceptionsAttr&>(*body).exceptionCps.assign(0x10000, 8);
        *slot = AttrSlot::makeNew(slot->nameCp, std::move(body));
        const auto r = writeClass(*cf);
        ASSERT_TRUE(r.is_err());
        EXPECT_EQ(r.unwrap_err(), WriteError::Unencodable);
    }
}

// ---- 手术口径一致：typed 读写与 Raw 手术 oracle 逐字节一致，且 typed
//      重读看到改写后的条目 ----

TEST(ExceptionsAttrTest, TypedSurgeryMatchesRawRemapOracle)
{
    const Bytes cls = classWithExceptions(excBody({8, 9}));
    const Bytes typedOut = typedSurgery(cls);
    const Bytes rawOut   = rawSurgeryOracle(cls);
    EXPECT_EQ(typedOut, rawOut) << "typed path diverges from raw-remap oracle";

    // typed 读出与手术结果一致。
    auto cf = jbc::test::readOk(typedOut);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findMethodSlot(*cf, "Exceptions");
    ASSERT_NE(slot, nullptr);
    auto d = decodeExceptionsBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto body = std::move(d).unwrap();
    const auto& exc = static_cast<const ExceptionsAttr&>(*body);
    ASSERT_EQ(exc.exceptionCps.size(), 2u);
    EXPECT_EQ(cf->cp.utf8Of(cf->cp.at(exc.exceptionCps[0]).a), "NewA");
    EXPECT_EQ(cf->cp.utf8Of(cf->cp.at(exc.exceptionCps[1]).a), "NewB");
}
