/// @file        strict_frame_test.cpp
/// @brief       Strict 帧计算域测试（design.md §10.1 C1 裁决的验收面）。
///
/// 三部分：
/// 1. 合成样本——策略三态 API、new+\<init\> 翻转、同/异 offset Uninit 合并、
///    跨类合并经 resolver、jsr/ret 子程序往返（C2 起建模；专项面在
///    subroutine_test.cpp）、描述符精确注入。冲突/不可算
///    样本断言 Err + failAt 指向正确指令。
/// 2. 异常边精确传播（J5/C3）——Strict 出口边播种 handler 入口（抛出点
///    locals + catch_type 精确类）、any 播 Throwable、死 try 的 handler
///    不可达、多抛出点合并（同类/resolver/冲突+failAt）；BestEffort 对照
///    逐项保持基础播种。
/// 3. javac oracle——本机 javac 编译带 StackMapTable 的样本（--release 8），
///    解码 SMT 条目展开成全帧，与 Strict 域算出的分支目标帧与 handler
///    入口帧逐槽比对（验收口径：可算者与 javac 一致）。javac 缺席时 skip。
///    语料与展开/比对机制在 smt_oracle_util.hpp（与 J6 生成器 oracle 共用）。
/// 4. 语料全量两态 oracle——自产语料 + golden 语料类逐方法两态统计：
///    可算 → 与 SMT 展开公共前缀逐槽一致（可指派性口径）；不可算 → 显式
///    Err 带指令定位；断言零伪帧。stub resolver 用语料自收层级 + JDK 种子，
///    知识缺席显式拒绝（不伪造 Object 兜底帧）。
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "jbc/jbc.hpp"
#include "corpus_util.hpp"
#include "smt_oracle_util.hpp"
#include "test_class_bytes.hpp"

using namespace jbc::core;
using jbc::analysis::FrameAt;
using jbc::analysis::FrameEngine;
using jbc::analysis::FramePolicy;
using jbc::analysis::FrameSummary;
using jbc::analysis::ITypeResolver;
using jbc::analysis::MethodSig;
using jbc::analysis::TypeMergeError;
using jbc::analysis::VKind;
using jbc::analysis::VType;
using jbc::ir::ClassSession;
using jbc::ir::CodeUnit;
using jbc::ir::makeSession;
using jbc::ir::promoteMethod;
using Bytes = std::vector<ca::u8>;
using jbc::test::AttrSpec;
using jbc::test::ClassBuilder;
using jbc::test::StubResolver;
using jbc::test::FullFrame;
using jbc::test::entryLocalsOf;
using jbc::test::expandSmt;
using jbc::test::findMethodSmt;
using jbc::test::fromSmtTag;
using jbc::test::headOf;
using jbc::test::indexAtPc;
using jbc::test::sameSlot;
using jbc::test::sessionOf;
using jbc::test::slotsText;
using jbc::test::trimmedSize;

namespace fs = std::filesystem;

namespace {

/// oracle 语料根（相对运行目录；J6 生成器 oracle 用独立根，语料同源）。
fs::path oracleRoot()
{
    return fs::path("test") / "libjbc_strict_oracle";
}

/// 单方法类骨架（p/T extends Object，版本 52）。
ClassBuilder baseClass()
{
    ClassBuilder cb;
    const ca::u16 thisIdx  = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setAccess(0x0021);
    cb.setVersion(52);
    return cb;
}

/// iconst_1; pop; return —— 策略面的最小载体。
std::shared_ptr<ClassSession> trivialClass()
{
    ClassBuilder cb = baseClass();
    const ca::u16 codeIdx = cb.utf8("Code");
    const ca::u16 mIdx    = cb.utf8("m");
    const ca::u16 voidIdx = cb.utf8("()V");
    const Bytes code = {0x04, 0x57, 0xb1};   // iconst_1; pop; return
    std::vector<AttrSpec> ma;
    ma.push_back(AttrSpec{codeIdx, jbc::test::codeContent(1, 0, code)});
    cb.addMethod(0x0009, mIdx, voidIdx, std::move(ma));
    return sessionOf(cb.build());
}

void put2(Bytes& b, ca::u16 v)
{
    b.push_back(static_cast<ca::u8>(v >> 8));
    b.push_back(static_cast<ca::u8>(v & 0xFF));
}

}   // namespace

// ---------------------------------------------------------------------------
// 策略三态 API 面（§10.1.3）
// ---------------------------------------------------------------------------

TEST(StrictFrameTest, StrictWithoutResolverFailsLoud)
{
    auto s = trivialClass();
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());

    FrameEngine fe(s->backing().cp, FramePolicy::Strict);
    const FrameSummary sum = fe.compute(*p.unwrap());
    EXPECT_FALSE(sum.ok);
    EXPECT_EQ(sum.fail, FrameSummary::Fail::NoResolver);
    EXPECT_EQ(sum.failAt, jbc::ir::kNoInsn);
}

TEST(StrictFrameTest, PreserveComputeRefusesReadSideIsSeparate)
{
    // J7 起 Preserve 不再是占位：读侧实现在 smt_read.hpp 的独立入口
    // （读属性不是数据流）。compute 对 Preserve 恒显式拒绝——语义路由不
    // 误导，本测试锁死该拒绝面。
    auto s = trivialClass();
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());

    FrameEngine fe(s->backing().cp, FramePolicy::Preserve);
    const FrameSummary sum = fe.compute(*p.unwrap());
    EXPECT_FALSE(sum.ok);
    EXPECT_EQ(sum.fail, FrameSummary::Fail::NotSupported);

    StubResolver res;
    FrameEngine fe2(s->backing().cp, res, FramePolicy::Preserve);
    const FrameSummary sum2 = fe2.compute(*p.unwrap());
    EXPECT_FALSE(sum2.ok);
    EXPECT_EQ(sum2.fail, FrameSummary::Fail::NotSupported);
}

TEST(StrictFrameTest, BestEffortIsStillDefault)
{
    auto s = trivialClass();
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());

    FrameEngine fe(s->backing().cp);   // 缺省 = BestEffort
    const FrameSummary sum = fe.compute(*p.unwrap());
    EXPECT_TRUE(sum.ok);
}

// ---------------------------------------------------------------------------
// new + <init> 翻转（§10.1.4）
// ---------------------------------------------------------------------------

namespace {

/// 0 new X / 3 dup / 4 invokespecial X.<init>:V / 7 astore_1 / 8 aload_1 /
/// 9 getfield T.f:Ljava/lang/String; / 12 astore_2 / 13 aconst_null /
/// 14 astore_3 / 15 return
std::shared_ptr<ClassSession> newInitClass()
{
    ClassBuilder cb = baseClass();
    const ca::u16 xCls  = cb.classRef(cb.utf8("p/X"));
    const ca::u16 xInit = cb.methodref(xCls, cb.nameAndType(cb.utf8("<init>"), cb.utf8("()V")));
    const ca::u16 fRef  = cb.fieldref(cb.classRef(cb.utf8("p/T")),
                                      cb.nameAndType(cb.utf8("f"), cb.utf8("Ljava/lang/String;")));
    const ca::u16 codeIdx = cb.utf8("Code");
    const ca::u16 mIdx    = cb.utf8("m");
    const ca::u16 voidIdx = cb.utf8("()V");

    Bytes code;
    code.push_back(0xbb);              // 0 new
    put2(code, xCls);
    code.push_back(0x59);              // 3 dup
    code.push_back(0xb7);              // 4 invokespecial
    put2(code, xInit);
    code.push_back(0x4c);              // 7 astore_1
    code.push_back(0x2b);              // 8 aload_1
    code.push_back(0xb4);              // 9 getfield
    put2(code, fRef);
    code.push_back(0x4d);              // 12 astore_2
    code.push_back(0x01);              // 13 aconst_null
    code.push_back(0x4e);              // 14 astore_3
    code.push_back(0xb1);              // 15 return

    std::vector<AttrSpec> ma;
    ma.push_back(AttrSpec{codeIdx, jbc::test::codeContent(4, 4, code)});
    cb.addMethod(0x0009, mIdx, voidIdx, std::move(ma));
    return sessionOf(cb.build());
}

}   // namespace

TEST(StrictFrameTest, NewPushesUninitializedAndInitFlips)
{
    auto s = newInitClass();
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());

    StubResolver res;
    FrameEngine fe(s->backing().cp, res, FramePolicy::Strict);
    std::vector<FrameAt> frames;
    const FrameSummary sum =
        fe.compute(*p.unwrap(), MethodSig{false, "()V", "m", "p/T"}, frames);
    ASSERT_TRUE(sum.ok) << jbc::analysis::to_string(sum.fail);
    ASSERT_EQ(frames.size(), p.unwrap()->dom.size());

    // new 处压入的两份 Uninitialized 都携带该 new 的 pc（offset 0）。
    const ca::usize atCall = indexAtPc(*p.unwrap(), 4);
    ASSERT_NE(atCall, static_cast<ca::usize>(-1));
    ASSERT_EQ(frames[atCall].stack.size(), 2u);
    EXPECT_EQ(frames[atCall].stack[0].k, VKind::Uninitialized);
    EXPECT_EQ(frames[atCall].stack[0].uninitOffset(), 0u);
    EXPECT_EQ(frames[atCall].stack[1].k, VKind::Uninitialized);
    EXPECT_EQ(frames[atCall].stack[1].uninitOffset(), 0u);

    // invokespecial <init> 后：栈上残留份翻转为精确类。
    const ca::usize afterCall = indexAtPc(*p.unwrap(), 7);
    ASSERT_NE(afterCall, static_cast<ca::usize>(-1));
    ASSERT_EQ(frames[afterCall].stack.size(), 1u);
    EXPECT_EQ(frames[afterCall].stack[0].k, VKind::Ref);
    EXPECT_EQ(frames[afterCall].stack[0].name, "p/X");

    // astore/aload 穿透已存精确值。
    const ca::usize atLoad = indexAtPc(*p.unwrap(), 8);
    ASSERT_NE(atLoad, static_cast<ca::usize>(-1));
    ASSERT_GE(frames[atLoad].locals.size(), 2u);
    EXPECT_EQ(frames[atLoad].locals[1].name, "p/X");

    // getfield 描述符精确注入。
    const ca::usize atStore2 = indexAtPc(*p.unwrap(), 12);
    ASSERT_NE(atStore2, static_cast<ca::usize>(-1));
    ASSERT_EQ(frames[atStore2].stack.size(), 1u);
    EXPECT_EQ(frames[atStore2].stack[0].name, "java/lang/String");

    // aconst_null 的 Null 原样入槽（Strict 不做 store 形状归一）。
    const ca::usize atRet = indexAtPc(*p.unwrap(), 15);
    ASSERT_NE(atRet, static_cast<ca::usize>(-1));
    ASSERT_GE(frames[atRet].locals.size(), 4u);
    EXPECT_EQ(frames[atRet].locals[2].name, "java/lang/String");
    EXPECT_EQ(frames[atRet].locals[3].k, VKind::Null);
}

