/// @file        enclosing_method_attr_test.cpp
/// @brief       EnclosingMethod（JVMS 4.7.7）typed 建模单测：分派接线钉门、
///              typed 解码/编码（含 method_index=0 形态）、半截体显式失败、
///              trailing 保持、u16 域宽闸门，以及「typed 读写与既有 Raw
///              手术口径一致」的对拍。
///
/// 分派已接线（design.md §32.9）：decodeAttrBody 对 "EnclosingMethod" 返回
/// typed 体（B3 接线批与消费方迁移同批落地）；Raw 手术 oracle 从槽
/// 源 span 直接取体字节，是旧栈 remapEnclosingMethodAttr 字节手术的
/// 测试内镜像（口径对齐用，非第二套手术）。
#include <gtest/gtest.h>

#include <array>
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

/// EnclosingMethod 体：u2 class_index + u2 method_index（NAT）。
Bytes emBody(ca::u16 classCp, ca::u16 natCp)
{
    Bytes b;
    jbc::test::detail::putU16(b, classCp);
    jbc::test::detail::putU16(b, natCp);
    return b;
}

/// 带类级 EnclosingMethod 的类。CP 分配：p/T=1, Object=2, OldOuter=3,
/// oldM=4, (LOldType;)V=5, EnclosingMethod=6; Class 条目 7=OldOuter(3);
/// NAT 8=(4,5)。
Bytes classWithEnclosingMethod(const Bytes& content)
{
    ClassBuilder cb;
    const ca::u16 thisIdx   = cb.utf8("p/T");
    const ca::u16 superIdx  = cb.utf8("java/lang/Object");
    const ca::u16 outerIdx  = cb.utf8("OldOuter");
    const ca::u16 mNameIdx  = cb.utf8("oldM");
    const ca::u16 mDescIdx  = cb.utf8("(LOldType;)V");
    const ca::u16 attrIdx   = cb.utf8("EnclosingMethod");
    [[maybe_unused]] const ca::u16 outerClass = cb.classRef(outerIdx);
    [[maybe_unused]] const ca::u16 nat = cb.nameAndType(mNameIdx, mDescIdx);
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setVersion(52);
    cb.addClassAttr(attrIdx, content);
    return cb.build();
}

