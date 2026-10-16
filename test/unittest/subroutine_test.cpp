/// @file        subroutine_test.cpp
/// @brief       Strict 域 jsr/ret 子程序数据流测试（C2，design.md §10.6）。
///
/// 语义依据 JVMS §4.10.2.5 族（jsr 压返回地址、ret 经局部槽跳回、
/// 子程序未改写的局部槽保留调用方值）+ §6.5 jsr/ret/astore 指令语义。
/// 五部分：
/// 1. 合成样本——嵌套子程序双返回点、子程序改写/保留调用方槽的对照、
///    入口栈上返回地址载荷。
/// 2. 多调用点共享入口——returnAddress 只与自身合并（§4.10.2.5），异调用
///    点互并在 Strict 域显式拒绝（failAt = 入口合并点）。
/// 3. 畸形路径行为表——ret 读非返回地址槽（Int/Null/空槽）、jsr 指向非
///    指令偏移（promote 层结构拒绝）、jsr 为方法末条指令（返回点越界）。
/// 4. SMT 生成裁决钉死——子程序帧显式 Err(Subroutine)（JVMS §4.7.4 tag 集
///    无 returnAddress 形态）；v49 走版本门 Err(ClassTooOld)（v≤49 不带
///    SMT 走旧校验是规范许可行为）。
/// 5. JVM 真跑门——v49 手造样本（无 SMT）javap 结构核对 + 本机 java 实际
///    加载执行，验证造码真实可验、语义正确；畸形样本 JVM 侧同样拒绝。
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "jbc/jbc.hpp"
#include "smt_oracle_util.hpp"
#include "test_class_bytes.hpp"

using namespace jbc::core;
using jbc::analysis::FrameAt;
using jbc::analysis::FrameEngine;
using jbc::analysis::FramePolicy;
using jbc::analysis::FrameSummary;
using jbc::analysis::MethodSig;
using jbc::analysis::SmtBuildError;
using jbc::analysis::VKind;
using jbc::analysis::VType;
using jbc::ir::ClassSession;
using jbc::ir::CodeUnit;
using jbc::ir::promoteMethod;
using Bytes = std::vector<ca::u8>;
using jbc::test::AttrSpec;
using jbc::test::ClassBuilder;
using jbc::test::StubResolver;
using jbc::test::indexAtPc;
using jbc::test::sessionOf;

namespace fs = std::filesystem;

namespace {

/// 单方法类骨架（p/T extends Object；版本可指定，jsr/ret 样本用 v49/50）。
ClassBuilder baseClass(ca::u16 major = 52)
{
    ClassBuilder cb;
    const ca::u16 thisIdx  = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setAccess(0x0021);
    cb.setVersion(major);
    return cb;
}

/// static ()V m + Code → 会话。
std::shared_ptr<ClassSession> methodClass(const Bytes& code, ca::u16 maxStack,
                                          ca::u16 maxLocals, ca::u16 major = 52)
{
    ClassBuilder cb = baseClass(major);
    const ca::u16 codeIdx = cb.utf8("Code");
    const ca::u16 mIdx    = cb.utf8("m");
    const ca::u16 voidIdx = cb.utf8("()V");
    std::vector<AttrSpec> ma;
    ma.push_back(AttrSpec{codeIdx, jbc::test::codeContent(maxStack, maxLocals, code)});
    cb.addMethod(0x0009, mIdx, voidIdx, std::move(ma));
    return sessionOf(cb.build());
}

void put2(Bytes& b, ca::u16 v)
{
    b.push_back(static_cast<ca::u8>(v >> 8));
    b.push_back(static_cast<ca::u8>(v & 0xFF));
}

/// 断言槽 = ReturnAddress 且载荷 = 指定 jsr pc。
void expectRetAddr(const char* what, const VType& v, ca::u32 jsrPc)
{
    EXPECT_EQ(v.k, VKind::ReturnAddress) << what;
    EXPECT_EQ(v.jsrPc(), jsrPc) << what;
}

/// Strict 计算快捷路径（static ()V 播种 + 逐指令帧）。
FrameSummary strictCompute(ClassSession& s, const CodeUnit& u,
                           std::vector<FrameAt>& frames)
{
    StubResolver res;
    FrameEngine fe(s.backing().cp, res, FramePolicy::Strict);
    return fe.compute(u, MethodSig{true, "()V", "m", ""}, frames);
}

}   // namespace

// ---------------------------------------------------------------------------
// 嵌套子程序：两级调用链的双返回点精确落位（§10.6）
// ---------------------------------------------------------------------------