TEST(StrictFrameTest, BestEffortSameClassStaysShapeOnly)
{
    auto s = newInitClass();
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());

    FrameEngine fe(s->backing().cp);   // BestEffort 缺省档
    std::vector<FrameAt> frames;
    const FrameSummary sum = fe.compute(*p.unwrap(), frames);
    ASSERT_TRUE(sum.ok);
    const ca::usize afterCall = indexAtPc(*p.unwrap(), 7);
    ASSERT_EQ(frames[afterCall].stack.size(), 1u);
    EXPECT_EQ(frames[afterCall].stack[0].k, VKind::Ref);
    EXPECT_TRUE(frames[afterCall].stack[0].name.empty());   // 无精确名（现状行为）
    const ca::usize atRet = indexAtPc(*p.unwrap(), 15);
    ASSERT_GE(frames[atRet].locals.size(), 4u);
    EXPECT_EQ(frames[atRet].locals[3].k, VKind::Ref);       // astore 的 Null 归一（现状行为）
}

// ---------------------------------------------------------------------------
// Uninitialized 合并（§10.1.4：同 offset 保持，异 offset Strict 冲突）
// ---------------------------------------------------------------------------

namespace {

/// 0 new X / 3 dup / 4 astore_1 / 5 iconst_1 / 6 ifeq→13 / 9 aload_1 /
/// 10 goto→16 / 13 aload_1 / 14-15 nop / 16 invokespecial X.<init>:V /
/// 19 return —— 合并点两路的栈与 local1 都是同一 new（offset 0）的
/// Uninitialized（同 offset 合并保持，翻转同时更新栈与局部变量）。
std::shared_ptr<ClassSession> sameOffsetMergeClass()
{
    ClassBuilder cb = baseClass();
    const ca::u16 xCls  = cb.classRef(cb.utf8("p/X"));
    const ca::u16 xInit = cb.methodref(xCls, cb.nameAndType(cb.utf8("<init>"), cb.utf8("()V")));
    const ca::u16 codeIdx = cb.utf8("Code");
    const ca::u16 mIdx    = cb.utf8("m");
    const ca::u16 voidIdx = cb.utf8("()V");

    Bytes code;
    code.push_back(0xbb);              // 0 new
    put2(code, xCls);
    code.push_back(0x59);              // 3 dup
    code.push_back(0x4c);              // 4 astore_1
    code.push_back(0x04);              // 5 iconst_1
    code.push_back(0x99);              // 6 ifeq -> 13
    put2(code, 7);
    code.push_back(0x2b);              // 9 aload_1
    code.push_back(0xa7);              // 10 goto -> 16
    put2(code, 6);
    code.push_back(0x2b);              // 13 aload_1
    code.push_back(0x00);              // 14 nop（落空垫）
    code.push_back(0x00);              // 15 nop
    code.push_back(0xb7);              // 16 invokespecial
    put2(code, xInit);
    code.push_back(0xb1);              // 19 return

    std::vector<AttrSpec> ma;
    ma.push_back(AttrSpec{codeIdx, jbc::test::codeContent(2, 2, code)});
    cb.addMethod(0x0009, mIdx, voidIdx, std::move(ma));
    return sessionOf(cb.build());
}

/// 0 iconst_1 / 1 ifeq→10 / 4 new A / 7 goto→13 / 10 new B / 13 astore_1 /
/// 14 return —— 合并点是两个不同 new 的 Uninitialized（冲突样本）。
std::shared_ptr<ClassSession> uninitConflictClass(const char* aName, const char* bName)
{
    ClassBuilder cb = baseClass();
    const ca::u16 aCls = cb.classRef(cb.utf8(aName));
    const ca::u16 bCls = cb.classRef(cb.utf8(bName));
    const ca::u16 codeIdx = cb.utf8("Code");
    const ca::u16 mIdx    = cb.utf8("m");
    const ca::u16 voidIdx = cb.utf8("()V");

    Bytes code;
    code.push_back(0x04);              // 0 iconst_1
    code.push_back(0x99);              // 1 ifeq -> 10
    put2(code, 9);
    code.push_back(0xbb);              // 4 new A
    put2(code, aCls);
    code.push_back(0xa7);              // 7 goto -> 13
    put2(code, 6);
    code.push_back(0xbb);              // 10 new B
    put2(code, bCls);
    code.push_back(0x4c);              // 13 astore_1（合并点）
    code.push_back(0xb1);              // 14 return

    std::vector<AttrSpec> ma;
    ma.push_back(AttrSpec{codeIdx, jbc::test::codeContent(1, 2, code)});
    cb.addMethod(0x0009, mIdx, voidIdx, std::move(ma));
    return sessionOf(cb.build());
}

/// 0 iconst_1 / 1 ifeq→10 / 4 getstatic A.f:LA; / 7 goto→13 / 10 getstatic B.f:LB; /
/// 13 astore_1 / 14 return —— 合并点两路是不同类的已初始化引用。
std::shared_ptr<ClassSession> refBranchMergeClass()
{
    ClassBuilder cb = baseClass();
    const ca::u16 aCls = cb.classRef(cb.utf8("p/A"));
    const ca::u16 bCls = cb.classRef(cb.utf8("p/B"));
    const ca::u16 aRef = cb.fieldref(aCls, cb.nameAndType(cb.utf8("f"), cb.utf8("Lp/A;")));
    const ca::u16 bRef = cb.fieldref(bCls, cb.nameAndType(cb.utf8("f"), cb.utf8("Lp/B;")));
    const ca::u16 codeIdx = cb.utf8("Code");
    const ca::u16 mIdx    = cb.utf8("m");
    const ca::u16 voidIdx = cb.utf8("()V");

    Bytes code;
    code.push_back(0x04);              // 0 iconst_1
    code.push_back(0x99);              // 1 ifeq -> 10
    put2(code, 9);
    code.push_back(0xb2);              // 4 getstatic A.f
    put2(code, aRef);
    code.push_back(0xa7);              // 7 goto -> 13
    put2(code, 6);
    code.push_back(0xb2);              // 10 getstatic B.f
    put2(code, bRef);
    code.push_back(0x4c);              // 13 astore_1（合并点）
    code.push_back(0xb1);              // 14 return

    std::vector<AttrSpec> ma;
    ma.push_back(AttrSpec{codeIdx, jbc::test::codeContent(1, 2, code)});
    cb.addMethod(0x0009, mIdx, voidIdx, std::move(ma));
    return sessionOf(cb.build());
}

}   // namespace

TEST(StrictFrameTest, SameOffsetUninitMergeKeeps)
{
    auto s = sameOffsetMergeClass();
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());

    StubResolver res;
    FrameEngine fe(s->backing().cp, res, FramePolicy::Strict);
    std::vector<FrameAt> frames;
    const FrameSummary sum = fe.compute(*p.unwrap(), frames);
    ASSERT_TRUE(sum.ok) << jbc::analysis::to_string(sum.fail);

    const ca::usize atMerge = indexAtPc(*p.unwrap(), 16);
    ASSERT_NE(atMerge, static_cast<ca::usize>(-1));
    ASSERT_EQ(frames[atMerge].stack.size(), 2u);
    EXPECT_EQ(frames[atMerge].stack[0].k, VKind::Uninitialized);
    EXPECT_EQ(frames[atMerge].stack[0].uninitOffset(), 0u);   // 同 offset：保持
    EXPECT_EQ(frames[atMerge].stack[1].k, VKind::Uninitialized);
    ASSERT_GE(frames[atMerge].locals.size(), 2u);
    EXPECT_EQ(frames[atMerge].locals[1].k, VKind::Uninitialized);
    EXPECT_EQ(frames[atMerge].locals[1].uninitOffset(), 0u);

    const ca::usize after = indexAtPc(*p.unwrap(), 19);
    ASSERT_EQ(frames[after].stack.size(), 1u);
    EXPECT_EQ(frames[after].stack[0].name, "p/X");            // 合并后翻转仍成立
    ASSERT_GE(frames[after].locals.size(), 2u);
    EXPECT_EQ(frames[after].locals[1].name, "p/X");           // local 里的同值同步翻转
}

TEST(StrictFrameTest, DiffOffsetUninitMergeStrictRejectsAtMergePoint)
{
    auto s = uninitConflictClass("p/A", "p/B");
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());

    StubResolver res;
    FrameEngine fe(s->backing().cp, res, FramePolicy::Strict);
    const FrameSummary sum = fe.compute(*p.unwrap());
    EXPECT_FALSE(sum.ok);
    EXPECT_EQ(sum.fail, FrameSummary::Fail::TypeConflict);
    const ca::usize atMerge = indexAtPc(*p.unwrap(), 13);
    ASSERT_NE(atMerge, static_cast<ca::usize>(-1));
    EXPECT_EQ(sum.failAt, p.unwrap()->dom.order()[atMerge]);   // 定位到合并点指令
}

TEST(StrictFrameTest, DiffOffsetUninitMergeBestEffortUnchanged)
{
    auto s = uninitConflictClass("p/A", "p/B");
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());

    FrameEngine fe(s->backing().cp);   // BestEffort：现行降级路径不变
    const FrameSummary sum = fe.compute(*p.unwrap());
    EXPECT_TRUE(sum.ok);
}

// ---------------------------------------------------------------------------
// 跨类合并经 resolver（§10.1.1/§10.1.3）
// ---------------------------------------------------------------------------

TEST(StrictFrameTest, BranchMergeResolvesCommonParent)
{
    auto s = refBranchMergeClass();
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());

    StubResolver res;
    res.parent["p/A"] = "p/M";
    res.parent["p/B"] = "p/M";
    res.parent["p/M"] = "java/lang/Object";
    FrameEngine fe(s->backing().cp, res, FramePolicy::Strict);
    std::vector<FrameAt> frames;
    const FrameSummary sum = fe.compute(*p.unwrap(), frames);
    ASSERT_TRUE(sum.ok) << jbc::analysis::to_string(sum.fail);
    EXPECT_GT(res.calls, 0);

    const ca::usize atMerge = indexAtPc(*p.unwrap(), 13);
    ASSERT_NE(atMerge, static_cast<ca::usize>(-1));
    ASSERT_EQ(frames[atMerge].stack.size(), 1u);
    EXPECT_EQ(frames[atMerge].stack[0].name, "p/M");   // 父链求解命中
}

