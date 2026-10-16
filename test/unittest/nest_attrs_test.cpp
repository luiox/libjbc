/// @file        nest_attrs_test.cpp
/// @brief       NestHost（JVMS 4.7.29）/ NestMembers（4.7.28）/
///              PermittedSubclasses（4.7.31）typed 建模单测：分派接线钉门、
///              typed 解码/编码、半截体显式失败、trailing 保持、u16 域宽闸门，
///              以及「typed 读写与既有 Raw 手术口径一致」的对拍。
///
/// 三件均为类级挂载、载荷全为 CP Class 索引（design.md §32.10）：分派接线与
/// 消费方迁移同批落地；Raw 手术 oracle 从槽源 span 直接取体字节，是
/// 旧栈 remapSingleClassAttr/remapClassListAttr 字节手术的测试内镜像
/// （口径对齐用，非第二套手术）。
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

/// NestHost 体：单 u2。
Bytes nestHostBody(ca::u16 hostCp)
{
    Bytes b;
    jbc::test::detail::putU16(b, hostCp);
    return b;
}

/// u2 计数 + u2 条目表（NestMembers/PermittedSubclasses 共形）。
Bytes classListBody(const std::vector<ca::u16>& cps)
{
    Bytes b;
    jbc::test::detail::putU16(b, static_cast<ca::u16>(cps.size()));
    for (const ca::u16 cp : cps) {
        jbc::test::detail::putU16(b, cp);
    }
    return b;
}

/// 带一个类级属性的类。classNames 逐个先驻 Utf8 再驻 Class，Class 条目索引
/// 经 classIdxOut 带出（断言用）。
Bytes classWithAttr(const std::string& name, const Bytes& content,
                    const std::vector<std::string>& classNames,
                    std::vector<ca::u16>* classIdxOut)
{
    ClassBuilder cb;
    const ca::u16 thisIdx  = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    std::vector<ca::u16> nameIdxs;
    for (const std::string& n : classNames) {
        nameIdxs.push_back(cb.utf8(n));
    }
    const ca::u16 attrIdx = cb.utf8(name);
    std::vector<ca::u16> clsIdxs;
    for (const ca::u16 ni : nameIdxs) {
        clsIdxs.push_back(cb.classRef(ni));
    }
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setVersion(52);
    cb.addClassAttr(attrIdx, content);
    if (classIdxOut != nullptr) {
        *classIdxOut = clsIdxs;
    }
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
    // 未命中：新驻 Utf8 + Class（与 CP 只追加纪律一致）。
    const ConstantPool& cp2 = cf.cp;
    ca::u32 nameCp = 0;
    for (ca::u32 i = 1; i < cp2.size(); ++i) {
        if (cp2.isUtf8(i) && cp2.utf8Equals(i, internalName)) {
            nameCp = i;
            break;
        }
    }
    if (nameCp == 0) {
        nameCp = cf.cp.addUtf8(internalName);
    }
    return cf.cp.addClass(nameCp);
}

/// typed 手术路径（NestHost）：typed 解码 → 宿主类名映射（Old* 前缀命中才动）
/// → makeNew 置 New。语义镜像旧栈 remapSingleClassAttr。
Bytes typedNestHostSurgery(const Bytes& cls)
{
    auto cf = jbc::test::readOk(cls);
    EXPECT_NE(cf, nullptr);
    AttrSlot* slot = findClassSlot(*cf, "NestHost");
    EXPECT_NE(slot, nullptr);
    auto d = decodeNestHostBody(*cf, *slot);
    EXPECT_TRUE(d.is_ok());
    auto body = std::move(d).unwrap();
    auto& nh  = static_cast<NestHostAttr&>(*body);

    const std::string_view name = cf->cp.utf8Of(cf->cp.at(nh.hostClassCp).a);
    if (name.rfind("Old", 0) == 0) {
        nh.hostClassCp = internClass(*cf, "New" + std::string{name.substr(3)});
    }
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(body));
    return jbc::test::writeOk(*cf);
}

