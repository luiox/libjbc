/// @file        frame_test.cpp
/// @brief       帧引擎测试：合成深度 + 真实类 maxStack/maxLocals 自对拍。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "jbc/jbc.hpp"
#include "corpus_util.hpp"
#include "test_class_bytes.hpp"

using namespace jbc::core;
using jbc::analysis::FrameEngine;
using jbc::analysis::FrameSummary;
using jbc::analysis::ShapeResolver;
using jbc::ir::ClassSession;
using jbc::ir::makeSession;
using jbc::ir::promoteMethod;
using Bytes = std::vector<ca::u8>;
using jbc::test::AttrSpec;
using jbc::test::ClassBuilder;
using jbc::test::corpus::resolveCorpus;

namespace {

/// 真实语料 oracle（EXPECT_GT(methods, 1000) 量级护栏）的语料解析序：
/// env 注入 > 仓内自产（javac 现场编译，F1）。两路全缺席时
/// skip 而不是硬失败（javac/JDK 缺席的环境不该挂仓内门禁）。
std::vector<std::string> collectClassFiles(const std::string& mainRoot)
{
    namespace fs = std::filesystem;
    std::vector<fs::path> roots{fs::path(mainRoot),
                                fs::path("test_resource/libjbc/annotated")};
    std::vector<std::string> out;
    std::error_code ec;
    for (const fs::path& root : roots) {
        for (fs::recursive_directory_iterator it(root, ec), end; it != end && !ec;
             it.increment(ec)) {
            if (!ec && it->is_regular_file() && it->path().extension() == ".class") {
                out.push_back(it->path().string());
            }
        }
        ec.clear();
    }
    return out;
}

Bytes slurpFile(const std::string& p)
{
    std::ifstream in(p.c_str(), std::ios::binary);
    if (!in) {
        return {};
    }
    return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// 一个最小可变换方法：iconst_1; pop; return + LineNumberTable(0→1, 2→9)。
Bytes makeEditableClass(bool withUnknownNested)
{
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 codeIdx = cb.utf8("Code");
    const ca::u16 lntIdx  = cb.utf8("LineNumberTable");
    const ca::u16 weirdIdx = cb.utf8("WeirdCustomAttr");
    const ca::u16 initIdx = cb.utf8("m");
    const ca::u16 voidIdx = cb.utf8("()V");

    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setAccess(0x0021);
    cb.setVersion(52);

    const Bytes code = {0x04, 0x57, 0xb1};   // iconst_1; pop; return
    std::vector<AttrSpec> nested;
    nested.push_back(AttrSpec{lntIdx, {0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x09}});
    if (withUnknownNested) {
        nested.push_back(AttrSpec{weirdIdx, {0xDE, 0xAD}});
    }
    std::vector<AttrSpec> methodAttrs;
    methodAttrs.push_back(AttrSpec{codeIdx, jbc::test::codeContent(1, 0, code, {}, nested)});
    cb.addMethod(0x0009, initIdx, voidIdx, std::move(methodAttrs));
    return cb.build();
}

std::shared_ptr<ClassSession> sessionOf(const Bytes& bytes)
{
    auto cf = jbc::test::readOk(bytes);
    if (cf == nullptr) {
        return nullptr;
    }
    auto s = makeSession(std::move(cf));
    return s.is_ok() ? std::move(s).unwrap() : nullptr;
}

}   // namespace

TEST(FrameEngineTest, SimpleDepths)
{
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 codeIdx = cb.utf8("Code");
    const ca::u16 mIdx = cb.utf8("m");
    const ca::u16 voidIdx = cb.utf8("()V");
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setAccess(0x0021);
    cb.setVersion(52);
    // iconst_1; iconst_2; iadd; pop; lconst_0; pop2; return
    // → int 段最大深度 2；lconst_0 占 2 槽深度同为 2。
    const Bytes code = {0x04, 0x05, 0x60, 0x57, 0x0a, 0x58, 0xb1};
    std::vector<AttrSpec> methodAttrs;
    methodAttrs.push_back(AttrSpec{codeIdx, jbc::test::codeContent(2, 0, code)});
    cb.addMethod(0x0009, mIdx, voidIdx, std::move(methodAttrs));

    auto s = sessionOf(cb.build());
    auto p = promoteMethod(*s, 0);
    ShapeResolver res;
    FrameEngine fe(s->backing().cp, res);
    const FrameSummary sum = fe.compute(*p.unwrap());
    EXPECT_TRUE(sum.ok) << jbc::analysis::to_string(sum.fail);
    EXPECT_EQ(sum.maxStack, 2u);
}

TEST(FrameEngineRealClassTest, ComputedMaxesNeverExceedDeclared)
{
    const jbc::test::corpus::CorpusSource src = resolveCorpus();
    if (src.stage == jbc::test::corpus::Stage::None) {
        GTEST_SKIP() << jbc::test::corpus::skipReason(src);
    }
    const std::vector<std::string> files = collectClassFiles(src.dir);
    if (files.empty()) {
        GTEST_SKIP() << jbc::test::corpus::skipReason(src);
    }

    ca::usize methods = 0;
    ca::usize computed = 0;
    ca::usize stackEq = 0;
    ca::usize localsEq = 0;
    ca::usize failures = 0;
    ca::usize failByReason[5] = {};   // None/Underflow/BadLocal/BadCpRef/Wide
    for (const std::string& path : files) {
        const Bytes bytes = slurpFile(path);
        if (bytes.empty()) {
            continue;
        }
        auto cf = jbc::test::readOk(bytes);
        if (cf == nullptr) {
            continue;
        }
        auto s = makeSession(std::move(cf));
        if (s.is_err()) {
            continue;
        }
        auto sess = std::move(s).unwrap();
        for (ca::usize i = 0; i < sess->unit().methods.size(); ++i) {
            auto p = promoteMethod(*sess, i);
            if (p.is_err()) {
                continue;   // abstract/native：无 Code
            }
            ++methods;
            ShapeResolver res;
            FrameEngine fe(sess->backing().cp, res);
            const FrameSummary sum = fe.compute(*p.unwrap());
            if (!sum.ok) {
                ++failures;
                ++failByReason[static_cast<int>(sum.fail)];
                continue;
            }
            ++computed;
            EXPECT_LE(sum.maxStack, p.unwrap()->maxStack) << path;
            EXPECT_LE(sum.maxLocals, p.unwrap()->maxLocals) << path;
            if (sum.maxStack == p.unwrap()->maxStack) {
                ++stackEq;
            }
            if (sum.maxLocals == p.unwrap()->maxLocals) {
                ++localsEq;
            }
        }
    }
    EXPECT_GT(methods, 1000u);   // 语料量级护栏
    // 覆盖率与一致率记录在测试输出里（人体工学/质量评估用）。
    std::printf("[frame-oracle] methods=%zu computed=%zu failures=%zu "
                "stackEq=%zu localsEq=%zu | fail: none=%zu underflow=%zu badlocal=%zu "
                "badcp=%zu wide=%zu\n",
                methods, computed, failures, stackEq, localsEq, failByReason[0],
                failByReason[1], failByReason[2], failByReason[3], failByReason[4]);
    EXPECT_GT(computed, methods * 9 / 10);   // 至少 90% 方法可算
}

// ---------------------------------------------------------------------------
// 逐指令到达帧（对标旧栈 Analyzer::getFrames，值语义自持有）
// ---------------------------------------------------------------------------

namespace {

/// iconst_1; ifeq L2; aconst_null; goto L3; L2: new X; L3: pop; return
/// —— 汇合点 L3 两条路径分别是 Null 与 Ref(new)，合并应为 Ref。
Bytes makeMergeClass()
{
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 codeIdx = cb.utf8("Code");
    const ca::u16 mIdx = cb.utf8("m");
    const ca::u16 voidIdx = cb.utf8("()V");
    const ca::u16 objIdx = cb.classRef(cb.utf8("java/lang/Object"));
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setAccess(0x0021);
    cb.setVersion(52);
    // pc: 0 iconst_1, 1 ifeq +6(→pc9), 4 aconst_null, 5 goto +5(→pc13? 用重算)
    // 精确布局: 0:iconst_1(1) 1:ifeq(3,→10) 4:aconst_null(1) 5:goto(3,→13)
    // 8:(填跳目标2) 需要两路等长——用 nop 对齐: 8: nop; 9: nop;
    // 10: new(3) 13: pop 14: return
    // ifeq at1 target 10: off=10-1=9; goto at5 target 13: off=13-5=8
    const Bytes code = {
        0x04,                   // 0 iconst_1
        0x99, 0x00, 0x09,       // 1 ifeq -> 10
        0x01,                   // 4 aconst_null
        0xa7, 0x00, 0x08,       // 5 goto -> 13
        0x00,                   // 8 nop (死代码)
        0x00,                   // 9 nop (死代码)
        0xbb, 0x00, 0x00,       // 10 new (cp 占位——用 objIdx 重写)
        0x57,                   // 13 pop
        0xb1,                   // 14 return
    };
    Bytes c = code;
    c[11] = static_cast<ca::u8>(objIdx >> 8);
    c[12] = static_cast<ca::u8>(objIdx & 0xFF);
    std::vector<AttrSpec> methodAttrs;
    methodAttrs.push_back(AttrSpec{codeIdx, jbc::test::codeContent(1, 0, c)});
    cb.addMethod(0x0009, mIdx, voidIdx, std::move(methodAttrs));
    return cb.build();
}

}   // namespace

TEST(FrameEngineTest, PerInsnFramesMergeAndDeadCode)
{
    auto s = sessionOf(makeMergeClass());
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());