TEST(SubroutineTest, NestedSubroutinesExactReturnSites)
{
    // 0 jsr→10 / 3 return / 4-9 nop / 10 astore_1（A 入口）/ 11 jsr→20 /
    // 14 ret 1（A 的 ret，兼 B 返回点）/ 16-19 nop / 20 astore_2（B 入口）/
    // 21 ret 2
    Bytes code;
    code.push_back(0xa8);              // 0 jsr -> 10
    put2(code, 10);
    code.push_back(0xb1);              // 3 return（A 返回点）
    for (int i = 0; i < 6; ++i) {
        code.push_back(0x00);          // 4-9 nop
    }
    code.push_back(0x4c);              // 10 astore_1（A 入口：local1 = RetAddr{0}）
    code.push_back(0xa8);              // 11 jsr -> 20
    put2(code, 9);
    code.push_back(0xa9);              // 14 ret 1
    code.push_back(0x01);
    for (int i = 0; i < 4; ++i) {
        code.push_back(0x00);          // 16-19 nop
    }
    code.push_back(0x4d);              // 20 astore_2（B 入口：local2 = RetAddr{11}）
    code.push_back(0xa9);              // 21 ret 2
    code.push_back(0x02);

    auto s = methodClass(code, 1, 3);
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    std::vector<FrameAt> frames;
    const FrameSummary sum = strictCompute(*s, *p.unwrap(), frames);
    ASSERT_TRUE(sum.ok) << jbc::analysis::to_string(sum.fail);

    // B 入口（pc20）：栈顶 = B 调用点的返回地址；A 的返回地址已入 local1
    //（调用链状态逐层穿透）。
    const ca::usize atB = indexAtPc(*p.unwrap(), 20);
    ASSERT_NE(atB, static_cast<ca::usize>(-1));
    ASSERT_EQ(frames[atB].stack.size(), 1u);
    expectRetAddr("B entry stack", frames[atB].stack[0], 11);
    ASSERT_GE(frames[atB].locals.size(), 2u);
    expectRetAddr("A retAddr local inside B", frames[atB].locals[1], 0);

    // B 出口（pc21）：local2 = B 的返回地址。
    const ca::usize atBret = indexAtPc(*p.unwrap(), 21);
    ASSERT_GE(frames[atBret].locals.size(), 3u);
    expectRetAddr("B ret slot", frames[atBret].locals[2], 11);

    // B 返回点（pc14，A 的 ret 所在）：B 出口态并入——栈空、local2 携带
    // B 返回地址、local1 保持 A 的返回地址。
    const ca::usize atAret = indexAtPc(*p.unwrap(), 14);
    ASSERT_NE(atAret, static_cast<ca::usize>(-1));
    EXPECT_TRUE(frames[atAret].reachable);
    EXPECT_EQ(frames[atAret].stack.size(), 0u);
    ASSERT_GE(frames[atAret].locals.size(), 3u);
    expectRetAddr("retSite local1", frames[atAret].locals[1], 0);
    expectRetAddr("retSite local2", frames[atAret].locals[2], 11);

    // A 返回点（pc3）：两级返回地址槽都随出口态保留。
    const ca::usize atSite = indexAtPc(*p.unwrap(), 3);
    ASSERT_NE(atSite, static_cast<ca::usize>(-1));
    EXPECT_TRUE(frames[atSite].reachable);
    ASSERT_GE(frames[atSite].locals.size(), 3u);
    expectRetAddr("A retSite local1", frames[atSite].locals[1], 0);
    expectRetAddr("A retSite local2", frames[atSite].locals[2], 11);
}

// ---------------------------------------------------------------------------
// 子程序改写 vs 保留调用方槽：出口态经 ret 并入返回点（§4.10.2.5 合并族）
// ---------------------------------------------------------------------------

