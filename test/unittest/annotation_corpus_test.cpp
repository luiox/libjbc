/// @file        annotation_corpus_test.cpp
/// @brief       注解族属性语料门：ParameterAnnotations（JVMS 4.7.18/4.7.19）/
///              AnnotationDefault（4.7.22）/ MethodParameters（4.7.24）/
///              ConstantValue（4.7.2）。扫描 + typed 解码 + 重编码整类字节
///              恒等 + 分布计数。
///
/// 分布断言只在自产语料模式硬性生效（annot 小语料 --release 8 -g -parameters
/// 保证参数名表、注解类型默认值与参数注解双保留齐备；主语料 --release 8 基
/// 础旗语不产 MethodParameters）；ConstantValue 在本门只记录不硬断言（annot
/// 样本不带常量字段，其分布由主语料门与压缩面覆盖）。env 注入（混淆产物）
/// 可能剥属性，分布是显式事实而非覆盖缺口（同款处理），恒等门两种模
/// 式一律生效。
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
    ca::u32 paramAnnosVisible = 0;    ///< RuntimeVisibleParameterAnnotations 槽数
    ca::u32 paramAnnosInvisible = 0;  ///< RuntimeInvisibleParameterAnnotations 槽数
    ca::u32 paramAnnoGroups = 0;      ///< 参数注解组总数（参数维内层表）
    ca::u32 annotationDefault = 0;    ///< AnnotationDefault 槽数
    ca::u32 methodParameters = 0;     ///< MethodParameters 槽数
    ca::u32 methodParameterRows = 0;  ///< 参数名表行总数
    ca::u32 constantValue = 0;        ///< ConstantValue 槽数
};

/// 槽名是否等于给定字面量。
bool slotNamed(const ClassFile& cf, const AttrSlot& slot, std::string_view name)
{
    return cf.cp.utf8Equals(slot.nameCp, name);
}

}   // namespace

TEST(AnnotationCorpusGate, AllAnnotAttrsTypedRoundtripByteIdentity)
{
    // annot 小语料独立于主语料来源（javac 现场编译，javac 缺席整体 skip）。
    const jbc::test::corpus::CorpusSource annot = jbc::test::corpus::resolveAnnotCorpus();
    if (annot.stage == jbc::test::corpus::Stage::None) {
        GTEST_SKIP() << "annot corpus unavailable (javac absent)";
    }
    const std::vector<std::string> files =
        jbc::test::corpus::collectClassFiles(annot.dir);
    ASSERT_FALSE(files.empty());

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

        // 门二：注解族逐槽 typed 解码 → 重编码整类字节恒等。分派解码 +
        // mutate 置脏与公开直读 + makeNew 置 New 两条路都过重编码面
        // （struct_attrs_corpus_test 同款混用）。
        auto cf = jbc::test::readOk(bytes);
        ASSERT_NE(cf, nullptr) << path;
        bool touched = false;

        const auto walkSlots = [&](std::vector<AttrSlot>& slots) {
            for (AttrSlot& slot : slots) {
                if (slotNamed(*cf, slot, "RuntimeVisibleParameterAnnotations")
                    || slotNamed(*cf, slot, "RuntimeInvisibleParameterAnnotations")) {
                    auto d = decodeParameterAnnotationsBody(*cf, slot);
                    ASSERT_TRUE(d.is_ok()) << path;
                    {
                        auto body = std::move(d).unwrap();
                        const auto& pa =
                            static_cast<const ParameterAnnotationsAttr&>(*body);
                        counts.paramAnnoGroups +=
                            static_cast<ca::u32>(pa.parameters.size());
                        slot = AttrSlot::makeNew(slot.nameCp, std::move(body));
                    }
                    touched = true;
                    if (slotNamed(*cf, slot, "RuntimeVisibleParameterAnnotations")) {
                        ++counts.paramAnnosVisible;
                    }
                    else {
                        ++counts.paramAnnosInvisible;
                    }
                }
                else if (slotNamed(*cf, slot, "AnnotationDefault")) {
                    auto d = decodeAnnotationDefaultBody(*cf, slot);
                    ASSERT_TRUE(d.is_ok()) << path;
                    slot = AttrSlot::makeNew(slot.nameCp, std::move(d).unwrap());
                    touched = true;
                    ++counts.annotationDefault;
                }
                else if (slotNamed(*cf, slot, "MethodParameters")) {
                    auto d = decodeMethodParametersBody(*cf, slot);
                    ASSERT_TRUE(d.is_ok()) << path;
                    {
                        auto body = std::move(d).unwrap();
                        const auto& mp = static_cast<const MethodParametersAttr&>(*body);
                        counts.methodParameterRows +=
                            static_cast<ca::u32>(mp.rows.size());
                        slot = AttrSlot::makeNew(slot.nameCp, std::move(body));
                    }
                    touched = true;
                    ++counts.methodParameters;
                }
                else if (slotNamed(*cf, slot, "ConstantValue")) {
                    auto d = decodeConstantValueBody(*cf, slot);
                    ASSERT_TRUE(d.is_ok()) << path;
                    slot = AttrSlot::makeNew(slot.nameCp, std::move(d).unwrap());
                    touched = true;
                    ++counts.constantValue;
                }
            }
        };
        walkSlots(cf->attrs);
        for (MemberInfo& f : cf->fields) {
            walkSlots(f.attrs);
        }
        for (MemberInfo& m : cf->methods) {
            walkSlots(m.attrs);
        }
        if (touched) {
            EXPECT_EQ(jbc::test::writeOk(*cf), bytes)
                << "typed re-encode diverged: " << path;
        }
        ++filesChecked;
    }

    EXPECT_GT(filesChecked, 0u);

    // 分布断言：annot 小语料为自产模式（javac 产物保证四件齐备）——参数注解
    // 双保留由带注解参数样本保证、AnnotationDefault 由注解类型默认值方法保
    // 证、MethodParameters 由 -parameters 旗语保证。
    EXPECT_GT(counts.paramAnnosVisible, 0u)
        << "annot corpus lost visible parameter-annotation samples?";
    EXPECT_GT(counts.paramAnnosInvisible, 0u)
        << "annot corpus lost invisible parameter-annotation samples?";
    EXPECT_GT(counts.paramAnnoGroups, 0u) << "annot corpus lost parameter groups?";
    EXPECT_GT(counts.annotationDefault, 0u)
        << "annot corpus lost annotation-default samples?";
    EXPECT_GT(counts.methodParameters, 0u)
        << "annot corpus lost method-parameters samples (-parameters flag)?";
    EXPECT_GT(counts.methodParameterRows, 0u)
        << "annot corpus lost parameter-name rows?";

    RecordProperty("paramAnnosVisible", std::to_string(counts.paramAnnosVisible));
    RecordProperty("paramAnnosInvisible", std::to_string(counts.paramAnnosInvisible));
    RecordProperty("annotationDefault", std::to_string(counts.annotationDefault));
    RecordProperty("methodParameters", std::to_string(counts.methodParameters));
    RecordProperty("methodParameterRows", std::to_string(counts.methodParameterRows));
    RecordProperty("constantValue", std::to_string(counts.constantValue));
}
