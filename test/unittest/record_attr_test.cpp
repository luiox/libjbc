/// @file        record_attr_test.cpp
/// @brief       Record（JVMS 4.7.30，类级）typed 建模单测：分派接线钉门、
///              typed 解码/编码、半截体与嵌套属性表 malformed 显式失败、
///              trailing 保持、u16 域宽闸门、组件嵌套属性置脏的父体重编码
///              传播，以及组件名/描述符 typed 手术往返。
///
/// Record 是第 17 个 typed 属性，也是第一个**体上带嵌套属性表**的非 Code
/// 属性（design.md §32.11）：组件嵌套槽与 Code 嵌套同款 lazy/copy-through/
/// 置脏语义，本文件逐面钉死。
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

/// 组件规格：名字/描述符 CP 索引 + 嵌套属性（名字索引 + 载荷字节）。
struct CompSpec
{
    ca::u16 nameIdx = 0;
    ca::u16 descIdx = 0;
    std::vector<jbc::test::AttrSpec> nested;
};

/// Record 属性体：u2 组件数 + 组件（name/desc 两 u2 + 嵌套属性表）。
Bytes recordBody(const std::vector<CompSpec>& comps)
{
    Bytes b;
    jbc::test::detail::putU16(b, static_cast<ca::u16>(comps.size()));
    for (const CompSpec& c : comps) {
        jbc::test::detail::putU16(b, c.nameIdx);
        jbc::test::detail::putU16(b, c.descIdx);
        jbc::test::detail::putAttrs(b, c.nested);
    }
    return b;
}

/// 带一个类级 Record 属性的类。CP 侧只驻 this/super 两个 Class 条目。
Bytes classWithRecord(const Bytes& content)
{
    ClassBuilder cb;
    const ca::u16 thisIdx  = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setVersion(61);   // record 事实形态（v60+）
    const ca::u16 attrIdx = cb.utf8("Record");
    cb.addClassAttr(attrIdx, content);
    return cb.build();
}

/// 类级属性槽定位（唯一槽 + 名字匹配）。
AttrSlot* findClassSlot(ClassFile& cf, std::string_view name)
{
    for (AttrSlot& slot : cf.attrs) {
        if (cf.cp.utf8Equals(slot.nameCp, name)) {
            return &slot;
        }
    }
    return nullptr;
}

/// 组件内嵌套属性槽定位（唯一槽 + 名字匹配）。
AttrSlot* findNestedSlot(RecordAttr::Component& c, ClassFile& cf, std::string_view name)
{
    for (AttrSlot& slot : c.attrs) {
        if (cf.cp.utf8Equals(slot.nameCp, name)) {
            return &slot;
        }
    }
    return nullptr;
}

}   // namespace

// ---- 双组件样本的分派钉门 + typed 直读 + copy-through ----
// CP：p/T=1, java/lang/Object=2, Record=3, value=4, Ljava/lang/String;=5,
//     rank=6, I=7, Signature=8, Ljava/lang/String;=9（签名串）。
// 组件 0：value/Ljava/lang/String; 带 Signature(8)->"Ljava/lang/String;"(9)；
// 组件 1：rank/I 无嵌套属性。

namespace {

/// 双组件 + 嵌套 Signature 的样本类与索引簿记。
struct TwoCompClass
{
    Bytes cls = {};
    ca::u16 uValue = 0;   ///< 组件 0 名字
    ca::u16 uStrDesc = 0; ///< 组件 0 描述符
    ca::u16 uRank = 0;    ///< 组件 1 名字
    ca::u16 uIntDesc = 0; ///< 组件 1 描述符
    ca::u16 uSigText = 0; ///< 嵌套 Signature 载荷串
    ca::u16 uOther = 0;   ///< 手术用新描述符串
};

TwoCompClass makeTwoCompClass()
{
    ClassBuilder cb;
    const ca::u16 thisIdx  = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 uRecord  = cb.utf8("Record");
    const ca::u16 uValue   = cb.utf8("value");
    const ca::u16 uStrDesc = cb.utf8("Ljava/lang/String;");
    const ca::u16 uRank    = cb.utf8("rank");
    const ca::u16 uIntDesc = cb.utf8("I");
    const ca::u16 uSig     = cb.utf8("Signature");
    const ca::u16 uSigText = cb.utf8("Ljava/lang/String;");
    const ca::u16 uOther   = cb.utf8("Ljava/lang/Integer;");
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setVersion(61);
    std::vector<CompSpec> comps;
    comps.push_back(CompSpec{uValue, uStrDesc,
                             {jbc::test::AttrSpec{uSig,
                                                  [&] {
                                                      Bytes p;
                                                      jbc::test::detail::putU16(p, uSigText);
                                                      return p;
                                                  }()}}});
    comps.push_back(CompSpec{uRank, uIntDesc, {}});
    cb.addClassAttr(uRecord, recordBody(comps));
    return TwoCompClass{cb.build(), uValue, uStrDesc, uRank, uIntDesc, uSigText, uOther};
}

}   // namespace