/// Raw 手术 oracle（NestHost）：remapSingleClassAttr 旧字节手术的测试内
/// 逐字节镜像。体字节从槽源 span 直接取，不依赖 decodeAttr 的分派形态。
Bytes rawNestHostSurgeryOracle(const Bytes& cls)
{
    auto cf = jbc::test::readOk(cls);
    if (cf == nullptr) {
        ADD_FAILURE() << "readOk failed";
        return {};
    }
    AttrSlot* slot = findClassSlot(*cf, "NestHost");
    if (slot == nullptr) {
        ADD_FAILURE() << "NestHost slot missing";
        return {};
    }
    const ByteView data = cf->source().spanOrEmpty(slot->data);
    if (data.size() != slot->data.length || data.size() < 2) {
        ADD_FAILURE() << "NestHost raw body unavailable";
        return {};
    }
    const ca::u32 classCp = (static_cast<ca::u32>(data[0]) << 8) | data[1];
    const std::string_view name = cf->cp.utf8Of(cf->cp.at(classCp).a);

    Bytes out;
    if (name.rfind("Old", 0) == 0) {
        jbc::test::detail::putU16(
            out, static_cast<ca::u16>(internClass(*cf, "New" + std::string{name.substr(3)})));
    }
    else {
        jbc::test::detail::putU16(out, static_cast<ca::u16>(classCp));
    }
    *slot = AttrSlot::makeNew(
        slot->nameCp, std::make_unique<RawAttr>(Bytes{out.begin(), out.end()}));
    return jbc::test::writeOk(*cf);
}

/// typed 手术路径（u2 类列表）：NestMembers/PermittedSubclasses 共形共用
/// （AttrT 区分体类型，cpsMember 指向各自的条目表成员）。语义镜像
/// 旧栈 remapClassListAttr：命中才动，miss 原样。
template <typename AttrT, typename DecodeFn>
Bytes typedClassListSurgery(const Bytes& cls, std::string_view attrName, DecodeFn decode,
                            std::vector<ca::u32> AttrT::* cpsMember)
{
    auto cf = jbc::test::readOk(cls);
    EXPECT_NE(cf, nullptr);
    AttrSlot* slot = findClassSlot(*cf, attrName);
    EXPECT_NE(slot, nullptr);
    auto d = decode(*cf, *slot);
    EXPECT_TRUE(d.is_ok());
    auto body = std::move(d).unwrap();
    std::vector<ca::u32>& cps = static_cast<AttrT&>(*body).*cpsMember;

    for (ca::u32& cp : cps) {
        const std::string_view name = cf->cp.utf8Of(cf->cp.at(cp).a);
        if (name.rfind("Old", 0) == 0) {
            cp = internClass(*cf, "New" + std::string{name.substr(3)});
        }
    }
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(body));
    return jbc::test::writeOk(*cf);
}

/// Raw 手术 oracle（u2 类列表）：remapClassListAttr 旧字节手术的测试内镜像。
Bytes rawClassListSurgeryOracle(const Bytes& cls, std::string_view attrName)
{
    auto cf = jbc::test::readOk(cls);
    if (cf == nullptr) {
        ADD_FAILURE() << "readOk failed";
        return {};
    }
    AttrSlot* slot = findClassSlot(*cf, attrName);
    if (slot == nullptr) {
        ADD_FAILURE() << "attr slot missing";
        return {};
    }
    const ByteView data = cf->source().spanOrEmpty(slot->data);
    if (data.size() != slot->data.length || data.size() < 2) {
        ADD_FAILURE() << "raw body unavailable";
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
    const ca::u32 n = rdU16();
    jbc::test::detail::putU16(out, static_cast<ca::u16>(n));
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

// ---- NestHost：分派钉门 + typed 直读 + 半截 + trailing + 闸门 + 手术对拍 ----

TEST(NestHostAttrTest, TypedDispatchProducesTypedBody)
{
    // CP：p/T=1, Object=2, OldHost=3, NestHost=4；Class 条目 5=OldHost(3)。
    std::vector<ca::u16> clsIdx;
    const Bytes cls = classWithAttr("NestHost", nestHostBody(5), {"OldHost"}, &clsIdx);
    ASSERT_EQ(clsIdx.size(), 1u);
    EXPECT_EQ(clsIdx[0], 5u);

    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    auto d = cf->decodeAttr(cf->attrs[0]);
    ASSERT_TRUE(d.is_ok());
    ASSERT_EQ(std::move(d).unwrap()->kind(), AttrKind::NestHost);
    const auto* nh = static_cast<const NestHostAttr*>(cf->attrs[0].peek());
    ASSERT_NE(nh, nullptr);
    EXPECT_EQ(nh->hostClassCp, 5u);
    EXPECT_EQ(cf->cp.utf8Of(cf->cp.at(nh->hostClassCp).a), "OldHost");
    // 未触碰（Decoded 态写回走 copy）：字节一致。
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

TEST(NestHostAttrTest, TypedBodyDecodeLeavesSlotUntouched)
{
    const Bytes cls = classWithAttr("NestHost", nestHostBody(5), {"OldHost"}, nullptr);
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findClassSlot(*cf, "NestHost");
    ASSERT_NE(slot, nullptr);
    auto d = decodeNestHostBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto body = std::move(d).unwrap();
    ASSERT_EQ(body->kind(), AttrKind::NestHost);
    // 直读入口不触碰槽状态（lazy 语义保持）。
    EXPECT_FALSE(slot->decoded());
    EXPECT_EQ(static_cast<const NestHostAttr&>(*body).hostClassCp, 5u);
    EXPECT_TRUE(static_cast<const NestHostAttr&>(*body).trailing.empty());
}

TEST(NestHostAttrTest, TruncatedBodyIsExplicitFailure)
{
    const Bytes cls = classWithAttr("NestHost", {0x00}, {"OldHost"}, nullptr);
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    auto d = cf->decodeAttr(cf->attrs[0]);
    ASSERT_TRUE(d.is_err());
    EXPECT_EQ(d.unwrap_err(), ReadError::Truncated);
    // 解码失败不影响 lazy copy-through。
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

TEST(NestHostAttrTest, TrailingSurvivesReencode)
{
    Bytes body = nestHostBody(5);
    body.push_back(0xDE);
    body.push_back(0xAD);
    const Bytes cls = classWithAttr("NestHost", body, {"OldHost"}, nullptr);
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findClassSlot(*cf, "NestHost");
    ASSERT_NE(slot, nullptr);
    auto d = decodeNestHostBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto body2 = std::move(d).unwrap();
    ASSERT_EQ(static_cast<const NestHostAttr&>(*body2).trailing.size(), 2u);
    // 未修改往返 + 置脏重编码都字节一致（trailing 原样写回）。
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(body2));
    EXPECT_EQ(jbc::test::writeOk(*cf), cls) << "re-encode dropped trailing";
}

TEST(NestHostAttrTest, EncoderRejectsOutOfRangeIndex)
{
    const Bytes cls = classWithAttr("NestHost", nestHostBody(5), {"OldHost"}, nullptr);
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findClassSlot(*cf, "NestHost");
    ASSERT_NE(slot, nullptr);
    auto d = decodeNestHostBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto body = std::move(d).unwrap();
    static_cast<NestHostAttr&>(*body).hostClassCp = 0x10000;
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(body));
    const auto r = writeClass(*cf);
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), WriteError::Unencodable);
}