    jbc::analysis::ShapeResolver res;
    jbc::analysis::FrameEngine fe(s->backing().cp, res);
    std::vector<jbc::analysis::FrameAt> frames;
    const auto sum = fe.compute(*p.unwrap(), frames);
    ASSERT_TRUE(sum.ok) << jbc::analysis::to_string(sum.fail);

    // 布局: 0 iconst_1 / 1 ifeq / 4 aconst_null / 5 goto / 8-9 nop(死) /
    // 10 new / 13 pop / 14 return
    ASSERT_EQ(frames.size(), 9u);
    EXPECT_TRUE(frames[0].reachable);
    ASSERT_EQ(frames[0].stack.size(), 0u);
    EXPECT_EQ(frames[1].stack.size(), 1u);
    EXPECT_EQ(frames[1].stack[0].k, jbc::analysis::VKind::Int);
    // 死代码：goto 跳过的两条 nop（下标 4/5）不可达。
    EXPECT_FALSE(frames[4].reachable);
    EXPECT_FALSE(frames[5].reachable);
    // 汇合点下标 7（pop）：Null 路与 Ref(new) 路合并为 Ref。
    ASSERT_TRUE(frames[7].reachable);
    ASSERT_EQ(frames[7].stack.size(), 1u);
    EXPECT_EQ(frames[7].stack[0].k, jbc::analysis::VKind::Ref);
}