TEST(StrictFrameTest, ResolverFailureIsExplicitAtMergePoint)
{
    auto s = refBranchMergeClass();
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());

    StubResolver res;
    res.failCrossClass = true;
    FrameEngine fe(s->backing().cp, res, FramePolicy::Strict);
    const FrameSummary sum = fe.compute(*p.unwrap());
    EXPECT_FALSE(sum.ok);
    EXPECT_EQ(sum.fail, FrameSummary::Fail::TypeConflict);
    const ca::usize atMerge = indexAtPc(*p.unwrap(), 13);
    ASSERT_NE(atMerge, static_cast<ca::usize>(-1));
    EXPECT_EQ(sum.failAt, p.unwrap()->dom.order()[atMerge]);
}

// ---------------------------------------------------------------------------
// jsr/ret：Strict 域建模子程序数据流（C2；专项覆盖在 subroutine_test.cpp）
// ---------------------------------------------------------------------------

namespace {

/// 0 jsr→5 / 3 nop / 4 return / 5 astore_2 / 6 ret 2
std::shared_ptr<ClassSession> jsrClass()
{
    ClassBuilder cb = baseClass();
    const ca::u16 codeIdx = cb.utf8("Code");
    const ca::u16 mIdx    = cb.utf8("m");
    const ca::u16 voidIdx = cb.utf8("()V");

    Bytes code;
    code.push_back(0xa8);              // 0 jsr -> 5
    put2(code, 5);
    code.push_back(0x00);              // 3 nop
    code.push_back(0xb1);              // 4 return
    code.push_back(0x4d);              // 5 astore_2
    code.push_back(0xa9);              // 6 ret 2
    code.push_back(0x02);

    std::vector<AttrSpec> ma;
    ma.push_back(AttrSpec{codeIdx, jbc::test::codeContent(2, 3, code)});
    cb.addMethod(0x0009, mIdx, voidIdx, std::move(ma));
    return sessionOf(cb.build());
}

}   // namespace

TEST(StrictFrameTest, JsrRetStrictComputesSingleCallerRoundtrip)
{
    // C2 起行为变化（曾为 Subroutine 显式拒绝）：jsr 压 ReturnAddress{0}，
    // astore 存入 local2，ret 跳回调用方 jsr 的顺序后继（pc3）。最小往返
    // 钉在主 strict 测试面，语义细节归 subroutine_test。
    auto s = jsrClass();
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());

    StubResolver res;
    FrameEngine fe(s->backing().cp, res, FramePolicy::Strict);
    std::vector<FrameAt> frames;
    const FrameSummary sum = fe.compute(*p.unwrap(), frames);
    ASSERT_TRUE(sum.ok) << jbc::analysis::to_string(sum.fail);
    EXPECT_TRUE(sum.hasSubroutine);
    EXPECT_EQ(sum.degraded, 0u);   // Strict 建模：ret 不再计 degraded

    // 子程序入口（pc5）栈顶 = 返回地址，载荷 = 调用方 jsr 的 pc。
    const ca::usize atEntry = indexAtPc(*p.unwrap(), 5);
    ASSERT_NE(atEntry, static_cast<ca::usize>(-1));
    ASSERT_EQ(frames[atEntry].stack.size(), 1u);
    EXPECT_EQ(frames[atEntry].stack[0].k, VKind::ReturnAddress);
    EXPECT_EQ(frames[atEntry].stack[0].jsrPc(), 0u);

    // ret（pc6）前：返回地址已在 local2。
    const ca::usize atRet = indexAtPc(*p.unwrap(), 6);
    ASSERT_NE(atRet, static_cast<ca::usize>(-1));
    ASSERT_GE(frames[atRet].locals.size(), 3u);
    EXPECT_EQ(frames[atRet].locals[2].k, VKind::ReturnAddress);
    EXPECT_EQ(frames[atRet].locals[2].jsrPc(), 0u);

    // 返回点（pc3，jsr 顺序后继）可达：栈空（返回地址已被 astore 消费）、
    // 局部槽保留子程序出口态。
    const ca::usize atSite = indexAtPc(*p.unwrap(), 3);
    ASSERT_NE(atSite, static_cast<ca::usize>(-1));
    EXPECT_TRUE(frames[atSite].reachable);
    EXPECT_EQ(frames[atSite].stack.size(), 0u);
    ASSERT_GE(frames[atSite].locals.size(), 3u);
    EXPECT_EQ(frames[atSite].locals[2].k, VKind::ReturnAddress);
}

TEST(StrictFrameTest, JsrRetBestEffortUnchanged)
{
    auto s = jsrClass();
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());

    FrameEngine fe(s->backing().cp);
    const FrameSummary sum = fe.compute(*p.unwrap());
    EXPECT_TRUE(sum.ok);
    EXPECT_TRUE(sum.hasSubroutine);
    EXPECT_GT(sum.degraded, 0u);   // BestEffort：legacy 计数不变（零漂移）
}

// ---------------------------------------------------------------------------
// 描述符驱动的精确注入：参数播种 / Null×Ref 合并 / 构造器 UninitThis
// ---------------------------------------------------------------------------

namespace {

/// static m(String,int)：0 iload_1 / 1 ifeq→8 / 4 aload_0 / 5 goto→8 /
/// 8 aconst_null / 9 astore_2 / 10 return —— 参数 String 与 null 在合并点汇合。
std::shared_ptr<ClassSession> paramMergeClass()
{
    ClassBuilder cb = baseClass();
    const ca::u16 codeIdx = cb.utf8("Code");
    const ca::u16 mIdx    = cb.utf8("m");
    const ca::u16 sigDesc = cb.utf8("(Ljava/lang/String;I)V");

    Bytes code;
    code.push_back(0x1a);              // 0 iload_1
    code.push_back(0x99);              // 1 ifeq -> 8
    put2(code, 7);
    code.push_back(0x2a);              // 4 aload_0
    code.push_back(0xa7);              // 5 goto -> 9
    put2(code, 4);
    code.push_back(0x01);              // 8 aconst_null（分支臂自带栈项）
    code.push_back(0x4d);              // 9 astore_2（合并点：[String] × [Null]）
    code.push_back(0xb1);              // 10 return

    std::vector<AttrSpec> ma;
    ma.push_back(AttrSpec{codeIdx, jbc::test::codeContent(2, 3, code)});
    cb.addMethod(0x0009, mIdx, sigDesc, std::move(ma));
    return sessionOf(cb.build());
}

/// p/T.<init>:()V：aload_0 / invokespecial Object.<init>:()V / return。
std::shared_ptr<ClassSession> ctorClass()
{
    ClassBuilder cb = baseClass();
    const ca::u16 objInit = cb.methodref(cb.classRef(cb.utf8("java/lang/Object")),
                                         cb.nameAndType(cb.utf8("<init>"), cb.utf8("()V")));
    const ca::u16 codeIdx = cb.utf8("Code");
    const ca::u16 initIdx = cb.utf8("<init>");

    Bytes code;
    code.push_back(0x2a);              // 0 aload_0
    code.push_back(0xb7);              // 1 invokespecial
    put2(code, objInit);
    code.push_back(0xb1);              // 4 return

    std::vector<AttrSpec> ma;
    ma.push_back(AttrSpec{codeIdx, jbc::test::codeContent(1, 1, code)});
    cb.addMethod(0x0001, initIdx, cb.utf8("()V"), std::move(ma));
    return sessionOf(cb.build());
}

}   // namespace

TEST(StrictFrameTest, ParameterSeededExactAndNullAbsorbedByRef)
{
    auto s = paramMergeClass();
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());

    StubResolver res;
    FrameEngine fe(s->backing().cp, res, FramePolicy::Strict);
    std::vector<FrameAt> frames;
    const FrameSummary sum =
        fe.compute(*p.unwrap(), MethodSig{true, "(Ljava/lang/String;I)V", "m", ""}, frames);
    ASSERT_TRUE(sum.ok) << jbc::analysis::to_string(sum.fail);

    // 参数槽携带描述符精确类名。
    const ca::usize atHead = indexAtPc(*p.unwrap(), 0);
    ASSERT_NE(atHead, static_cast<ca::usize>(-1));
    ASSERT_GE(frames[atHead].locals.size(), 2u);
    EXPECT_EQ(frames[atHead].locals[0].name, "java/lang/String");
    EXPECT_EQ(frames[atHead].locals[1].k, VKind::Int);

    // 合并点（双臂同高，javac 可产出形态）：Ref{String} × Null →
    // Ref{String}（null 是万能引用底；与 Top 支配规则正交，两格同规）。
    const ca::usize atMerge = indexAtPc(*p.unwrap(), 9);
    ASSERT_NE(atMerge, static_cast<ca::usize>(-1));
    ASSERT_EQ(frames[atMerge].stack.size(), 1u);
    EXPECT_EQ(frames[atMerge].stack[0].name, "java/lang/String");
}

TEST(StrictFrameTest, CtorThisIsUninitThisUntilSuperCall)
{
    auto s = ctorClass();
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());

    StubResolver res;
    FrameEngine fe(s->backing().cp, res, FramePolicy::Strict);
    std::vector<FrameAt> frames;
    const FrameSummary sum =
        fe.compute(*p.unwrap(), MethodSig{false, "()V", "<init>", "p/T"}, frames);
    ASSERT_TRUE(sum.ok) << jbc::analysis::to_string(sum.fail);

    // 超调用前：this 槽 = UninitThis。
    const ca::usize atSuper = indexAtPc(*p.unwrap(), 1);
    ASSERT_NE(atSuper, static_cast<ca::usize>(-1));
    ASSERT_GE(frames[atSuper].locals.size(), 1u);
    EXPECT_EQ(frames[atSuper].locals[0].k, VKind::UninitThis);

    // 超调用后：翻转为本类精确名。
    const ca::usize atRet = indexAtPc(*p.unwrap(), 4);
    ASSERT_NE(atRet, static_cast<ca::usize>(-1));
    ASSERT_GE(frames[atRet].locals.size(), 1u);
    EXPECT_EQ(frames[atRet].locals[0].k, VKind::Ref);
    EXPECT_EQ(frames[atRet].locals[0].name, "p/T");

    // BestEffort 对照：this 槽始终是形状 Ref（现状行为，无 UninitThis）。
    FrameEngine fe2(s->backing().cp);
    std::vector<FrameAt> frames2;
    const FrameSummary sum2 =
        fe2.compute(*p.unwrap(), MethodSig{false, "()V", "<init>", "p/T"}, frames2);
    ASSERT_TRUE(sum2.ok);
    ASSERT_GE(frames2[atSuper].locals.size(), 1u);
    EXPECT_EQ(frames2[atSuper].locals[0].k, VKind::Ref);
    EXPECT_TRUE(frames2[atSuper].locals[0].name.empty());
}