TEST(NestHostAttrTest, TypedSurgeryMatchesRawRemapOracle)
{
    const Bytes cls = classWithAttr("NestHost", nestHostBody(5), {"OldHost"}, nullptr);
    const Bytes typedOut = typedNestHostSurgery(cls);
    const Bytes rawOut   = rawNestHostSurgeryOracle(cls);
    EXPECT_EQ(typedOut, rawOut) << "typed path diverges from raw-remap oracle";

    // typed 读出与手术结果一致。
    auto cf = jbc::test::readOk(typedOut);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findClassSlot(*cf, "NestHost");
    ASSERT_NE(slot, nullptr);
    auto d = decodeNestHostBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto body = std::move(d).unwrap();
    const auto& nh = static_cast<const NestHostAttr&>(*body);
    EXPECT_EQ(cf->cp.utf8Of(cf->cp.at(nh.hostClassCp).a), "NewHost");
}

// ---- NestMembers：同铁律 + 空表 + 手术对拍 ----

TEST(NestMembersAttrTest, TypedDispatchProducesTypedBody)
{
    // CP：p/T=1, Object=2, OldA=3, OldB=4, NestMembers=5；Class 条目 6=OldA、
    // 7=OldB。
    std::vector<ca::u16> clsIdx;
    const Bytes cls = classWithAttr("NestMembers", classListBody({6, 7}), {"OldA", "OldB"},
                                    &clsIdx);
    ASSERT_EQ(clsIdx.size(), 2u);
    EXPECT_EQ(clsIdx[0], 6u);
    EXPECT_EQ(clsIdx[1], 7u);

    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    auto d = cf->decodeAttr(cf->attrs[0]);
    ASSERT_TRUE(d.is_ok());
    ASSERT_EQ(std::move(d).unwrap()->kind(), AttrKind::NestMembers);
    const auto* nm = static_cast<const NestMembersAttr*>(cf->attrs[0].peek());
    ASSERT_NE(nm, nullptr);
    ASSERT_EQ(nm->memberCps.size(), 2u);
    EXPECT_EQ(nm->memberCps[0], 6u);
    EXPECT_EQ(nm->memberCps[1], 7u);
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

TEST(NestMembersAttrTest, EmptyTableRoundtrips)
{
    const Bytes cls = classWithAttr("NestMembers", classListBody({}), {"OldA"}, nullptr);
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findClassSlot(*cf, "NestMembers");
    ASSERT_NE(slot, nullptr);
    auto d = decodeNestMembersBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto body = std::move(d).unwrap();
    EXPECT_TRUE(static_cast<const NestMembersAttr&>(*body).memberCps.empty());
    // makeNew 重编码字节一致（空表形态无损）。
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(body));
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

TEST(NestMembersAttrTest, TruncatedTableIsExplicitFailure)
{
    // 声明 2 条，只带 1 条 + 1 字节。
    Bytes half = classListBody({6, 7});
    half.resize(2 + 2 + 1);
    const Bytes cls = classWithAttr("NestMembers", half, {"OldA", "OldB"}, nullptr);
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    auto d = cf->decodeAttr(cf->attrs[0]);
    ASSERT_TRUE(d.is_err());
    EXPECT_EQ(d.unwrap_err(), ReadError::Truncated);
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

TEST(NestMembersAttrTest, TrailingSurvivesReencode)
{
    Bytes content = classListBody({6, 7});
    content.push_back(0xDE);
    content.push_back(0xAD);
    const Bytes cls = classWithAttr("NestMembers", content, {"OldA", "OldB"}, nullptr);
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findClassSlot(*cf, "NestMembers");
    ASSERT_NE(slot, nullptr);
    auto d = decodeNestMembersBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto body = std::move(d).unwrap();
    ASSERT_EQ(static_cast<const NestMembersAttr&>(*body).trailing.size(), 2u);
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(body));
    EXPECT_EQ(jbc::test::writeOk(*cf), cls) << "re-encode dropped trailing";
}

TEST(NestMembersAttrTest, EncoderRejectsOutOfRangeFields)
{
    // 单条目超 u16。
    {
        const Bytes cls = classWithAttr("NestMembers", classListBody({6}), {"OldA"}, nullptr);
        auto cf = jbc::test::readOk(cls);
        ASSERT_NE(cf, nullptr);
        AttrSlot* slot = findClassSlot(*cf, "NestMembers");
        ASSERT_NE(slot, nullptr);
        auto d = decodeNestMembersBody(*cf, *slot);
        ASSERT_TRUE(d.is_ok());
        auto body = std::move(d).unwrap();
        static_cast<NestMembersAttr&>(*body).memberCps[0] = 0x10000;
        *slot = AttrSlot::makeNew(slot->nameCp, std::move(body));
        const auto r = writeClass(*cf);
        ASSERT_TRUE(r.is_err());
        EXPECT_EQ(r.unwrap_err(), WriteError::Unencodable);
    }
    // 条目数超 u16 声明域（上界拒绝）。
    {
        const Bytes cls = classWithAttr("NestMembers", classListBody({6}), {"OldA"}, nullptr);
        auto cf = jbc::test::readOk(cls);
        ASSERT_NE(cf, nullptr);
        AttrSlot* slot = findClassSlot(*cf, "NestMembers");
        ASSERT_NE(slot, nullptr);
        auto d = decodeNestMembersBody(*cf, *slot);
        ASSERT_TRUE(d.is_ok());
        auto body = std::move(d).unwrap();
        static_cast<NestMembersAttr&>(*body).memberCps.assign(0x10000, 6);
        *slot = AttrSlot::makeNew(slot->nameCp, std::move(body));
        const auto r = writeClass(*cf);
        ASSERT_TRUE(r.is_err());
        EXPECT_EQ(r.unwrap_err(), WriteError::Unencodable);
    }
}

TEST(NestMembersAttrTest, TypedSurgeryMatchesRawRemapOracle)
{
    const Bytes cls = classWithAttr("NestMembers", classListBody({6, 7}), {"OldA", "OldB"},
                                    nullptr);
    const Bytes typedOut =
        typedClassListSurgery<NestMembersAttr>(cls, "NestMembers", decodeNestMembersBody,
                                               &NestMembersAttr::memberCps);
    const Bytes rawOut = rawClassListSurgeryOracle(cls, "NestMembers");
    EXPECT_EQ(typedOut, rawOut) << "typed path diverges from raw-remap oracle";

    // typed 读出与手术结果一致。
    auto cf = jbc::test::readOk(typedOut);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findClassSlot(*cf, "NestMembers");
    ASSERT_NE(slot, nullptr);
    auto d = decodeNestMembersBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto body = std::move(d).unwrap();
    const auto& nm = static_cast<const NestMembersAttr&>(*body);
    ASSERT_EQ(nm.memberCps.size(), 2u);
    EXPECT_EQ(cf->cp.utf8Of(cf->cp.at(nm.memberCps[0]).a), "NewA");
    EXPECT_EQ(cf->cp.utf8Of(cf->cp.at(nm.memberCps[1]).a), "NewB");
}

// ---- PermittedSubclasses：形状与 NestMembers 同构，同铁律 ----

TEST(PermittedSubclassesAttrTest, TypedDispatchProducesTypedBody)
{
    // CP：p/T=1, Object=2, OldP=3, PermittedSubclasses=4；Class 条目 5=OldP。
    std::vector<ca::u16> clsIdx;
    const Bytes cls = classWithAttr("PermittedSubclasses", classListBody({5}), {"OldP"},
                                    &clsIdx);
    ASSERT_EQ(clsIdx.size(), 1u);
    EXPECT_EQ(clsIdx[0], 5u);

    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    auto d = cf->decodeAttr(cf->attrs[0]);
    ASSERT_TRUE(d.is_ok());
    ASSERT_EQ(std::move(d).unwrap()->kind(), AttrKind::PermittedSubclasses);
    const auto* ps = static_cast<const PermittedSubclassesAttr*>(cf->attrs[0].peek());
    ASSERT_NE(ps, nullptr);
    ASSERT_EQ(ps->permittedCps.size(), 1u);
    EXPECT_EQ(ps->permittedCps[0], 5u);
    EXPECT_EQ(cf->cp.utf8Of(cf->cp.at(ps->permittedCps[0]).a), "OldP");
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

TEST(PermittedSubclassesAttrTest, TruncatedBodyIsExplicitFailure)
{
    Bytes half = classListBody({5, 5});
    half.resize(2 + 2 + 1);
    const Bytes cls = classWithAttr("PermittedSubclasses", half, {"OldP"}, nullptr);
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    auto d = cf->decodeAttr(cf->attrs[0]);
    ASSERT_TRUE(d.is_err());
    EXPECT_EQ(d.unwrap_err(), ReadError::Truncated);
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

TEST(PermittedSubclassesAttrTest, TrailingSurvivesReencode)
{
    Bytes content = classListBody({5});
    content.push_back(0xDE);
    const Bytes cls = classWithAttr("PermittedSubclasses", content, {"OldP"}, nullptr);
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findClassSlot(*cf, "PermittedSubclasses");
    ASSERT_NE(slot, nullptr);
    auto d = decodePermittedSubclassesBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto body = std::move(d).unwrap();
    ASSERT_EQ(static_cast<const PermittedSubclassesAttr&>(*body).trailing.size(), 1u);
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(body));
    EXPECT_EQ(jbc::test::writeOk(*cf), cls) << "re-encode dropped trailing";
}

TEST(PermittedSubclassesAttrTest, EncoderRejectsOutOfRangeIndex)
{
    const Bytes cls = classWithAttr("PermittedSubclasses", classListBody({5}), {"OldP"},
                                    nullptr);
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findClassSlot(*cf, "PermittedSubclasses");
    ASSERT_NE(slot, nullptr);
    auto d = decodePermittedSubclassesBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto body = std::move(d).unwrap();
    static_cast<PermittedSubclassesAttr&>(*body).permittedCps[0] = 0x10000;
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(body));
    const auto r = writeClass(*cf);
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), WriteError::Unencodable);
}

