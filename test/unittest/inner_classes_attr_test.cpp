/// @file        inner_classes_attr_test.cpp
/// @brief       InnerClasses（JVMS 4.7.6）typed 建模单测：分派接线钉门、
///              typed 解码/编码、半截体显式失败、trailing 保持、u16 域宽
///              闸门，以及「typed 读写与既有 Raw 手术口径一致」的对拍。
///
/// 分派已接线（design.md §32.9）：decodeAttrBody 对 "InnerClasses" 返回
/// typed 体（B3 接线批与消费方迁移同批落地）；Raw 手术 oracle 从
/// 槽源 span 直接取体字节，是旧栈 remapInnerClassesAttr 字节手术的
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

/// InnerClasses 体：u2 行数 + {u2, u2, u2, u2}*。
Bytes icBody(const std::vector<std::array<ca::u16, 4>>& rows)
{
    Bytes b;
    jbc::test::detail::putU16(b, static_cast<ca::u16>(rows.size()));
    for (const auto& r : rows) {
        for (const ca::u16 v : r) {
            jbc::test::detail::putU16(b, v);
        }
    }
    return b;
}

/// 带两行 InnerClasses 的类：成员内部类 OldOuter$OldInner/OldInner +
/// 匿名类 OldOuter$1。CP 分配：p/T=1, Object=2, OldOuter$OldInner=3,
/// OldOuter=4, OldInner=5, OldOuter$1=6, InnerClasses=7; Class 条目
/// 8=inner(3), 9=outer(4), 10=anon(6)。
Bytes classWithInnerClasses(const Bytes& content)
{
    ClassBuilder cb;
    const ca::u16 thisIdx  = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 innerIdx = cb.utf8("OldOuter$OldInner");
    const ca::u16 outerIdx = cb.utf8("OldOuter");
    const ca::u16 nameIdx  = cb.utf8("OldInner");
    const ca::u16 anonIdx  = cb.utf8("OldOuter$1");
    const ca::u16 attrIdx  = cb.utf8("InnerClasses");
    [[maybe_unused]] const ca::u16 innerClass = cb.classRef(innerIdx);
    [[maybe_unused]] const ca::u16 outerClass = cb.classRef(outerIdx);
    [[maybe_unused]] const ca::u16 anonClass = cb.classRef(anonIdx);
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setVersion(52);
    cb.addClassAttr(attrIdx, content);
    return cb.build();
}

/// 行内容（引用上面的 CP 分配）：成员内部类行 {inner=8, outer=9, name=5,
/// flags=0x0001} + 匿名行 {inner=10, outer=0, name=0, flags=0x0002}。
std::vector<std::array<ca::u16, 4>> twoRows()
{
    return {{8, 9, 5, 0x0001}, {10, 0, 0, 0x0002}};
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

/// 扫描式 Utf8 驻留：命中既有条目返回原索引，miss 才追加（CP 只追加不
/// 膨胀；与上层 remapper 的 findOrAddUtf8 同口径）。
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

/// 内部类简单名（最后一个 '$' 之后的尾段；无 '$' 为空）——旧栈同款。
std::string_view innerSimpleName(std::string_view internalName) noexcept
{
    const auto dollar = internalName.rfind('$');
    if (dollar == std::string_view::npos || dollar + 1 >= internalName.size()) {
        return {};
    }
    return internalName.substr(dollar + 1);
}

/// 改名映射：Old → New（全名整体映射，模拟改名 plan 对内部类二进制名的
/// 派生映射 OldOuter$OldInner → NewOuter$NewInner）。
std::string mapName(std::string_view name)
{
    std::string out;
    ca::usize at = 0;
    while (at < name.size()) {
        if (name.compare(at, 3, "Old") == 0) {
            out += "New";
            at += 3;
        }
        else {
            out += name[at++];
        }
    }
    return out;
}

/// typed 手术路径：typed 解码 → 逐行改三 CP 槽（类名映射命中才动、
/// outer==0/name==0 保 0、简单名随映射同步且仅在全名命中旧简单名时改写）
/// → makeNew 置 New → 写回。语义镜像旧栈 remapInnerClassesAttr。
Bytes typedSurgery(const Bytes& cls)
{
    auto cf = jbc::test::readOk(cls);
    EXPECT_NE(cf, nullptr);
    AttrSlot* slot = findSlot(*cf, cf->attrs, "InnerClasses");
    EXPECT_NE(slot, nullptr);
    auto d = decodeInnerClassesBody(*cf, *slot);
    EXPECT_TRUE(d.is_ok());
    auto body = std::move(d).unwrap();
    auto& ic = static_cast<InnerClassesAttr&>(*body);

    for (InnerClassesAttr::Row& row : ic.rows) {
        const std::string_view innerName = cf->cp.utf8Of(cf->cp.at(row.innerClassInfoCp).a);
        const std::string mapped = mapName(innerName);
        if (mapped != innerName) {
            row.innerClassInfoCp = internClass(*cf, mapped);
        }
        if (row.outerClassInfoCp != 0) {
            const std::string_view outerName =
                cf->cp.utf8Of(cf->cp.at(row.outerClassInfoCp).a);
            const std::string outerMapped = mapName(outerName);
            if (outerMapped != outerName) {
                row.outerClassInfoCp = internClass(*cf, outerMapped);
            }
        }
        if (row.innerNameCp != 0 && mapped != innerName) {
            const std::string_view oldSimple = innerSimpleName(innerName);
            const std::string_view newSimple = innerSimpleName(mapped);
            if (!oldSimple.empty() && !newSimple.empty() &&
                cf->cp.utf8Equals(row.innerNameCp, oldSimple)) {
                row.innerNameCp = internUtf8(*cf, newSimple);
            }
        }
    }
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(body));
    return jbc::test::writeOk(*cf);
}