// ---------------------------------------------------------------------------
// 异常边精确传播（J5/C3）：Strict 出口边播种 handler 入口，BestEffort 原样
// ---------------------------------------------------------------------------

namespace {

/// 异常边样本的公共 CP 面（T.g/T.f/X.a/X.b/Exception/Throwable 一次性
/// 备齐；个别样本用不到的条目留在池里无副作用）。
struct ExcClassCp
{
    ca::u16 gRef = 0;
    ca::u16 fRef = 0;
    ca::u16 aRef = 0;
    ca::u16 bRef = 0;
    ca::u16 excCls = 0;
    ca::u16 thrCls = 0;
};

/// 在给定 builder 上分配公共 CP 面；先调本函数再拼 code，索引才可回填。
ExcClassCp excCpOf(ClassBuilder& cb)
{
    const ca::u16 tCls = cb.classRef(cb.utf8("p/T"));
    const ca::u16 xCls = cb.classRef(cb.utf8("p/X"));
    ExcClassCp cp;
    cp.gRef = cb.fieldref(tCls, cb.nameAndType(cb.utf8("g"), cb.utf8("Ljava/lang/Object;")));
    cp.fRef = cb.fieldref(tCls, cb.nameAndType(cb.utf8("f"), cb.utf8("Ljava/lang/String;")));
    cp.aRef = cb.fieldref(xCls, cb.nameAndType(cb.utf8("a"), cb.utf8("Lp/A;")));
    cp.bRef = cb.fieldref(xCls, cb.nameAndType(cb.utf8("b"), cb.utf8("Lp/B;")));
    cp.excCls = cb.classRef(cb.utf8("java/lang/Exception"));
    cp.thrCls = cb.classRef(cb.utf8("java/lang/Throwable"));
    return cp;
}

/// 收尾：静态 ()V + Code（含异常表）→ 会话。
std::shared_ptr<ClassSession> excClassOf(ClassBuilder& cb, const Bytes& code,
                                         ca::u16 maxStack, ca::u16 maxLocals,
                                         std::vector<jbc::test::ExcSpec> exc)
{
    const ca::u16 codeIdx = cb.utf8("Code");
    const ca::u16 mIdx    = cb.utf8("m");
    const ca::u16 voidIdx = cb.utf8("()V");
    std::vector<AttrSpec> ma;
    ma.push_back(AttrSpec{codeIdx, jbc::test::codeContent(maxStack, maxLocals, code, exc)});
    cb.addMethod(0x0009, mIdx, voidIdx, std::move(ma));
    return sessionOf(cb.build());
}

void putRef(Bytes& b, ca::u16 cpIdx)
{
    b.push_back(0xb2);   // getstatic
    put2(b, cpIdx);
}

}   // namespace

/// 多抛出点 → 同一 handler：入口帧 = 各抛出点 in-state locals 逐槽
/// verifier 合并（单边赋值槽支配为 Top——§10.4.8 裁决口径；全边已赋值
/// 槽具体保持）+ [catch_type 精确类]。BestEffort 对照保持基础播种（空
/// locals + 万能 Ref）。
TEST(StrictFrameExcTest, HandlerEntryCarriesThrowPointLocals)
{
    ClassBuilder cb = baseClass();
    const ExcClassCp cp = excCpOf(cb);

    // 0 iconst_1 / 1 istore_2（先于 try 的全边已赋值槽）/ 2 getstatic T.g /
    // 5 pop / 6 getstatic T.g / 9 astore_0 / 10 iconst_1 / 11 istore_1 /
    // 12 getstatic T.f / 15 pop / 16 return / 17 astore_2（handler，顺序伪
    // 边被截断——只能经异常边进入）/ 18 return
    Bytes code;
    code.push_back(0x04);              // 0 iconst_1
    code.push_back(0x3d);              // 1 istore_2
    putRef(code, cp.gRef);             // 2 getstatic T.g（抛点：locals [T,T,Int]）
    code.push_back(0x57);              // 5 pop
    putRef(code, cp.gRef);             // 6 getstatic T.g
    code.push_back(0x4b);              // 9 astore_0
    code.push_back(0x04);              // 10 iconst_1
    code.push_back(0x3c);              // 11 istore_1
    putRef(code, cp.fRef);             // 12 getstatic T.f（抛点：[Ref{Object},Int,Int]）
    code.push_back(0x57);              // 15 pop
    code.push_back(0xb1);              // 16 return
    code.push_back(0x4d);              // 17 astore_2（handler）
    code.push_back(0xb1);              // 18 return

    auto s = excClassOf(cb, code, 1, 3, {jbc::test::ExcSpec{0, 16, 17, cp.excCls}});
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());

    StubResolver res;
    FrameEngine fe(s->backing().cp, res, FramePolicy::Strict);
    std::vector<FrameAt> frames;
    const FrameSummary sum =
        fe.compute(*p.unwrap(), MethodSig{true, "()V", "m", ""}, frames);
    ASSERT_TRUE(sum.ok) << jbc::analysis::to_string(sum.fail);

    const ca::usize atHandler = indexAtPc(*p.unwrap(), 17);
    ASSERT_NE(atHandler, static_cast<ca::usize>(-1));
    ASSERT_TRUE(frames[atHandler].reachable);
    // 入口 locals：local0/local1 只在后段抛点被赋值（首抛点在存入之前）→
    // 单边赋值槽按 verifier 口径支配为 Top；local2 全边已赋值 → Int 保持。
    ASSERT_GE(frames[atHandler].locals.size(), 3u);
    EXPECT_EQ(frames[atHandler].locals[0].k, VKind::Top);
    EXPECT_EQ(frames[atHandler].locals[1].k, VKind::Top);
    EXPECT_EQ(frames[atHandler].locals[2].k, VKind::Int);
    // 入口栈 = catch_type 精确类（不再是万能 Ref）。
    ASSERT_EQ(frames[atHandler].stack.size(), 1u);
    EXPECT_EQ(frames[atHandler].stack[0].k, VKind::Ref);
    EXPECT_EQ(frames[atHandler].stack[0].name, "java/lang/Exception");

    // BestEffort 对照：handler 入口仍是基础播种——空 locals + 万能 Ref。
    FrameEngine fe2(s->backing().cp);
    std::vector<FrameAt> frames2;
    const FrameSummary sum2 = fe2.compute(*p.unwrap(), frames2);
    ASSERT_TRUE(sum2.ok);
    ASSERT_TRUE(frames2[atHandler].reachable);
    EXPECT_EQ(frames2[atHandler].locals.size(), 0u);
    ASSERT_EQ(frames2[atHandler].stack.size(), 1u);
    EXPECT_EQ(frames2[atHandler].stack[0].k, VKind::Ref);
    EXPECT_TRUE(frames2[atHandler].stack[0].name.empty());
}

/// catch_type=0（any/finally 汇聚 handler）播 java/lang/Throwable。
TEST(StrictFrameExcTest, AnyHandlerSeedsThrowable)
{
    ClassBuilder cb = baseClass();
    const ExcClassCp cp = excCpOf(cb);

    // 0 getstatic T.g / 3 pop / 4 return / 5 astore_0（any handler）/ 6 return
    Bytes code;
    putRef(code, cp.gRef);
    code.push_back(0x57);              // 3 pop
    code.push_back(0xb1);              // 4 return
    code.push_back(0x4b);              // 5 astore_0（handler）
    code.push_back(0xb1);              // 6 return

    auto s = excClassOf(cb, code, 1, 1, {jbc::test::ExcSpec{0, 4, 5, 0}});
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());

    StubResolver res;
    FrameEngine fe(s->backing().cp, res, FramePolicy::Strict);
    std::vector<FrameAt> frames;
    const FrameSummary sum = fe.compute(*p.unwrap(), frames);
    ASSERT_TRUE(sum.ok) << jbc::analysis::to_string(sum.fail);

    const ca::usize atHandler = indexAtPc(*p.unwrap(), 5);
    ASSERT_NE(atHandler, static_cast<ca::usize>(-1));
    ASSERT_TRUE(frames[atHandler].reachable);
    ASSERT_EQ(frames[atHandler].stack.size(), 1u);
    EXPECT_EQ(frames[atHandler].stack[0].k, VKind::Ref);
    EXPECT_EQ(frames[atHandler].stack[0].name, "java/lang/Throwable");

    // BestEffort 对照：catch_type 0 → 万能 Ref 且 cls 诊断位为 0。
    FrameEngine fe2(s->backing().cp);
    std::vector<FrameAt> frames2;
    ASSERT_TRUE(fe2.compute(*p.unwrap(), frames2).ok);
    ASSERT_EQ(frames2[atHandler].stack.size(), 1u);
    EXPECT_EQ(frames2[atHandler].stack[0].k, VKind::Ref);
    EXPECT_EQ(frames2[atHandler].stack[0].cls, 0u);
    EXPECT_TRUE(frames2[atHandler].stack[0].name.empty());
}

/// 死 try 范围（范围内指令全部不可达）→ handler 无异常边并入 → Strict 下
/// 不可达；BestEffort 无条件播种保持可达（零漂移）。
TEST(StrictFrameExcTest, DeadTryHandlerUnreachableInStrict)
{
    ClassBuilder cb = baseClass();
    const ExcClassCp cp = excCpOf(cb);

    // 0 return / 1 getstatic T.g（死抛点）/ 4 pop / 5 return /
    // 6 astore_0（handler）/ 7 return
    Bytes code;
    code.push_back(0xb1);              // 0 return
    putRef(code, cp.gRef);
    code.push_back(0x57);              // 4 pop
    code.push_back(0xb1);              // 5 return
    code.push_back(0x4b);              // 6 astore_0（handler）
    code.push_back(0xb1);              // 7 return

    auto s = excClassOf(cb, code, 1, 1, {jbc::test::ExcSpec{1, 5, 6, cp.excCls}});
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());

    StubResolver res;
    FrameEngine fe(s->backing().cp, res, FramePolicy::Strict);
    std::vector<FrameAt> frames;
    const FrameSummary sum = fe.compute(*p.unwrap(), frames);
    ASSERT_TRUE(sum.ok) << jbc::analysis::to_string(sum.fail);
    const ca::usize atHandler = indexAtPc(*p.unwrap(), 6);
    ASSERT_NE(atHandler, static_cast<ca::usize>(-1));
    EXPECT_FALSE(frames[atHandler].reachable);   // 死 try 的 handler 不可达

    // BestEffort 对照：无条件播种 → handler 恒可达（现状行为不变）。
    FrameEngine fe2(s->backing().cp);
    std::vector<FrameAt> frames2;
    ASSERT_TRUE(fe2.compute(*p.unwrap(), frames2).ok);
    EXPECT_TRUE(frames2[atHandler].reachable);
}