TEST(SubroutineTest, SubroutineRewritesAndPreservesCallerSlots)
{
    // 0 iconst_1 / 1 istore_1 / 2 iconst_7 / 3 istore_2 / 4 jsr→12 /
    // 7 iload_1（返回点）/ 8 pop / 9 iload_2 / 10 pop / 11 return /
    // 12 astore_3（入口）/ 13 aconst_null / 14 astore_1（改写 local1:
    // Int→Null，kind 变化可见）/ 15 ret 3
    Bytes code;
    code.push_back(0x04);              // 0 iconst_1
    code.push_back(0x3c);              // 1 istore_1
    code.push_back(0x08);              // 2 iconst_5
    code.push_back(0x3d);              // 3 istore_2
    code.push_back(0xa8);              // 4 jsr -> 12
    put2(code, 8);
    code.push_back(0x1b);              // 7 iload_1（返回点）
    code.push_back(0x57);              // 8 pop
    code.push_back(0x1c);              // 9 iload_2
    code.push_back(0x57);              // 10 pop
    code.push_back(0xb1);              // 11 return
    code.push_back(0x4e);              // 12 astore_3（入口：local3 = RetAddr{4}）
    code.push_back(0x01);              // 13 aconst_null
    code.push_back(0x4c);              // 14 astore_1（改写 local1）
    code.push_back(0xa9);              // 15 ret 3
    code.push_back(0x03);

    auto s = methodClass(code, 1, 4);
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    std::vector<FrameAt> frames;
    const FrameSummary sum = strictCompute(*s, *p.unwrap(), frames);
    ASSERT_TRUE(sum.ok) << jbc::analysis::to_string(sum.fail);

    // 入口（pc12）：栈上返回地址载荷 = 调用方 jsr pc。
    const ca::usize atEntry = indexAtPc(*p.unwrap(), 12);
    ASSERT_NE(atEntry, static_cast<ca::usize>(-1));
    ASSERT_EQ(frames[atEntry].stack.size(), 1u);
    expectRetAddr("entry stack", frames[atEntry].stack[0], 4);
    ASSERT_GE(frames[atEntry].locals.size(), 3u);
    EXPECT_EQ(frames[atEntry].locals[1].k, VKind::Int);    // 调用方值进子程序
    EXPECT_EQ(frames[atEntry].locals[2].k, VKind::Int);

    // 返回点（pc7）：local1 被子程序改写为 Null（不再 Int），local2 保留
    // 调用方值 Int——改写/保留双对照；local3 的返回地址随出口态保留
    //（JVMS 忠实形态：槽内容不因 ret 清除）。
    const ca::usize atSite = indexAtPc(*p.unwrap(), 7);
    ASSERT_NE(atSite, static_cast<ca::usize>(-1));
    EXPECT_TRUE(frames[atSite].reachable);
    ASSERT_GE(frames[atSite].locals.size(), 4u);
    EXPECT_EQ(frames[atSite].locals[1].k, VKind::Null);
    EXPECT_EQ(frames[atSite].locals[2].k, VKind::Int);
    expectRetAddr("retSite local3", frames[atSite].locals[3], 4);
    EXPECT_EQ(frames[atSite].stack.size(), 0u);
}

// ---------------------------------------------------------------------------
// 多调用点共享入口：returnAddress 只与自身合并（§4.10.2.5），异调用点
// 互并在 Strict 域显式拒绝（failAt = 入口合并点）
// ---------------------------------------------------------------------------

TEST(SubroutineTest, MultiCallerSharedEntryRejectedAtEntry)
{
    // 0 jsr→12 / 3 goto→15（返回点串入第二调用点链）/ 6-11 nop /
    // 12 astore_1（入口）/ 13 ret 1 / 15-20 nop / 21 jsr→12（第二调用点，
    // 向后）/ 24 return
    Bytes code;
    code.push_back(0xa8);              // 0 jsr -> 12
    put2(code, 12);
    code.push_back(0xa7);              // 3 goto -> 15
    put2(code, 12);
    for (int i = 0; i < 6; ++i) {
        code.push_back(0x00);          // 6-11 nop
    }
    code.push_back(0x4c);              // 12 astore_1（入口）
    code.push_back(0xa9);              // 13 ret 1
    code.push_back(0x01);
    for (int i = 0; i < 6; ++i) {
        code.push_back(0x00);          // 15-20 nop
    }
    code.push_back(0xa8);              // 21 jsr -> 12
    put2(code, 0xFFF7);                // -9
    code.push_back(0xb1);              // 24 return

    auto s = methodClass(code, 1, 2);
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    std::vector<FrameAt> frames;
    const FrameSummary sum = strictCompute(*s, *p.unwrap(), frames);
    EXPECT_FALSE(sum.ok);
    EXPECT_EQ(sum.fail, FrameSummary::Fail::Subroutine);
    const ca::usize atEntry = indexAtPc(*p.unwrap(), 12);
    ASSERT_NE(atEntry, static_cast<ca::usize>(-1));
    EXPECT_EQ(sum.failAt, p.unwrap()->dom.order()[atEntry]);   // 入口合并点

    // BestEffort 对照：不建模子程序，冲突不存在（零漂移）。
    FrameEngine fe(s->backing().cp);
    const FrameSummary sum2 = fe.compute(*p.unwrap());
    EXPECT_TRUE(sum2.ok);
    EXPECT_TRUE(sum2.hasSubroutine);
}

// ---------------------------------------------------------------------------
// 畸形路径行为表（§10.6）：ret 读非返回地址槽 / 返回点越界
// ---------------------------------------------------------------------------

