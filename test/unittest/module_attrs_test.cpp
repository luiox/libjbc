/// @file        module_attrs_test.cpp
/// @brief       Module 族三件（Module（JVMS 4.7.25）/ ModulePackages
///             （4.7.26）/ ModuleMainClass（4.7.27））typed 建模单测：分派
///              接线钉门、typed 解码/编码、半截体显式失败、trailing 保持、
///              u16 域宽闸门，以及 Module 体全段（requires/exports/opens/
///              uses/provides，含 transitive/static 旗语与 to 限定目标）的
///              结构等值钉面与 cp_compact 全槽重映射行为等值。
///
/// ModulePackages/ModuleMainClass 在 javac 直译模式实测不产（语料门不背其
/// 分布），全形态覆盖由本文件 crafted 字节钉住。ModuleHashes/
/// ModuleResolution/ModuleTarget 非 JVMS 条目、javac 语料构造上不可产，
/// 不建模（Raw 透传，压缩封闭集外照旧显式拒绝）。
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "jbc/jbc.hpp"
#include "test_class_bytes.hpp"

using namespace jbc::core;
using namespace jbc::ir;
using jbc::test::Bytes;
using jbc::test::ClassBuilder;

namespace {

/// Module 体：名/旗语/版本三槽 + 五段全形态（JVMS 4.7.25，体在 provides 后
/// 结束——主类走独立 ModuleMainClass 属性）。
Bytes moduleBody(
    ca::u16 nameCp, ca::u16 flags, ca::u16 verCp,
    const std::vector<std::tuple<ca::u16, ca::u16, ca::u16>>& requires,
    const std::vector<std::tuple<ca::u16, ca::u16, std::vector<ca::u16>>>& exports,
    const std::vector<std::tuple<ca::u16, ca::u16, std::vector<ca::u16>>>& opens,
    const std::vector<ca::u16>& uses,
    const std::vector<std::pair<ca::u16, std::vector<ca::u16>>>& provides)
{
    Bytes b;
    jbc::test::detail::putU16(b, nameCp);
    jbc::test::detail::putU16(b, flags);
    jbc::test::detail::putU16(b, verCp);
    jbc::test::detail::putU16(b, static_cast<ca::u16>(requires.size()));
    for (const auto& [m, f, v] : requires) {
        jbc::test::detail::putU16(b, m);
        jbc::test::detail::putU16(b, f);
        jbc::test::detail::putU16(b, v);
    }
    const auto putGroups =
        [&b](const std::vector<std::tuple<ca::u16, ca::u16, std::vector<ca::u16>>>& rows) {
            jbc::test::detail::putU16(b, static_cast<ca::u16>(rows.size()));
            for (const auto& [pkg, f, to] : rows) {
                jbc::test::detail::putU16(b, pkg);
                jbc::test::detail::putU16(b, f);
                jbc::test::detail::putU16(b, static_cast<ca::u16>(to.size()));
                for (const ca::u16 t : to) {
                    jbc::test::detail::putU16(b, t);
                }
            }
        };
    putGroups(exports);
    putGroups(opens);
    jbc::test::detail::putU16(b, static_cast<ca::u16>(uses.size()));
    for (const ca::u16 u : uses) {
        jbc::test::detail::putU16(b, u);
    }
    jbc::test::detail::putU16(b, static_cast<ca::u16>(provides.size()));
    for (const auto& [svc, with] : provides) {
        jbc::test::detail::putU16(b, svc);
        jbc::test::detail::putU16(b, static_cast<ca::u16>(with.size()));
        for (const ca::u16 w : with) {
            jbc::test::detail::putU16(b, w);
        }
    }
    return b;
}

/// 带一个类级属性的类； CP 索引构造由调用方自管（module 族条目类型各异）。
Bytes classWithAttrCp(const std::string& attrName, const Bytes& content)
{
    ClassBuilder cb;
    const ca::u16 thisIdx  = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 attrIdx  = cb.utf8(attrName);
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setVersion(61);
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

/// CP Module 条目的名字文本。
std::string_view moduleNameOf(const ClassFile& cf, ca::u32 moduleCp)
{
    return cf.cp.utf8Of(cf.cp.at(moduleCp).a);
}

/// CP Package 条目的名字文本。
std::string_view packageNameOf(const ClassFile& cf, ca::u32 packageCp)
{
    return cf.cp.utf8Of(cf.cp.at(packageCp).a);
}

/// CP Class 条目的二进制名文本。
std::string_view classNameOf(const ClassFile& cf, ca::u32 classCp)
{
    return cf.cp.utf8Of(cf.cp.at(classCp).a);
}

/// u16 期望表 ↔ u32 实表逐元素对拍（gtest 无跨元素类型 vector==）。
void expectCpListEq(const std::vector<ca::u32>& got, const std::vector<ca::u16>& want,
                    const char* what)
{
    ASSERT_EQ(got.size(), want.size()) << what;
    for (ca::usize j = 0; j < want.size(); ++j) {
        EXPECT_EQ(got[j], static_cast<ca::u32>(want[j])) << what << " j=" << j;
    }
}

/// 解码后的 Module 体逐字段钉面（全段等值）。
void expectModuleBodyMatches(const ClassFile& cf, const ModuleAttr& m,
                             ca::u32 nameCp, ca::u16 flags, ca::u32 verCp,
                             const std::vector<std::tuple<ca::u16, ca::u16, ca::u16>>& requires,
                             const std::vector<std::tuple<ca::u16, ca::u16, std::vector<ca::u16>>>& exports,
                             const std::vector<std::tuple<ca::u16, ca::u16, std::vector<ca::u16>>>& opens,
                             const std::vector<ca::u16>& uses,
                             const std::vector<std::pair<ca::u16, std::vector<ca::u16>>>& provides)
{
    EXPECT_EQ(m.moduleNameCp, nameCp);
    EXPECT_EQ(m.moduleFlags, flags);
    EXPECT_EQ(m.moduleVersionCp, verCp);
    ASSERT_EQ(m.requiresEntries.size(), requires.size());
    for (ca::usize i = 0; i < requires.size(); ++i) {
        EXPECT_EQ(m.requiresEntries[i].moduleCp, std::get<0>(requires[i]));
        EXPECT_EQ(m.requiresEntries[i].flags, std::get<1>(requires[i]));
        EXPECT_EQ(m.requiresEntries[i].versionCp, std::get<2>(requires[i]));
    }
    ASSERT_EQ(m.exports.size(), exports.size());
    for (ca::usize i = 0; i < exports.size(); ++i) {
        EXPECT_EQ(m.exports[i].packageCp, std::get<0>(exports[i]));
        EXPECT_EQ(m.exports[i].flags, std::get<1>(exports[i]));
        expectCpListEq(m.exports[i].toCps, std::get<2>(exports[i]), "exports.to");
    }
    ASSERT_EQ(m.opens.size(), opens.size());
    for (ca::usize i = 0; i < opens.size(); ++i) {
        EXPECT_EQ(m.opens[i].packageCp, std::get<0>(opens[i]));
        EXPECT_EQ(m.opens[i].flags, std::get<1>(opens[i]));
        expectCpListEq(m.opens[i].toCps, std::get<2>(opens[i]), "opens.to");
    }
    expectCpListEq(m.usesCps, uses, "uses");
    ASSERT_EQ(m.provides.size(), provides.size());
    for (ca::usize i = 0; i < provides.size(); ++i) {
        EXPECT_EQ(m.provides[i].serviceCp, provides[i].first);
        expectCpListEq(m.provides[i].withCps, provides[i].second, "provides.with");
    }
}

/// 建会话；失败即断言（cp_compact craft 用）。
std::shared_ptr<ClassSession> sessionOf(const Bytes& bytes)
{
    auto r = makeSessionFromBytes(ByteView{bytes.data(), bytes.size()});
    if (r.is_err()) {
        ADD_FAILURE() << "makeSessionFromBytes failed";
        return nullptr;
    }
    return std::move(r).unwrap();
}

}   // namespace

// ---- Module：分派钉门 + 全段等值 + typed 直读 + 半截 + trailing + 闸门 ----

TEST(ModuleAttrTest, TypedDispatchProducesTypedBody)
{
    // CP（自管索引）：4="jbc.corpus" 5="17.0" 6="svc" 7="app" 8="impl"
    // 9="svc/Svc" 10="impl/SvcImpl"；Module/Package/Class 条目依次后驻。
    ClassBuilder cb;
    const ca::u16 thisIdx   = cb.utf8("p/T");
    const ca::u16 superIdx  = cb.utf8("java/lang/Object");
    const ca::u16 attrIdx   = cb.utf8("Module");
    const ca::u16 uModName  = cb.utf8("jbc.corpus");
    const ca::u16 uVer      = cb.utf8("17.0");
    const ca::u16 uPkgSvc   = cb.utf8("svc");
    const ca::u16 uPkgApp   = cb.utf8("app");
    const ca::u16 uPkgImpl  = cb.utf8("impl");
    const ca::u16 uSvcName  = cb.utf8("svc/Svc");
    const ca::u16 uImplName = cb.utf8("impl/SvcImpl");
    const ca::u16 cThis     = cb.classRef(thisIdx);
    const ca::u16 cSuper    = cb.classRef(superIdx);
    const ca::u16 mMain     = cb.moduleRef(uModName);
    const ca::u16 mLog      = cb.moduleRef(cb.utf8("java.logging"));
    const ca::u16 mDesk     = cb.moduleRef(cb.utf8("java.desktop"));
    const ca::u16 pSvc      = cb.packageRef(uPkgSvc);
    const ca::u16 pApp      = cb.packageRef(uPkgApp);
    const ca::u16 pImpl     = cb.packageRef(uPkgImpl);
    const ca::u16 cSvc      = cb.classRef(uSvcName);
    const ca::u16 cImpl     = cb.classRef(uImplName);
    cb.setThis(cThis).setSuper(cSuper).setVersion(61);

    // 全形态：requires 携带 MANDATED|TRANSITIVE 与 STATIC_PHASE 双旗语；
    // exports 无限定与 to 限定双形态；opens；uses；provides...with。
    const std::vector<std::tuple<ca::u16, ca::u16, ca::u16>> reqs{
        {mLog, 0x8020, 0}, {mDesk, 0x0040, 0}};
    const std::vector<std::tuple<ca::u16, ca::u16, std::vector<ca::u16>>> exps{
        {pSvc, 0, {}}, {pApp, 0, {mLog}}};
    const std::vector<std::tuple<ca::u16, ca::u16, std::vector<ca::u16>>> opns{
        {pImpl, 0, {}}};
    const std::vector<ca::u16> uses{cSvc};
    const std::vector<std::pair<ca::u16, std::vector<ca::u16>>> provides{{cSvc, {cImpl}}};
    cb.addClassAttr(attrIdx, moduleBody(mMain, 0, 0, reqs, exps, opns, uses, provides));
    const Bytes cls = cb.build();

    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    auto d = cf->decodeAttr(cf->attrs[0]);
    ASSERT_TRUE(d.is_ok());
    ASSERT_EQ(std::move(d).unwrap()->kind(), AttrKind::Module);
    const auto* m = static_cast<const ModuleAttr*>(cf->attrs[0].peek());
    ASSERT_NE(m, nullptr);
    // 名/旗语/版本槽逐项钉面（版本 0 = 无版本段）。
    EXPECT_EQ(m->moduleNameCp, mMain);
    EXPECT_EQ(moduleNameOf(*cf, m->moduleNameCp), "jbc.corpus");
    EXPECT_EQ(m->moduleFlags, 0u);
    EXPECT_EQ(m->moduleVersionCp, 0u);
    // requires 双旗语形态（TRANSITIVE+MANDATED 合成 0x8020 / STATIC_PHASE）。
    ASSERT_EQ(m->requiresEntries.size(), 2u);
    EXPECT_EQ(m->requiresEntries[0].moduleCp, mLog);
    EXPECT_EQ(m->requiresEntries[0].flags, 0x8020u);
    EXPECT_EQ(m->requiresEntries[1].moduleCp, mDesk);
    EXPECT_EQ(m->requiresEntries[1].flags, 0x0040u);
    // exports：无限定 + to 限定。
    ASSERT_EQ(m->exports.size(), 2u);
    EXPECT_EQ(packageNameOf(*cf, m->exports[0].packageCp), "svc");
    EXPECT_TRUE(m->exports[0].toCps.empty());
    EXPECT_EQ(packageNameOf(*cf, m->exports[1].packageCp), "app");
    ASSERT_EQ(m->exports[1].toCps.size(), 1u);
    EXPECT_EQ(moduleNameOf(*cf, m->exports[1].toCps[0]), "java.logging");
    // opens / uses / provides。
    ASSERT_EQ(m->opens.size(), 1u);
    EXPECT_EQ(packageNameOf(*cf, m->opens[0].packageCp), "impl");
    ASSERT_EQ(m->usesCps.size(), 1u);
    EXPECT_EQ(classNameOf(*cf, m->usesCps[0]), "svc/Svc");
    ASSERT_EQ(m->provides.size(), 1u);
    EXPECT_EQ(classNameOf(*cf, m->provides[0].serviceCp), "svc/Svc");
    ASSERT_EQ(m->provides[0].withCps.size(), 1u);
    EXPECT_EQ(classNameOf(*cf, m->provides[0].withCps[0]), "impl/SvcImpl");
    // 未触碰（Decoded 态写回走 copy）：字节一致。
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

TEST(ModuleAttrTest, TypedBodyDecodeLeavesSlotUntouched)
{
    const Bytes cls = classWithAttrCp("Module", moduleBody(0, 0, 0, {}, {}, {}, {}, {}));
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findClassSlot(*cf, "Module");
    ASSERT_NE(slot, nullptr);
    auto d = decodeModuleBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto body = std::move(d).unwrap();
    ASSERT_EQ(body->kind(), AttrKind::Module);
    // 直读入口不触碰槽状态（lazy 语义保持）。
    EXPECT_FALSE(slot->decoded());
    const auto& m = static_cast<const ModuleAttr&>(*body);
    EXPECT_TRUE(m.requiresEntries.empty());
    EXPECT_TRUE(m.exports.empty());
    EXPECT_TRUE(m.opens.empty());
    EXPECT_TRUE(m.usesCps.empty());
    EXPECT_TRUE(m.provides.empty());
    EXPECT_TRUE(m.trailing.empty());
}

TEST(ModuleAttrTest, TruncatedBodiesAreExplicitFailures)
{
    // 全形态体的各个截断点：头三槽 / requires 中段 / exports to 表中段 /
    // uses 中段 / provides with 中段。
    ClassBuilder cb;
    const ca::u16 mLog = cb.moduleRef(cb.utf8("java.logging"));
    const ca::u16 pSvc = cb.packageRef(cb.utf8("svc"));
    const ca::u16 cSvc = cb.classRef(cb.utf8("svc/Svc"));
    cb.setThis(cb.classRef(cb.utf8("p/T")));
    cb.setSuper(cb.classRef(cb.utf8("java/lang/Object")));
    cb.setVersion(61);
    static_cast<void>(cb.utf8("Module"));   // 属性名条目（索引无关截断面）

    const std::vector<std::tuple<ca::u16, ca::u16, ca::u16>> reqs{{mLog, 0x0020, 0}};
    const std::vector<std::tuple<ca::u16, ca::u16, std::vector<ca::u16>>> exps{
        {pSvc, 0, {mLog}}};
    const std::vector<ca::u16> uses{cSvc};
    const std::vector<std::pair<ca::u16, std::vector<ca::u16>>> provides{{cSvc, {cSvc}}};
    const Bytes full = moduleBody(mLog, 0, 0, reqs, exps, exps, uses, provides);

    const ca::usize cutPoints[] = {1, 5, 9, 14, 15, 17, 19, 21, 23, 25};
    for (const ca::usize cut : cutPoints) {
        if (cut >= full.size()) {
            continue;
        }
        Bytes half(full.begin(), full.begin() + static_cast<ca::usize>(cut));
        const Bytes cls = classWithAttrCp("Module", half);
        auto cf = jbc::test::readOk(cls);
        ASSERT_NE(cf, nullptr);
        auto d = cf->decodeAttr(cf->attrs[0]);
        ASSERT_TRUE(d.is_err()) << "cut=" << cut;
        EXPECT_EQ(d.unwrap_err(), ReadError::Truncated) << "cut=" << cut;
        // 解码失败不影响 lazy copy-through。
        EXPECT_EQ(jbc::test::writeOk(*cf), cls) << "cut=" << cut;
    }
}

TEST(ModuleAttrTest, TrailingSurvivesReencode)
{
    Bytes body = moduleBody(0, 0, 0, {}, {}, {}, {}, {});
    body.push_back(0xDE);
    body.push_back(0xAD);
    const Bytes cls = classWithAttrCp("Module", body);
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findClassSlot(*cf, "Module");
    ASSERT_NE(slot, nullptr);
    auto d = decodeModuleBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto body2 = std::move(d).unwrap();
    ASSERT_EQ(static_cast<const ModuleAttr&>(*body2).trailing.size(), 2u);
    // 未修改往返 + 置脏重编码都字节一致（trailing 原样写回）。
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(body2));
    EXPECT_EQ(jbc::test::writeOk(*cf), cls) << "re-encode dropped trailing";
}

TEST(ModuleAttrTest, TypedReencodeIsByteIdenticalForFullForm)
{
    // 全形态体经 makeNew 强制重编码后逐字节一致（解码 → 重编码闭环）。
    ClassBuilder cb;
    const ca::u16 uMod  = cb.utf8("jbc.corpus");
    const ca::u16 uVer  = cb.utf8("17.0");
    const ca::u16 mMain = cb.moduleRef(uMod);
    const ca::u16 mLog  = cb.moduleRef(cb.utf8("java.logging"));
    const ca::u16 pSvc  = cb.packageRef(cb.utf8("svc"));
    const ca::u16 cSvc  = cb.classRef(cb.utf8("svc/Svc"));
    cb.setThis(cb.classRef(cb.utf8("p/T")));
    cb.setSuper(cb.classRef(cb.utf8("java/lang/Object")));
    cb.setVersion(61);
    const std::vector<std::tuple<ca::u16, ca::u16, ca::u16>> reqs{{mLog, 0x8020, uVer}};
    const std::vector<std::tuple<ca::u16, ca::u16, std::vector<ca::u16>>> exps{{pSvc, 0, {mLog}}};
    const Bytes body = moduleBody(mMain, 0x0020, uVer, reqs, exps, exps, {cSvc}, {{cSvc, {cSvc}}});
    const ca::u16 attrIdx = cb.utf8("Module");
    cb.addClassAttr(attrIdx, body);
    const Bytes cls = cb.build();

    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findClassSlot(*cf, "Module");
    ASSERT_NE(slot, nullptr);
    auto d = decodeModuleBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto body2 = std::move(d).unwrap();
    expectModuleBodyMatches(
        *cf, static_cast<const ModuleAttr&>(*body2), mMain, 0x0020, uVer, reqs, exps, exps,
        {cSvc}, {{cSvc, {cSvc}}});
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(body2));
    EXPECT_EQ(jbc::test::writeOk(*cf), cls) << "full-form re-encode diverged";
}