TEST(RecordAttrTest, TypedDispatchProducesTypedBody)
{
    const TwoCompClass fx = makeTwoCompClass();
    auto cf = jbc::test::readOk(fx.cls);
    ASSERT_NE(cf, nullptr);
    auto d = cf->decodeAttr(cf->attrs[0]);
    ASSERT_TRUE(d.is_ok());
    ASSERT_EQ(std::move(d).unwrap()->kind(), AttrKind::Record);
    const auto* rec = static_cast<const RecordAttr*>(cf->attrs[0].peek());
    ASSERT_NE(rec, nullptr);
    ASSERT_EQ(rec->components.size(), 2u);
    EXPECT_EQ(rec->components[0].nameCp, fx.uValue);
    EXPECT_EQ(rec->components[0].descriptorCp, fx.uStrDesc);
    EXPECT_EQ(rec->components[1].nameCp, fx.uRank);
    EXPECT_EQ(rec->components[1].descriptorCp, fx.uIntDesc);
    // 组件 0 带嵌套 Signature 槽；组件 1 无嵌套属性。
    ASSERT_EQ(rec->components[0].attrs.size(), 1u);
    EXPECT_TRUE(cf->cp.utf8Equals(rec->components[0].attrs[0].nameCp, "Signature"));
    EXPECT_TRUE(rec->components[1].attrs.empty());
    // 未触碰（Decoded 态写回走 copy）：字节一致。
    EXPECT_EQ(jbc::test::writeOk(*cf), fx.cls);
}