/// 多抛出点异类引用在 handler locals 合并：经 resolver 求公共父类。
/// try 从首个 store 之后开始（store 自身不在 try 内）——首抛点的 in-state
/// 已携带 local0，入口边不出现缺席槽，合并是真引用 × 真引用。
TEST(StrictFrameExcTest, HandlerLocalsMergeViaResolver)
{
    ClassBuilder cb = baseClass();
    const ExcClassCp cp = excCpOf(cb);

    // 0 getstatic X.a / 3 astore_0 / 4 getstatic T.g（抛点1，local0=A）/
    // 7 pop / 8 getstatic X.b / 11 astore_0 / 12 getstatic T.g（抛点2，
    // local0=B）/ 15 pop / 16 return / 17 astore_1（handler）/ 18 return
    Bytes code;
    putRef(code, cp.aRef);
    code.push_back(0x4b);              // 3 astore_0
    putRef(code, cp.gRef);
    code.push_back(0x57);              // 7 pop
    putRef(code, cp.bRef);
    code.push_back(0x4b);              // 11 astore_0
    putRef(code, cp.gRef);
    code.push_back(0x57);              // 15 pop
    code.push_back(0xb1);              // 16 return
    code.push_back(0x4c);              // 17 astore_1（handler）
    code.push_back(0xb1);              // 18 return

    auto s = excClassOf(cb, code, 1, 2, {jbc::test::ExcSpec{4, 16, 17, cp.excCls}});
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());

    StubResolver res;
    res.parent["p/A"] = "p/M";
    res.parent["p/B"] = "p/M";
    res.parent["p/M"] = "java/lang/Object";
    FrameEngine fe(s->backing().cp, res, FramePolicy::Strict);
    std::vector<FrameAt> frames;
    const FrameSummary sum =
        fe.compute(*p.unwrap(), MethodSig{true, "()V", "m", ""}, frames);
    ASSERT_TRUE(sum.ok) << jbc::analysis::to_string(sum.fail);
    EXPECT_GT(res.calls, 0);   // 合并确实经 resolver 求解

    const ca::usize atHandler = indexAtPc(*p.unwrap(), 17);
    ASSERT_NE(atHandler, static_cast<ca::usize>(-1));
    ASSERT_TRUE(frames[atHandler].reachable);
    ASSERT_GE(frames[atHandler].locals.size(), 1u);
    EXPECT_EQ(frames[atHandler].locals[0].k, VKind::Ref);
    EXPECT_EQ(frames[atHandler].locals[0].name, "p/M");
    ASSERT_EQ(frames[atHandler].stack.size(), 1u);
    EXPECT_EQ(frames[atHandler].stack[0].name, "java/lang/Exception");
}

/// 多抛出点 locals 尺寸类冲突（Int × Ref）→ TypeConflict，failAt 定位到
/// handler 入口指令；BestEffort 冲突吸收继续（零漂移）。
TEST(StrictFrameExcTest, HandlerLocalsConflictFailsAtHandler)
{
    ClassBuilder cb = baseClass();
    const ExcClassCp cp = excCpOf(cb);

    // 0 iconst_1 / 1 istore_0 / 2 getstatic T.g（抛点1，local0=Int）/
    // 5 pop / 6 getstatic X.a / 9 astore_0 / 10 getstatic T.g（抛点2，
    // local0=Ref）/ 13 pop / 14 return / 15 astore_1（handler）/ 16 return
    Bytes code;
    code.push_back(0x04);              // 0 iconst_1
    code.push_back(0x3b);              // 1 istore_0
    putRef(code, cp.gRef);
    code.push_back(0x57);              // 5 pop
    putRef(code, cp.aRef);
    code.push_back(0x4b);              // 9 astore_0
    putRef(code, cp.gRef);
    code.push_back(0x57);              // 13 pop
    code.push_back(0xb1);              // 14 return
    code.push_back(0x4c);              // 15 astore_1（handler）
    code.push_back(0xb1);              // 16 return

    auto s = excClassOf(cb, code, 1, 2, {jbc::test::ExcSpec{0, 14, 15, cp.excCls}});
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());

    StubResolver res;
    FrameEngine fe(s->backing().cp, res, FramePolicy::Strict);
    const FrameSummary sum = fe.compute(*p.unwrap());
    EXPECT_FALSE(sum.ok);
    EXPECT_EQ(sum.fail, FrameSummary::Fail::TypeConflict);
    const ca::usize atHandler = indexAtPc(*p.unwrap(), 15);
    ASSERT_NE(atHandler, static_cast<ca::usize>(-1));
    EXPECT_EQ(sum.failAt, p.unwrap()->dom.order()[atHandler]);   // 定位到 handler 入口

    // BestEffort 对照：冲突吸收为 Bottom 继续，不失败。
    FrameEngine fe2(s->backing().cp);
    const FrameSummary sum2 = fe2.compute(*p.unwrap());
    EXPECT_TRUE(sum2.ok);
}

/// 两条异常表条目指向同一 handler（javac 多 catch 的编码形态）：两表的
/// catch_type 值都并入 handler 入口栈，经 resolver 收敛为公共父类——
/// 若只并首表（旧播种语义）栈会停在首表类型。
TEST(StrictFrameExcTest, MultiCatchEntriesMergeCatchTypes)
{
    ClassBuilder cb = baseClass();
    const ExcClassCp cp = excCpOf(cb);

    // 0 getstatic T.g / 3 pop / 4 return / 5 astore_0（共享 handler）/ 6 return
    Bytes code;
    putRef(code, cp.gRef);
    code.push_back(0x57);              // 3 pop
    code.push_back(0xb1);              // 4 return
    code.push_back(0x4b);              // 5 astore_0（handler）
    code.push_back(0xb1);              // 6 return

    auto s = excClassOf(cb, code, 1, 1, {jbc::test::ExcSpec{0, 4, 5, cp.excCls},
                                         jbc::test::ExcSpec{0, 4, 5, cp.thrCls}});
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());

    StubResolver res;
    res.parent["java/lang/Exception"] = "java/lang/Throwable";
    FrameEngine fe(s->backing().cp, res, FramePolicy::Strict);
    std::vector<FrameAt> frames;
    const FrameSummary sum = fe.compute(*p.unwrap(), frames);
    ASSERT_TRUE(sum.ok) << jbc::analysis::to_string(sum.fail);

    const ca::usize atHandler = indexAtPc(*p.unwrap(), 5);
    ASSERT_NE(atHandler, static_cast<ca::usize>(-1));
    ASSERT_TRUE(frames[atHandler].reachable);
    ASSERT_EQ(frames[atHandler].stack.size(), 1u);
    // Exception ⊔ Throwable = Throwable（两表条目的边都实际并入）。
    EXPECT_EQ(frames[atHandler].stack[0].name, "java/lang/Throwable");
}

namespace {

bool toolPresent(const char* exe)
{
    return jbc::test::oracleToolPresent(exe);
}

}   // namespace

TEST(StrictFrameOracle, ComputedFramesMatchJavacSmt)
{
    if (!toolPresent("javac")) {
        GTEST_SKIP() << "javac not present";
    }
    ASSERT_TRUE(jbc::test::compileOracleSamples(oracleRoot()));

    StubResolver res;
    res.seedJdkParents();
    res.parent["demo/CtorUninit"] = "demo/CtorBase";

    ca::usize methods = 0;
    ca::usize framesChecked = 0;
    ca::usize handlerFrames = 0;
    ca::usize mismatch = 0;

    // javac -d 会落进包目录（classes/demo/*.class），递归收集。
    std::error_code iterEc;
    for (fs::recursive_directory_iterator it(oracleRoot() / "classes", iterEc), end;
         it != end && !iterEc; it.increment(iterEc)) {
        if (!it->is_regular_file() || it->path().extension() != ".class") {
            continue;
        }
        const fs::path clsPath = it->path();
        const Bytes bytes = [&clsPath] {
            std::ifstream in(clsPath, std::ios::binary);
            return Bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        }();
        auto s = sessionOf(bytes);
        ASSERT_NE(s, nullptr) << clsPath;
        for (ca::usize i = 0; i < s->unit().methods.size(); ++i) {
            auto p = promoteMethod(*s, i);
            if (p.is_err()) {
                continue;
            }
            const jbc::ir::MethodUnit& mu = s->unit().methods[i];
            MethodSig sig;
            sig.isStatic = (mu.access & 0x0008) != 0;
            sig.desc = s->unit().symbols().str(mu.desc);
            sig.name = s->unit().symbols().str(mu.name);
            sig.thisClass = s->unit().symbols().str(s->unit().thisClass);

            // 定位该方法的 SMT 属性；没有则无事可比（无分支的直线方法）。
            const StackMapTableAttr* smt = findMethodSmt(*s, i);
            if (smt == nullptr) {
                continue;
            }
            ++methods;

            FrameEngine fe(s->backing().cp, res, FramePolicy::Strict);
            std::vector<FrameAt> frames;
            const FrameSummary sum = fe.compute(*p.unwrap(), sig, frames);
            ASSERT_TRUE(sum.ok)
                << clsPath.filename() << "." << sig.name << ": strict compute failed: "
                << jbc::analysis::to_string(sum.fail);

            // handler 入口 pc 集合：不再豁免（J5 起入口帧纳入比对），仅用于
            // 计量「含 handler 帧 N 个」的验证口径。
            std::set<ca::u32> handlerPcs;
            for (const jbc::ir::CodeUnit::Exc& e : p.unwrap()->exc) {
                auto t = p.unwrap()->dom.resolveLabel(e.handler);
                if (t.is_ok() && !t.unwrap().isEnd()) {
                    const jbc::ir::Insn* hi = p.unwrap()->dom.peek(t.unwrap().insn);
                    if (hi != nullptr) {
                        handlerPcs.insert(hi->srcPc);
                    }
                }
            }

            const std::vector<FullFrame> expect = expandSmt(*smt, entryLocalsOf(sig),
                                                            s->backing().cp);
            ASSERT_FALSE(expect.empty());
            for (const FullFrame& ef : expect) {
                const ca::usize idx = indexAtPc(*p.unwrap(), ef.offset);
                ASSERT_NE(idx, static_cast<ca::usize>(-1))
                    << clsPath.filename() << "." << sig.name << ": frame offset "
                    << ef.offset << " not an instruction";
                ASSERT_TRUE(frames[idx].reachable)
                    << clsPath.filename() << "." << sig.name << ": offset "
                    << ef.offset << " unreachable in computed frames";
                ++framesChecked;
                if (handlerPcs.count(ef.offset) != 0) {
                    ++handlerFrames;
                }
                const std::vector<VType> gotLocals =
                    headOf(frames[idx].locals, trimmedSize(frames[idx].locals));
                const std::vector<VType> expLocals =
                    headOf(ef.locals, trimmedSize(ef.locals));
                // locals 按公共前缀逐槽一致：javac 对死变量做 chop 投影
                //（monitor 副本等——§10.2.6 陈述口径差异的读侧体现），声明
                // 的 locals 可少于计算所得（JVMS 4.10.1.4 可指派性允许
                // handler/目标帧声明更少 locals），截去部分不构成约束；
                // 前缀内仍要求逐槽一致（含 handler 帧）。
                const ca::usize nLocals = gotLocals.size() < expLocals.size()
                                              ? gotLocals.size()
                                              : expLocals.size();
                const bool localsOk =
                    std::equal(gotLocals.begin(), gotLocals.begin() + static_cast<std::ptrdiff_t>(nLocals),
                               expLocals.begin(), sameSlot);
                const bool stackOk =
                    frames[idx].stack.size() == ef.stack.size() &&
                    std::equal(frames[idx].stack.begin(), frames[idx].stack.end(),
                               ef.stack.begin(), sameSlot);
                if (!localsOk || !stackOk) {
                    ++mismatch;
                    ADD_FAILURE()
                        << clsPath.filename() << "." << sig.name << " @" << ef.offset
                        << "\n  expect locals " << slotsText(expLocals) << "\n  actual locals "
                        << slotsText(gotLocals) << "\n  expect stack " << slotsText(ef.stack)
                        << "\n  actual stack " << slotsText(frames[idx].stack);
                }
            }
        }
    }
    std::printf("[strict-frame-oracle] methods=%zu framesChecked=%zu handlerFrames=%zu"
                " mismatch=%zu\n",
                methods, framesChecked, handlerFrames, mismatch);
    EXPECT_GE(methods, 8u);      // 样本量护栏：oracle 必须真的跑到了
    EXPECT_GE(framesChecked, 10u);
    EXPECT_GE(handlerFrames, 5u);   // handler 入口帧必须真的纳入了比对
    EXPECT_EQ(mismatch, 0u);     // 验收口径：可算者与 javac 一致
}