TEST(ModuleAttrTest, EncoderRejectsOutOfRangeFields)
{
    const Bytes base = classWithAttrCp("Module", moduleBody(0, 0, 0, {}, {}, {}, {}, {}));
    // 模块名索引超 u16。
    {
        auto cf = jbc::test::readOk(base);
        ASSERT_NE(cf, nullptr);
        AttrSlot* slot = findClassSlot(*cf, "Module");
        ASSERT_NE(slot, nullptr);
        auto d = decodeModuleBody(*cf, *slot);
        ASSERT_TRUE(d.is_ok());
        auto body = std::move(d).unwrap();
        static_cast<ModuleAttr&>(*body).moduleNameCp = 0x10000;
        *slot = AttrSlot::makeNew(slot->nameCp, std::move(body));
        const auto r = writeClass(*cf);
        ASSERT_TRUE(r.is_err());
        EXPECT_EQ(r.unwrap_err(), WriteError::Unencodable);
    }
    // requires 条目版本索引超 u16。
    {
        auto cf = jbc::test::readOk(base);
        ASSERT_NE(cf, nullptr);
        AttrSlot* slot = findClassSlot(*cf, "Module");
        ASSERT_NE(slot, nullptr);
        auto d = decodeModuleBody(*cf, *slot);
        ASSERT_TRUE(d.is_ok());
        auto body = std::move(d).unwrap();
        ModuleAttr::Require e;
        e.moduleCp  = 1;
        e.flags     = 0x0020;
        e.versionCp = 0x10000;
        static_cast<ModuleAttr&>(*body).requiresEntries.push_back(e);
        *slot = AttrSlot::makeNew(slot->nameCp, std::move(body));
        const auto r = writeClass(*cf);
        ASSERT_TRUE(r.is_err());
        EXPECT_EQ(r.unwrap_err(), WriteError::Unencodable);
    }
    // uses 条目数超 u16 声明域（上界拒绝）。
    {
        auto cf = jbc::test::readOk(base);
        ASSERT_NE(cf, nullptr);
        AttrSlot* slot = findClassSlot(*cf, "Module");
        ASSERT_NE(slot, nullptr);
        auto d = decodeModuleBody(*cf, *slot);
        ASSERT_TRUE(d.is_ok());
        auto body = std::move(d).unwrap();
        static_cast<ModuleAttr&>(*body).usesCps.assign(0x10000, 1);
        *slot = AttrSlot::makeNew(slot->nameCp, std::move(body));
        const auto r = writeClass(*cf);
        ASSERT_TRUE(r.is_err());
        EXPECT_EQ(r.unwrap_err(), WriteError::Unencodable);
    }
}