TEST(PermittedSubclassesAttrTest, TypedSurgeryMatchesRawRemapOracle)
{
    const Bytes cls = classWithAttr("PermittedSubclasses", classListBody({5}), {"OldP"},
                                    nullptr);
    // PermittedSubclasses 与 NestMembers 体同构，typed 手术共用镜像模板。
    const Bytes typedOut =
        typedClassListSurgery<PermittedSubclassesAttr>(cls, "PermittedSubclasses",
                                                       decodePermittedSubclassesBody,
                                                       &PermittedSubclassesAttr::permittedCps);
    const Bytes rawOut = rawClassListSurgeryOracle(cls, "PermittedSubclasses");
    EXPECT_EQ(typedOut, rawOut) << "typed path diverges from raw-remap oracle";

    // typed 读出与手术结果一致。
    auto cf = jbc::test::readOk(typedOut);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findClassSlot(*cf, "PermittedSubclasses");
    ASSERT_NE(slot, nullptr);
    auto d = decodePermittedSubclassesBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto body = std::move(d).unwrap();
    const auto& ps = static_cast<const PermittedSubclassesAttr&>(*body);
    ASSERT_EQ(ps.permittedCps.size(), 1u);
    EXPECT_EQ(cf->cp.utf8Of(cf->cp.at(ps.permittedCps[0]).a), "NewP");
}
