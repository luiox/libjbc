/// @file        struct_attrs_corpus_test.cpp
/// @brief       结构元数据属性语料门：四件（SourceFile/InnerClasses/
///              EnclosingMethod/Exceptions）+ Nest 族三件（NestHost/
///              NestMembers/PermittedSubclasses）+ Record + Module 族三件
///              （Module/ModulePackages/ModuleMainClass）。全量扫描 +
///              typed 解码 + 重编码整类字节恒等 + 分布计数。
///
/// 分布断言只在自产语料模式硬性生效（javac 产物保证四件齐备，含局部类/
/// 匿名类的 EnclosingMethod；Nest 族三件由独立 --release 17 小语料保证、
/// Record 由独立 --release 16 小语料保证、Module 由独立 --release 17 小语
/// 料保证——主语料 --release 8 产物是 v52，构造上不带这些面）；env 注入
/// （混淆产物）可能剥属性，分布是显式事实而非覆盖缺口（同款
/// 处理），恒等门两种模式一律生效。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "jbc/jbc.hpp"
#include "corpus_util.hpp"
#include "test_class_bytes.hpp"

using namespace jbc::core;
using jbc::test::Bytes;

namespace {

namespace fs = std::filesystem;

Bytes slurp(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return Bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

struct Counts
{
    ca::u32 sourceFile = 0;
    ca::u32 innerClasses = 0;
    ca::u32 enclosingMethod = 0;
    ca::u32 exceptions = 0;
    ca::u32 nestHost = 0;
    ca::u32 nestMembers = 0;
    ca::u32 permittedSubclasses = 0;
    ca::u32 recordAttrs = 0;         ///< Record 属性槽数
    ca::u32 recordComponents = 0;    ///< record 组件总数
    ca::u32 recordCompSignatures = 0; ///< 组件嵌套 Signature 槽数
    ca::u32 recordCompAnnos = 0;      ///< 组件嵌套 Runtime{V,I}Annotations 槽数
    ca::u32 moduleAttrs = 0;          ///< Module 属性槽数
    ca::u32 moduleRequires = 0;       ///< requires 指令行数
    ca::u32 moduleRequiresTransitive = 0; ///< 带 ACC_TRANSITIVE 的 requires 行
    ca::u32 moduleRequiresStatic = 0;     ///< 带 ACC_STATIC_PHASE 的 requires 行
    ca::u32 moduleExports = 0;        ///< exports 指令行数
    ca::u32 moduleExportsQualified = 0;   ///< 带 to 目标的 exports 行
    ca::u32 moduleOpens = 0;          ///< opens 指令行数
    ca::u32 moduleUses = 0;           ///< uses 条目数
    ca::u32 moduleProvides = 0;       ///< provides 指令行数
    ca::u32 modulePackages = 0;       ///< ModulePackages 属性槽数
    ca::u32 moduleMainClass = 0;      ///< ModuleMainClass 属性槽数
};

/// 槽名是否等于给定字面量。
bool slotNamed(const ClassFile& cf, const AttrSlot& slot, std::string_view name)
{
    return cf.cp.utf8Equals(slot.nameCp, name);
}

}   // namespace

TEST(StructAttrsCorpusGate, AllStructAttrsTypedRoundtripByteIdentity)
{
    const jbc::test::corpus::CorpusSource src = jbc::test::corpus::resolveCorpus();
    if (src.stage == jbc::test::corpus::Stage::None) {
        GTEST_SKIP() << jbc::test::corpus::skipReason(src);
    }
    const jbc::test::corpus::CorpusSource nest = jbc::test::corpus::resolveNestCorpus();
    const jbc::test::corpus::CorpusSource record = jbc::test::corpus::resolveRecordCorpus();
    const jbc::test::corpus::CorpusSource module = jbc::test::corpus::resolveModuleCorpus();
    std::vector<std::string> files = jbc::test::corpus::collectClassFiles(src.dir);
    if (nest.stage == jbc::test::corpus::Stage::SelfProduced) {
        const std::vector<std::string> nestFiles =
            jbc::test::corpus::collectClassFiles(nest.dir);
        files.insert(files.end(), nestFiles.begin(), nestFiles.end());
    }
    if (record.stage == jbc::test::corpus::Stage::SelfProduced) {
        const std::vector<std::string> recordFiles =
            jbc::test::corpus::collectClassFiles(record.dir);
        files.insert(files.end(), recordFiles.begin(), recordFiles.end());
    }
    if (module.stage == jbc::test::corpus::Stage::SelfProduced) {
        const std::vector<std::string> moduleFiles =
            jbc::test::corpus::collectClassFiles(module.dir);
        files.insert(files.end(), moduleFiles.begin(), moduleFiles.end());
    }
    if (files.empty()) {
        GTEST_SKIP() << jbc::test::corpus::skipReason(src);
    }

    Counts counts;
    ca::u32 filesChecked = 0;

    for (const std::string& path : files) {
        const Bytes bytes = slurp(fs::path(path));
        ASSERT_FALSE(bytes.empty()) << path;

        // 门一：整类未触碰写回逐字节不动。
        {
            auto cf = jbc::test::readOk(bytes);
            ASSERT_NE(cf, nullptr) << path;
            EXPECT_EQ(jbc::test::writeOk(*cf), bytes) << path;
        }

        // 门二：七属性逐槽 typed 解码 → 重编码整类字节恒等。
        // 七件均已接分派：分派解码 + mutate 置脏与公开解码 + makeNew 置 New
        // 两条路都过重编码面；此处保持两形混用以同时钉住公开直读入口。
        auto cf = jbc::test::readOk(bytes);
        ASSERT_NE(cf, nullptr) << path;
        bool touched = false;

        for (ca::usize i = 0; i < cf->attrs.size(); ++i) {
            AttrSlot& slot = cf->attrs[i];
            if (slotNamed(*cf, slot, "SourceFile")) {
                auto d = cf->decodeAttr(slot);
                ASSERT_TRUE(d.is_ok()) << path;
                EXPECT_EQ(d.unwrap()->kind(), AttrKind::SourceFile) << path;
                ASSERT_NE(slot.mutate(), nullptr) << path;
                touched = true;
                ++counts.sourceFile;
            }
            else if (slotNamed(*cf, slot, "InnerClasses")) {
                auto d = decodeInnerClassesBody(*cf, slot);
                ASSERT_TRUE(d.is_ok()) << path;
                slot = AttrSlot::makeNew(slot.nameCp, std::move(d).unwrap());
                touched = true;
                ++counts.innerClasses;
            }
            else if (slotNamed(*cf, slot, "EnclosingMethod")) {
                auto d = decodeEnclosingMethodBody(*cf, slot);
                ASSERT_TRUE(d.is_ok()) << path;
                slot = AttrSlot::makeNew(slot.nameCp, std::move(d).unwrap());
                touched = true;
                ++counts.enclosingMethod;
            }
            else if (slotNamed(*cf, slot, "NestHost")) {
                auto d = decodeNestHostBody(*cf, slot);
                ASSERT_TRUE(d.is_ok()) << path;
                slot = AttrSlot::makeNew(slot.nameCp, std::move(d).unwrap());
                touched = true;
                ++counts.nestHost;
            }
            else if (slotNamed(*cf, slot, "NestMembers")) {
                auto d = decodeNestMembersBody(*cf, slot);
                ASSERT_TRUE(d.is_ok()) << path;
                slot = AttrSlot::makeNew(slot.nameCp, std::move(d).unwrap());
                touched = true;
                ++counts.nestMembers;
            }
            else if (slotNamed(*cf, slot, "PermittedSubclasses")) {
                auto d = decodePermittedSubclassesBody(*cf, slot);
                ASSERT_TRUE(d.is_ok()) << path;
                slot = AttrSlot::makeNew(slot.nameCp, std::move(d).unwrap());
                touched = true;
                ++counts.permittedSubclasses;
            }
            else if (slotNamed(*cf, slot, "Record")) {
                // 门二之 Record：typed 直读（组件表 + 嵌套属性槽分布统计）→
                // makeNew 强制重编码。嵌套槽未触碰走 copy-through，字节恒等
                // 同时钉住「组件表重编码不动嵌套面」。
                auto d = decodeRecordBody(*cf, slot);
                ASSERT_TRUE(d.is_ok()) << path;
                {
                    auto body = std::move(d).unwrap();
                    const auto& rec = static_cast<const RecordAttr&>(*body);
                    for (const RecordAttr::Component& c : rec.components) {
                        ++counts.recordComponents;
                        for (const AttrSlot& nested : c.attrs) {
                            if (slotNamed(*cf, nested, "Signature")) {
                                ++counts.recordCompSignatures;
                            }
                            else if (slotNamed(*cf, nested, "RuntimeVisibleAnnotations")
                                     || slotNamed(*cf, nested,
                                                  "RuntimeInvisibleAnnotations")) {
                                ++counts.recordCompAnnos;
                            }
                        }
                    }
                    slot = AttrSlot::makeNew(slot.nameCp, std::move(body));
                }
                touched = true;
                ++counts.recordAttrs;
            }
            else if (slotNamed(*cf, slot, "Module")) {
                // 门二之 Module：typed 直读（五段指令行全分布统计，旗语位
                // 按 u16 原值判定 TRANSITIVE(0x20)/STATIC_PHASE(0x40)）→
                // makeNew 强制重编码。字节恒等钉住全段重编码无损。
                auto d = decodeModuleBody(*cf, slot);
                ASSERT_TRUE(d.is_ok()) << path;
                {
                    auto body = std::move(d).unwrap();
                    const auto& mod = static_cast<const ModuleAttr&>(*body);
                    counts.moduleRequires += static_cast<ca::u32>(mod.requiresEntries.size());
                    for (const ModuleAttr::Require& e : mod.requiresEntries) {
                        if ((e.flags & 0x0020u) != 0) {
                            ++counts.moduleRequiresTransitive;
                        }
                        if ((e.flags & 0x0040u) != 0) {
                            ++counts.moduleRequiresStatic;
                        }
                    }
                    counts.moduleExports += static_cast<ca::u32>(mod.exports.size());
                    for (const ModuleAttr::Export& e : mod.exports) {
                        if (!e.toCps.empty()) {
                            ++counts.moduleExportsQualified;
                        }
                    }
                    counts.moduleOpens += static_cast<ca::u32>(mod.opens.size());
                    counts.moduleUses += static_cast<ca::u32>(mod.usesCps.size());
                    counts.moduleProvides += static_cast<ca::u32>(mod.provides.size());
                    slot = AttrSlot::makeNew(slot.nameCp, std::move(body));
                }
                touched = true;
                ++counts.moduleAttrs;
            }
            else if (slotNamed(*cf, slot, "ModulePackages")) {
                auto d = decodeModulePackagesBody(*cf, slot);
                ASSERT_TRUE(d.is_ok()) << path;
                slot = AttrSlot::makeNew(slot.nameCp, std::move(d).unwrap());
                touched = true;
                ++counts.modulePackages;
            }
            else if (slotNamed(*cf, slot, "ModuleMainClass")) {
                auto d = decodeModuleMainClassBody(*cf, slot);
                ASSERT_TRUE(d.is_ok()) << path;
                slot = AttrSlot::makeNew(slot.nameCp, std::move(d).unwrap());
                touched = true;
                ++counts.moduleMainClass;
            }
        }
        for (MemberInfo& m : cf->methods) {
            for (AttrSlot& slot : m.attrs) {
                if (slotNamed(*cf, slot, "Exceptions")) {
                    auto d = decodeExceptionsBody(*cf, slot);
                    ASSERT_TRUE(d.is_ok()) << path;
                    slot = AttrSlot::makeNew(slot.nameCp, std::move(d).unwrap());
                    touched = true;
                    ++counts.exceptions;
                }
            }
        }
        if (touched) {
            EXPECT_EQ(jbc::test::writeOk(*cf), bytes)
                << "typed re-encode diverged: " << path;
        }
        ++filesChecked;
    }

    EXPECT_GT(filesChecked, 0u);

    // 分布断言：自产语料（javac 产物）保证四件齐备。局部类/匿名类样本
    // （VolInner 等）产出 EnclosingMethod；带 throws 方法产出 Exceptions。
    if (src.stage == jbc::test::corpus::Stage::SelfProduced) {
        EXPECT_GT(counts.sourceFile, 0u);
        EXPECT_GT(counts.innerClasses, 0u);
        EXPECT_GT(counts.enclosingMethod, 0u) << "corpus lost local/anonymous samples?";
        EXPECT_GT(counts.exceptions, 0u);
    }
    // Nest 族分布断言：自产 nest 语料（--release 17）保证三件齐备——
    // nestmate 对产出 NestHost+NestMembers，sealed 层级产出 PermittedSubclasses。
    // 主语料（--release 8）构造上不带这三件，硬断言只钉 nest 语料可用时。
    if (nest.stage == jbc::test::corpus::Stage::SelfProduced) {
        EXPECT_GT(counts.nestHost, 0u) << "nest corpus lost nestmate samples?";
        EXPECT_GT(counts.nestMembers, 0u) << "nest corpus lost nestmate samples?";
        EXPECT_GT(counts.permittedSubclasses, 0u) << "nest corpus lost sealed samples?";
    }
    // Record 分布断言：自产 record 语料（--release 16）保证属性与两个组件
    // 嵌套面（Signature/注解）齐备——泛型组件产出组件 Signature、RECORD_
    // COMPONENT 目标注解产出组件 RuntimeVisibleAnnotations。主语料（--release 8）
    // 构造上不带 Record，硬断言只钉 record 语料可用时；env
    // 注入模式分布只记录（实测注入语料 54 类携带）。
    if (record.stage == jbc::test::corpus::Stage::SelfProduced) {
        EXPECT_GT(counts.recordAttrs, 0u) << "record corpus lost record samples?";
        EXPECT_GT(counts.recordComponents, 0u) << "record corpus lost components?";
        EXPECT_GT(counts.recordCompSignatures, 0u)
            << "record corpus lost generic component samples?";
        EXPECT_GT(counts.recordCompAnnos, 0u)
            << "record corpus lost annotated component samples?";
    }
    // Module 分布断言：自产 module 语料（--release 17）保证 Module 属性与
    // 五段全形态齐备——transitive/static 旗语、to 限定导出、opens、uses、
    // provides 由样本指令面保证。ModulePackages/ModuleMainClass javac 直译
    // 模式实测不产（typed 面由 crafted 用例钉住），不设分布硬断言。
    if (module.stage == jbc::test::corpus::Stage::SelfProduced) {
        EXPECT_GT(counts.moduleAttrs, 0u) << "module corpus lost module-info samples?";
        EXPECT_GT(counts.moduleRequires, 0u) << "module corpus lost requires rows?";
        EXPECT_GT(counts.moduleRequiresTransitive, 0u)
            << "module corpus lost transitive flag samples?";
        EXPECT_GT(counts.moduleRequiresStatic, 0u)
            << "module corpus lost static-phase flag samples?";
        EXPECT_GT(counts.moduleExports, 0u) << "module corpus lost exports rows?";
        EXPECT_GT(counts.moduleExportsQualified, 0u)
            << "module corpus lost qualified-export samples?";
        EXPECT_GT(counts.moduleOpens, 0u) << "module corpus lost opens rows?";
        EXPECT_GT(counts.moduleUses, 0u) << "module corpus lost uses entries?";
        EXPECT_GT(counts.moduleProvides, 0u) << "module corpus lost provides rows?";
    }
    RecordProperty("sourceFile", std::to_string(counts.sourceFile));
    RecordProperty("innerClasses", std::to_string(counts.innerClasses));
    RecordProperty("enclosingMethod", std::to_string(counts.enclosingMethod));
    RecordProperty("exceptions", std::to_string(counts.exceptions));
    RecordProperty("nestHost", std::to_string(counts.nestHost));
    RecordProperty("nestMembers", std::to_string(counts.nestMembers));
    RecordProperty("permittedSubclasses", std::to_string(counts.permittedSubclasses));
    RecordProperty("recordAttrs", std::to_string(counts.recordAttrs));
    RecordProperty("recordComponents", std::to_string(counts.recordComponents));
    RecordProperty("recordCompSignatures", std::to_string(counts.recordCompSignatures));
    RecordProperty("recordCompAnnos", std::to_string(counts.recordCompAnnos));
    RecordProperty("moduleAttrs", std::to_string(counts.moduleAttrs));
    RecordProperty("moduleRequires", std::to_string(counts.moduleRequires));
    RecordProperty("moduleExports", std::to_string(counts.moduleExports));
    RecordProperty("moduleOpens", std::to_string(counts.moduleOpens));
    RecordProperty("moduleUses", std::to_string(counts.moduleUses));
    RecordProperty("moduleProvides", std::to_string(counts.moduleProvides));
    RecordProperty("modulePackages", std::to_string(counts.modulePackages));
    RecordProperty("moduleMainClass", std::to_string(counts.moduleMainClass));
}