// ---- ModulePackages：同铁律 + 空表 + 闸门 ----

TEST(ModulePackagesAttrTest, TypedDispatchProducesTypedBody)
{
    ClassBuilder cb;
    const ca::u16 uPkgA = cb.utf8("svc");
    const ca::u16 uPkgB = cb.utf8("impl");
    const ca::u16 pA    = cb.packageRef(uPkgA);
    const ca::u16 pB    = cb.packageRef(uPkgB);
    cb.setThis(cb.classRef(cb.utf8("p/T")));
    cb.setSuper(cb.classRef(cb.utf8("java/lang/Object")));
    cb.setVersion(61);
    Bytes body;
    jbc::test::detail::putU16(body, 2);
    jbc::test::detail::putU16(body, pA);
    jbc::test::detail::putU16(body, pB);
    cb.addClassAttr(cb.utf8("ModulePackages"), body);
    const Bytes cls = cb.build();

    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    auto d = cf->decodeAttr(cf->attrs[0]);
    ASSERT_TRUE(d.is_ok());
    ASSERT_EQ(std::move(d).unwrap()->kind(), AttrKind::ModulePackages);
    const auto* mp = static_cast<const ModulePackagesAttr*>(cf->attrs[0].peek());
    ASSERT_NE(mp, nullptr);
    ASSERT_EQ(mp->packageCps.size(), 2u);
    EXPECT_EQ(packageNameOf(*cf, mp->packageCps[0]), "svc");
    EXPECT_EQ(packageNameOf(*cf, mp->packageCps[1]), "impl");
    // makeNew 强制重编码字节一致。
    AttrSlot& slot = cf->attrs[0];
    auto d2 = decodeModulePackagesBody(*cf, slot);
    ASSERT_TRUE(d2.is_ok());
    slot = AttrSlot::makeNew(slot.nameCp, std::move(d2).unwrap());
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

TEST(ModulePackagesAttrTest, EmptyTableRoundtrips)
{
    const Bytes cls = classWithAttrCp("ModulePackages", {0x00, 0x00});
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findClassSlot(*cf, "ModulePackages");
    ASSERT_NE(slot, nullptr);
    auto d = decodeModulePackagesBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto body = std::move(d).unwrap();
    EXPECT_TRUE(static_cast<const ModulePackagesAttr&>(*body).packageCps.empty());
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(body));
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

TEST(ModulePackagesAttrTest, TruncatedTableIsExplicitFailure)
{
    // 声明 2 条，只带 1 条 + 1 字节。
    Bytes half;
    jbc::test::detail::putU16(half, 2);
    jbc::test::detail::putU16(half, 1);
    half.push_back(0x00);
    const Bytes cls = classWithAttrCp("ModulePackages", half);
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    auto d = cf->decodeAttr(cf->attrs[0]);
    ASSERT_TRUE(d.is_err());
    EXPECT_EQ(d.unwrap_err(), ReadError::Truncated);
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

TEST(ModulePackagesAttrTest, TrailingSurvivesReencode)
{
    Bytes body;
    jbc::test::detail::putU16(body, 1);
    jbc::test::detail::putU16(body, 1);
    body.push_back(0xDE);
    const Bytes cls = classWithAttrCp("ModulePackages", body);
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findClassSlot(*cf, "ModulePackages");
    ASSERT_NE(slot, nullptr);
    auto d = decodeModulePackagesBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto body2 = std::move(d).unwrap();
    ASSERT_EQ(static_cast<const ModulePackagesAttr&>(*body2).trailing.size(), 1u);
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(body2));
    EXPECT_EQ(jbc::test::writeOk(*cf), cls) << "re-encode dropped trailing";
}

TEST(ModulePackagesAttrTest, EncoderRejectsOutOfRangeIndex)
{
    Bytes body;
    jbc::test::detail::putU16(body, 1);
    jbc::test::detail::putU16(body, 1);
    const Bytes cls = classWithAttrCp("ModulePackages", body);
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findClassSlot(*cf, "ModulePackages");
    ASSERT_NE(slot, nullptr);
    auto d = decodeModulePackagesBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto body2 = std::move(d).unwrap();
    static_cast<ModulePackagesAttr&>(*body2).packageCps[0] = 0x10000;
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(body2));
    const auto r = writeClass(*cf);
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), WriteError::Unencodable);
}