/// Raw 手术 oracle：旧栈 remapInnerClassesAttr 字节手术的测试内逐字节
/// 镜像（口径对齐用，非第二套手术——生产手术已迁 typed）。体字节从槽源
/// span 直接取，不依赖 decodeAttr 的分派形态。
Bytes rawSurgeryOracle(const Bytes& cls)
{
    auto cf = jbc::test::readOk(cls);
    if (cf == nullptr) {
        ADD_FAILURE() << "readOk failed";
        return {};
    }
    AttrSlot* slot = findSlot(*cf, cf->attrs, "InnerClasses");
    if (slot == nullptr) {
        ADD_FAILURE() << "InnerClasses slot missing";
        return {};
    }
    const ByteView data = cf->source().spanOrEmpty(slot->data);
    if (data.size() != slot->data.length || data.size() < 2) {
        ADD_FAILURE() << "InnerClasses raw body unavailable";
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
        const ca::u32 innerCp = rdU16();
        const ca::u32 outerCp = rdU16();
        const ca::u32 nameCp  = rdU16();
        const ca::u32 access  = rdU16();

        const std::string_view innerName = cf->cp.utf8Of(cf->cp.at(innerCp).a);
        const std::string mapped = mapName(innerName);
        jbc::test::detail::putU16(out, static_cast<ca::u16>(
            mapped != innerName ? internClass(*cf, mapped) : innerCp));

        if (outerCp != 0) {
            const std::string_view outerName = cf->cp.utf8Of(cf->cp.at(outerCp).a);
            const std::string outerMapped = mapName(outerName);
            jbc::test::detail::putU16(out, static_cast<ca::u16>(
                outerMapped != outerName ? internClass(*cf, outerMapped) : outerCp));
        }
        else {
            jbc::test::detail::putU16(out, 0);
        }

        if (nameCp != 0 && mapped != innerName) {
            const std::string_view oldSimple = innerSimpleName(innerName);
            const std::string_view newSimple = innerSimpleName(mapped);
            if (!oldSimple.empty() && !newSimple.empty() &&
                cf->cp.utf8Equals(nameCp, oldSimple)) {
                jbc::test::detail::putU16(out, static_cast<ca::u16>(internUtf8(*cf, newSimple)));
            }
            else {
                jbc::test::detail::putU16(out, static_cast<ca::u16>(nameCp));
            }
        }
        else {
            jbc::test::detail::putU16(out, static_cast<ca::u16>(nameCp));
        }
        jbc::test::detail::putU16(out, static_cast<ca::u16>(access));
    }
    *slot = AttrSlot::makeNew(
        slot->nameCp, std::make_unique<RawAttr>(Bytes{out.begin(), out.end()}));
    return jbc::test::writeOk(*cf);
}

}   // namespace

// ---- 分派接线钉门：decodeAttrBody 对 InnerClasses 返回 typed 体 ----
//
// B3 接线批翻转分派（与消费方迁移同批，design.md §32.9）：本用例
// 在挂账期钉 Raw 形、接线时翻红对账，现钉 typed 形。