TEST(SubroutineTest, MalformedRetReadsIntSlot)
{
    // 0 jsr→6 / 3 return / 4-5 nop / 6 iconst_0 / 7 istore_1 / 8 ret 1
    Bytes code;
    code.push_back(0xa8);              // 0 jsr -> 6
    put2(code, 6);
    code.push_back(0xb1);              // 3 return
    code.push_back(0x00);              // 4 nop
    code.push_back(0x00);              // 5 nop
    code.push_back(0x03);              // 6 iconst_0
    code.push_back(0x3c);              // 7 istore_1
    code.push_back(0xa9);              // 8 ret 1
    code.push_back(0x01);

    auto s = methodClass(code, 1, 2);
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    std::vector<FrameAt> frames;
    const FrameSummary sum = strictCompute(*s, *p.unwrap(), frames);
    EXPECT_FALSE(sum.ok);
    EXPECT_EQ(sum.fail, FrameSummary::Fail::Subroutine);
    const ca::usize atRet = indexAtPc(*p.unwrap(), 8);
    ASSERT_NE(atRet, static_cast<ca::usize>(-1));
    EXPECT_EQ(sum.failAt, p.unwrap()->dom.order()[atRet]);   // 定位到 ret
}

TEST(SubroutineTest, MalformedRetReadsEmptySlot)
{
    // 0 jsr→6 / 3 return / 4-5 nop / 6 nop / 7 ret 1（local1 从未赋值）
    Bytes code;
    code.push_back(0xa8);              // 0 jsr -> 6
    put2(code, 6);
    code.push_back(0xb1);              // 3 return
    code.push_back(0x00);              // 4 nop
    code.push_back(0x00);              // 5 nop
    code.push_back(0x00);              // 6 nop
    code.push_back(0xa9);              // 7 ret 1
    code.push_back(0x01);

    auto s = methodClass(code, 1, 2);
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    std::vector<FrameAt> frames;
    const FrameSummary sum = strictCompute(*s, *p.unwrap(), frames);
    EXPECT_FALSE(sum.ok);
    EXPECT_EQ(sum.fail, FrameSummary::Fail::Subroutine);
    const ca::usize atRet = indexAtPc(*p.unwrap(), 7);
    ASSERT_NE(atRet, static_cast<ca::usize>(-1));
    EXPECT_EQ(sum.failAt, p.unwrap()->dom.order()[atRet]);
}

TEST(SubroutineTest, MalformedRetReadsNullSlot)
{
    // 0 jsr→6 / 3 return / 4-5 nop / 6 aconst_null / 7 astore_1 / 8 ret 1
    //（astore 能存 Null，但 ret 只接受 returnAddress）
    Bytes code;
    code.push_back(0xa8);              // 0 jsr -> 6
    put2(code, 6);
    code.push_back(0xb1);              // 3 return
    code.push_back(0x00);              // 4 nop
    code.push_back(0x00);              // 5 nop
    code.push_back(0x01);              // 6 aconst_null
    code.push_back(0x4c);              // 7 astore_1
    code.push_back(0xa9);              // 8 ret 1
    code.push_back(0x01);

    auto s = methodClass(code, 1, 2);
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    std::vector<FrameAt> frames;
    const FrameSummary sum = strictCompute(*s, *p.unwrap(), frames);
    EXPECT_FALSE(sum.ok);
    EXPECT_EQ(sum.fail, FrameSummary::Fail::Subroutine);
    const ca::usize atRet = indexAtPc(*p.unwrap(), 8);
    ASSERT_NE(atRet, static_cast<ca::usize>(-1));
    EXPECT_EQ(sum.failAt, p.unwrap()->dom.order()[atRet]);
}

TEST(SubroutineTest, MalformedJsrNoReturnSite)
{
    // 0 goto→6 / 3 astore_1（子程序入口，只经 jsr 可达）/ 4 ret 1 /
    // 6 jsr→3（jsr 是方法末条指令：返回地址 9 = code 长度，无落点）
    Bytes code;
    code.push_back(0xa7);              // 0 goto -> 6
    put2(code, 6);
    code.push_back(0x4c);              // 3 astore_1
    code.push_back(0xa9);              // 4 ret 1
    code.push_back(0x01);
    code.push_back(0xa8);              // 6 jsr -> 3
    put2(code, 0xFFFD);                // -3

    auto s = methodClass(code, 1, 2);
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    std::vector<FrameAt> frames;
    const FrameSummary sum = strictCompute(*s, *p.unwrap(), frames);
    EXPECT_FALSE(sum.ok);
    EXPECT_EQ(sum.fail, FrameSummary::Fail::Subroutine);
    const ca::usize atRet = indexAtPc(*p.unwrap(), 4);
    ASSERT_NE(atRet, static_cast<ca::usize>(-1));
    EXPECT_EQ(sum.failAt, p.unwrap()->dom.order()[atRet]);
}