// ---- ModuleMainClass：形状同 NestHost（单 u2 Class 槽），同铁律 ----

TEST(ModuleMainClassAttrTest, TypedDispatchProducesTypedBody)
{
    ClassBuilder cb;
    const ca::u16 uMain = cb.utf8("app/Main");
    const ca::u16 cMain = cb.classRef(uMain);
    cb.setThis(cb.classRef(cb.utf8("p/T")));
    cb.setSuper(cb.classRef(cb.utf8("java/lang/Object")));
    cb.setVersion(61);
    Bytes body;
    jbc::test::detail::putU16(body, cMain);
    cb.addClassAttr(cb.utf8("ModuleMainClass"), body);
    const Bytes cls = cb.build();

    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    auto d = cf->decodeAttr(cf->attrs[0]);
    ASSERT_TRUE(d.is_ok());
    ASSERT_EQ(std::move(d).unwrap()->kind(), AttrKind::ModuleMainClass);
    const auto* mc = static_cast<const ModuleMainClassAttr*>(cf->attrs[0].peek());
    ASSERT_NE(mc, nullptr);
    EXPECT_EQ(mc->mainClassCp, cMain);
    EXPECT_EQ(classNameOf(*cf, mc->mainClassCp), "app/Main");
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

TEST(ModuleMainClassAttrTest, TruncatedBodyIsExplicitFailure)
{
    const Bytes cls = classWithAttrCp("ModuleMainClass", {0x00});
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    auto d = cf->decodeAttr(cf->attrs[0]);
    ASSERT_TRUE(d.is_err());
    EXPECT_EQ(d.unwrap_err(), ReadError::Truncated);
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

TEST(ModuleMainClassAttrTest, TrailingSurvivesReencode)
{
    Bytes body;
    jbc::test::detail::putU16(body, 1);
    body.push_back(0xDE);
    body.push_back(0xAD);
    const Bytes cls = classWithAttrCp("ModuleMainClass", body);
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findClassSlot(*cf, "ModuleMainClass");
    ASSERT_NE(slot, nullptr);
    auto d = decodeModuleMainClassBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto body2 = std::move(d).unwrap();
    ASSERT_EQ(static_cast<const ModuleMainClassAttr&>(*body2).trailing.size(), 2u);
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(body2));
    EXPECT_EQ(jbc::test::writeOk(*cf), cls) << "re-encode dropped trailing";
}

TEST(ModuleMainClassAttrTest, EncoderRejectsOutOfRangeIndex)
{
    Bytes body;
    jbc::test::detail::putU16(body, 1);
    const Bytes cls = classWithAttrCp("ModuleMainClass", body);
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findClassSlot(*cf, "ModuleMainClass");
    ASSERT_NE(slot, nullptr);
    auto d = decodeModuleMainClassBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto body2 = std::move(d).unwrap();
    static_cast<ModuleMainClassAttr&>(*body2).mainClassCp = 0x10000;
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(body2));
    const auto r = writeClass(*cf);
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), WriteError::Unencodable);
}