TEST(InnerClassesAttrTest, TypedDispatchWiredAfterConsumerMigration)
{
    const Bytes cls = classWithInnerClasses(icBody(twoRows()));
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    auto d = cf->decodeAttr(cf->attrs[0]);
    ASSERT_TRUE(d.is_ok());
    EXPECT_EQ(d.unwrap()->kind(), AttrKind::InnerClasses);
    // 接线后整类写回仍逐字节不动（decode 是观察侧）。
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

// ---- typed 解码：公开入口直读，结构逐字段命中 ----

TEST(InnerClassesAttrTest, TypedBodyDecodeMatchesBytes)
{
    const Bytes cls = classWithInnerClasses(icBody(twoRows()));
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findSlot(*cf, cf->attrs, "InnerClasses");
    ASSERT_NE(slot, nullptr);
    auto d = decodeInnerClassesBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto body = std::move(d).unwrap();
    ASSERT_EQ(body->kind(), AttrKind::InnerClasses);
    // 直读入口不触碰槽状态（lazy 语义保持，写回仍走 copy-through）。
    EXPECT_FALSE(slot->decoded());
    EXPECT_EQ(slot->state, AttrSlot::State::Untouched);
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

TEST(InnerClassesAttrTest, TypedBodyRowStructure)
{
    const Bytes cls = classWithInnerClasses(icBody(twoRows()));
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findSlot(*cf, cf->attrs, "InnerClasses");
    ASSERT_NE(slot, nullptr);
    auto d = decodeInnerClassesBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto body = std::move(d).unwrap();
    const auto& ic = static_cast<const InnerClassesAttr&>(*body);
    ASSERT_EQ(ic.rows.size(), 2u);
    EXPECT_EQ(ic.rows[0].innerClassInfoCp, 8u);
    EXPECT_EQ(ic.rows[0].outerClassInfoCp, 9u);
    EXPECT_EQ(ic.rows[0].innerNameCp, 5u);
    EXPECT_EQ(ic.rows[0].accessFlags, 0x0001u);
    EXPECT_EQ(ic.rows[1].innerClassInfoCp, 10u);
    EXPECT_EQ(ic.rows[1].outerClassInfoCp, 0u);   // 匿名：无外围
    EXPECT_EQ(ic.rows[1].innerNameCp, 0u);        // 匿名：无简单名
    EXPECT_EQ(ic.rows[1].accessFlags, 0x0002u);
    EXPECT_TRUE(ic.trailing.empty());
}

// ---- 半截体：行数声明与剩余字节不符，显式 Err ----

TEST(InnerClassesAttrTest, TruncatedRowIsExplicitFailure)
{
    const Bytes full = icBody(twoRows());
    Bytes half(full.begin(), full.begin() + 2 + 8 + 5);   // 1 行 + 半行
    const Bytes cls = classWithInnerClasses(half);
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findSlot(*cf, cf->attrs, "InnerClasses");
    ASSERT_NE(slot, nullptr);
    auto d = decodeInnerClassesBody(*cf, *slot);
    ASSERT_TRUE(d.is_err());
    EXPECT_EQ(d.unwrap_err(), ReadError::Truncated);
    // 解码失败不影响 lazy copy-through。
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

// ---- typed 编码：makeNew 置 New 重编码，字节与原 Raw 内容一致 ----

TEST(InnerClassesAttrTest, TypedEncodeIsCanonical)
{
    const Bytes cls = classWithInnerClasses(icBody(twoRows()));
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findSlot(*cf, cf->attrs, "InnerClasses");
    ASSERT_NE(slot, nullptr);
    auto d = decodeInnerClassesBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(d).unwrap());
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

// ---- trailing（design.md §27.1）：多余尾部字节保留 + 重编码原样写回 ----

TEST(InnerClassesAttrTest, TrailingSurvivesReencode)
{
    Bytes content = icBody(twoRows());
    content.push_back(0xDE);
    content.push_back(0xAD);
    const Bytes cls = classWithInnerClasses(content);
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findSlot(*cf, cf->attrs, "InnerClasses");
    ASSERT_NE(slot, nullptr);
    auto d = decodeInnerClassesBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto body = std::move(d).unwrap();
    const auto& ic = static_cast<const InnerClassesAttr&>(*body);
    ASSERT_EQ(ic.trailing.size(), 2u);
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(body));
    EXPECT_EQ(jbc::test::writeOk(*cf), cls) << "re-encode dropped trailing";
}

// ---- 编码闸门（design.md §27.2）：u2 编码列超域宽显式 Err ----

TEST(InnerClassesAttrTest, EncoderRejectsOutOfRangeFields)
{
    // accessFlags 超 u16。
    {
        const Bytes cls = classWithInnerClasses(icBody(twoRows()));
        auto cf = jbc::test::readOk(cls);
        ASSERT_NE(cf, nullptr);
        AttrSlot* slot = findSlot(*cf, cf->attrs, "InnerClasses");
        ASSERT_NE(slot, nullptr);
        auto d = decodeInnerClassesBody(*cf, *slot);
        ASSERT_TRUE(d.is_ok());
        auto body = std::move(d).unwrap();
        static_cast<InnerClassesAttr&>(*body).rows[0].accessFlags = 0x10000;
        *slot = AttrSlot::makeNew(slot->nameCp, std::move(body));
        const auto r = writeClass(*cf);
        ASSERT_TRUE(r.is_err());
        EXPECT_EQ(r.unwrap_err(), WriteError::Unencodable);
    }
    // inner_class_info 超 u16。
    {
        const Bytes cls = classWithInnerClasses(icBody(twoRows()));
        auto cf = jbc::test::readOk(cls);
        ASSERT_NE(cf, nullptr);
        AttrSlot* slot = findSlot(*cf, cf->attrs, "InnerClasses");
        ASSERT_NE(slot, nullptr);
        auto d = decodeInnerClassesBody(*cf, *slot);
        ASSERT_TRUE(d.is_ok());
        auto body = std::move(d).unwrap();
        static_cast<InnerClassesAttr&>(*body).rows[0].innerClassInfoCp = 0x10000;
        *slot = AttrSlot::makeNew(slot->nameCp, std::move(body));
        const auto r = writeClass(*cf);
        ASSERT_TRUE(r.is_err());
        EXPECT_EQ(r.unwrap_err(), WriteError::Unencodable);
    }
}

// ---- 手术口径一致：typed 读写与 Raw 手术 oracle 逐字节一致，且 typed
//      重读看到改写后的三槽 ----

TEST(InnerClassesAttrTest, TypedSurgeryMatchesRawRemapOracle)
{
    const Bytes cls = classWithInnerClasses(icBody(twoRows()));
    const Bytes typedOut = typedSurgery(cls);
    const Bytes rawOut   = rawSurgeryOracle(cls);
    EXPECT_EQ(typedOut, rawOut) << "typed path diverges from raw-remap oracle";

    // typed 读出与手术结果一致：重读 typed 产物，三槽语义 = New 名。
    auto cf = jbc::test::readOk(typedOut);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findSlot(*cf, cf->attrs, "InnerClasses");
    ASSERT_NE(slot, nullptr);
    auto d = decodeInnerClassesBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto body = std::move(d).unwrap();
    const auto& ic = static_cast<const InnerClassesAttr&>(*body);
    ASSERT_EQ(ic.rows.size(), 2u);
    EXPECT_EQ(cf->cp.utf8Of(cf->cp.at(ic.rows[0].innerClassInfoCp).a), "NewOuter$NewInner");
    EXPECT_EQ(cf->cp.utf8Of(cf->cp.at(ic.rows[0].outerClassInfoCp).a), "NewOuter");
    EXPECT_EQ(cf->cp.utf8Of(ic.rows[0].innerNameCp), "NewInner");
    EXPECT_EQ(cf->cp.utf8Of(cf->cp.at(ic.rows[1].innerClassInfoCp).a), "NewOuter$1");
    EXPECT_EQ(ic.rows[1].outerClassInfoCp, 0u);
    EXPECT_EQ(ic.rows[1].innerNameCp, 0u);
    // 扫描式驻留：新名不重复追加（逆向 rename 复用回原条目的前提）。
    ca::u32 newInnerCount = 0;
    for (ca::u32 i = 1; i < cf->cp.size(); ++i) {
        if (cf->cp.isUtf8(i) && cf->cp.utf8Equals(i, "NewOuter$NewInner")) {
            ++newInnerCount;
        }
    }
    EXPECT_EQ(newInnerCount, 1u);
}