/// jsr 指向非法偏移：JVMS §4.9.2 结构约束（跳转目标必须是方法内某条指令
/// 的 opcode）由 promote 层拒绝（importFrom 的 BadTarget 面），帧计算前
/// 即不可达——结构校验归 reader/promote，帧域不重复把关。
TEST(SubroutineTest, MalformedJsrToNonInsnOffsetPromoteRejects)
{
    // 0 nop / 1 nop / 2 jsr→6 / 5 goto→8（3 字节，pc6 落在其中间）
    Bytes mid;
    mid.push_back(0x00);               // 0 nop
    mid.push_back(0x00);               // 1 nop
    mid.push_back(0xa8);               // 2 jsr -> 6（pc6 非指令起点）
    put2(mid, 4);
    mid.push_back(0xa7);               // 5 goto -> 8
    put2(mid, 3);
    {
        auto s = methodClass(mid, 1, 1);
        ASSERT_NE(s, nullptr);
        auto p = promoteMethod(*s, 0);
        ASSERT_FALSE(p.is_ok());
        EXPECT_EQ(p.unwrap_err(), jbc::ir::PromoteError::BadInstructions);
    }
    // 变体：目标越出 code 界（dst = pc + off >= code 长度）。
    Bytes out;
    out.push_back(0xa8);               // 0 jsr -> 100（越界）
    put2(out, 100);
    out.push_back(0xb1);               // 3 return
    {
        auto s = methodClass(out, 1, 1);
        ASSERT_NE(s, nullptr);
        auto p = promoteMethod(*s, 0);
        ASSERT_FALSE(p.is_ok());
        EXPECT_EQ(p.unwrap_err(), jbc::ir::PromoteError::BadInstructions);
    }
}

// ---------------------------------------------------------------------------
// SMT 生成裁决钉死（§10.6）：子程序帧显式 Err(Subroutine)；v49 走版本门
// ---------------------------------------------------------------------------

/// 0 jsr→8 / 3 nop / 4 return / 5-7 nop / 8 astore_1 / 9 ret 1（最小往返）。
Bytes minimalJsrCode()
{
    Bytes code;
    code.push_back(0xa8);              // 0 jsr -> 8
    put2(code, 8);
    code.push_back(0x00);              // 3 nop
    code.push_back(0xb1);              // 4 return
    code.push_back(0x00);              // 5 nop
    code.push_back(0x00);              // 6 nop
    code.push_back(0x00);              // 7 nop
    code.push_back(0x4c);              // 8 astore_1
    code.push_back(0xa9);              // 9 ret 1
    code.push_back(0x01);
    return code;
}

TEST(SubroutineTest, SmtBuildRejectsSubroutineFrames)
{
    // v50：Strict 帧可算（引擎版本无关），但 full_frame 的
    // verification_type_info 无 returnAddress 形态（JVMS §4.7.4）——显式
    // Err(Subroutine)，不改码 Top/Int 伪造帧。
    auto s = methodClass(minimalJsrCode(), 1, 2, 50);
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    std::vector<FrameAt> frames;
    const FrameSummary sum = strictCompute(*s, *p.unwrap(), frames);
    ASSERT_TRUE(sum.ok) << jbc::analysis::to_string(sum.fail);

    auto r = jbc::analysis::buildStackMapTable(*s, *p.unwrap(), frames);
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), SmtBuildError::Subroutine);
}

TEST(SubroutineTest, SmtBuildOnV49HitsVersionGateFirst)
{
    // v49：SMT 属性无定义——版本门先行（Err(ClassTooOld)）；v≤49 不带 SMT
    // 走旧校验是规范许可行为（JVMS §4.10.1 failover），不需要也不得生成。
    auto s = methodClass(minimalJsrCode(), 1, 2, 49);
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    std::vector<FrameAt> frames;
    const FrameSummary sum = strictCompute(*s, *p.unwrap(), frames);
    ASSERT_TRUE(sum.ok) << jbc::analysis::to_string(sum.fail);

    auto r = jbc::analysis::buildStackMapTable(*s, *p.unwrap(), frames);
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), SmtBuildError::ClassTooOld);
}

// ---------------------------------------------------------------------------
// JVM 真跑门（A1 批同款）：v49 手造样本（无 SMT）→ javap -v 结构核对 →
// 本机 java 实际加载执行。验证三件事：造码真实可验（旧推理 verifier 通过）、
// 子程序语义正确（改写/保留/嵌套的计算结果）、畸形样本 JVM 侧同样拒绝。
// ---------------------------------------------------------------------------

