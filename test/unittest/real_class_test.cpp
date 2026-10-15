/// @file        real_class_test.cpp
/// @brief       真实 class 样本上的字节级往返验证（未触碰 / 全解码两条路径）。
///
/// 样本表随语料来源切换（F1 解析序）：env 模式取注入语料内确定性选取的
/// 代表类，仓内自产模式取 jbcx.corpus 的 8 个形态代表类（枚举/
/// lambda/泛型/接口默认/内部类/长方法/宽局部/异常）。两类样本走同一套断言：
/// 未触碰写回字节一致 + 全解码写回字节一致。
#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

#include "jbc/jbc.hpp"
#include "corpus_util.hpp"
#include "test_class_bytes.hpp"

using namespace jbc::core;
using jbc::test::Bytes;

namespace {

/// 仓内自产模式的样本：jbcx.corpus 形态代表类（相对自产根）。
const char* kCorpusSamples[] = {
    "jbcx/corpus/VolEnumShape.class",        // 枚举 + 常量体匿名类
    "jbcx/corpus/VolLambda.class",           // lambda indy + BootstrapMethods
    "jbcx/corpus/VolGeneric.class",          // 泛型 Signature + 桥方法
    "jbcx/corpus/VolIface.class",            // 接口默认方法 + 菱形消解
    "jbcx/corpus/VolInner.class",            // 内部类/匿名类 + access$ 合成
    "jbcx/corpus/VolLongMethod.class",       // 长方法（大 code 属性）
    "jbcx/corpus/VolWide.class",             // wide 前缀 + 大 maxLocals
    "jbcx/corpus/VolTry.class",              // 异常表全形态 + TWR
};

std::vector<std::string> sampleList(const jbc::test::corpus::CorpusSource& src)
{
    std::vector<std::string> out;
    if (src.stage == jbc::test::corpus::Stage::SelfProduced) {
        for (const char* s : kCorpusSamples) {
            out.push_back(s);
        }
    }
    else {
        // env 注入模式：注入语料内确定性选取（递归收集排序取前 8）。
        std::vector<std::string> files =
            jbc::test::corpus::collectClassFiles(src.dir);
        std::sort(files.begin(), files.end());
        const std::size_t take = files.size() < 8 ? files.size() : 8;
        for (std::size_t i = 0; i < take; ++i) {
            out.push_back(std::filesystem::path(files[i]).lexically_relative(src.dir).string());
        }
    }
    return out;
}

void checkRealClass(const std::string& path);

/// 按序号取样本跑真类校验；env 语料不足 8 个类时后续样本 skip。
void checkSample(const jbc::test::corpus::CorpusSource& src, std::size_t i)
{
    const std::vector<std::string> samples = sampleList(src);
    if (i >= samples.size() || samples[i].empty()) {
        GTEST_SKIP() << "env corpus smaller than 8 classes";
        return;
    }
    checkRealClass(src.dir + "/" + samples[i]);
}

Bytes slurp(const std::string& path, bool& found)
{
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) {
        found = false;
        return {};
    }
    found = true;
    return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void checkRealClass(const std::string& path)
{
    bool found = false;
    const Bytes bytes = slurp(path, found);
    if (!found) {
        GTEST_SKIP() << "sample not built: " << path;
        return;
    }
    ASSERT_FALSE(bytes.empty()) << path;

    auto r = readClass(ByteView{bytes});
    ASSERT_TRUE(r.is_ok()) << path << " read failed: " << to_string(r.unwrap_err());
    auto cf = std::move(r).unwrap().file;
    ASSERT_NE(cf, nullptr);

    // 1) 未触碰 → 字节完全相同
    auto w = writeClass(*cf);
    ASSERT_TRUE(w.is_ok()) << path << " write failed";
    EXPECT_EQ(std::move(w).unwrap(), bytes) << "roundtrip mismatch: " << path;

    // 2) 全解码但不修改 → 仍字节相同
    auto r2 = readClass(ByteView{bytes});
    ASSERT_TRUE(r2.is_ok());
    auto rr2 = std::move(r2).unwrap();
    ClassFile& f2 = *rr2.file;
    ASSERT_TRUE(decodeAllAttrs(f2).is_ok()) << path << " decodeAll failed";
    auto w2 = writeClass(f2);
    ASSERT_TRUE(w2.is_ok());
    EXPECT_EQ(std::move(w2).unwrap(), bytes) << "decode-then-write mismatch: " << path;
}

}   // namespace

TEST(RealClassTest, Sample1)
{
    const auto src = jbc::test::corpus::resolveCorpus();
    if (src.stage == jbc::test::corpus::Stage::None) {
        GTEST_SKIP() << jbc::test::corpus::skipReason(src);
    }
    checkSample(src, 0);
}
TEST(RealClassTest, Sample2)
{
    const auto src = jbc::test::corpus::resolveCorpus();
    if (src.stage == jbc::test::corpus::Stage::None) {
        GTEST_SKIP() << jbc::test::corpus::skipReason(src);
    }
    checkSample(src, 1);
}
TEST(RealClassTest, Sample3)
{
    const auto src = jbc::test::corpus::resolveCorpus();
    if (src.stage == jbc::test::corpus::Stage::None) {
        GTEST_SKIP() << jbc::test::corpus::skipReason(src);
    }
    checkSample(src, 2);
}
TEST(RealClassTest, Sample4)
{
    const auto src = jbc::test::corpus::resolveCorpus();
    if (src.stage == jbc::test::corpus::Stage::None) {
        GTEST_SKIP() << jbc::test::corpus::skipReason(src);
    }
    checkSample(src, 3);
}
TEST(RealClassTest, Sample5)
{
    const auto src = jbc::test::corpus::resolveCorpus();
    if (src.stage == jbc::test::corpus::Stage::None) {
        GTEST_SKIP() << jbc::test::corpus::skipReason(src);
    }
    checkSample(src, 4);
}
TEST(RealClassTest, Sample6)
{
    const auto src = jbc::test::corpus::resolveCorpus();
    if (src.stage == jbc::test::corpus::Stage::None) {
        GTEST_SKIP() << jbc::test::corpus::skipReason(src);
    }
    checkSample(src, 5);
}
TEST(RealClassTest, Sample7)
{
    const auto src = jbc::test::corpus::resolveCorpus();
    if (src.stage == jbc::test::corpus::Stage::None) {
        GTEST_SKIP() << jbc::test::corpus::skipReason(src);
    }
    checkSample(src, 6);
}
TEST(RealClassTest, Sample8)
{
    const auto src = jbc::test::corpus::resolveCorpus();
    if (src.stage == jbc::test::corpus::Stage::None) {
        GTEST_SKIP() << jbc::test::corpus::skipReason(src);
    }
    checkSample(src, 7);
}