TEST(FrameEngineTest, PerInsnFramesOwnedAfterSessionDeath)
{
    // 结果自持有（旧栈 Analyzer 结果借用 Analyzer 寿命——六制之⑥）：
    // 会话销毁后帧数据仍可读。
    std::vector<jbc::analysis::FrameAt> frames;
    {
        auto s = sessionOf(makeMergeClass());
        ASSERT_NE(s, nullptr);
        auto p = promoteMethod(*s, 0);
        ASSERT_TRUE(p.is_ok());
        jbc::analysis::ShapeResolver res;
        jbc::analysis::FrameEngine fe(s->backing().cp, res);
        ASSERT_TRUE(fe.compute(*p.unwrap(), frames).ok);
    }
    ASSERT_EQ(frames.size(), 9u);
    EXPECT_TRUE(frames[0].reachable);
    EXPECT_EQ(frames[1].stack[0].k, jbc::analysis::VKind::Int);
}

TEST(FrameEngineRealClassTest, PerInsnFramesWithinDeclaredMaxes)
{
    const jbc::test::corpus::CorpusSource src = resolveCorpus();
    if (src.stage == jbc::test::corpus::Stage::None) {
        GTEST_SKIP() << jbc::test::corpus::skipReason(src);
    }
    const std::vector<std::string> files = collectClassFiles(src.dir);
    if (files.empty()) {
        GTEST_SKIP() << jbc::test::corpus::skipReason(src);
    }
    ca::usize methods = 0;
    ca::usize withFrames = 0;
    ca::usize unreachableInsns = 0;
    for (const std::string& path : files) {
        const Bytes bytes = slurpFile(path);
        if (bytes.empty()) {
            continue;
        }
        auto s = sessionOf(bytes);
        if (s == nullptr) {
            continue;
        }
        for (ca::usize i = 0; i < s->unit().methods.size(); ++i) {
            auto p = promoteMethod(*s, i);
            if (p.is_err()) {
                continue;
            }
            ++methods;
            jbc::analysis::FrameEngine fe(s->backing().cp);   // 默认 resolver
            std::vector<jbc::analysis::FrameAt> frames;
            if (!fe.compute(*p.unwrap(), frames).ok) {
                continue;
            }
            ++withFrames;
            ASSERT_EQ(frames.size(), p.unwrap()->dom.size());
            for (const auto& f : frames) {
                if (!f.reachable) {
                    ++unreachableInsns;
                    continue;
                }
                ASSERT_LE(f.stack.size(), p.unwrap()->maxStack) << path;
            }
        }
    }
    EXPECT_GT(methods, 1000u);
    std::printf("[frames-oracle] methods=%zu withFrames=%zu unreachableInsns=%zu\n",
                methods, withFrames, unreachableInsns);
    EXPECT_GT(withFrames, methods * 9 / 10);
}
