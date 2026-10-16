/// @file        corpus_compact_test.cpp
/// @brief       CP 显式压缩的语料扫——封闭集诚实性与确定性的行为级门（A8）。
///
/// 对语料逐类 compactConstantPool：
/// 1. 拒绝诚实性：每个 AttrNotModeled 拒绝点名的属性在原类上 decodeAttr
///    必须是 Raw 形（拒绝只落在真正未建模的属性上，不诬陷建模属性）；
///    MalformedAttr 在 javac 级语料上必须为零（命中即查格式知识）；
///    拒收面清零：封闭集收编完成后 javac 级语料两种来源模式均不得出现
///    AttrNotModeled；
/// 2. 确定性：同一原始字节两次独立压缩写出逐字节一致；
/// 3. 保真：产物重读 → 全解码 → 再写逐字节一致；类名/方法表字符串恒等；
///    产物尺寸不大于原类（池单调缩，u2 引用面等宽）。
#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "jbc/jbc.hpp"
#include "corpus_util.hpp"
#include "test_class_bytes.hpp"

using namespace jbc::core;
using namespace jbc::ir;
using Bytes = std::vector<ca::u8>;

namespace {

Bytes slurpCompact(const std::string& path, bool& found)
{
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) {
        found = false;
        return {};
    }
    found = true;
    return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// 失败点名的属性在原类上必须是 Raw 形：类级 → 字段 → 方法逐层找同名属性。
bool attrIsRawOnOriginal(ClassFile& cf, const std::string& name)
{
    const auto scan = [&cf, &name](std::vector<AttrSlot>& attrs) -> int {
        for (AttrSlot& a : attrs) {
            if (cf.utf8(a.nameCp) == name) {
                if (!cf.decodeAttr(a).is_ok()) {
                    return -1;
                }
                return a.body->kind() == AttrKind::Raw ? 1 : 0;
            }
        }
        return -1;   // 本层未命中
    };
    int r = scan(cf.attrs);
    if (r >= 0) {
        return r == 1;
    }
    for (MemberInfo& f : cf.fields) {
        r = scan(f.attrs);
        if (r >= 0) {
            return r == 1;
        }
    }
    for (MemberInfo& m : cf.methods) {
        r = scan(m.attrs);
        if (r >= 0) {
            return r == 1;
        }
    }
    return false;   // 点名属性不存在：视为不诚实
}

}   // namespace