/// 属性槽按名字定位（比较名字字节；CP 只追加，索引等值不可靠）。
AttrSlot* findSlot(ClassFile& cf, std::vector<AttrSlot>& attrs, std::string_view name)
{
    for (AttrSlot& slot : attrs) {
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

/// typed 手术路径：typed 解码 → class 槽换驻留条目 + NAT 解析重指新条目
/// → makeNew 置 New → 写回。语义镜像旧栈 remapEnclosingMethodAttr 的
/// 「规则命中改方法名与描述符」分支。
Bytes typedSurgery(const Bytes& cls)
{
    auto cf = jbc::test::readOk(cls);
    EXPECT_NE(cf, nullptr);
    AttrSlot* slot = findSlot(*cf, cf->attrs, "EnclosingMethod");
    EXPECT_NE(slot, nullptr);
    auto d = decodeEnclosingMethodBody(*cf, *slot);
    EXPECT_TRUE(d.is_ok());
    auto body = std::move(d).unwrap();
    auto& em = static_cast<EnclosingMethodAttr&>(*body);

    // 外围类：OldOuter → NewOuter。
    em.classCp = internClass(*cf, "NewOuter");
    // 包围方法：规则命中 oldM/(LOldType;)V → newM/(LNewType;)V，新 NAT 条目。
    const ConstantPool::Entry& nat = cf->cp.at(em.methodNatCp);
    EXPECT_EQ(nat.tag, CpTag::NameAndType);
    EXPECT_EQ(cf->cp.utf8Of(nat.a), "oldM");
    em.methodNatCp =
        cf->cp.addNameAndType(internUtf8(*cf, "newM"), internUtf8(*cf, "(LNewType;)V"));

    *slot = AttrSlot::makeNew(slot->nameCp, std::move(body));
    return jbc::test::writeOk(*cf);
}

/// Raw 手术 oracle：旧栈 remapEnclosingMethodAttr 字节手术的测试内
/// 逐字节镜像（口径对齐用，非第二套手术——生产手术已迁 typed）。体字节
/// 从槽源 span 直接取，不依赖 decodeAttr 的分派形态。
Bytes rawSurgeryOracle(const Bytes& cls)
{
    auto cf = jbc::test::readOk(cls);
    if (cf == nullptr) {
        ADD_FAILURE() << "readOk failed";
        return {};
    }
    AttrSlot* slot = findSlot(*cf, cf->attrs, "EnclosingMethod");
    if (slot == nullptr) {
        ADD_FAILURE() << "EnclosingMethod slot missing";
        return {};
    }
    const ByteView data = cf->source().spanOrEmpty(slot->data);
    if (data.size() != slot->data.length || data.size() < 4) {
        ADD_FAILURE() << "EnclosingMethod raw body unavailable";
        return {};
    }
    const ca::u32 classCp = (static_cast<ca::u32>(data[0]) << 8) | data[1];
    const ca::u32 natIdx  = (static_cast<ca::u32>(data[2]) << 8) | data[3];

    // 外围类名解析（旧栈口径：classNameOfCp → mapInternalName 命中才动）。
    const ca::u32 oldClassNameCp = cf->cp.at(classCp).a;
    EXPECT_EQ(cf->cp.utf8Of(oldClassNameCp), "OldOuter");

    // NAT 解析（旧栈口径：结构不符不动；此处结构合法）。
    const ConstantPool::Entry& nat = cf->cp.at(natIdx);
    EXPECT_EQ(nat.tag, CpTag::NameAndType);
    EXPECT_EQ(cf->cp.utf8Of(nat.a), "oldM");
    EXPECT_EQ(cf->cp.utf8Of(nat.b), "(LOldType;)V");

    // 外围类映射命中。
    const ca::u32 newClassCp = internClass(*cf, "NewOuter");
    // 规则命中：方法名与描述符都改，新 NAT 条目。
    const ca::u32 newNat =
        cf->cp.addNameAndType(internUtf8(*cf, "newM"), internUtf8(*cf, "(LNewType;)V"));

    Bytes out;
    jbc::test::detail::putU16(out, static_cast<ca::u16>(newClassCp));
    jbc::test::detail::putU16(out, static_cast<ca::u16>(newNat));
    *slot = AttrSlot::makeNew(
        slot->nameCp, std::make_unique<RawAttr>(Bytes{out.begin(), out.end()}));
    return jbc::test::writeOk(*cf);
}

}   // namespace

// ---- 分派接线钉门：decodeAttrBody 对 EnclosingMethod 返回 typed 体 ----
//
// B3 接线批翻转分派（与消费方迁移同批，design.md §32.9）：本用例
// 在挂账期钉 Raw 形、接线时翻红对账，现钉 typed 形。

TEST(EnclosingMethodAttrTest, TypedDispatchWiredAfterConsumerMigration)
{
    const Bytes cls = classWithEnclosingMethod(emBody(7, 8));
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    auto d = cf->decodeAttr(cf->attrs[0]);
    ASSERT_TRUE(d.is_ok());
    EXPECT_EQ(d.unwrap()->kind(), AttrKind::EnclosingMethod);
    // 接线后整类写回仍逐字节不动（decode 是观察侧）。
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

// ---- typed 解码：两槽逐字段命中；method_index=0 合法形态 ----

TEST(EnclosingMethodAttrTest, TypedBodyDecodeMatchesBytes)
{
    const Bytes cls = classWithEnclosingMethod(emBody(7, 8));
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findSlot(*cf, cf->attrs, "EnclosingMethod");
    ASSERT_NE(slot, nullptr);
    auto d = decodeEnclosingMethodBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto body = std::move(d).unwrap();
    ASSERT_EQ(body->kind(), AttrKind::EnclosingMethod);
    // 直读入口不触碰槽状态（lazy 语义保持）。
    EXPECT_FALSE(slot->decoded());
    const auto& em = static_cast<const EnclosingMethodAttr&>(*body);
    EXPECT_EQ(em.classCp, 7u);
    EXPECT_EQ(em.methodNatCp, 8u);
    EXPECT_TRUE(em.trailing.empty());
}

TEST(EnclosingMethodAttrTest, ZeroMethodFormIsLegal)
{
    // 外围类是初始化器时不指名方法：method_index = 0。
    const Bytes cls = classWithEnclosingMethod(emBody(7, 0));
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findSlot(*cf, cf->attrs, "EnclosingMethod");
    ASSERT_NE(slot, nullptr);
    auto d = decodeEnclosingMethodBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto body = std::move(d).unwrap();
    const auto& em = static_cast<const EnclosingMethodAttr&>(*body);
    EXPECT_EQ(em.methodNatCp, 0u);
    // makeNew 重编码字节一致（0 形态在编码面无损）。
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(body));
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

// ---- 半截体：3 字节体显式 Err；lazy 写回不受影响 ----

TEST(EnclosingMethodAttrTest, TruncatedBodyIsExplicitFailure)
{
    const Bytes cls = classWithEnclosingMethod({0x00, 0x07, 0x00});
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findSlot(*cf, cf->attrs, "EnclosingMethod");
    ASSERT_NE(slot, nullptr);
    auto d = decodeEnclosingMethodBody(*cf, *slot);
    ASSERT_TRUE(d.is_err());
    EXPECT_EQ(d.unwrap_err(), ReadError::Truncated);
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

// ---- typed 编码：makeNew 置 New 重编码，字节与原 Raw 内容一致 ----

TEST(EnclosingMethodAttrTest, TypedEncodeIsCanonical)
{
    const Bytes cls = classWithEnclosingMethod(emBody(7, 8));
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findSlot(*cf, cf->attrs, "EnclosingMethod");
    ASSERT_NE(slot, nullptr);
    auto d = decodeEnclosingMethodBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(d).unwrap());
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

// ---- trailing（design.md §27.1）：多余尾部字节保留 + 重编码原样写回 ----

TEST(EnclosingMethodAttrTest, TrailingSurvivesReencode)
{
    Bytes content = emBody(7, 8);
    content.push_back(0xDE);
    content.push_back(0xAD);
    const Bytes cls = classWithEnclosingMethod(content);
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findSlot(*cf, cf->attrs, "EnclosingMethod");
    ASSERT_NE(slot, nullptr);
    auto d = decodeEnclosingMethodBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto body = std::move(d).unwrap();
    ASSERT_EQ(static_cast<const EnclosingMethodAttr&>(*body).trailing.size(), 2u);
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(body));
    EXPECT_EQ(jbc::test::writeOk(*cf), cls) << "re-encode dropped trailing";
}

// ---- 编码闸门（design.md §27.2）：u2 编码槽超域宽显式 Err ----

TEST(EnclosingMethodAttrTest, EncoderRejectsOutOfRangeFields)
{
    for (const bool badClass : {true, false}) {
        const Bytes cls = classWithEnclosingMethod(emBody(7, 8));
        auto cf = jbc::test::readOk(cls);
        ASSERT_NE(cf, nullptr);
        AttrSlot* slot = findSlot(*cf, cf->attrs, "EnclosingMethod");
        ASSERT_NE(slot, nullptr);
        auto d = decodeEnclosingMethodBody(*cf, *slot);
        ASSERT_TRUE(d.is_ok());
        auto body = std::move(d).unwrap();
        auto& em = static_cast<EnclosingMethodAttr&>(*body);
        if (badClass) {
            em.classCp = 0x10000;
        }
        else {
            em.methodNatCp = 0x10000;
        }
        *slot = AttrSlot::makeNew(slot->nameCp, std::move(body));
        const auto r = writeClass(*cf);
        ASSERT_TRUE(r.is_err());
        EXPECT_EQ(r.unwrap_err(), WriteError::Unencodable);
    }
}

// ---- 手术口径一致：typed 读写与 Raw 手术 oracle 逐字节一致，且 typed
//      重读看到改写后的两槽 ----

TEST(EnclosingMethodAttrTest, TypedSurgeryMatchesRawRemapOracle)
{
    const Bytes cls = classWithEnclosingMethod(emBody(7, 8));
    const Bytes typedOut = typedSurgery(cls);
    const Bytes rawOut   = rawSurgeryOracle(cls);
    EXPECT_EQ(typedOut, rawOut) << "typed path diverges from raw-remap oracle";

    // typed 读出与手术结果一致。
    auto cf = jbc::test::readOk(typedOut);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findSlot(*cf, cf->attrs, "EnclosingMethod");
    ASSERT_NE(slot, nullptr);
    auto d = decodeEnclosingMethodBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto body = std::move(d).unwrap();
    const auto& em = static_cast<const EnclosingMethodAttr&>(*body);
    EXPECT_EQ(cf->cp.utf8Of(cf->cp.at(em.classCp).a), "NewOuter");
    const ConstantPool::Entry& nat = cf->cp.at(em.methodNatCp);
    EXPECT_EQ(nat.tag, CpTag::NameAndType);
    EXPECT_EQ(cf->cp.utf8Of(nat.a), "newM");
    EXPECT_EQ(cf->cp.utf8Of(nat.b), "(LNewType;)V");
}