namespace {

/// javap/java 命令执行（输出落 test/ 目录，退出码即成败）。
int runTool(const std::string& cmd)
{
    return std::system(cmd.c_str());
}

/// javap 解析：PATH 缺席时从 java.home（java -XshowSettings:properties）
/// 推导 <home>\bin\javap——Oracle javapath 目录只链 java/javac/jshell
///（本机实测形态），javap 常不在 PATH。
std::string resolveJavap()
{
    if (runTool("javap -version > nul 2>&1") == 0) {
        return "javap";
    }
    const fs::path propOut = fs::path("test") / "libjbc_subroutine_jhome.txt";
    if (runTool("java -XshowSettings:properties -version > " + propOut.string()
                + " 2>&1") != 0) {
        return {};
    }
    const Bytes b = jbc::test::slurpFile(propOut);
    const std::string text(b.begin(), b.end());
    const std::string key = "java.home = ";
    const auto pos = text.find(key);
    if (pos == std::string::npos) {
        return {};
    }
    const auto end = text.find('\n', pos);
    if (end == std::string::npos) {
        return {};
    }
    std::string home = text.substr(pos + key.size(), end - pos - key.size());
    while (!home.empty() && (home.back() == '\r' || home.back() == ' ')) {
        home.pop_back();
    }
    const std::string javap = (fs::path(home) / "bin" / "javap.exe").string();
    if (runTool("\"" + javap + "\" -version > nul 2>&1") != 0) {
        return {};
    }
    return "\"" + javap + "\"";
}

/// 字节写盘（test/libjbc_subroutine_run/<name>.class）。
fs::path spitClass(const std::string& name, const Bytes& bytes)
{
    const fs::path dir = fs::path("test") / "libjbc_subroutine_run";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path file = dir / (name + ".class");
    std::ofstream out(file, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return file;
}

/// SubMain：三个用 jsr/ret 实现的静态方法 + main 打印结果。
/// work(21)=42（子程序改写调用方槽 v*2）、preserve(5)=104（子程序读调用方
/// 槽 v-1，保留 local2）、nest(10)=13（嵌套两级 iinc）。main 逐个 println。
Bytes buildSubMainClass()
{
    ClassBuilder cb;
    const ca::u16 uThis  = cb.utf8("SubMain");
    const ca::u16 uSuper = cb.utf8("java/lang/Object");
    const ca::u16 cThis  = cb.classRef(uThis);
    const ca::u16 cSuper = cb.classRef(uSuper);
    const ca::u16 uSys   = cb.utf8("java/lang/System");
    const ca::u16 cSys   = cb.classRef(uSys);
    const ca::u16 uOut   = cb.utf8("out");
    const ca::u16 uPS    = cb.utf8("Ljava/io/PrintStream;");
    const ca::u16 fOut   = cb.fieldref(cSys, cb.nameAndType(uOut, uPS));
    const ca::u16 cPS    = cb.classRef(cb.utf8("java/io/PrintStream"));
    const ca::u16 uPln   = cb.utf8("println");
    const ca::u16 uIV    = cb.utf8("(I)V");
    const ca::u16 mPln   = cb.methodref(cPS, cb.nameAndType(uPln, uIV));
    const ca::u16 uII    = cb.utf8("(I)I");
    const ca::u16 uWork  = cb.utf8("work");
    const ca::u16 mWork  = cb.methodref(cThis, cb.nameAndType(uWork, uII));
    const ca::u16 uPres  = cb.utf8("preserve");
    const ca::u16 mPres  = cb.methodref(cThis, cb.nameAndType(uPres, uII));
    const ca::u16 uNest  = cb.utf8("nest");
    const ca::u16 mNest  = cb.methodref(cThis, cb.nameAndType(uNest, uII));
    const ca::u16 uMain  = cb.utf8("main");
    const ca::u16 uMdesc = cb.utf8("([Ljava/lang/String;)V");
    const ca::u16 uCode  = cb.utf8("Code");

    cb.setThis(cThis);
    cb.setSuper(cSuper);
    cb.setAccess(0x0021);
    cb.setVersion(49);                 // v49：无 SMT 走旧推理 verifier

    // work(I)I：local1 = v；jsr 子程序 local1 *= 2；返回。
    {
        Bytes c;
        c.push_back(0x1a);             // 0 iload_0
        c.push_back(0x3c);             // 1 istore_1
        c.push_back(0xa8);             // 2 jsr -> 10
        put2(c, 8);
        c.push_back(0x1b);             // 5 iload_1
        c.push_back(0xac);             // 6 ireturn
        c.push_back(0x00);             // 7 nop
        c.push_back(0x00);             // 8 nop
        c.push_back(0x00);             // 9 nop
        c.push_back(0x4d);             // 10 astore_2（local2 = RetAddr）
        c.push_back(0x1b);             // 11 iload_1
        c.push_back(0x05);             // 12 iconst_2
        c.push_back(0x68);             // 13 imul
        c.push_back(0x3c);             // 14 istore_1
        c.push_back(0xa9);             // 15 ret 2
        c.push_back(0x02);
        std::vector<AttrSpec> ma;
        ma.push_back(AttrSpec{uCode, jbc::test::codeContent(2, 3, c)});
        cb.addMethod(0x0009, uWork, uII, std::move(ma));
    }
    // preserve(I)I：local1 = v、local2 = 100；jsr 子程序 local1 -= 1（读
    // 调用方槽、改写 local1、保留 local2）；返回 local1 + local2。
    {
        Bytes c;
        c.push_back(0x1a);             // 0 iload_0
        c.push_back(0x3c);             // 1 istore_1
        c.push_back(0x10);             // 2 bipush 100
        c.push_back(0x64);
        c.push_back(0x3d);             // 4 istore_2
        c.push_back(0xa8);             // 5 jsr -> 13
        put2(c, 8);
        c.push_back(0x1c);             // 8 iload_2
        c.push_back(0x1b);             // 9 iload_1
        c.push_back(0x60);             // 10 iadd
        c.push_back(0xac);             // 11 ireturn
        c.push_back(0x00);             // 12 nop
        c.push_back(0x4e);             // 13 astore_3（local3 = RetAddr）
        c.push_back(0x1b);             // 14 iload_1
        c.push_back(0x04);             // 15 iconst_1
        c.push_back(0x64);             // 16 isub
        c.push_back(0x3c);             // 17 istore_1
        c.push_back(0xa9);             // 18 ret 3
        c.push_back(0x03);
        std::vector<AttrSpec> ma;
        ma.push_back(AttrSpec{uCode, jbc::test::codeContent(2, 4, c)});
        cb.addMethod(0x0009, uPres, uII, std::move(ma));
    }
    // nest(I)I：local1 = v；jsr A → A 内 jsr B（B: local1 += 1）→ A 续
    // local1 += 2 → 返回。
    {
        Bytes c;
        c.push_back(0x1a);             // 0 iload_0
        c.push_back(0x3c);             // 1 istore_1
        c.push_back(0xa8);             // 2 jsr -> 12（A）
        put2(c, 10);
        c.push_back(0x1b);             // 5 iload_1
        c.push_back(0xac);             // 6 ireturn
        for (int i = 0; i < 5; ++i) {
            c.push_back(0x00);         // 7-11 nop
        }
        c.push_back(0x4d);             // 12 astore_2（A：local2 = RetAddr{5}）
        c.push_back(0xa8);             // 13 jsr -> 22（B）
        put2(c, 9);
        c.push_back(0x84);             // 16 iinc local1, 2
        c.push_back(0x01);
        c.push_back(0x02);
        c.push_back(0xa9);             // 19 ret 2
        c.push_back(0x02);
        c.push_back(0x00);             // 21 nop
        c.push_back(0x4e);             // 22 astore_3（B：local3 = RetAddr{16}）
        c.push_back(0x84);             // 23 iinc local1, 1
        c.push_back(0x01);
        c.push_back(0x01);
        c.push_back(0xa9);             // 26 ret 3
        c.push_back(0x03);
        std::vector<AttrSpec> ma;
        ma.push_back(AttrSpec{uCode, jbc::test::codeContent(1, 4, c)});
        cb.addMethod(0x0009, uNest, uII, std::move(ma));
    }
    // main：println(work(21)); println(preserve(5)); println(nest(10))。
    {
        Bytes c;
        const auto call = [&c, mPln](ca::u16 fieldref, ca::u8 imm, ca::u16 methodref) {
            c.push_back(0xb2);         // getstatic System.out
            put2(c, fieldref);
            c.push_back(0x10);         // bipush imm
            c.push_back(imm);
            c.push_back(0xb8);         // invokestatic
            put2(c, methodref);
            c.push_back(0xb6);         // invokevirtual println
            put2(c, mPln);
        };
        call(fOut, 21, mWork);
        call(fOut, 5, mPres);
        call(fOut, 10, mNest);
        c.push_back(0xb1);             // return
        std::vector<AttrSpec> ma;
        ma.push_back(AttrSpec{uCode, jbc::test::codeContent(2, 1, c)});
        cb.addMethod(0x0009, uMain, uMdesc, std::move(ma));
    }
    return cb.build();
}

/// BadSub：ret 读非返回地址槽（iconst_0; istore_1; ret 1）——JVM 侧应
/// VerifyError（旧推理 verifier 同样拒绝）。
Bytes buildBadSubClass()
{
    ClassBuilder cb;
    const ca::u16 cThis  = cb.classRef(cb.utf8("BadSub"));
    const ca::u16 cSuper = cb.classRef(cb.utf8("java/lang/Object"));
    cb.setThis(cThis);
    cb.setSuper(cSuper);
    cb.setAccess(0x0021);
    cb.setVersion(49);
    const ca::u16 uCode = cb.utf8("Code");
    const ca::u16 uM    = cb.utf8("m");
    const ca::u16 uV    = cb.utf8("()V");
    Bytes c;
    c.push_back(0xa8);                 // 0 jsr -> 6
    put2(c, 6);
    c.push_back(0xb1);                 // 3 return
    c.push_back(0x00);                 // 4 nop
    c.push_back(0x00);                 // 5 nop
    c.push_back(0x03);                 // 6 iconst_0
    c.push_back(0x3c);                 // 7 istore_1
    c.push_back(0xa9);                 // 8 ret 1
    c.push_back(0x01);
    std::vector<AttrSpec> ma;
    ma.push_back(AttrSpec{uCode, jbc::test::codeContent(1, 2, c)});
    cb.addMethod(0x0009, uM, uV, std::move(ma));
    return cb.build();
}

}   // namespace

TEST(SubroutineJvmGate, RealRunOnV49Classes)
{
    if (!jbc::test::jdkPresent()) {
        GTEST_SKIP() << "jdk (javac/java) not present";
    }

    // 1) javap -v 结构核对：v49 类结构合法、jsr/ret 指令在场。
    const std::string javap = resolveJavap();
    if (javap.empty()) {
        GTEST_SKIP() << "javap not resolvable";
    }
    const fs::path cls = spitClass("SubMain", buildSubMainClass());
    ASSERT_FALSE(cls.empty());
    const std::string dir = cls.parent_path().string();
    const fs::path javapOut = fs::path("test") / "libjbc_subroutine_javap.txt";
    const int javapRc = runTool(javap + " -v -p -cp " + dir + " SubMain > "
                                + javapOut.string() + " 2>&1");
    ASSERT_EQ(javapRc, 0) << "javap rejected the crafted v49 class";
    const Bytes javapBytes = jbc::test::slurpFile(javapOut);
    const std::string javapText(javapBytes.begin(), javapBytes.end());
    EXPECT_NE(javapText.find("jsr"), std::string::npos) << "jsr must be present";
    EXPECT_NE(javapText.find("ret"), std::string::npos) << "ret must be present";

    // 2) java 实际加载执行：旧推理 verifier 通过且语义正确
    //    （work=42、preserve=104、nest=13）。
    const fs::path runOut = fs::path("test") / "libjbc_subroutine_run_out.txt";
    const int runRc = runTool("java -cp " + dir + " SubMain > "
                              + runOut.string() + " 2>&1");
    ASSERT_EQ(runRc, 0) << "java run failed (VerifyError or execution error)";
    const Bytes outBytes = jbc::test::slurpFile(runOut);
    std::string outText(outBytes.begin(), outBytes.end());
    outText.erase(std::remove(outText.begin(), outText.end(), '\r'), outText.end());
    EXPECT_EQ(outText, "42\n104\n13\n");

    // 3) 畸形样本（ret 读非返回地址槽）：JVM 侧同样拒绝（linking 阶段
    //    VerifyError 先于 main 查找）。
    const fs::path bad = spitClass("BadSub", buildBadSubClass());
    ASSERT_FALSE(bad.empty());
    const int badRc = runTool("java -cp " + bad.parent_path().string()
                              + " BadSub > " + (fs::path("test") / "libjbc_subroutine_bad_out.txt").string() + " 2>&1");
    EXPECT_NE(badRc, 0) << "JVM must reject ret on a non-returnAddress slot";

    // 4) 引擎对拍：JVM 真跑通过的类回读，Strict 域逐方法可算且重算 maxes
    //    与声明一致（verifier 地板经 applyComputedMaxes 同款并入）。
    const Bytes roundtrip = jbc::test::slurpFile(cls);
    auto s2 = sessionOf(roundtrip);
    ASSERT_NE(s2, nullptr);
    StubResolver res;
    for (ca::usize i = 0; i < s2->unit().methods.size(); ++i) {
        auto p2 = promoteMethod(*s2, i);
        ASSERT_TRUE(p2.is_ok()) << "method " << i;
        const jbc::ir::MethodUnit& mu = s2->unit().methods[i];
        MethodSig sig;
        sig.isStatic = (mu.access & 0x0008) != 0;
        sig.desc = s2->unit().symbols().str(mu.desc);
        sig.name = s2->unit().symbols().str(mu.name);
        sig.thisClass = s2->unit().symbols().str(s2->unit().thisClass);
        FrameEngine fe(s2->backing().cp, res, FramePolicy::Strict);
        const FrameSummary sum = fe.compute(*p2.unwrap(), sig);
        ASSERT_TRUE(sum.ok) << sig.name << ": " << jbc::analysis::to_string(sum.fail);
        // 重算 maxes（含参数槽地板）与声明逐项相等。
        const ca::i32 args = jbc::analysis::methodArgSlots(sig.desc);
        ca::u32 localsFloor = args < 0 ? 0u : static_cast<ca::u32>(args);
        if (!sig.isStatic) {
            ++localsFloor;
        }
        EXPECT_EQ(sum.maxStack, p2.unwrap()->maxStack) << sig.name;
        EXPECT_GE(p2.unwrap()->maxLocals, localsFloor) << sig.name;
        EXPECT_LE(sum.maxLocals, p2.unwrap()->maxLocals) << sig.name;
    }
}

// libjstr/libjhasm 不在 libjbc 依赖闭包内；JASM 文本路线需引依赖，按任务
// 边界不引入——造码全走 test_class_bytes.hpp 手写字节先例。