// ---------------------------------------------------------------------------
// 语料全量两态 oracle（strict oracle 扩容）：自产语料（jbc_corpus）+ golden
// 注解样本类，逐方法两态统计——
//   可算 → 与 SMT 展开公共前缀逐槽一致（可指派性口径：计算帧槽可指派到
//          javac 陈述槽即一致，「更精确类型不算分歧」§10.4.1）；
//   不可算 → 显式 Err 且 failAt 定位到存活指令（§10.1.3 验收口径）。
// 断言零伪帧（ok 但与 SMT 矛盾）。两类已知陈述口径差异按机制排除并单独
// 计数，不算伪帧：
//   1) §10.3.7 死 try handler：try 范围内无可抛指令时 javac 仍为 handler
//      陈述帧、引擎按 §10.3.4 判不可达——以「覆盖范围内无 canThrow 指令」
//      机械识别（engine 的可抛全集 detail::canThrow 单一知识源）；
//   2) stub 知识外的跨类合并：resolver 拒绝（Unresolvable）落入不可算态，
//      不伪造 Object 兜底帧——实证：Object 兜底会把 javac 陈述接口/子类
//      的汇合槽陈述成 Object，真 verifier 必拒（伪帧）。
// ---------------------------------------------------------------------------

namespace {

/// 测试侧层级知识：parent = superclass 链（子 → 父，JVMS 4.10.1.2 公共父类
/// 求解口径，resolver 用）；ifaces = implements/extends-interface 边
///（可指派性判定用，BFS）。语料类边从骨架自收，JDK 族经 seed 补。
using ParentMap = std::map<std::string, std::string>;
using IfaceMap = std::map<std::string, std::vector<std::string>>;

/// 去掉一层数组维度；非数组返回空。
std::string_view arrayElementOf(std::string_view name)
{
    return !name.empty() && name[0] == '[' ? name.substr(1) : std::string_view();
}

/// 语料 stub resolver（§10.1 契约的测试内实现，不进库）。
///
/// 求解纪律：同名自解；空名不可解；数组协变规则；跨类沿层级图（superclass
/// 边 + implements/extends-interface 边，接口边优先展开——更具体的公共类型
/// 优先）求首个公共祖先。**节点知识必须完整**：只出现在 parent/ifaces 表内
///（或为 Object 根）的节点可展开，知识缺席即 Unresolvable——不伪造 Object
/// 兜底帧（兜底帧与 javac 陈述矛盾即伪帧，语料实证：PluginRunner.
/// initDCState 汇合槽 javac 陈述接口 ClassFileSource2、FilterManager 构造器
/// 汇合槽陈述 List）。求解结果经驻留表交付——VType.name 是借用视图，不得
/// 指向临时串。
class CorpusResolver final : public ITypeResolver
{
public:
    ParentMap parent;   ///< 子 → 父（superclass 边；语料自收 + JDK 种子）
    IfaceMap ifaces;    ///< 子 → 接口/超接口边（语料自收 + JDK 种子）
    int calls = 0;

    ca::core::Result<VType, TypeMergeError> merge(VType a, VType b) override
    {
        ++calls;
        return mergeNames(a.name, b.name);
    }

private:
    std::set<std::string> interned_;   ///< 求解结果驻留（节点地址稳定）

    /// 具名合并（入口）：同名自解；空名不可解；其余交数组/跨类规则。
    ca::core::Result<VType, TypeMergeError> mergeNames(std::string_view x,
                                                       std::string_view y)
    {
        if (x == y) {
            return ca::core::Ok(VType::ref(*interned_.insert(std::string(x)).first));
        }
        if (x.empty() || y.empty()) {
            return ca::core::Err(TypeMergeError::Unresolvable);
        }
        const bool xa = x.front() == '[';
        const bool ya = y.front() == '[';
        if (xa || ya) {
            // 数组 × 非数组引用 → Object（数组类型仅继承 Object，恒真）。
            if (xa != ya) {
                return refObject();
            }
            // 数组 × 数组：元素级递归（verifier 协变口径）。
            const std::string_view xe = arrayElementOf(x);
            const std::string_view ye = arrayElementOf(y);
            if (xe.empty() || ye.empty()) {
                return ca::core::Err(TypeMergeError::Unresolvable);
            }
            if (xe == ye) {
                return ca::core::Ok(VType::ref(*interned_.insert(std::string(x)).first));
            }
            if (xe.front() == 'L' && ye.front() == 'L') {
                auto m = crossClassLca(xe.substr(1, xe.size() - 2),
                                       ye.substr(1, ye.size() - 2));
                if (m.is_err()) {
                    return m;
                }
                return ca::core::Ok(
                    VType::ref(*interned_.insert("[" + std::string(m.unwrap().name)).first));
            }
            // 原始元素异形（int[] × String[] 等）：公共父恒 Object。
            return refObject();
        }
        return crossClassLca(x, y);
    }

    /// 节点知识完整性：表内出现过（或为 Object 根）才可展开。
    bool known(const std::string& n) const
    {
        return n == "java/lang/Object" || parent.count(n) != 0 || ifaces.count(n) != 0;
    }

    /// from 的层序超类型闭包（含自身；接口边优先、parent 边随后）。
    /// 走到知识缺席的节点返回 false（不静默补 Object）。
    bool buildClosure(const std::string& from, std::set<std::string>& out) const
    {
        if (!known(from)) {
            return false;
        }
        out.insert(from);
        std::vector<std::string> layer{from};
        std::set<std::string> seen{from};
        for (ca::usize hop = 0; hop <= 64 && !layer.empty(); ++hop) {
            std::vector<std::string> next;
            for (const std::string& cur : layer) {
                if (cur == "java/lang/Object") {
                    continue;
                }
                const auto f = ifaces.find(cur);
                if (f != ifaces.end()) {
                    for (const std::string& i : f->second) {
                        if (!known(i) || !seen.insert(i).second) {
                            if (!known(i)) {
                                return false;
                            }
                            continue;
                        }
                        out.insert(i);
                        next.push_back(i);
                    }
                }
                const auto p = parent.find(cur);
                const std::string up =
                    p != parent.end() ? p->second : std::string("java/lang/Object");
                if (seen.insert(up).second) {
                    if (!known(up)) {
                        return false;
                    }
                    out.insert(up);
                    next.push_back(up);
                }
            }
            layer = std::move(next);
        }
        return true;
    }

    /// 跨类父链求解：x 闭包完整 + y 层序展开首个命中（接口边优先——
    /// 更具体的公共类型优先）即公共祖先；知识缺席拒绝，不伪造 Object 兜底。
    ca::core::Result<VType, TypeMergeError> crossClassLca(std::string_view xs,
                                                          std::string_view ys)
    {
        std::set<std::string> cx;
        if (!buildClosure(std::string(xs), cx)) {
            return ca::core::Err(TypeMergeError::Unresolvable);
        }
        const std::string y(ys);
        if (!known(y)) {
            return ca::core::Err(TypeMergeError::Unresolvable);
        }
        if (cx.count(y) != 0) {
            return ca::core::Ok(VType::ref(*interned_.insert(y).first));
        }
        std::vector<std::string> layer{y};
        std::set<std::string> seen{y};
        for (ca::usize hop = 0; hop <= 64 && !layer.empty(); ++hop) {
            std::vector<std::string> next;
            for (const std::string& cur : layer) {
                if (cur == "java/lang/Object") {
                    continue;
                }
                std::vector<std::string> succ;
                const auto f = ifaces.find(cur);
                if (f != ifaces.end()) {
                    for (const std::string& i : f->second) {
                        succ.push_back(i);
                    }
                }
                const auto p = parent.find(cur);
                succ.push_back(p != parent.end() ? p->second
                                                 : std::string("java/lang/Object"));
                for (const std::string& up : succ) {
                    if (cx.count(up) != 0) {
                        return ca::core::Ok(VType::ref(*interned_.insert(up).first));
                    }
                    if (!known(up)) {
                        return ca::core::Err(TypeMergeError::Unresolvable);
                    }
                    if (seen.insert(up).second) {
                        next.push_back(up);
                    }
                }
            }
            layer = std::move(next);
        }
        return ca::core::Err(TypeMergeError::Unresolvable);
    }