TEST(RecordAttrTest, TypedBodyDecodeLeavesSlotUntouched)
{
    const TwoCompClass fx = makeTwoCompClass();
    auto cf = jbc::test::readOk(fx.cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findClassSlot(*cf, "Record");
    ASSERT_NE(slot, nullptr);
    auto d = decodeRecordBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto body = std::move(d).unwrap();
    ASSERT_EQ(body->kind(), AttrKind::Record);
    // 直读入口不触碰槽状态（lazy 语义保持）。
    EXPECT_FALSE(slot->decoded());
    const auto& rec = static_cast<const RecordAttr&>(*body);
    ASSERT_EQ(rec.components.size(), 2u);
    EXPECT_TRUE(rec.trailing.empty());
}

TEST(RecordAttrTest, TruncatedBodyIsExplicitFailure)
{
    // 组件计数只有 1 字节。
    const Bytes cls = classWithRecord({0x00});
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    auto d = cf->decodeAttr(cf->attrs[0]);
    ASSERT_TRUE(d.is_err());
    EXPECT_EQ(d.unwrap_err(), ReadError::Truncated);
    // 解码失败不影响 lazy copy-through。
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

TEST(RecordAttrTest, TruncatedComponentIsExplicitFailure)
{
    // 声明 1 组件，只带 name 两字节（desc 缺）。
    Bytes half;
    jbc::test::detail::putU16(half, 1);
    jbc::test::detail::putU16(half, 4);
    const Bytes cls = classWithRecord(half);
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    auto d = cf->decodeAttr(cf->attrs[0]);
    ASSERT_TRUE(d.is_err());
    EXPECT_EQ(d.unwrap_err(), ReadError::Truncated);
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

TEST(RecordAttrTest, MalformedNestedTableIsExplicitFailure)
{
    // 组件嵌套属性表长度声明越界（malformed 嵌套体）：显式 Err，不猜。
    // 体 = count(1) + name(4) + desc(5) + attrCount(1) + 一个声明 8 字节但
    // 只有 2 字节载荷的嵌套属性。
    ClassBuilder cb;
    const ca::u16 thisIdx  = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 uRecord  = cb.utf8("Record");
    const ca::u16 uValue   = cb.utf8("value");
    const ca::u16 uStrDesc = cb.utf8("Ljava/lang/String;");
    const ca::u16 uSig     = cb.utf8("Signature");
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setVersion(61);
    Bytes body;
    jbc::test::detail::putU16(body, 1);
    jbc::test::detail::putU16(body, uValue);
    jbc::test::detail::putU16(body, uStrDesc);
    jbc::test::detail::putU16(body, 1);   // 嵌套属性表 1 项
    jbc::test::detail::putU16(body, uSig);
    jbc::test::detail::putU32(body, 8);   // 声明 8 字节
    jbc::test::detail::putU16(body, 0x00AA);   // 实际只有 2 字节
    cb.addClassAttr(uRecord, body);
    const Bytes cls = cb.build();

    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    auto d = cf->decodeAttr(cf->attrs[0]);
    ASSERT_TRUE(d.is_err());
    EXPECT_EQ(d.unwrap_err(), ReadError::BadAttributeLength);
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

TEST(RecordAttrTest, TrailingSurvivesReencode)
{
    Bytes content = recordBody({CompSpec{4, 5, {}}});
    content.push_back(0xDE);
    content.push_back(0xAD);
    const Bytes cls = classWithRecord(content);
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findClassSlot(*cf, "Record");
    ASSERT_NE(slot, nullptr);
    auto d = decodeRecordBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto body = std::move(d).unwrap();
    ASSERT_EQ(static_cast<const RecordAttr&>(*body).trailing.size(), 2u);
    // 未修改往返 + makeNew 置脏重编码都字节一致（trailing 原样写回）。
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(body));
    EXPECT_EQ(jbc::test::writeOk(*cf), cls) << "re-encode dropped trailing";
}

TEST(RecordAttrTest, EmptyComponentTableRoundtrips)
{
    const Bytes cls = classWithRecord(recordBody({}));
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findClassSlot(*cf, "Record");
    ASSERT_NE(slot, nullptr);
    auto d = decodeRecordBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto body = std::move(d).unwrap();
    EXPECT_TRUE(static_cast<const RecordAttr&>(*body).components.empty());
    // makeNew 重编码字节一致（空表形态无损）。
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(body));
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

TEST(RecordAttrTest, EncoderRejectsOutOfRangeFields)
{
    // 组件名字索引超 u16。
    {
        const Bytes cls = classWithRecord(recordBody({CompSpec{4, 5, {}}}));
        auto cf = jbc::test::readOk(cls);
        ASSERT_NE(cf, nullptr);
        AttrSlot* slot = findClassSlot(*cf, "Record");
        ASSERT_NE(slot, nullptr);
        auto d = decodeRecordBody(*cf, *slot);
        ASSERT_TRUE(d.is_ok());
        auto body = std::move(d).unwrap();
        static_cast<RecordAttr&>(*body).components[0].nameCp = 0x10000;
        *slot = AttrSlot::makeNew(slot->nameCp, std::move(body));
        const auto r = writeClass(*cf);
        ASSERT_TRUE(r.is_err());
        EXPECT_EQ(r.unwrap_err(), WriteError::Unencodable);
    }
    // 组件数超 u16 声明域（上界拒绝）。
    {
        const Bytes cls = classWithRecord(recordBody({CompSpec{4, 5, {}}}));
        auto cf = jbc::test::readOk(cls);
        ASSERT_NE(cf, nullptr);
        AttrSlot* slot = findClassSlot(*cf, "Record");
        ASSERT_NE(slot, nullptr);
        auto d = decodeRecordBody(*cf, *slot);
        ASSERT_TRUE(d.is_ok());
        auto body = std::move(d).unwrap();
        // resize（非 assign）：AttrSlot 持 unique_ptr 只可移动，Component
        // 无拷贝赋值；resize 只要求可默认构造 + 可移动插入。
        static_cast<RecordAttr&>(*body).components.resize(0x10000);        *slot = AttrSlot::makeNew(slot->nameCp, std::move(body));
        const auto r = writeClass(*cf);
        ASSERT_TRUE(r.is_err());
        EXPECT_EQ(r.unwrap_err(), WriteError::Unencodable);
    }
    // 组件嵌套属性表长超 u16（嵌套槽 declarations 上界拒绝）。
    {
        const Bytes cls = classWithRecord(recordBody({CompSpec{4, 5, {}}}));
        auto cf = jbc::test::readOk(cls);
        ASSERT_NE(cf, nullptr);
        AttrSlot* slot = findClassSlot(*cf, "Record");
        ASSERT_NE(slot, nullptr);
        auto d = decodeRecordBody(*cf, *slot);
        ASSERT_TRUE(d.is_ok());
        auto body = std::move(d).unwrap();
        static_cast<RecordAttr&>(*body).components[0].attrs.resize(0x10000);
        *slot = AttrSlot::makeNew(slot->nameCp, std::move(body));
        const auto r = writeClass(*cf);
        ASSERT_TRUE(r.is_err());
        EXPECT_EQ(r.unwrap_err(), WriteError::Unencodable);
    }
}

TEST(RecordAttrTest, NestedAttrDirtyReencodesParent)
{
    // 父 Record 槽只置 Decoded，仅组件嵌套 Signature 槽被改：写回必须感知
    // 嵌套置脏并重编码父体（attrNeedsReencode 递归闸）——这是嵌套槽
    // copy-through 闸门的反面钉门。
    const TwoCompClass fx = makeTwoCompClass();
    auto cf = jbc::test::readOk(fx.cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findClassSlot(*cf, "Record");
    ASSERT_NE(slot, nullptr);
    auto d = cf->decodeAttr(*slot);
    ASSERT_TRUE(d.is_ok());
    auto& rec = static_cast<RecordAttr&>(*slot->body);
    AttrSlot* sigSlot = findNestedSlot(rec.components[0], *cf, "Signature");
    ASSERT_NE(sigSlot, nullptr);
    auto sd = cf->decodeAttr(*sigSlot);
    ASSERT_TRUE(sd.is_ok());
    ASSERT_EQ(std::move(sd).unwrap()->kind(), AttrKind::Signature);
    auto& sig = static_cast<SignatureAttr&>(*sigSlot->body);
    EXPECT_EQ(sig.signatureCp, fx.uSigText);
    sig.signatureCp = fx.uOther;             // 改签名串指向
    *sigSlot = AttrSlot::makeNew(sigSlot->nameCp, std::move(sigSlot->body));

    const Bytes out = jbc::test::writeOk(*cf);
    ASSERT_NE(out, fx.cls) << "nested signature change was silently dropped";

    // 重读：组件嵌套 Signature 读出新串，其余面逐字节不动（差集只在载荷）。
    auto cf2 = jbc::test::readOk(out);
    ASSERT_NE(cf2, nullptr);
    AttrSlot* slot2 = findClassSlot(*cf2, "Record");
    ASSERT_NE(slot2, nullptr);
    auto d2 = decodeRecordBody(*cf2, *slot2);
    ASSERT_TRUE(d2.is_ok());
    const auto body2 = std::move(d2).unwrap();
    const auto& rec2 = static_cast<const RecordAttr&>(*body2);
    ASSERT_EQ(rec2.components.size(), 2u);
    EXPECT_EQ(rec2.components[0].nameCp, fx.uValue);
    EXPECT_EQ(rec2.components[0].descriptorCp, fx.uStrDesc);
    ASSERT_EQ(rec2.components[0].attrs.size(), 1u);
    const ByteView span = cf2->source().spanOrEmpty(rec2.components[0].attrs[0].data);
    ASSERT_EQ(span.size(), 2u);
    const ca::u32 sigCp = (static_cast<ca::u32>(span[0]) << 8) | span[1];
    EXPECT_EQ(sigCp, fx.uOther);
}

TEST(RecordAttrTest, TypedComponentSurgeryRoundtrip)
{
    // 组件名/描述符 typed 手术：命中换新串（CP 只追加），miss 原样；
    // makeNew 置 New 重编码后重读断言。
    const TwoCompClass fx = makeTwoCompClass();
    auto cf = jbc::test::readOk(fx.cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findClassSlot(*cf, "Record");
    ASSERT_NE(slot, nullptr);
    auto d = decodeRecordBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto body = std::move(d).unwrap();
    auto& rec = static_cast<RecordAttr&>(*body);

    // 组件 0 名字换 "rank"（复用既有串），描述符换 "Ljava/lang/Integer;"；
    // 组件 1 原样。
    rec.components[0].nameCp = fx.uRank;
    rec.components[0].descriptorCp = fx.uOther;
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(body));

    const Bytes out = jbc::test::writeOk(*cf);
    auto cf2 = jbc::test::readOk(out);
    ASSERT_NE(cf2, nullptr);
    AttrSlot* slot2 = findClassSlot(*cf2, "Record");
    ASSERT_NE(slot2, nullptr);
    auto d2 = decodeRecordBody(*cf2, *slot2);
    ASSERT_TRUE(d2.is_ok());
    const auto body2 = std::move(d2).unwrap();
    const auto& rec2 = static_cast<const RecordAttr&>(*body2);
    ASSERT_EQ(rec2.components.size(), 2u);
    EXPECT_EQ(rec2.components[0].nameCp, fx.uRank);
    EXPECT_EQ(cf2->cp.utf8Of(rec2.components[0].nameCp), "rank");
    EXPECT_EQ(rec2.components[0].descriptorCp, fx.uOther);
    EXPECT_EQ(cf2->cp.utf8Of(rec2.components[0].descriptorCp), "Ljava/lang/Integer;");
    EXPECT_EQ(rec2.components[1].nameCp, fx.uRank);
    EXPECT_EQ(rec2.components[1].descriptorCp, fx.uIntDesc);
}