TEST(CorpusCompact, CompactionScanHonestAndDeterministic)
{
    const auto src = jbc::test::corpus::resolveCorpus();
    if (src.stage == jbc::test::corpus::Stage::None) {
        GTEST_SKIP() << jbc::test::corpus::skipReason(src);
    }
    auto files = jbc::test::corpus::collectClassFiles(src.dir);
    // nest 语料（--release 17）并入压缩扫：NestHost/NestMembers/
    // PermittedSubclasses 进封闭集后，自产模式下它们的压缩行为也过门
    // （env 注入主语料可能携带 Nest 面，拒绝面重测经主语料生效）。
    const auto nest = jbc::test::corpus::resolveNestCorpus();
    if (nest.stage == jbc::test::corpus::Stage::SelfProduced) {
        const auto nestFiles = jbc::test::corpus::collectClassFiles(nest.dir);
        files.insert(files.end(), nestFiles.begin(), nestFiles.end());
    }
    // module 语料（--release 17）并入压缩扫：Module 属性进封闭集后，自产
    // 模式下 module-info 的全段压缩行为也过门（ModulePackages/MainClass
    // javac 不产，crafted 面由单测覆盖；module 语料各类均无名单外属性，
    // 构造上可达压缩成功路径）。
    const auto moduleC = jbc::test::corpus::resolveModuleCorpus();
    if (moduleC.stage == jbc::test::corpus::Stage::SelfProduced) {
        const auto moduleFiles = jbc::test::corpus::collectClassFiles(moduleC.dir);
        files.insert(files.end(), moduleFiles.begin(), moduleFiles.end());
    }
    // annot 语料（-parameters）并入压缩扫：注解族四件（ParameterAnnotations/
    // AnnotationDefault/MethodParameters/ConstantValue）进封闭集后，自产模式
    // 下参数名表/注解默认值/参数注解的压缩行为也过门（env 注入主语料的
    // MethodParameters 拒收面重测经主语料生效）。
    const auto annot = jbc::test::corpus::resolveAnnotCorpus();
    if (annot.stage == jbc::test::corpus::Stage::SelfProduced) {
        const auto annotFiles = jbc::test::corpus::collectClassFiles(annot.dir);
        files.insert(files.end(), annotFiles.begin(), annotFiles.end());
    }
    ASSERT_FALSE(files.empty());

    int filesWalked = 0;
    int compacted   = 0;
    int noChanged   = 0;
    int rejected    = 0;
    int malformed   = 0;
    int dishonest   = 0;
    std::map<std::string, int> rejectByName;   // 逐点名拒绝统计（报告用）

    for (const auto& path : files) {
        bool found = false;
        const Bytes bytes = slurpCompact(path, found);
        if (!found || bytes.empty()) {
            continue;
        }
        auto r = readClass(ByteView{bytes});
        if (r.is_err()) {
            ADD_FAILURE() << path << " read failed: " << jbc::core::to_string(r.unwrap_err());
            continue;
        }
        ++filesWalked;
        auto rd = std::move(r).unwrap();
        ClassFile& orig = *rd.file;
        ASSERT_TRUE(decodeAllAttrs(orig).is_ok()) << path << " original decodeAll failed";

        // 第一遍压缩。
        std::string thisName(orig.className(orig.thisClass));
        std::vector<std::string> mstrings;
        for (const MemberInfo& m : orig.methods) {
            mstrings.push_back(std::string(orig.nameOf(m)) + ":"
                               + std::string(orig.descOf(m)));
        }
        auto s1r = makeSession(std::move(rd).file);
        if (s1r.is_err()) {
            ADD_FAILURE() << path << " session failed";
            continue;
        }
        auto s1 = std::move(s1r).unwrap();
        const auto c1 = compactConstantPool(*s1);
        Bytes out1;
        if (c1.is_err()) {
            const CompactFailure f = c1.unwrap_err();
            ++rejected;
            if (f.code == CompactError::MalformedAttr) {
                ++malformed;
                ADD_FAILURE() << path << " MalformedAttr on: " << f.attrName;
                continue;
            }
            rejectByName[std::string(f.attrName)]++;
            std::fprintf(stderr, "[corpus-compact] reject path=%s code=%d name=%s\n",
                         path.c_str(), int(f.code), std::string(f.attrName).c_str());
            // 诚实性：拒绝点名的属性在原类上必须是 Raw 形。
            auto rOrig = readClass(ByteView{bytes});
            ASSERT_TRUE(rOrig.is_ok()) << path;
            auto rdOrig = std::move(rOrig).unwrap();
            ASSERT_TRUE(decodeAllAttrs(*rdOrig.file).is_ok()) << path;
            if (!attrIsRawOnOriginal(*rdOrig.file, std::string(f.attrName))) {
                ++dishonest;
                ADD_FAILURE() << path << " 拒绝点名的属性不是 Raw 形: " << f.attrName;
            }
            continue;
        }
        out1 = jbc::test::writeOk(s1->backing());
        ASSERT_FALSE(out1.empty()) << path;
        EXPECT_LE(out1.size(), bytes.size()) << path << " 压缩后不得变大";

        // 确定性：独立第二遍逐字节一致。
        auto s2r = makeSessionFromBytes(ByteView{bytes});
        ASSERT_TRUE(s2r.is_ok()) << path;
        auto s2 = std::move(s2r).unwrap();
        ASSERT_TRUE(compactConstantPool(*s2).is_ok()) << path;
        const Bytes out2 = jbc::test::writeOk(s2->backing());
        EXPECT_EQ(out2, out1) << path << " 两次压缩不确定";

        // 保真：重读 → 全解码 → 再写逐字节一致；类名/方法表字符串恒等。
        auto r2 = readClass(ByteView{out1});
        ASSERT_TRUE(r2.is_ok()) << path;
        auto rd2 = std::move(r2).unwrap();
        ASSERT_TRUE(decodeAllAttrs(*rd2.file).is_ok()) << path << " decodeAll failed";
        EXPECT_EQ(jbc::test::writeOk(*rd2.file), out1) << path << " roundtrip";
        EXPECT_EQ(rd2.file->className(rd2.file->thisClass), thisName) << path;
        ASSERT_EQ(rd2.file->methods.size(), mstrings.size()) << path;
        for (ca::usize i = 0; i < mstrings.size(); ++i) {
            const std::string got = std::string(rd2.file->nameOf(rd2.file->methods[i]))
                + ":" + std::string(rd2.file->descOf(rd2.file->methods[i]));
            EXPECT_EQ(got, mstrings[i]) << path << " method " << i;
        }
        if (out1.size() == bytes.size()) {
            ++noChanged;
        }
        else {
            ++compacted;
        }
    }

    std::fprintf(stderr,
                 "[corpus-compact] stage=%d files=%d compacted=%d unchanged=%d "
                 "rejected=%d malformed=%d dishonest=%d\n",
                 int(src.stage), filesWalked, compacted, noChanged, rejected,
                 malformed, dishonest);
    for (const auto& kv : rejectByName) {
        std::fprintf(stderr, "[corpus-compact] reject %s = %d\n",
                     kv.first.c_str(), kv.second);
    }
    EXPECT_EQ(dishonest, 0) << "拒绝面不诚实：建模属性被诬拒或点名属性失踪";
    EXPECT_EQ(malformed, 0) << "javac 级语料出现 MalformedAttr：压缩侧格式知识有误";
    // 拒收面清零：注解族四件（ParameterAnnotations/AnnotationDefault/
    // MethodParameters/ConstantValue）收编进封闭集后，javac 级语料构造上
    // 无名单外属性——批前自产 5（全是 AnnotationDefault）/env 注入 54（全是
    // MethodParameters）的拒收归零，两种模式同门生效。
    EXPECT_EQ(rejected, 0) << "压缩拒收面未清零（见上方 reject 明细）";
}