    ca::core::Result<VType, TypeMergeError> refObject()
    {
        return ca::core::Ok(VType::ref(*interned_.insert("java/lang/Object").first));
    }

    /// 拒绝诊断（env 门控：JBC_TRACE_RESOLVER=1 时打印知识缺口）。
    void refuseTrace(const std::string& name) const
    {
        if (getenv("JBC_TRACE_RESOLVER") != nullptr) {
            std::printf("[resolver] refuse: unknown hierarchy at %s\n", name.c_str());
        }
    }
};

/// JDK 族种子（测试内知识，按语料所需渐增；缺席不猜测——resolver 拒绝、
/// 比对按精确/数组规则）。superclass 链入 parent，接口边入 ifaces。接口
/// 节点必须以（可空的）ifaces 条目标记为已知——求解闭包只展开表内节点。
void seedJdkHierarchy(ParentMap& parent, IfaceMap& ifaces)
{
    const auto mark = [&ifaces](const std::string& n) {
        ifaces[n];   // 空边表 = 已知且无超接口（Object 直达由求解器补）
    };
    // 异常族（multi-catch lub 求解走 super-chain）。
    parent["java/lang/RuntimeException"]       = "java/lang/Exception";
    parent["java/lang/Exception"]              = "java/lang/Throwable";
    parent["java/lang/Error"]                  = "java/lang/Throwable";
    parent["java/lang/Throwable"]              = "java/lang/Object";
    parent["java/lang/IllegalStateException"]  = "java/lang/RuntimeException";
    parent["java/lang/NumberFormatException"]  = "java/lang/IllegalArgumentException";
    parent["java/lang/IllegalArgumentException"] = "java/lang/RuntimeException";
    parent["java/lang/ArithmeticException"]    = "java/lang/RuntimeException";
    parent["java/lang/NullPointerException"]   = "java/lang/RuntimeException";
    parent["java/lang/ClassCastException"]     = "java/lang/RuntimeException";
    // 集合族（帧槽陈述为接口、计算值为实现类的陈述口径差异）。
    parent["java/util/ArrayList"]         = "java/util/AbstractList";
    parent["java/util/AbstractList"]      = "java/util/AbstractCollection";
    parent["java/util/AbstractCollection"] = "java/lang/Object";
    ifaces["java/util/ArrayList"].push_back("java/util/List");
    ifaces["java/util/AbstractList"].push_back("java/util/List");
    ifaces["java/util/List"].push_back("java/util/Collection");
    ifaces["java/util/Set"].push_back("java/util/Collection");
    ifaces["java/util/Queue"].push_back("java/util/Collection");
    ifaces["java/util/Collection"].push_back("java/lang/Iterable");
    mark("java/lang/Iterable");
    parent["java/util/HashMap"]           = "java/util/AbstractMap";
    parent["java/util/AbstractMap"]       = "java/lang/Object";
    ifaces["java/util/HashMap"].push_back("java/util/Map");
    ifaces["java/util/AbstractMap"].push_back("java/util/Map");
    mark("java/util/Map");
    parent["java/util/IdentityHashMap"]   = "java/util/AbstractMap";
    ifaces["java/util/IdentityHashMap"].push_back("java/util/Map");
    parent["java/util/LinkedHashMap"]     = "java/util/HashMap";
    ifaces["java/util/LinkedHashMap"].push_back("java/util/Map");
    parent["java/util/concurrent/ConcurrentHashMap"] = "java/util/AbstractMap";
    ifaces["java/util/concurrent/ConcurrentHashMap"].push_back("java/util/concurrent/ConcurrentMap");
    ifaces["java/util/concurrent/ConcurrentHashMap"].push_back("java/util/Map");
    ifaces["java/util/concurrent/ConcurrentMap"].push_back("java/util/Map");
    parent["java/util/HashSet"]           = "java/util/AbstractSet";
    parent["java/util/AbstractSet"]       = "java/util/AbstractCollection";
    ifaces["java/util/AbstractSet"].push_back("java/util/Set");
    ifaces["java/util/HashSet"].push_back("java/util/Set");
    parent["java/util/LinkedHashSet"]     = "java/util/HashSet";
    ifaces["java/util/LinkedHashSet"].push_back("java/util/Set");
    parent["java/util/concurrent/ConcurrentHashMap$KeySetView"] = "java/lang/Object";
    ifaces["java/util/concurrent/ConcurrentHashMap$KeySetView"].push_back("java/util/Set");
    parent["java/util/ArrayDeque"]        = "java/util/AbstractCollection";
    ifaces["java/util/ArrayDeque"].push_back("java/util/Deque");
    ifaces["java/util/Deque"].push_back("java/util/Queue");
    ifaces["java/util/ListIterator"].push_back("java/util/Iterator");
    mark("java/util/Iterator");
    // 字符串/文本/数值族。
    parent["java/lang/String"]            = "java/lang/Object";
    ifaces["java/lang/String"].push_back("java/lang/CharSequence");
    ifaces["java/lang/String"].push_back("java/lang/Comparable");
    mark("java/lang/CharSequence");
    mark("java/lang/Comparable");
    parent["java/lang/StringBuilder"]     = "java/lang/Object";
    ifaces["java/lang/StringBuilder"].push_back("java/lang/CharSequence");
    parent["java/lang/Enum"]              = "java/lang/Object";
    parent["java/lang/Number"]            = "java/lang/Object";
    parent["java/lang/Integer"]           = "java/lang/Number";
    parent["java/lang/Long"]              = "java/lang/Number";
    parent["java/text/StringCharacterIterator"] = "java/lang/Object";
    ifaces["java/text/StringCharacterIterator"].push_back("java/text/CharacterIterator");
    mark("java/text/CharacterIterator");
    // 并发族。
    parent["java/util/concurrent/ExecutorCompletionService"] = "java/lang/Object";
    ifaces["java/util/concurrent/ExecutorCompletionService"].push_back(
        "java/util/concurrent/CompletionService");
    ifaces["java/util/concurrent/CompletionService"].push_back(
        "java/util/concurrent/ExecutorService");
    ifaces["java/util/concurrent/ExecutorService"].push_back("java/util/concurrent/Executor");
    mark("java/util/concurrent/Executor");
    // JDK 基础族（大语料常见汇合对象）。第三方库（jackson/asm/benf）
    // 刻意不种子：其跨类汇合按知识缺席显式拒绝，保留不可算分布的活样本。
    parent["java/io/IOException"]         = "java/lang/Exception";
    parent["java/lang/ClassNotFoundException"] = "java/lang/ReflectiveOperationException";
    parent["java/lang/ReflectiveOperationException"] = "java/lang/Exception";
    parent["java/io/InputStream"]         = "java/lang/Object";
    parent["java/io/OutputStream"]        = "java/lang/Object";
    parent["java/lang/Class"]             = "java/lang/Object";
    parent["java/util/jar/JarFile"]       = "java/util/zip/ZipFile";
    parent["java/util/zip/ZipFile"]       = "java/lang/Object";
    mark("java/lang/AutoCloseable");
}

/// 引用可指派性（受限层级）：sub 沿 superclass + 接口边 BFS 到 sup 即成立
///（§10.4.1 更精确类型不算分歧的机械口径）。数组协变 + 任意数组 ⊆ Object。
bool refAssignable(const ParentMap& parent, const IfaceMap& ifaces,
                   std::string_view sub, std::string_view sup)
{
    if (!sup.empty() && sup[0] == '[') {
        // 数组 ⊆ 数组：元素维协变（引用元素递归，原始元素同形才可指派）。
        if (sub.empty() || sub[0] != '[') {
            return false;
        }
        if (sub == sup) {
            return true;
        }
        const std::string_view se = arrayElementOf(sub);
        const std::string_view ee = arrayElementOf(sup);
        if (se.empty() || ee.empty()) {
            return false;
        }
        if (se.front() == 'L' && ee.front() == 'L') {
            return refAssignable(parent, ifaces, se.substr(1, se.size() - 2),
                                 ee.substr(1, ee.size() - 2));
        }
        return se == ee;
    }
    if (!sub.empty() && sub[0] == '[') {
        return sup == "java/lang/Object";   // 任意数组 ⊆ Object
    }
    if (sup == "java/lang/Object") {
        return true;   // 任意引用类型 ⊆ Object（平凡真，无需层级知识）
    }
    std::vector<std::string> frontier{std::string(sub)};
    std::set<std::string> seen{std::string(sub)};
    for (ca::usize hop = 0; hop <= 64 && !frontier.empty(); ++hop) {
        std::vector<std::string> next;
        for (const std::string& cur : frontier) {
            if (cur == sup) {
                return true;
            }
            const auto p = parent.find(cur);
            if (p != parent.end() && seen.insert(p->second).second) {
                next.push_back(p->second);
            }
            const auto f = ifaces.find(cur);
            if (f != ifaces.end()) {
                for (const std::string& i : f->second) {
                    if (seen.insert(i).second) {
                        next.push_back(i);
                    }
                }
            }
        }
        frontier = std::move(next);
    }
    return false;
}

/// 槽级一致性：sameSlot 精确相等，或计算槽可指派到 javac 陈述槽
///（null ⊆ 引用、子类/实现类 ⊆ 父类/接口、数组协变——陈述口径差异
/// 不算分歧）；其余即矛盾（伪帧证据）。
bool slotConsistent(const ParentMap& parent, const IfaceMap& ifaces,
                    const VType& got, const VType& exp)
{
    if (sameSlot(got, exp)) {
        return true;
    }
    if (got.k == VKind::Null && exp.k == VKind::Ref) {
        return true;
    }
    if (got.k != VKind::Ref || exp.k != VKind::Ref) {
        return false;
    }
    return refAssignable(parent, ifaces, got.name, exp.name);
}

/// 从类骨架收集 this → super 父链与 implements 接口边（只读骨架，不解方法体）。
void harvestHierarchy(const Bytes& bytes, ParentMap& parent, IfaceMap& ifaces)
{
    auto cf = jbc::test::readOk(bytes);
    if (cf == nullptr) {
        return;
    }
    const std::string thisName(cf->thisName());
    const std::string_view superName = cf->superName();
    if (!superName.empty()) {
        parent.emplace(thisName, std::string(superName));
    }
    for (const ca::u16 i : cf->interfaces) {
        ifaces[thisName].push_back(std::string(cf->className(i)));
    }
}

}   // namespace