// ---- cp_compact：Module 族全槽重映射 + 去重 + 行为等值 ----

TEST(ModuleCompact, ModuleFamilySlotsRemapAndDedupe)
{
    // 池布置（自管索引，制造死条目与等值重复）：
    //   1 "p/T" 2 "java/lang/Object" 3 "Module" 4 "ModulePackages"
    //   5 "ModuleMainClass" 6 "jbc.corpus" 7 "jbc.corpus"(重复→去重)
    //   8 "svc" 9 "svc"(重复) 10 "app/Main"(重复由 Class 侧共享)
    //   11 "java.logging" 12 "svc/Svc"(死条目只被 Module uses 引用？否——
    //   uses 引用它即活；真正的死条目是 13 "dead/Unused")
    ClassBuilder cb;
    const ca::u16 uThis = cb.utf8("p/T");
    const ca::u16 uSup  = cb.utf8("java/lang/Object");
    const ca::u16 uMod  = cb.utf8("Module");
    const ca::u16 uMP   = cb.utf8("ModulePackages");
    const ca::u16 uMC   = cb.utf8("ModuleMainClass");
    const ca::u16 uName = cb.utf8("jbc.corpus");
    const ca::u16 uNameDup = cb.utf8("jbc.corpus");   // 等值重复 → 压缩去重
    const ca::u16 uPkg  = cb.utf8("svc");
    const ca::u16 uPkgDup = cb.utf8("svc");           // 等值重复 → 压缩去重
    const ca::u16 uMain = cb.utf8("app/Main");
    const ca::u16 uLog  = cb.utf8("java.logging");
    const ca::u16 uSvc  = cb.utf8("svc/Svc");
    const ca::u16 uDead = cb.utf8("dead/Unused");     // 死条目：无引用
    const ca::u16 cThis = cb.classRef(uThis);
    const ca::u16 cSup  = cb.classRef(uSup);
    const ca::u16 mName = cb.moduleRef(uName);
    const ca::u16 mLog  = cb.moduleRef(uLog);
    const ca::u16 pSvc  = cb.packageRef(uPkg);
    const ca::u16 cMain = cb.classRef(uMain);
    const ca::u16 cSvc  = cb.classRef(uSvc);
    cb.setThis(cThis).setSuper(cSup).setVersion(61);

    // Module：全段引用（requires 版本与模块名同串去重面）。
    const std::vector<std::tuple<ca::u16, ca::u16, ca::u16>> reqs{{mLog, 0x8020, uLog}};
    const std::vector<std::tuple<ca::u16, ca::u16, std::vector<ca::u16>>> exps{{pSvc, 0, {mLog}}};
    cb.addClassAttr(uMod, moduleBody(mName, 0, uNameDup, reqs, exps, exps, {cSvc}, {{cSvc, {cSvc}}}));
    // ModulePackages：包名与 exports 同串（跨属性去重面）。
    Bytes mpBody;
    jbc::test::detail::putU16(mpBody, 1);
    jbc::test::detail::putU16(mpBody, pSvc);
    cb.addClassAttr(uMP, mpBody);
    // ModuleMainClass：类条目与 uses 同条目（跨段共享）。
    Bytes mcBody;
    jbc::test::detail::putU16(mcBody, cMain);
    cb.addClassAttr(uMC, mcBody);
    const Bytes bytes = cb.build();

    auto sess = sessionOf(bytes);
    ASSERT_NE(sess, nullptr);
    ASSERT_TRUE(compactConstantPool(*sess).is_ok());
    const Bytes out = jbc::test::writeOk(sess->backing());

    // 重读 → typed 解码 → 全部引用文本等值（行为等值：压缩只换索引不改语义）。
    auto rr = readClass(ByteView{out});
    ASSERT_TRUE(rr.is_ok());
    auto rd = std::move(rr).unwrap();
    ClassFile& cf = *rd.file;
    const AttrSlot* modSlot = cf.findClassAttr("Module");
    ASSERT_NE(modSlot, nullptr);
    ASSERT_TRUE(cf.decodeAttr(const_cast<AttrSlot&>(*modSlot)).is_ok());
    const auto& m = static_cast<const ModuleAttr&>(*modSlot->body);
    EXPECT_EQ(moduleNameOf(cf, m.moduleNameCp), "jbc.corpus");
    EXPECT_EQ(cf.utf8(m.moduleVersionCp), "jbc.corpus");
    // Utf8 去重面：两条等值 "jbc.corpus"（版本串与重复驻留）压缩后同条目；
    // 模块名是 CP Module 条目（不同 tag），与 Utf8 侧永不等价、各归各。
    ca::u32 nameUtf8Count = 0;
    for (ca::u32 i = 1; i < cf.cp.size(); ++i) {
        if (cf.cp.valid(i) && cf.cp.isUtf8(i) && cf.cp.utf8Equals(i, "jbc.corpus")) {
            ++nameUtf8Count;
        }
    }
    EXPECT_EQ(nameUtf8Count, 1u) << "equal utf8 entries not deduped";
    ASSERT_EQ(m.requiresEntries.size(), 1u);
    EXPECT_EQ(moduleNameOf(cf, m.requiresEntries[0].moduleCp), "java.logging");
    EXPECT_EQ(cf.utf8(m.requiresEntries[0].versionCp), "java.logging");
    EXPECT_EQ(m.requiresEntries[0].flags, 0x8020u);
    ASSERT_EQ(m.exports.size(), 1u);
    EXPECT_EQ(packageNameOf(cf, m.exports[0].packageCp), "svc");
    ASSERT_EQ(m.exports[0].toCps.size(), 1u);
    EXPECT_EQ(moduleNameOf(cf, m.exports[0].toCps[0]), "java.logging");
    ASSERT_EQ(m.opens.size(), 1u);
    EXPECT_EQ(packageNameOf(cf, m.opens[0].packageCp), "svc");
    ASSERT_EQ(m.usesCps.size(), 1u);
    EXPECT_EQ(classNameOf(cf, m.usesCps[0]), "svc/Svc");
    ASSERT_EQ(m.provides.size(), 1u);
    EXPECT_EQ(classNameOf(cf, m.provides[0].serviceCp), "svc/Svc");
    ASSERT_EQ(m.provides[0].withCps.size(), 1u);
    EXPECT_EQ(classNameOf(cf, m.provides[0].withCps[0]), "svc/Svc");

    const AttrSlot* mpSlot = cf.findClassAttr("ModulePackages");
    ASSERT_NE(mpSlot, nullptr);
    ASSERT_TRUE(cf.decodeAttr(const_cast<AttrSlot&>(*mpSlot)).is_ok());
    const auto& mp = static_cast<const ModulePackagesAttr&>(*mpSlot->body);
    ASSERT_EQ(mp.packageCps.size(), 1u);
    EXPECT_EQ(packageNameOf(cf, mp.packageCps[0]), "svc");
    // 跨属性去重：ModulePackages 包条目与 exports 包条目同条目。
    EXPECT_EQ(mp.packageCps[0], m.exports[0].packageCp);

    const AttrSlot* mcSlot = cf.findClassAttr("ModuleMainClass");
    ASSERT_NE(mcSlot, nullptr);
    ASSERT_TRUE(cf.decodeAttr(const_cast<AttrSlot&>(*mcSlot)).is_ok());
    const auto& mc = static_cast<const ModuleMainClassAttr&>(*mcSlot->body);
    EXPECT_EQ(classNameOf(cf, mc.mainClassCp), "app/Main");

    // 死条目清除：压缩后池中不再有 "dead/Unused"。
    bool deadSurvivor = false;
    for (ca::u32 i = 1; i < cf.cp.size(); ++i) {
        if (cf.cp.valid(i) && cf.cp.isUtf8(i) && cf.cp.utf8Equals(i, "dead/Unused")) {
            deadSurvivor = true;
        }
    }
    EXPECT_FALSE(deadSurvivor) << "dead entry survived compaction";

    // 确定性：独立第二遍逐字节一致。
    auto sess2 = sessionOf(bytes);
    ASSERT_NE(sess2, nullptr);
    ASSERT_TRUE(compactConstantPool(*sess2).is_ok());
    EXPECT_EQ(jbc::test::writeOk(sess2->backing()), out) << "两次压缩不确定";
}

TEST(ModuleCompact, UnmodeledModuleHashesStillRejected)
{
    // ModuleHashes（JDK 内部件，非 JVMS 条目，不建模）：Raw 透传 + 压缩
    // 封闭集外显式拒绝，拒绝点名属性名（诚实性）。
    ClassBuilder cb;
    cb.setThis(cb.classRef(cb.utf8("p/T")));
    cb.setSuper(cb.classRef(cb.utf8("java/lang/Object")));
    cb.setVersion(61);
    Bytes body;
    jbc::test::detail::putU16(body, 1);   // 形状不实（建模面外，仅构造拒绝路径）
    jbc::test::detail::putU16(body, 1);
    cb.addClassAttr(cb.utf8("ModuleHashes"), body);
    const Bytes bytes = cb.build();

    auto sess = sessionOf(bytes);
    ASSERT_NE(sess, nullptr);
    const auto r = compactConstantPool(*sess);
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err().code, CompactError::AttrNotModeled);
    EXPECT_EQ(r.unwrap_err().attrName, "ModuleHashes");
}