TEST(StrictFrameOracle, CorpusSmtTwoState)
{
    if (!toolPresent("javac")) {
        GTEST_SKIP() << "javac not present";   // 自产语料依赖 javac 现场编译
    }
    const jbc::test::corpus::CorpusSource src = jbc::test::corpus::resolveCorpus();
    if (src.stage == jbc::test::corpus::Stage::None) {
        GTEST_SKIP() << jbc::test::corpus::skipReason(src);
    }

    // 语料根：主语料（env 注入 / 仓内自产）+ golden 注解样本类
    //（固定入仓，与其它语料闸门同源）。
    std::vector<fs::path> roots{fs::path(src.dir), fs::path("test_resource/libjbc/annotated")};
    std::vector<fs::path> files;
    for (const fs::path& root : roots) {
        std::error_code ec;
        for (fs::recursive_directory_iterator it(root, ec), end; it != end && !ec;
             it.increment(ec)) {
            if (!ec && it->is_regular_file() && it->path().extension() == ".class") {
                files.push_back(it->path());
            }
        }
    }

    // 第一遍：层级知识（stub 的求解与比对知识全部来自语料骨架 + JDK 种子）。
    ParentMap parent;
    IfaceMap ifaces;
    for (const fs::path& p : files) {
        harvestHierarchy(jbc::test::slurpFile(p.string()), parent, ifaces);
    }
    seedJdkHierarchy(parent, ifaces);

    CorpusResolver res;
    res.parent  = parent;   // 语料自收 + JDK 种子的 superclass 边
    res.ifaces  = ifaces;   // implements/extends-interface 边

    ca::usize promoted = 0;      // 有 Code 且升格成功的方法（量级护栏对象）
    ca::usize noSmt = 0;         // 无 SMT（直线方法）：无可比 oracle，观测项
    ca::usize noSmtFailed = 0;   // 其中 Strict 不可算（无伪帧可能，观测项）
    ca::usize comparable = 0;    // 带 SMT 参与两态统计的方法
    ca::usize computed = 0;      // 可算且与 SMT 比对
    ca::usize framesChecked = 0;
    ca::usize pseudoFrames = 0;  // 伪帧：ok 但与 SMT 矛盾（断言恒零）
    ca::usize deadTryFrames = 0; // §10.3.7 死 try handler 帧（已知口径差异）
    ca::usize errs = 0;          // 不可算（显式 Err）
    std::map<std::string, ca::usize> errReasons;

    for (const fs::path& clsPath : files) {
        const Bytes bytes = jbc::test::slurpFile(clsPath.string());
        if (bytes.empty()) {
            continue;
        }
        auto s = sessionOf(bytes);
        ASSERT_NE(s, nullptr) << clsPath.string();
        for (ca::usize i = 0; i < s->unit().methods.size(); ++i) {
            if (!jbc::ir::hasCode(*s, i)) {
                continue;   // abstract/native：无升格对象
            }
            auto p = promoteMethod(*s, i);
            if (p.is_err()) {
                ADD_FAILURE() << clsPath.string() << " method " << i
                              << " promote failed: " << jbc::ir::to_string(p.unwrap_err());
                continue;
            }
            ++promoted;
            const jbc::ir::CodeUnit& cu = *p.unwrap();
            const jbc::ir::MethodUnit& mu = s->unit().methods[i];
            MethodSig sig;
            sig.isStatic = (mu.access & 0x0008) != 0;
            sig.desc     = s->unit().symbols().str(mu.desc);
            sig.name     = s->unit().symbols().str(mu.name);
            sig.thisClass = s->unit().symbols().str(s->unit().thisClass);
            const std::string where =
                clsPath.filename().string() + "." + std::string(sig.name);

            const StackMapTableAttr* smt = findMethodSmt(*s, i);
            FrameEngine fe(s->backing().cp, res, FramePolicy::Strict);
            std::vector<FrameAt> frames;
            const FrameSummary sum = fe.compute(cu, sig, frames);
            if (smt == nullptr) {
                ++noSmt;
                if (!sum.ok) {
                    ++noSmtFailed;
                }
                continue;
            }
            ++comparable;
            if (!sum.ok) {
                // 不可算态：显式 Err 必须带定位（failAt 是存活指令句柄）。
                ++errs;
                ++errReasons[jbc::analysis::to_string(sum.fail)];
                if (sum.failAt == jbc::ir::kNoInsn || cu.dom.peek(sum.failAt) == nullptr) {
                    ADD_FAILURE() << where << ": strict err "
                                  << jbc::analysis::to_string(sum.fail)
                                  << " without located instruction";
                } else {
                    std::printf("[strict-corpus-err] %s @pc %u: %s\n", where.c_str(),
                                cu.dom.peek(sum.failAt)->srcPc,
                                jbc::analysis::to_string(sum.fail));
                }
                continue;
            }
            ++computed;

            // handler 入口 pc 集合（死 try 口径差异只可能发生在 handler 帧）。
            std::set<ca::u32> handlerPcs;
            std::map<ca::u32, std::vector<std::pair<ca::u32, ca::u32>>> handlerRanges;
            for (const jbc::ir::CodeUnit::Exc& e : cu.exc) {
                auto h = cu.dom.resolveLabel(e.handler);
                auto t0 = cu.dom.resolveLabel(e.start);
                auto t1 = cu.dom.resolveLabel(e.end);
                if (!h.is_ok() || h.unwrap().isEnd() || !t0.is_ok() || !t1.is_ok()) {
                    continue;
                }
                const jbc::ir::Insn* hi = cu.dom.peek(h.unwrap().insn);
                const jbc::ir::Insn* bi = cu.dom.peek(t0.unwrap().insn);
                const jbc::ir::Insn* ei = t1.unwrap().isEnd()
                                              ? nullptr : cu.dom.peek(t1.unwrap().insn);
                if (hi == nullptr || bi == nullptr) {
                    continue;
                }
                const ca::u32 startPc = bi->srcPc;
                const ca::u32 endPc = ei != nullptr ? ei->srcPc : 0xFFFFFFFFu;
                handlerPcs.insert(hi->srcPc);
                handlerRanges[hi->srcPc].push_back({startPc, endPc});
            }

            const std::vector<FullFrame> expect =
                expandSmt(*smt, entryLocalsOf(sig), s->backing().cp);
            for (const FullFrame& ef : expect) {
                const ca::usize idx = indexAtPc(cu, ef.offset);
                if (idx == static_cast<ca::usize>(-1)) {
                    ++pseudoFrames;
                    ADD_FAILURE() << where << " @" << ef.offset
                                  << ": SMT frame offset is not an instruction";
                    continue;
                }
                if (!frames[idx].reachable) {
                    // §10.3.7 口径差异的机械识别：handler 帧且全部覆盖范围
                    // 内无任何可抛指令 → javac 照常陈述、引擎判不可达（文档
                    // 在案的字面分歧），单独计数不算伪帧；覆盖范围内确有可
                    // 抛指令（或帧在分支目标上）而引擎判不可达 → 真分歧证据。
                    bool coveredThrowable = false;
                    if (handlerPcs.count(ef.offset) != 0) {
                        for (const auto& range : handlerRanges[ef.offset]) {
                            for (const jbc::ir::InsnId& id : cu.dom.order()) {
                                const jbc::ir::Insn* in = cu.dom.peek(id);
                                if (in == nullptr || in->srcPc < range.first ||
                                    in->srcPc >= range.second) {
                                    continue;
                                }
                                if (jbc::analysis::detail::canThrow(in->op)) {
                                    coveredThrowable = true;
                                    break;
                                }
                            }
                            if (coveredThrowable) {
                                break;
                            }
                        }
                    }
                    if (coveredThrowable) {
                        ++pseudoFrames;
                        ADD_FAILURE() << where << " @" << ef.offset
                                      << ": SMT declares frame but computed unreachable"
                                         " (throwable insn in range)";
                    } else {
                        ++deadTryFrames;
                    }
                    continue;
                }
                ++framesChecked;
                const std::vector<VType> gotLocals =
                    headOf(frames[idx].locals, trimmedSize(frames[idx].locals));
                const std::vector<VType> expLocals =
                    headOf(ef.locals, trimmedSize(ef.locals));
                const ca::usize nLocals = gotLocals.size() < expLocals.size()
                                              ? gotLocals.size()
                                              : expLocals.size();
                bool frameOk = frames[idx].stack.size() == ef.stack.size();
                std::string evidence;
                for (ca::usize k = 0; frameOk && k < nLocals; ++k) {
                    if (!slotConsistent(parent, ifaces, gotLocals[k], expLocals[k])) {
                        frameOk = false;
                        evidence = "local[" + std::to_string(k) + "] expect "
                                   + slotsText({expLocals[k]}) + " got "
                                   + slotsText({gotLocals[k]});
                    }
                }
                for (ca::usize k = 0; frameOk && k < ef.stack.size(); ++k) {
                    if (!slotConsistent(parent, ifaces, frames[idx].stack[k], ef.stack[k])) {
                        frameOk = false;
                        evidence = "stack[" + std::to_string(k) + "] expect "
                                   + slotsText({ef.stack[k]}) + " got "
                                   + slotsText({frames[idx].stack[k]});
                    }
                }
                if (!frameOk) {
                    ++pseudoFrames;
                    ADD_FAILURE() << where << " @" << ef.offset << ": pseudo frame — "
                                  << evidence << "\n  computed locals "
                                  << slotsText(gotLocals) << "\n  smt locals "
                                  << slotsText(expLocals);
                }
            }
        }
    }

    std::string reasonText;
    for (const auto& [reason, count] : errReasons) {
        reasonText += " " + reason + "=" + std::to_string(count);
    }
    std::printf("[strict-corpus-oracle] stage=%d promoted=%zu noSmt=%zu(noSmtFailed=%zu)"
                " comparable=%zu computed=%zu errs=%zu pseudoFrames=%zu deadTryFrames=%zu"
                " framesChecked=%zu resolverCalls=%d | fail:%s\n",
                static_cast<int>(src.stage), promoted, noSmt, noSmtFailed, comparable,
                computed, errs, pseudoFrames, deadTryFrames, framesChecked, res.calls,
                reasonText.c_str());
    // 量级护栏：语料必须真的全量跑到（自产 1071+；env 注入更大）。
    EXPECT_GE(promoted, 1000u);
    EXPECT_GE(comparable, 300u);      // 带 SMT 的可比方法必须成规模
    EXPECT_EQ(pseudoFrames, 0u);      // 零伪帧（可算者与 SMT 无矛盾）
}
