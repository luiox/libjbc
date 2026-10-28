/// @file        ir_test.cpp
/// @brief       v0.3 测试：session/promote/lower、编辑器、relocation contract、
///        帧引擎自对拍（计算值 ≤ 声明值）。
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "jbc/jbc.hpp"
#include "test_class_bytes.hpp"

using namespace jbc::core;
using jbc::analysis::FrameEngine;
using jbc::analysis::FrameSummary;
using jbc::analysis::ShapeResolver;
using jbc::ir::ClassSession;
using jbc::ir::ClassUnit;
using jbc::ir::CodeEditor;
using jbc::ir::makeSession;
using jbc::ir::promoteMethod;
using jbc::ir::lowerMethod;
using Bytes = std::vector<ca::u8>;
using jbc::test::AttrSpec;
using jbc::test::ClassBuilder;
using jbc::test::ExcSpec;

namespace {

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

Bytes makeStackMapClass(const Bytes& code, const Bytes& stackMapBody)
{
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 codeIdx = cb.utf8("Code");
    const ca::u16 smtIdx  = cb.utf8("StackMapTable");
    const ca::u16 mIdx = cb.utf8("m");
    const ca::u16 voidIdx = cb.utf8("()V");
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setAccess(0x0021);
    cb.setVersion(52);
    cb.addMethod(0x0009, mIdx, voidIdx,
                 {AttrSpec{codeIdx, jbc::test::codeContent(1, 0, code, {},
                                                            {AttrSpec{smtIdx, stackMapBody}})}});
    return cb.build();
}

}   // namespace

TEST(IrSessionTest, SkeletonBuiltFromBacking)
{
    auto s = sessionOf(makeEditableClass(false));
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->unit().symbols().str(s->unit().thisClass), "p/T");
    EXPECT_EQ(s->unit().symbols().str(s->unit().superClass), "java/lang/Object");
    ASSERT_EQ(s->unit().methods.size(), 1u);
    EXPECT_EQ(s->unit().symbols().str(s->unit().methods[0].name), "m");
    EXPECT_EQ(s->unit().symbols().str(s->unit().methods[0].desc), "()V");
}

TEST(IrSessionTest, PromoteLowerNoopKeepsBytes)
{
    const Bytes cls = makeEditableClass(false);
    auto s = sessionOf(cls);
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok()) << jbc::ir::to_string(p.unwrap_err());
    jbc::ir::CodeUnit& cu = *p.unwrap();
    ASSERT_EQ(cu.dom.size(), 3u);
    EXPECT_EQ(cu.maxStack, 1);

    ASSERT_TRUE(lowerMethod(*s, 0).is_ok());
    // 未变化 → 不置脏 → 写回字节一致。
    EXPECT_EQ(jbc::test::writeOk(s->backing()), cls);
}

TEST(IrSessionTest, EditLowerRewriteAndSemanticPreservation)
{
    const Bytes cls = makeEditableClass(false);
    auto s = sessionOf(cls);
    auto p = promoteMethod(*s, 0);
    jbc::ir::CodeUnit& cu = *p.unwrap();
    CodeEditor ed(cu);

    // 在 pop 之后插入 nop（合成指令）：三条变四条，LNT 第二行 pc 2 → 3。
    const auto popId = cu.dom.order()[1];
    auto r = ed.insertAfter(popId, CodeEditor::simple(Op::NOP));
    ASSERT_TRUE(r.is_ok());

    ASSERT_TRUE(lowerMethod(*s, 0).is_ok());
    const Bytes out = jbc::test::writeOk(s->backing());
    ASSERT_FALSE(out.empty());
    EXPECT_NE(out, cls);   // 确认真的重编码了

    // 重新读取：Code 变 4 条指令；LNT 行 pc 已重定向。
    auto s2 = sessionOf(out);
    ASSERT_NE(s2, nullptr);
    auto dc = s2->backing().decodeAttr(s2->backing().methods[0].attrs[0]);
    ASSERT_TRUE(dc.is_ok());
    auto& ca2 = static_cast<CodeAttr&>(*dc.unwrap());
    auto p2 = promoteMethod(*s2, 0);
    ASSERT_TRUE(p2.is_ok());
    EXPECT_EQ(p2.unwrap()->dom.size(), 4u);

    ASSERT_EQ(ca2.attrs.size(), 1u);
    auto dl = s2->backing().decodeAttr(ca2.attrs[0]);
    ASSERT_TRUE(dl.is_ok());
    const auto& lnt = static_cast<const LineNumberTableAttr&>(*dl.unwrap());
    ASSERT_EQ(lnt.rows.size(), 2u);
    EXPECT_EQ(lnt.rows[0].startPc, 0u);
    EXPECT_EQ(lnt.rows[1].startPc, 3u);   // 原 2 → 3（nop 插在其前）
}

TEST(IrSessionTest, RelocationContractRejectsUnknownNestedAttr)
{
    auto s = sessionOf(makeEditableClass(true));
    auto p = promoteMethod(*s, 0);
    jbc::ir::CodeUnit& cu = *p.unwrap();
    CodeEditor ed(cu);
    ASSERT_TRUE(ed.insertAfter(cu.dom.order()[0], CodeEditor::simple(Op::NOP)).is_ok());

    // 布局改变 + Code 内存在未建模嵌套属性 → 明确失败。
    auto r = lowerMethod(*s, 0);
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), jbc::ir::LowerError::RelocationNotModeled);
}

TEST(IrSessionTest, FailedLowerDoesNotPartiallyCommitMetadataOrRelocation)
{
    const Bytes original = makeEditableClass(true);
    auto s = sessionOf(original);
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    jbc::ir::CodeUnit& cu = *p.unwrap();
    CodeEditor ed(cu);
    ASSERT_TRUE(ed.insertAfter(cu.dom.order()[0], CodeEditor::simple(Op::NOP)).is_ok());

    const ca::u32 cpSize = s->backing().cp.size();
    const ca::u32 oldNameCp = s->backing().methods[0].nameCp;
    s->unit().methods[0].name = s->unit().symbols().intern("renamed");

    const auto failed = lowerMethod(*s, 0);
    ASSERT_TRUE(failed.is_err());
    EXPECT_EQ(failed.unwrap_err(), jbc::ir::LowerError::RelocationNotModeled);
    EXPECT_EQ(s->backing().cp.size(), cpSize);
    EXPECT_EQ(s->backing().methods[0].nameCp, oldNameCp);
    EXPECT_EQ(jbc::test::writeOk(s->backing()), original);

    auto dc = s->backing().decodeAttr(s->backing().methods[0].attrs[0]);
    ASSERT_TRUE(dc.is_ok());
    auto& code = static_cast<CodeAttr&>(*dc.unwrap());
    auto dl = s->backing().decodeAttr(code.attrs[0]);
    ASSERT_TRUE(dl.is_ok());
    const auto& lnt = static_cast<const LineNumberTableAttr&>(*dl.unwrap());
    ASSERT_EQ(lnt.rows.size(), 2u);
    EXPECT_EQ(lnt.rows[0].startPc, 0u);
    EXPECT_EQ(lnt.rows[1].startPc, 2u);
    EXPECT_EQ(code.attrs[0].state, AttrSlot::State::Decoded);
}

TEST(IrSessionTest, RepeatedLowerUsesLatestBackingLayoutForRelocation)
{
    auto s = sessionOf(makeEditableClass(false));
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    jbc::ir::CodeUnit& cu = *p.unwrap();
    CodeEditor ed(cu);

    const auto first = cu.dom.order()[0];
    ASSERT_TRUE(ed.insertAfter(first, CodeEditor::simple(Op::NOP)).is_ok());
    ASSERT_TRUE(lowerMethod(*s, 0).is_ok());

    const auto pop = cu.dom.order()[2];
    ASSERT_TRUE(ed.insertBefore(pop, CodeEditor::simple(Op::NOP)).is_ok());
    ASSERT_TRUE(lowerMethod(*s, 0).is_ok());

    auto dc = s->backing().decodeAttr(s->backing().methods[0].attrs[0]);
    ASSERT_TRUE(dc.is_ok());
    auto& code = static_cast<CodeAttr&>(*dc.unwrap());
    auto dl = s->backing().decodeAttr(code.attrs[0]);
    ASSERT_TRUE(dl.is_ok());
    const auto& lnt = static_cast<const LineNumberTableAttr&>(*dl.unwrap());
    ASSERT_EQ(lnt.rows.size(), 2u);
    EXPECT_EQ(lnt.rows[0].startPc, 0u);
    EXPECT_EQ(lnt.rows[1].startPc, 4u);
}

TEST(IrSessionTest, EditorEraseRetargetsUses)
{
    // goto L; nop; L: return —— 删 nop（retarget 策略）：goto 改指 return。
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
    // goto +3; nop; return
    const Bytes code = {0xa7, 0x00, 0x03, 0x00, 0xb1};
    std::vector<AttrSpec> methodAttrs;
    methodAttrs.push_back(AttrSpec{codeIdx, jbc::test::codeContent(0, 0, code)});
    cb.addMethod(0x0009, mIdx, voidIdx, std::move(methodAttrs));
    const Bytes cls = cb.build();

    auto s = sessionOf(cls);
    auto p = promoteMethod(*s, 0);
    jbc::ir::CodeUnit& cu = *p.unwrap();
    CodeEditor ed(cu);

    const auto nopId = cu.dom.order()[1];
    // 默认策略：nop 有 incoming use（goto 正指向它）→ 拒绝删除。
    {
        auto rej = ed.erase(nopId);
        ASSERT_TRUE(rej.is_err());
        EXPECT_EQ(rej.unwrap_err(), jbc::ir::EditError::StillUsed);
    }
    // retarget 策略：uses 改指顺序后继（return）后删除，语义保持。
    ASSERT_TRUE(ed.erase(nopId, CodeEditor::ErasePolicy::kRetargetToFallthrough).is_ok());
    EXPECT_EQ(cu.dom.size(), 2u);

    // goto 的目标仍是 return（语义保持），lower 后重读验证。
    ASSERT_TRUE(lowerMethod(*s, 0).is_ok());
    auto s2 = sessionOf(jbc::test::writeOk(s->backing()));
    auto p2 = promoteMethod(*s2, 0);
    ASSERT_EQ(p2.unwrap()->dom.size(), 2u);
    const auto& g = *p2.unwrap()->dom.peek(p2.unwrap()->dom.order()[0]);
    EXPECT_EQ(g.op, Op::GOTO);
    const auto target = p2.unwrap()->dom.resolveLabel(g.target);
    ASSERT_TRUE(target.is_ok());
    ASSERT_FALSE(target.unwrap().isEnd());
    const auto& tgt = p2.unwrap()->dom.peek(target.unwrap().insn);
    ASSERT_NE(tgt, nullptr);
    EXPECT_EQ(tgt->op, Op::RETURN);
}

namespace {

/// iconst_1; pop; return + 一条异常表项（边界可配置）的最小可变换类。
Bytes makeExcClass(ca::u16 startPc, ca::u16 endPc, ca::u16 handlerPc)
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

    const Bytes code = {0x04, 0x57, 0xb1};   // iconst_1(0); pop(1); return(2)
    std::vector<AttrSpec> methodAttrs;
    methodAttrs.push_back(
        AttrSpec{codeIdx, jbc::test::codeContent(1, 0, code, {ExcSpec{startPc, endPc, handlerPc, 0}})});
    cb.addMethod(0x0009, mIdx, voidIdx, std::move(methodAttrs));
    return cb.build();
}

}   // namespace

TEST(IrSessionTest, EditorEraseRejectsExceptionBoundary)
{
    // P0 回归（design.md §7.3：try region 引用必须进 use 域）：异常表
    // start/end/handler 三边界各指一条指令，erase 在 CodeEditor 层前置拒绝
    // （dom 的 usersOf 只扫 branch/switch，StillUsed 错误现场留在编辑层）；
    // 绕过编辑器的死句柄由 lower 的 pcOfId 兜底，见
    // LowerCatchesDanglingExceptionHandle。
    const Bytes cls = makeExcClass(0, 2, 1);
    auto s = sessionOf(cls);
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok()) << jbc::ir::to_string(p.unwrap_err());
    jbc::ir::CodeUnit& cu = *p.unwrap();
    CodeEditor ed(cu);
    ASSERT_EQ(cu.exc.size(), 1u);

    const auto startId   = cu.dom.order()[0];   // iconst_1 = exc.start
    const auto handlerId = cu.dom.order()[1];   // pop      = exc.handler
    const auto endId     = cu.dom.order()[2];   // return   = exc.end

    // 三边界双策略一律 StillUsed（实现先查异常表后分支策略，两策略同果；
    // 机械改写异常表边界 = 改保护区间语义，必须显式改写 u_.exc 后再删）。
    {
        auto r1 = ed.erase(handlerId);
        ASSERT_TRUE(r1.is_err());
        EXPECT_EQ(r1.unwrap_err(), jbc::ir::EditError::StillUsed);
        auto r2 = ed.erase(handlerId, CodeEditor::ErasePolicy::kRetargetToFallthrough);
        ASSERT_TRUE(r2.is_err());
        EXPECT_EQ(r2.unwrap_err(), jbc::ir::EditError::StillUsed);
    }
    {
        auto r1 = ed.erase(startId);
        ASSERT_TRUE(r1.is_err());
        EXPECT_EQ(r1.unwrap_err(), jbc::ir::EditError::StillUsed);
        auto r2 = ed.erase(startId, CodeEditor::ErasePolicy::kRetargetToFallthrough);
        ASSERT_TRUE(r2.is_err());
        EXPECT_EQ(r2.unwrap_err(), jbc::ir::EditError::StillUsed);
    }
    {
        auto r1 = ed.erase(endId);
        ASSERT_TRUE(r1.is_err());
        EXPECT_EQ(r1.unwrap_err(), jbc::ir::EditError::StillUsed);
        auto r2 = ed.erase(endId, CodeEditor::ErasePolicy::kRetargetToFallthrough);
        ASSERT_TRUE(r2.is_err());
        EXPECT_EQ(r2.unwrap_err(), jbc::ir::EditError::StillUsed);
    }

    // 不被异常表引用的指令删除不受影响：入口插 nop 再删，往返照常。
    {
        auto n = ed.insertAtStart(CodeEditor::simple(Op::NOP));
        ASSERT_TRUE(n.is_ok());
        ASSERT_TRUE(ed.erase(n.unwrap()).is_ok());
    }

    // 合法编辑（入口插桩保留）下异常表随布局重定向：lower 后重读逐项相等。
    ASSERT_TRUE(ed.insertAtStart(CodeEditor::simple(Op::NOP)).is_ok());
    ASSERT_TRUE(lowerMethod(*s, 0).is_ok());
    auto s2 = sessionOf(jbc::test::writeOk(s->backing()));
    ASSERT_NE(s2, nullptr);
    auto dc = s2->backing().decodeAttr(s2->backing().methods[0].attrs[0]);
    ASSERT_TRUE(dc.is_ok());
    auto& ca2 = static_cast<CodeAttr&>(*dc.unwrap());
    ASSERT_EQ(ca2.excTable.size(), 1u);
    EXPECT_EQ(ca2.excTable[0].startPc, 1u);    // iconst_1: 0 → 1
    EXPECT_EQ(ca2.excTable[0].handlerPc, 2u);  // pop:      1 → 2
    EXPECT_EQ(ca2.excTable[0].endPc, 3u);      // return:   2 → 3
}

// ---------------------------------------------------------------------------
// D3：eraseBatch 批量删除（语义等价 + 成本模型）
// ---------------------------------------------------------------------------

namespace {

/// goto B; nop(B); nop; goto H; nop×3; H: return —— 批删 {B,E,F} 时
/// A→B 是唯一跨批引用（retarget 后 A→C），D→H 全程批外。
Bytes makeChainClass()
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
    const Bytes code = {0xa7, 0x00, 0x03,             // A@0 goto +3 → B@3
                        0x00,                          // B@3 nop（批内）
                        0x00,                          // C@4 nop（承接者）
                        0xa7, 0x00, 0x06,             // D@5 goto +6 → H@11
                        0x00, 0x00, 0x00,             // E@8 F@9 G@10（E、F 批内）
                        0xb1};                         // H@11 return
    std::vector<AttrSpec> methodAttrs;
    methodAttrs.push_back(AttrSpec{codeIdx, jbc::test::codeContent(0, 0, code)});
    cb.addMethod(0x0009, mIdx, voidIdx, std::move(methodAttrs));
    return cb.build();
}

}   // namespace

TEST(IrSessionTest, EditorEraseBatchMatchesSequentialErase)
{
    // 同一二进制两份会话：一份整批删 {B,E,F}，一份同集合逐条删（retarget
    // 策略）。语义等价的判据是 lower 落盘字节逐位一致 + exportRaw 一致。
    const Bytes cls = makeChainClass();
    auto sBatch = sessionOf(cls);
    auto sSeq   = sessionOf(cls);
    ASSERT_NE(sBatch, nullptr);
    ASSERT_NE(sSeq, nullptr);
    auto pb = promoteMethod(*sBatch, 0);
    auto ps = promoteMethod(*sSeq, 0);
    ASSERT_TRUE(pb.is_ok());
    ASSERT_TRUE(ps.is_ok());
    jbc::ir::CodeUnit& cuB = *pb.unwrap();
    jbc::ir::CodeUnit& cuS = *ps.unwrap();
    CodeEditor edB(cuB);
    CodeEditor edS(cuS);
    ASSERT_EQ(cuB.dom.size(), 8u);

    const std::vector<jbc::ir::InsnId> doomed{cuB.dom.order()[1], cuB.dom.order()[4],
                                              cuB.dom.order()[5]};
    const auto policy = CodeEditor::ErasePolicy::kRetargetToFallthrough;

    // 批量：一趟规划 + 单趟重建。
    ASSERT_TRUE(edB.eraseBatch(doomed, policy).is_ok());
    EXPECT_EQ(cuB.dom.size(), 5u);

    // 逐条：程序序（B 先删，A 的引用落到 C；E、F 无引用者）。句柄按
    // owner 归属，逐条侧用自己的 DOM 句柄（同槽位、不同会话）。
    const std::vector<jbc::ir::InsnId> doomedS{cuS.dom.order()[1], cuS.dom.order()[4],
                                               cuS.dom.order()[5]};
    ASSERT_TRUE(edS.erase(doomedS[0], policy).is_ok());
    ASSERT_TRUE(edS.erase(doomedS[1], policy).is_ok());
    ASSERT_TRUE(edS.erase(doomedS[2], policy).is_ok());
    EXPECT_EQ(cuS.dom.size(), 5u);

    auto encB = cuB.dom.encode();
    auto encS = cuS.dom.encode();
    ASSERT_TRUE(encB.is_ok());
    ASSERT_TRUE(encS.is_ok());
    EXPECT_EQ(encB.unwrap(), encS.unwrap());   // 指令序列编码逐位一致

    ASSERT_TRUE(lowerMethod(*sBatch, 0).is_ok());
    ASSERT_TRUE(lowerMethod(*sSeq, 0).is_ok());
    EXPECT_EQ(jbc::test::writeOk(sBatch->backing()), jbc::test::writeOk(sSeq->backing()));

    // 重读：goto A 的目标解析为 nop（原 B 的承接者 C），goto D 仍指 return。
    auto s3 = sessionOf(jbc::test::writeOk(sBatch->backing()));
    ASSERT_NE(s3, nullptr);
    auto p3 = promoteMethod(*s3, 0);
    ASSERT_TRUE(p3.is_ok());
    jbc::ir::CodeUnit& cu3 = *p3.unwrap();
    ASSERT_EQ(cu3.dom.size(), 5u);
    const auto tA = cu3.dom.resolveLabel(cu3.dom.peek(cu3.dom.order()[0])->target);
    ASSERT_TRUE(tA.is_ok());
    ASSERT_FALSE(tA.unwrap().isEnd());
    EXPECT_EQ(cu3.dom.peek(tA.unwrap().insn)->op, Op::NOP);
    const auto tD = cu3.dom.resolveLabel(cu3.dom.peek(cu3.dom.order()[2])->target);
    ASSERT_TRUE(tD.is_ok());
    EXPECT_EQ(cu3.dom.peek(tD.unwrap().insn)->op, Op::RETURN);
}

TEST(IrSessionTest, EditorEraseBatchAllOrNothing)
{
    const Bytes cls = makeChainClass();
    auto s = sessionOf(cls);
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    jbc::ir::CodeUnit& cu = *p.unwrap();
    CodeEditor ed(cu);

    // 批内含被 goto 引用的成员且无承接安排 → 整批拒绝（reject 策略）。
    const std::vector<jbc::ir::InsnId> used{cu.dom.order()[1], cu.dom.order()[2]};
    auto rej = ed.eraseBatch(used);
    ASSERT_TRUE(rej.is_err());
    EXPECT_EQ(rej.unwrap_err(), jbc::ir::EditError::StillUsed);
    EXPECT_EQ(cu.dom.size(), 8u);   // 一个都没删

    // retarget 策略的批量版：异常表边界成员不可机械改锚，整批拒绝。
    const Bytes excCls = makeExcClass(0, 2, 1);
    auto s2 = sessionOf(excCls);
    ASSERT_NE(s2, nullptr);
    auto p2 = promoteMethod(*s2, 0);
    ASSERT_TRUE(p2.is_ok());
    jbc::ir::CodeUnit& cu2 = *p2.unwrap();
    CodeEditor ed2(cu2);
    const std::vector<jbc::ir::InsnId> excDoomed{cu2.dom.order()[0], cu2.dom.order()[1]};
    auto rej2 = ed2.eraseBatch(excDoomed, CodeEditor::ErasePolicy::kRetargetToFallthrough);
    ASSERT_TRUE(rej2.is_err());
    EXPECT_EQ(rej2.unwrap_err(), jbc::ir::EditError::StillUsed);
    EXPECT_EQ(cu2.dom.size(), 3u);
}

TEST(IrSessionTest, EditorEraseBatchBeatsSequentialScanCost)
{
    // 成本模型：N 次单删 O(N×n) vs 一次批删 O(n)。大方法 12000 条指令、
    // 批删 3000 条：两份会话同集合对跑，批量必须更快且产物逐位一致。
    constexpr ca::usize kInsns   = 12000;
    constexpr ca::usize kErase   = 3000;
    constexpr ca::usize kStep    = 4;   // 每 4 条删 1（i % kStep == 1）
    Bytes code(kInsns - 1, 0x00);
    code.push_back(0xb1);
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
    std::vector<AttrSpec> methodAttrs;
    methodAttrs.push_back(AttrSpec{codeIdx, jbc::test::codeContent(0, 0, code)});
    cb.addMethod(0x0009, mIdx, voidIdx, std::move(methodAttrs));
    const Bytes cls = cb.build();

    auto sB = sessionOf(cls);
    auto sS = sessionOf(cls);
    ASSERT_NE(sB, nullptr);
    ASSERT_NE(sS, nullptr);
    auto pB = promoteMethod(*sB, 0);
    auto pS = promoteMethod(*sS, 0);
    ASSERT_TRUE(pB.is_ok());
    ASSERT_TRUE(pS.is_ok());
    jbc::ir::CodeUnit& cuB = *pB.unwrap();
    jbc::ir::CodeUnit& cuS = *pS.unwrap();
    CodeEditor edB(cuB);
    CodeEditor edS(cuS);
    ASSERT_EQ(cuB.dom.size(), kInsns);

    std::vector<jbc::ir::InsnId> idsB;
    std::vector<jbc::ir::InsnId> idsS;
    const auto& orderB = cuB.dom.order();
    const auto& orderS = cuS.dom.order();
    for (ca::usize i = 1; i + 1 < kInsns && idsB.size() < kErase; i += kStep) {
        idsB.push_back(orderB[i]);   // 句柄按 owner 归属：两会话各持一份
        idsS.push_back(orderS[i]);
    }
    ASSERT_EQ(idsB.size(), kErase);

    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_TRUE(edB.eraseBatch(idsB).is_ok());
    const auto t1 = std::chrono::steady_clock::now();
    for (const jbc::ir::InsnId& id : idsS) {
        auto r = edS.erase(id);
        ASSERT_TRUE(r.is_ok()) << jbc::ir::to_string(r.unwrap_err());
    }
    const auto t2 = std::chrono::steady_clock::now();

    const auto us = [](auto a, auto b) {
        return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count();
    };
    const auto batchUs = us(t0, t1);
    const auto seqUs   = us(t1, t2);
    printf("[eraseBatch-cost] insns=%u erased=%u batch=%lldus sequential=%lldus (%lldx)\n",
           static_cast<ca::u32>(kInsns), static_cast<ca::u32>(kErase),
           static_cast<long long>(batchUs), static_cast<long long>(seqUs),
           seqUs > 0 ? static_cast<long long>(seqUs / (batchUs > 0 ? batchUs : 1)) : 0LL);

    EXPECT_EQ(cuB.dom.size(), kInsns - kErase);
    EXPECT_EQ(cuS.dom.size(), kInsns - kErase);
    EXPECT_LT(batchUs, seqUs);   // 分析成本 O(n) vs O(N×n)；量级差留数据说话
    auto bB = cuB.dom.encode();
    auto bS = cuS.dom.encode();
    ASSERT_TRUE(bB.is_ok());
    ASSERT_TRUE(bS.is_ok());
    EXPECT_EQ(bB.unwrap(), bS.unwrap());
}

// ---------------------------------------------------------------------------
// 区间搬移原语 moveRange（issue #10）：段整体重定位、标签随迁、段首边界
// 策略与原子性
// ---------------------------------------------------------------------------

namespace {

/// iconst_1; pop; return、无嵌套属性的最小类：组合测试要在删除原始指令
/// 后 lower，带 LNT 的基座会按 relocation contract 拒绝悬垂 pc 引用。
Bytes makePlainClass()
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
    const Bytes code = {0x04, 0x57, 0xb1};   // iconst_1(0); pop(1); return(2)
    cb.addMethod(0x0009, mIdx, voidIdx,
                 {AttrSpec{codeIdx, jbc::test::codeContent(1, 0, code, {}, {})}});
    return cb.build();
}

}   // namespace


TEST(IrSessionTest, MoveRangeMovesSegmentAndKeepsReferences)
{
    // 基座 [iconst_1, pop, return] 上搭：T0: goto Lh; N1(Lh); N2(Lb);
    // N3(Lt); G1: goto Lb。段 [N1, G1) = {N1,N2,N3}：段首标签 Lh 被段外
    // T0 引用，段内标签 Lb 被段外（exclusive 端）G1 引用。整体搬到 pop
    // 之前：段内相对顺序不变，标签锚定随句柄自然随迁，段外引用仍指向
    // 同一逻辑指令；lower 重读后布局逐位成立。
    auto s = sessionOf(makeEditableClass(false));
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    jbc::ir::CodeUnit& cu = *p.unwrap();
    CodeEditor ed(cu);
    ASSERT_EQ(cu.dom.size(), 3u);

    const auto ic  = cu.dom.order()[0];   // iconst_1
    const auto pop = cu.dom.order()[1];   // pop
    const auto ret = cu.dom.order()[2];   // return

    auto lh = ed.createLabel();
    auto lb = ed.createLabel();
    auto lt = ed.createLabel();
    ASSERT_TRUE(lh.is_ok() && lb.is_ok() && lt.is_ok());
    auto n1 = ed.append(CodeEditor::simple(Op::NOP));
    ASSERT_TRUE(n1.is_ok());
    ASSERT_TRUE(ed.bindLabel(lh.unwrap(), n1.unwrap()).is_ok());
    auto n2 = ed.append(CodeEditor::simple(Op::NOP));
    ASSERT_TRUE(n2.is_ok());
    ASSERT_TRUE(ed.bindLabel(lb.unwrap(), n2.unwrap()).is_ok());
    auto n3 = ed.append(CodeEditor::simple(Op::NOP));
    ASSERT_TRUE(n3.is_ok());
    ASSERT_TRUE(ed.bindLabel(lt.unwrap(), n3.unwrap()).is_ok());
    auto g1 = ed.append(CodeEditor::branch(Op::GOTO, lb.unwrap()));
    ASSERT_TRUE(g1.is_ok());
    auto t0 = ed.insertAtStart(CodeEditor::branch(Op::GOTO, lh.unwrap()));
    ASSERT_TRUE(t0.is_ok());
    // 布局：T0 iconst_1 pop return N1 N2 N3 G1。
    ASSERT_EQ(cu.dom.size(), 8u);

    const auto segFirst = n1.unwrap();
    const auto segLast  = g1.unwrap();   // exclusive 端：段 = {N1,N2,N3}

    // 无操作锚点（锚即段首 / 锚紧随段尾）：合法且不触发布局与改绑。
    ASSERT_TRUE(ed.moveRange(segFirst, segLast, segFirst).is_ok());
    ASSERT_TRUE(ed.moveRange(segFirst, segLast, segLast).is_ok());
    ASSERT_EQ(cu.dom.order()[0], t0.unwrap());
    ASSERT_EQ(cu.dom.order()[4], segFirst);

    // 整体搬到 pop 之前：T0 iconst_1 | N1 N2 N3 | pop return G1。
    ASSERT_TRUE(ed.moveRange(segFirst, segLast, pop).is_ok());
    const std::vector<jbc::ir::InsnId> want = {
        t0.unwrap(), ic, segFirst, n2.unwrap(), n3.unwrap(), pop, ret, segLast};
    const auto& order = cu.dom.order();
    ASSERT_EQ(order.size(), want.size());
    for (ca::usize i = 0; i < want.size(); ++i) {
        EXPECT_EQ(order[i], want[i]);
    }

    // 标签随段迁移：句柄未变，锚点指令未变，只是指令换了程序位置。
    const auto th = cu.dom.resolveLabel(lh.unwrap());
    ASSERT_TRUE(th.is_ok());
    EXPECT_EQ(th.unwrap().insn, segFirst);
    const auto tb = cu.dom.resolveLabel(lb.unwrap());
    ASSERT_TRUE(tb.is_ok());
    EXPECT_EQ(tb.unwrap().insn, n2.unwrap());
    const auto tt = cu.dom.resolveLabel(lt.unwrap());
    ASSERT_TRUE(tt.is_ok());
    EXPECT_EQ(tt.unwrap().insn, n3.unwrap());

    // lower 重读：迁移后的逻辑引用原样落盘（T0→N1、G1→N2）。
    ASSERT_TRUE(cu.dom.encode().is_ok());
    ASSERT_TRUE(lowerMethod(*s, 0).is_ok());
    auto s2 = sessionOf(jbc::test::writeOk(s->backing()));
    ASSERT_NE(s2, nullptr);
    auto p2 = promoteMethod(*s2, 0);
    ASSERT_TRUE(p2.is_ok());
    jbc::ir::CodeUnit& cu2 = *p2.unwrap();
    ASSERT_EQ(cu2.dom.size(), 8u);
    const auto& o2    = cu2.dom.order();
    const auto& head0 = *cu2.dom.peek(o2[0]);
    EXPECT_EQ(head0.op, Op::GOTO);
    const auto t0t = cu2.dom.resolveLabel(head0.target);
    ASSERT_TRUE(t0t.is_ok());
    EXPECT_EQ(t0t.unwrap().insn, o2[2]);
    EXPECT_EQ(cu2.dom.peek(o2[2])->op, Op::NOP);
    const auto& tail7 = *cu2.dom.peek(o2[7]);
    EXPECT_EQ(tail7.op, Op::GOTO);
    const auto g1t = cu2.dom.resolveLabel(tail7.target);
    ASSERT_TRUE(g1t.is_ok());
    EXPECT_EQ(g1t.unwrap().insn, o2[3]);
    EXPECT_EQ(cu2.dom.peek(o2[3])->op, Op::NOP);
}

TEST(IrSessionTest, MoveRangeHeadStaysRebindsHeadLabels)
{
    // 段首边界标签的「留原地」策略：锚点重定向到原段位后继，标签句柄
    // 身份不变，段外引用跟着指到承接者；尾段（last 用尾哨兵）的承接者
    // 是方法末边界，此时仍被控制字段引用的段首标签显式拒绝。
    auto s = sessionOf(makeEditableClass(false));
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    jbc::ir::CodeUnit& cu = *p.unwrap();
    CodeEditor ed(cu);
    ASSERT_EQ(cu.dom.size(), 3u);

    const auto ic  = cu.dom.order()[0];
    const auto pop = cu.dom.order()[1];
    const auto ret = cu.dom.order()[2];

    // 段 [N1, N3) = {N1,N2}，段首标签 Lh 被段外 G1 引用。
    auto lh = ed.createLabel();
    ASSERT_TRUE(lh.is_ok());
    auto n1 = ed.append(CodeEditor::simple(Op::NOP));
    ASSERT_TRUE(n1.is_ok());
    ASSERT_TRUE(ed.bindLabel(lh.unwrap(), n1.unwrap()).is_ok());
    auto n2 = ed.append(CodeEditor::simple(Op::NOP));
    ASSERT_TRUE(n2.is_ok());
    auto n3 = ed.append(CodeEditor::simple(Op::NOP));
    ASSERT_TRUE(n3.is_ok());
    auto g1 = ed.append(CodeEditor::branch(Op::GOTO, lh.unwrap()));
    ASSERT_TRUE(g1.is_ok());
    // 布局：iconst_1 pop return N1 N2 N3 G1。

    // 无操作锚点下的 kHeadStays 也不改绑：Lh 仍锚 N1。
    ASSERT_TRUE(ed.moveRange(n1.unwrap(), n3.unwrap(), n1.unwrap(),
                             CodeEditor::MoveRangePolicy::kHeadStays)
                    .is_ok());
    EXPECT_EQ(cu.dom.resolveLabel(lh.unwrap()).unwrap().insn, n1.unwrap());

    // 整体搬到方法首：Lh 留在原位，锚点重定向到原段位后继 N3；
    // 引用它的 G1 跟着重定向（引用重定向到原段位后继）。
    ASSERT_TRUE(ed.moveRange(n1.unwrap(), n3.unwrap(), ic,
                             CodeEditor::MoveRangePolicy::kHeadStays)
                    .is_ok());
    const std::vector<jbc::ir::InsnId> want = {
        n1.unwrap(), n2.unwrap(), ic, pop, ret, n3.unwrap(), g1.unwrap()};
    const auto& order = cu.dom.order();
    ASSERT_EQ(order.size(), want.size());
    for (ca::usize i = 0; i < want.size(); ++i) {
        EXPECT_EQ(order[i], want[i]);
    }
    EXPECT_TRUE(cu.dom.validLabel(lh.unwrap()));
    EXPECT_EQ(cu.dom.resolveLabel(lh.unwrap()).unwrap().insn, n3.unwrap());
    const auto& g1Insn = *cu.dom.peek(g1.unwrap());
    ASSERT_EQ(g1Insn.target, lh.unwrap());   // 引用句柄本身未换
    const auto g1t = cu.dom.resolveLabel(g1Insn.target);
    ASSERT_TRUE(g1t.is_ok());
    EXPECT_EQ(g1t.unwrap().insn, n3.unwrap());

    // 尾段 [N4, end)（last = 尾哨兵）+ 无引用段首标签 Lm：kHeadStays 把
    // Lm 改绑到方法末边界（resolveLabel → End）。
    auto lm = ed.createLabel();
    ASSERT_TRUE(lm.is_ok());
    auto n4 = ed.append(CodeEditor::simple(Op::NOP));
    ASSERT_TRUE(n4.is_ok());
    ASSERT_TRUE(ed.bindLabel(lm.unwrap(), n4.unwrap()).is_ok());
    ASSERT_TRUE(ed.moveRange(n4.unwrap(), jbc::ir::kNoInsn, ic,
                             CodeEditor::MoveRangePolicy::kHeadStays)
                    .is_ok());
    const auto lmTarget = cu.dom.resolveLabel(lm.unwrap());
    ASSERT_TRUE(lmTarget.is_ok());
    EXPECT_TRUE(lmTarget.unwrap().isEnd());

    // 尾段段首标签被控制字段引用：kHeadStays 拒绝（end 锚定的控制目标
    // 不可表达），kHeadFollows 照常随段迁移。
    auto lq = ed.createLabel();
    ASSERT_TRUE(lq.is_ok());
    auto n5 = ed.append(CodeEditor::simple(Op::NOP));
    ASSERT_TRUE(n5.is_ok());
    ASSERT_TRUE(ed.bindLabel(lq.unwrap(), n5.unwrap()).is_ok());
    auto g2 = ed.append(CodeEditor::branch(Op::GOTO, lq.unwrap()));
    ASSERT_TRUE(g2.is_ok());
    auto n6 = ed.append(CodeEditor::simple(Op::NOP));
    ASSERT_TRUE(n6.is_ok());
    {
        auto rej = ed.moveRange(n5.unwrap(), jbc::ir::kNoInsn, ic,
                                CodeEditor::MoveRangePolicy::kHeadStays);
        ASSERT_TRUE(rej.is_err());
        EXPECT_EQ(rej.unwrap_err(), jbc::ir::EditError::StillUsed);
    }
    {
        auto ok = ed.moveRange(n5.unwrap(), jbc::ir::kNoInsn, ic,
                               CodeEditor::MoveRangePolicy::kHeadFollows);
        ASSERT_TRUE(ok.is_ok()) << jbc::ir::to_string(ok.unwrap_err());
    }
    // lq 锚点随段走（仍指 N5），G2 的引用照常解析。
    EXPECT_EQ(cu.dom.resolveLabel(lq.unwrap()).unwrap().insn, n5.unwrap());
    const auto& g2Insn = *cu.dom.peek(g2.unwrap());
    ASSERT_EQ(g2Insn.target, lq.unwrap());
    const auto g2t = cu.dom.resolveLabel(g2Insn.target);
    ASSERT_TRUE(g2t.is_ok());
    EXPECT_EQ(g2t.unwrap().insn, n5.unwrap());
    ASSERT_TRUE(cu.dom.encode().is_ok());
}

TEST(IrSessionTest, MoveRangeRejectsBadGeometry)
{
    // 空段 / 倒置段 / 锚点在段内 → BadRange；坏句柄 → InvalidHandle；
    // 尾哨兵 last 是合法形；失败后编辑器状态原样可继续用（原子性）。
    auto s = sessionOf(makeEditableClass(false));
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    jbc::ir::CodeUnit& cu = *p.unwrap();
    CodeEditor ed(cu);
    ASSERT_EQ(cu.dom.size(), 3u);

    const auto ic  = cu.dom.order()[0];
    const auto pop = cu.dom.order()[1];
    const auto ret = cu.dom.order()[2];
    auto n1 = ed.append(CodeEditor::simple(Op::NOP));
    auto n2 = ed.append(CodeEditor::simple(Op::NOP));
    auto n3 = ed.append(CodeEditor::simple(Op::NOP));
    ASSERT_TRUE(n1.is_ok() && n2.is_ok() && n3.is_ok());
    // 布局：iconst_1 pop return N1 N2 N3。

    {   // 空段：first == last。
        auto r = ed.moveRange(n1.unwrap(), n1.unwrap(), ic);
        ASSERT_TRUE(r.is_err());
        EXPECT_EQ(r.unwrap_err(), jbc::ir::EditError::BadRange);
    }
    {   // 倒置段：first 在 last 之后。
        auto r = ed.moveRange(n2.unwrap(), n1.unwrap(), ic);
        ASSERT_TRUE(r.is_err());
        EXPECT_EQ(r.unwrap_err(), jbc::ir::EditError::BadRange);
    }
    {   // 锚点在段内：锚会被搬移卷走。
        auto r = ed.moveRange(ic, ret, pop);
        ASSERT_TRUE(r.is_err());
        EXPECT_EQ(r.unwrap_err(), jbc::ir::EditError::BadRange);
    }
    {   // 坏句柄：kNoInsn 段首（段首必须是真实成员）与跨 DOM 假句柄。
        auto r1 = ed.moveRange(jbc::ir::kNoInsn, n1.unwrap(), ic);
        ASSERT_TRUE(r1.is_err());
        EXPECT_EQ(r1.unwrap_err(), jbc::ir::EditError::InvalidHandle);
        const jbc::ir::InsnId fake{cu.dom.owner() + 4096, 0, 0};
        auto r2 = ed.moveRange(n1.unwrap(), n2.unwrap(), fake);
        ASSERT_TRUE(r2.is_err());
        EXPECT_EQ(r2.unwrap_err(), jbc::ir::EditError::InvalidHandle);
    }
    // 连续失败不留半搬状态：布局原样，编辑器可继续正常工作。
    const std::vector<jbc::ir::InsnId> want = {
        ic, pop, ret, n1.unwrap(), n2.unwrap(), n3.unwrap()};
    const auto& order = cu.dom.order();
    ASSERT_EQ(order.size(), want.size());
    for (ca::usize i = 0; i < want.size(); ++i) {
        EXPECT_EQ(order[i], want[i]);
    }
    // 尾段 [N1, end) 整体搬到方法末尾 = 已在位置：合法无操作。
    ASSERT_TRUE(ed.moveRange(n1.unwrap(), jbc::ir::kNoInsn, jbc::ir::kNoInsn)
                    .is_ok());
    EXPECT_EQ(cu.dom.order().size(), want.size());
    auto enc = cu.dom.encode();
    ASSERT_TRUE(enc.is_ok());
}

TEST(IrSessionTest, MoveRangeExcBoundaryPolicyAndAtomicity)
{
    // 异常表边界引用的段首标签：kHeadStays 不可机械改绑（改边界 = 改保护
    // 区间语义，与 erase 的 fallthrough 策略同规）→ StillUsed，且失败现场
    // 逐位原样（原子性）；kHeadFollows 下异常边界随段整体迁移，lower 重读
    // 后保护区间指向搬移后的布局。exc 布局 start@iconst_1、end@pop（段内
    // 双边界）、handler@return（段外），搬移后区间仍非退化。
    const Bytes cls = makeExcClass(0, 1, 2);
    auto s = sessionOf(cls);
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    jbc::ir::CodeUnit& cu = *p.unwrap();
    CodeEditor ed(cu);
    ASSERT_EQ(cu.exc.size(), 1u);

    const auto startId = cu.dom.order()[0];   // iconst_1 = exc.start
    const auto popId   = cu.dom.order()[1];   // pop      = exc.end
    const auto retId   = cu.dom.order()[2];   // return   = exc.handler
    auto tail = ed.append(CodeEditor::simple(Op::NOP));   // 锚点候选（段外）
    ASSERT_TRUE(tail.is_ok());

    const auto orderBefore = cu.dom.order();
    const auto startBefore = cu.dom.resolveLabel(cu.exc[0].start);
    ASSERT_TRUE(startBefore.is_ok());

    {   // 段 [iconst_1, return) = {iconst_1, pop}，段首标签 = exc.start。
        auto rej = ed.moveRange(startId, retId, tail.unwrap(),
                                CodeEditor::MoveRangePolicy::kHeadStays);
        ASSERT_TRUE(rej.is_err());
        EXPECT_EQ(rej.unwrap_err(), jbc::ir::EditError::StillUsed);
    }
    // 原子性：失败后顺序与边界解析逐位原样，编辑器照常可用。
    ASSERT_EQ(cu.dom.order(), orderBefore);
    const auto startAfter = cu.dom.resolveLabel(cu.exc[0].start);
    ASSERT_TRUE(startAfter.is_ok());
    EXPECT_EQ(startAfter.unwrap().insn, startBefore.unwrap().insn);

    {   // kHeadFollows：异常边界随段整体迁移（标签锚定不动，代码搬家）。
        auto ok = ed.moveRange(startId, retId, tail.unwrap(),
                               CodeEditor::MoveRangePolicy::kHeadFollows);
        ASSERT_TRUE(ok.is_ok()) << jbc::ir::to_string(ok.unwrap_err());
    }
    // 布局 [return, iconst_1, pop, nop]：双边界随段走，handler 原位。
    EXPECT_EQ(cu.dom.resolveLabel(cu.exc[0].start).unwrap().insn, startId);
    EXPECT_EQ(cu.dom.resolveLabel(cu.exc[0].end).unwrap().insn, popId);
    EXPECT_EQ(cu.dom.resolveLabel(cu.exc[0].handler).unwrap().insn, retId);
    // lower 重读：保护区间 [1, 2) 覆盖搬移后的 {iconst_1, pop}，handler@0。
    ASSERT_TRUE(lowerMethod(*s, 0).is_ok());
    auto s2 = sessionOf(jbc::test::writeOk(s->backing()));
    ASSERT_NE(s2, nullptr);
    auto dc = s2->backing().decodeAttr(s2->backing().methods[0].attrs[0]);
    ASSERT_TRUE(dc.is_ok());
    auto& ca2 = static_cast<CodeAttr&>(*dc.unwrap());
    ASSERT_EQ(ca2.excTable.size(), 1u);
    EXPECT_EQ(ca2.excTable[0].startPc, 1u);
    EXPECT_EQ(ca2.excTable[0].endPc, 2u);
    EXPECT_EQ(ca2.excTable[0].handlerPc, 0u);
}

TEST(IrSessionTest, MoveRangeComposesWithInsertAtLabelAndEraseBatch)
{
    // 与其他原语组合：moveRange 尾段前移 → insertAtLabel 落在迁移后的段首
    // 标签边界 → eraseBatch 清掉原地残余 → lower 重读全链成立。
    auto s = sessionOf(makePlainClass());
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    jbc::ir::CodeUnit& cu = *p.unwrap();
    CodeEditor ed(cu);
    ASSERT_EQ(cu.dom.size(), 3u);

    const auto ic  = cu.dom.order()[0];
    const auto pop = cu.dom.order()[1];
    auto lh = ed.createLabel();
    ASSERT_TRUE(lh.is_ok());
    auto n1 = ed.append(CodeEditor::simple(Op::NOP));
    ASSERT_TRUE(n1.is_ok());
    ASSERT_TRUE(ed.bindLabel(lh.unwrap(), n1.unwrap()).is_ok());
    auto n2 = ed.append(CodeEditor::simple(Op::NOP));
    ASSERT_TRUE(n2.is_ok());
    // 布局：iconst_1 pop return N1(Lh) N2。

    // 尾段 [N1, end) = {N1,N2} 整体搬到方法首。
    ASSERT_TRUE(ed.moveRange(n1.unwrap(), jbc::ir::kNoInsn, ic).is_ok());
    ASSERT_EQ(cu.dom.order()[0], n1.unwrap());
    ASSERT_EQ(cu.dom.order()[1], n2.unwrap());

    // insertAtLabel 在迁移后的段首标签边界插入：新头接管标签。
    auto n3 = ed.insertAtLabel(lh.unwrap(), CodeEditor::simple(Op::NOP));
    ASSERT_TRUE(n3.is_ok());
    ASSERT_EQ(cu.dom.order()[0], n3.unwrap());
    EXPECT_EQ(cu.dom.resolveLabel(lh.unwrap()).unwrap().insn, n3.unwrap());

    // eraseBatch 清掉原基座残余（retarget 策略，pop 的承接者是 return）。
    std::vector<jbc::ir::InsnId> doomed = {ic, pop};
    ASSERT_TRUE(ed.eraseBatch(doomed, CodeEditor::ErasePolicy::kRetargetToFallthrough)
                    .is_ok());
    ASSERT_EQ(cu.dom.size(), 4u);
    const auto& order = cu.dom.order();
    EXPECT_EQ(order[0], n3.unwrap());
    EXPECT_EQ(order[1], n1.unwrap());
    EXPECT_EQ(order[2], n2.unwrap());

    // lower 重读：N3 N1 N2 return 全链成立，标签边界落位正确。
    ASSERT_TRUE(lowerMethod(*s, 0).is_ok());
    auto s2 = sessionOf(jbc::test::writeOk(s->backing()));
    ASSERT_NE(s2, nullptr);
    auto p2 = promoteMethod(*s2, 0);
    ASSERT_TRUE(p2.is_ok());
    jbc::ir::CodeUnit& cu2 = *p2.unwrap();
    ASSERT_EQ(cu2.dom.size(), 4u);
    EXPECT_EQ(cu2.dom.peek(cu2.dom.order()[0])->op, Op::NOP);
    EXPECT_EQ(cu2.dom.peek(cu2.dom.order()[3])->op, Op::RETURN);
}

TEST(IrSessionTest, LowerCatchesDanglingExceptionHandle)
{
    // end 用方法末尾标签（endPc = code 长度，合法形态）：return 不被异常表
    // 边界绑定，删除合法；而绕过边界协议直接破坏 handler 标签后删除指令，
    // lower 必须显式失败（RelocationBadTarget，不落盘）。
    const Bytes cls = makeExcClass(0, 3, 1);

    // endPc 3 不落在任何指令起点 → 合法 End 标签。
    {
        auto s = sessionOf(cls);
        ASSERT_NE(s, nullptr);
        auto p = promoteMethod(*s, 0);
        ASSERT_TRUE(p.is_ok());
        jbc::ir::CodeUnit& cu = *p.unwrap();
        ASSERT_EQ(cu.exc.size(), 1u);
        const auto endTarget = cu.dom.resolveLabel(cu.exc[0].end);
        ASSERT_TRUE(endTarget.is_ok());
        EXPECT_TRUE(endTarget.unwrap().isEnd());

        // end 是方法末尾标签，不绑定末条指令 → return 删除合法。
        CodeEditor ed(cu);
        ASSERT_TRUE(ed.erase(cu.dom.order()[2]).is_ok());
        ASSERT_TRUE(lowerMethod(*s, 0).is_ok());
        auto s2 = sessionOf(jbc::test::writeOk(s->backing()));
        ASSERT_NE(s2, nullptr);
        auto dc = s2->backing().decodeAttr(s2->backing().methods[0].attrs[0]);
        ASSERT_TRUE(dc.is_ok());
        auto& ca2 = static_cast<CodeAttr&>(*dc.unwrap());
        ASSERT_EQ(ca2.excTable.size(), 1u);
        EXPECT_EQ(ca2.excTable[0].startPc, 0u);
        EXPECT_EQ(ca2.excTable[0].handlerPc, 1u);
        EXPECT_EQ(ca2.excTable[0].endPc, 2u);   // End 标签随新长度收口：3 → 2
    }

    // 越权路径：先销毁 handler 标签，再直接 dom.erase；lower 是第二道防线，
    // 发现 CodeUnit 中的 LabelId 已失效后报错且不落盘。
    {
        auto s = sessionOf(cls);
        ASSERT_NE(s, nullptr);
        auto p = promoteMethod(*s, 0);
        ASSERT_TRUE(p.is_ok());
        jbc::ir::CodeUnit& cu = *p.unwrap();
        ASSERT_TRUE(cu.dom.destroyLabel(cu.exc[0].handler).is_ok());
        ASSERT_TRUE(cu.dom.erase(cu.dom.order()[1]).is_ok());   // pop = handler
        auto r = lowerMethod(*s, 0);
        ASSERT_TRUE(r.is_err());
        EXPECT_EQ(r.unwrap_err(), jbc::ir::LowerError::RelocationBadTarget);
        EXPECT_EQ(jbc::test::writeOk(s->backing()), cls);
    }
}

TEST(IrSessionTest, PromoteRejectsExceptionEndInsideInstruction)
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

    const Bytes code = {0x11, 0x00, 0x01, 0x57, 0xb1}; // sipush; pop; return
    const auto codeBody = jbc::test::codeContent(
        1, 0, code, {ExcSpec{0, 1, 3, 0}}); // end_pc=1 points inside sipush
    cb.addMethod(0x0009, mIdx, voidIdx, {AttrSpec{codeIdx, codeBody}});

    auto s = sessionOf(cb.build());
    ASSERT_NE(s, nullptr);
    const auto promoted = promoteMethod(*s, 0);
    ASSERT_TRUE(promoted.is_err());
    EXPECT_EQ(promoted.unwrap_err(), jbc::ir::PromoteError::BadInstructions);
}

TEST(IrSessionTest, InsertAtEndLabelAppendsWithEndBoundaryPreserved)
{
    // End 是开区间末端而非「当前末条指令」：insertAtLabel 只追加指令，
    // 标签继续解析为方法末尾；lower 后异常表 end_pc 跟随新 code 长度。
    const Bytes cls = makeExcClass(0, 3, 1);   // end_pc=3 == code 长度 → End 标签
    auto s = sessionOf(cls);
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    jbc::ir::CodeUnit& cu = *p.unwrap();
    CodeEditor ed(cu);

    const auto before = cu.dom.resolveLabel(cu.exc[0].end);
    ASSERT_TRUE(before.is_ok());
    ASSERT_TRUE(before.unwrap().isEnd());

    auto r = ed.insertAtLabel(cu.exc[0].end, CodeEditor::simple(Op::NOP));
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(cu.dom.order().back(), r.unwrap());
    const auto after = cu.dom.resolveLabel(cu.exc[0].end);
    ASSERT_TRUE(after.is_ok());
    EXPECT_TRUE(after.unwrap().isEnd());

    ASSERT_TRUE(lowerMethod(*s, 0).is_ok());
    auto s2 = sessionOf(jbc::test::writeOk(s->backing()));
    ASSERT_NE(s2, nullptr);
    auto dc = s2->backing().decodeAttr(s2->backing().methods[0].attrs[0]);
    ASSERT_TRUE(dc.is_ok());
    auto& ca2 = static_cast<CodeAttr&>(*dc.unwrap());
    ASSERT_EQ(ca2.code.size(), 4u);
    ASSERT_EQ(ca2.excTable.size(), 1u);
    EXPECT_EQ(ca2.excTable[0].startPc, 0u);
    EXPECT_EQ(ca2.excTable[0].handlerPc, 1u);
    EXPECT_EQ(ca2.excTable[0].endPc, 4u);   // End 随新 code 长度收口：3 → 4
}

TEST(IrSessionTest, SetTargetReleasesOldControlLabelAnchor)
{
    // goto@0 +3 → nop@3；return@4。setTarget 把 goto 改指 return 后：
    // 旧控制流标签不再被字段使用，不能单独阻止删除 nop；普通 Boundary
    // 标签（元数据语义）仍然保护锚点。
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
    const Bytes code = {0xa7, 0x00, 0x03, 0x00, 0xb1};
    cb.addMethod(0x0009, mIdx, voidIdx,
                 {AttrSpec{codeIdx, jbc::test::codeContent(0, 0, code)}});

    auto s = sessionOf(cb.build());
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    jbc::ir::CodeUnit& cu = *p.unwrap();
    CodeEditor ed(cu);

    const auto gotoId = cu.dom.order()[0];
    const auto nopId  = cu.dom.order()[1];
    const auto retId  = cu.dom.order()[2];

    // 未重定向前：goto 的控制流字段仍引用 nop。
    auto rej = ed.erase(nopId);
    ASSERT_TRUE(rej.is_err());
    EXPECT_EQ(rej.unwrap_err(), jbc::ir::EditError::StillUsed);

    auto rl = cu.dom.createLabel(retId);
    ASSERT_TRUE(rl.is_ok());
    ASSERT_TRUE(ed.setTarget(gotoId, rl.unwrap()).is_ok());
    ASSERT_TRUE(ed.erase(nopId).is_ok());   // 残留旧控制流标签不阻止删除

    // 对照：Boundary 标签锚定的指令仍被保护，销毁标签后可删。
    auto n2 = ed.insertAtStart(CodeEditor::simple(Op::NOP));
    ASSERT_TRUE(n2.is_ok());
    auto meta = cu.dom.createLabel(n2.unwrap());
    ASSERT_TRUE(meta.is_ok());
    auto rej2 = ed.erase(n2.unwrap());
    ASSERT_TRUE(rej2.is_err());
    EXPECT_EQ(rej2.unwrap_err(), jbc::ir::EditError::StillUsed);
    ASSERT_TRUE(cu.dom.destroyLabel(meta.unwrap()).is_ok());
    ASSERT_TRUE(ed.erase(n2.unwrap()).is_ok());

    // 语义保持：lower 重读后 goto 仍指向 return。
    ASSERT_TRUE(lowerMethod(*s, 0).is_ok());
    auto s2 = sessionOf(jbc::test::writeOk(s->backing()));
    ASSERT_NE(s2, nullptr);
    auto p2 = promoteMethod(*s2, 0);
    ASSERT_TRUE(p2.is_ok());
    ASSERT_EQ(p2.unwrap()->dom.size(), 2u);
    const auto& g = *p2.unwrap()->dom.peek(p2.unwrap()->dom.order()[0]);
    EXPECT_EQ(g.op, Op::GOTO);
    const auto t = p2.unwrap()->dom.resolveLabel(g.target);
    ASSERT_TRUE(t.is_ok());
    ASSERT_FALSE(t.unwrap().isEnd());
    EXPECT_EQ(t.unwrap().insn, p2.unwrap()->dom.last());
    EXPECT_EQ(p2.unwrap()->dom.peek(p2.unwrap()->dom.last())->op, Op::RETURN);
}

TEST(IrSessionTest, LowerRejectsLvtEndpointInsideInstruction)
{
    // sipush(0..3); pop(3); return(4) + LVT 行 start=0, length=2 → oldEnd=2
    // 落在 sipush 操作数字节中间。布局改变后 lower 必须 fail-closed 返回
    // RelocationBadTarget，且 backing/CP/嵌套属性状态都没有半提交变化。
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 codeIdx = cb.utf8("Code");
    const ca::u16 lvtIdx  = cb.utf8("LocalVariableTable");
    const ca::u16 nameIdx = cb.utf8("this");
    const ca::u16 intIdx = cb.utf8("I");
    const ca::u16 mIdx = cb.utf8("m");
    const ca::u16 voidIdx = cb.utf8("()V");
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setAccess(0x0021);
    cb.setVersion(52);

    const Bytes code = {0x11, 0x00, 0x01, 0x57, 0xb1};
    std::vector<AttrSpec> nested;
    nested.push_back(AttrSpec{lvtIdx, {0x00, 0x01, 0x00, 0x00, 0x00, 0x02,
                                       static_cast<ca::u8>(nameIdx >> 8),
                                       static_cast<ca::u8>(nameIdx & 0xFF),
                                       static_cast<ca::u8>(intIdx >> 8),
                                       static_cast<ca::u8>(intIdx & 0xFF),
                                       0x00, 0x01}});
    cb.addMethod(0x0009, mIdx, voidIdx,
                 {AttrSpec{codeIdx, jbc::test::codeContent(1, 1, code, {}, nested)}});
    const Bytes cls = cb.build();

    auto s = sessionOf(cls);
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    jbc::ir::CodeUnit& cu = *p.unwrap();
    CodeEditor ed(cu);
    ASSERT_TRUE(ed.insertAtStart(CodeEditor::simple(Op::NOP)).is_ok());

    const ca::u32 cpSize = s->backing().cp.size();
    const ca::u32 oldNameCp = s->backing().methods[0].nameCp;
    s->unit().methods[0].name = s->unit().symbols().intern("renamed");

    const auto r = lowerMethod(*s, 0);
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), jbc::ir::LowerError::RelocationBadTarget);
    EXPECT_EQ(s->backing().cp.size(), cpSize);
    EXPECT_EQ(s->backing().methods[0].nameCp, oldNameCp);
    EXPECT_EQ(jbc::test::writeOk(s->backing()), cls);

    auto dc = s->backing().decodeAttr(s->backing().methods[0].attrs[0]);
    ASSERT_TRUE(dc.is_ok());
    auto& codeAttr = static_cast<CodeAttr&>(*dc.unwrap());
    auto dl = s->backing().decodeAttr(codeAttr.attrs[0]);
    ASSERT_TRUE(dl.is_ok());
    const auto& lvt = static_cast<const LocalVariableTableAttr&>(*dl.unwrap());
    ASSERT_EQ(lvt.rows.size(), 1u);
    EXPECT_EQ(lvt.rows[0].startPc, 0u);
    EXPECT_EQ(lvt.rows[0].length, 2u);
    EXPECT_EQ(codeAttr.attrs[0].state, AttrSlot::State::Decoded);
}

TEST(IrSessionTest, EditRelocatesLocalVariableTable)
{
    // iconst_1; pop; return + LVT 行(start=0, len=3, name, desc, slot=1)。
    // 入口插 nop 后：LVT 行 start 0→1；end 3（方法末尾，非指令 pc）钳到
    // 新长度 4 → length 3 保持。
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 codeIdx = cb.utf8("Code");
    const ca::u16 lvtIdx  = cb.utf8("LocalVariableTable");
    const ca::u16 nameIdx = cb.utf8("this");
    const ca::u16 intIdx = cb.utf8("I");
    const ca::u16 mIdx = cb.utf8("m");
    const ca::u16 voidIdx = cb.utf8("()V");
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setAccess(0x0021);
    cb.setVersion(52);

    const Bytes code = {0x04, 0x57, 0xb1};
    std::vector<AttrSpec> nested;
    nested.push_back(AttrSpec{lvtIdx, {0x00, 0x01, 0x00, 0x00, 0x00, 0x03,
                                        static_cast<ca::u8>(nameIdx >> 8),
                                        static_cast<ca::u8>(nameIdx & 0xFF),
                                        static_cast<ca::u8>(intIdx >> 8),
                                        static_cast<ca::u8>(intIdx & 0xFF),
                                        0x00, 0x01}});
    std::vector<AttrSpec> methodAttrs;
    methodAttrs.push_back(AttrSpec{codeIdx, jbc::test::codeContent(1, 1, code, {}, nested)});
    cb.addMethod(0x0009, mIdx, voidIdx, std::move(methodAttrs));

    auto s = sessionOf(cb.build());
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    CodeEditor ed(*p.unwrap());
    ASSERT_TRUE(ed.insertBefore(p.unwrap()->dom.order()[0], CodeEditor::simple(Op::NOP)).is_ok());
    ASSERT_TRUE(lowerMethod(*s, 0).is_ok());

    auto s2 = sessionOf(jbc::test::writeOk(s->backing()));
    ASSERT_NE(s2, nullptr);
    auto dc = s2->backing().decodeAttr(s2->backing().methods[0].attrs[0]);
    ASSERT_TRUE(dc.is_ok());
    auto& code2 = static_cast<CodeAttr&>(*dc.unwrap());
    ASSERT_EQ(code2.attrs.size(), 1u);
    auto dl = s2->backing().decodeAttr(code2.attrs[0]);
    ASSERT_TRUE(dl.is_ok());
    const auto& lvt = static_cast<const LocalVariableTableAttr&>(*dl.unwrap());
    ASSERT_EQ(lvt.rows.size(), 1u);
    EXPECT_EQ(lvt.rows[0].startPc, 1u);
    EXPECT_EQ(lvt.rows[0].length, 3u);
}

TEST(IrSessionTest, EditRelocatesLocalVariableTypeTableViaLvtChannel)
{
    // LVTT 与 LVT 同 kind（kind 6）合流：属性名不同、行形同构（start/len/
    // name/sig/slot）。两条属性各挂一行（start=1, len=1），在 pop 后插 nop
    // （old pc 0,1,2 → new 0,1,3）：start 1→1，旧终点 2 命中 pcmap → 新终点
    // 3 → length 1→2（range 跨过插入的 nop）。重读断言两条都重定位且 LVTT
    // 槽位的 typed kind 就是 LocalVariableTable（流入 relocateLvt 的证据）。
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 codeIdx = cb.utf8("Code");
    const ca::u16 lvtIdx = cb.utf8("LocalVariableTable");
    const ca::u16 lvttIdx = cb.utf8("LocalVariableTypeTable");
    const ca::u16 nameIdx = cb.utf8("v");
    const ca::u16 descIdx = cb.utf8("Ljava/util/List;");
    const ca::u16 sigIdx = cb.utf8("Ljava/util/List<Ljava/lang/String;>;");
    const ca::u16 mIdx = cb.utf8("m");
    const ca::u16 voidIdx = cb.utf8("()V");
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setAccess(0x0021);
    cb.setVersion(52);

    auto lvtRow = [nameIdx](ca::u16 typeIdx) {
        return Bytes{0x00, 0x01,                     // row_count = 1
                     0x00, 0x01, 0x00, 0x01,         // start_pc=1, length=1
                     static_cast<ca::u8>(nameIdx >> 8),
                     static_cast<ca::u8>(nameIdx & 0xFF),
                     static_cast<ca::u8>(typeIdx >> 8),
                     static_cast<ca::u8>(typeIdx & 0xFF),
                     0x00, 0x01};                    // slot=1
    };
    const Bytes code = {0x04, 0x57, 0xb1};   // iconst_1; pop; return
    std::vector<AttrSpec> nested;
    nested.push_back(AttrSpec{lvtIdx, lvtRow(descIdx)});
    nested.push_back(AttrSpec{lvttIdx, lvtRow(sigIdx)});
    std::vector<AttrSpec> methodAttrs;
    methodAttrs.push_back(AttrSpec{codeIdx, jbc::test::codeContent(1, 2, code, {}, nested)});
    cb.addMethod(0x0009, mIdx, voidIdx, std::move(methodAttrs));

    auto s = sessionOf(cb.build());
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    jbc::ir::CodeUnit& cu = *p.unwrap();
    CodeEditor ed(cu);
    ASSERT_TRUE(ed.insertAfter(cu.dom.order()[1], CodeEditor::simple(Op::NOP)).is_ok());
    ASSERT_TRUE(lowerMethod(*s, 0).is_ok());

    auto s2 = sessionOf(jbc::test::writeOk(s->backing()));
    ASSERT_NE(s2, nullptr);
    auto dc = s2->backing().decodeAttr(s2->backing().methods[0].attrs[0]);
    ASSERT_TRUE(dc.is_ok());
    auto& code2 = static_cast<CodeAttr&>(*dc.unwrap());
    ASSERT_EQ(code2.attrs.size(), 2u);

    // LVTT 槽位与 LVT 槽位同 typed kind（kind 6 合流，重用同一条 relocateLvt）。
    for (AttrSlot& slot : code2.attrs) {
        auto d = s2->backing().decodeAttr(slot);
        ASSERT_TRUE(d.is_ok());
        EXPECT_EQ(d.unwrap()->kind(), AttrKind::LocalVariableTable);
    }
    auto dl = s2->backing().decodeAttr(code2.attrs[0]);
    ASSERT_TRUE(dl.is_ok());
    const auto& lvt = static_cast<const LocalVariableTableAttr&>(*dl.unwrap());
    ASSERT_EQ(lvt.rows.size(), 1u);
    EXPECT_EQ(lvt.rows[0].startPc, 1u);
    EXPECT_EQ(lvt.rows[0].length, 2u);   // 旧终点 2 → 新 3：range 跨过 nop
    auto dt = s2->backing().decodeAttr(code2.attrs[1]);
    ASSERT_TRUE(dt.is_ok());
    const auto& lvtt = static_cast<const LocalVariableTableAttr&>(*dt.unwrap());
    ASSERT_EQ(lvtt.rows.size(), 1u);
    EXPECT_EQ(lvtt.rows[0].startPc, 1u);
    EXPECT_EQ(lvtt.rows[0].length, 2u);
    EXPECT_EQ(lvtt.rows[0].descCp, sigIdx);   // LVTT 第 4 列是 Signature 索引
}

/// @brief TA 注解条目最小构造：targetType + target_info + type_path +
///        type_index + 空元素值表（重定位测试只关心 pc 载荷）。
///        path 为 type_path 段字节序列（每段 2 字节），空表传缺省。
static Bytes taEntry(ca::u8 targetType, const Bytes& targetInfo, ca::u16 typeIdx,
                     const Bytes& path = {})
{
    Bytes b;
    b.push_back(targetType);
    b.insert(b.end(), targetInfo.begin(), targetInfo.end());
    b.push_back(static_cast<ca::u8>(path.size() / 2));   // type_path: path_length
    b.insert(b.end(), path.begin(), path.end());
    b.push_back(static_cast<ca::u8>(typeIdx >> 8));
    b.push_back(static_cast<ca::u8>(typeIdx & 0xFF));
    b.push_back(0x00);   // num_element_value_pairs = 0
    b.push_back(0x00);
    return b;
}

/// @brief TA 属性体：num_annotations 前缀 + 条目（测试规模 ≤255 条）。
static Bytes taAttr(const std::vector<Bytes>& entries)
{
    Bytes b{0x00, static_cast<ca::u8>(entries.size())};
    for (const Bytes& e : entries) {
        b.insert(b.end(), e.begin(), e.end());
    }
    return b;
}

/// @brief u2 大端字节。
static Bytes u2b(ca::u16 v)
{
    return {static_cast<ca::u8>(v >> 8), static_cast<ca::u8>(v & 0xFF)};
}

/// @brief u2 大端三元组（localvar 行：start/length/slot）。
static Bytes u2x3(ca::u16 a, ca::u16 b, ca::u16 c)
{
    Bytes r = u2b(a);
    const Bytes hi = u2b(b);
    const Bytes lo = u2b(c);
    r.insert(r.end(), hi.begin(), hi.end());
    r.insert(r.end(), lo.begin(), lo.end());
    return r;
}

/// @brief 字节拼接。
static Bytes cat(Bytes a, const Bytes& b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

TEST(IrSessionTest, EditRelocatesTypeAnnotationsOffsetTargets)
{
    // Code 内 RITA 双条目：CAST(0x47) offset=1（pop）、OFFSET(0x43) offset=2
    // （return）。iconst_1 后插 nop（old 0,1,2 → new 0,1,3）：offset 1→2、
    // 2→3；cast/type-arg 载荷后段（序号）与 type_path 不动。
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 codeIdx = cb.utf8("Code");
    const ca::u16 ritaIdx = cb.utf8("RuntimeInvisibleTypeAnnotations");
    const ca::u16 tagIdx = cb.utf8("Lp/Tag;");
    const ca::u16 mIdx = cb.utf8("m");
    const ca::u16 voidIdx = cb.utf8("()V");
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setAccess(0x0021);
    cb.setVersion(52);

    Bytes castInfo = u2b(1);       // u2 offset
    castInfo.push_back(0x00);      // u1 cast type_index（数组维度序号）
    const Bytes castEntry =
        taEntry(TypeAnnotationsAttr::kCast, castInfo, tagIdx, {0x03, 0x00});   // 实参路径 1 段
    const Bytes rita = taAttr({
        castEntry,
        taEntry(0x43, u2b(2), tagIdx),   // instanceof 族 offset 目标
    });
    const Bytes code = {0x04, 0x57, 0xb1};   // iconst_1; pop; return
    std::vector<AttrSpec> methodAttrs;
    methodAttrs.push_back(AttrSpec{
        codeIdx, jbc::test::codeContent(1, 0, code, {}, {AttrSpec{ritaIdx, rita}})});
    cb.addMethod(0x0009, mIdx, voidIdx, std::move(methodAttrs));

    auto s = sessionOf(cb.build());
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    jbc::ir::CodeUnit& cu = *p.unwrap();
    CodeEditor ed(cu);
    ASSERT_TRUE(ed.insertAfter(cu.dom.order()[0], CodeEditor::simple(Op::NOP)).is_ok());
    ASSERT_TRUE(lowerMethod(*s, 0).is_ok());

    auto s2 = sessionOf(jbc::test::writeOk(s->backing()));
    ASSERT_NE(s2, nullptr);
    auto dc = s2->backing().decodeAttr(s2->backing().methods[0].attrs[0]);
    ASSERT_TRUE(dc.is_ok());
    auto& code2 = static_cast<CodeAttr&>(*dc.unwrap());
    ASSERT_EQ(code2.attrs.size(), 1u);
    auto dr = s2->backing().decodeAttr(code2.attrs[0]);
    ASSERT_TRUE(dr.is_ok());
    const auto& ta = static_cast<const TypeAnnotationsAttr&>(*dr.unwrap());
    ASSERT_EQ(ta.annotations.size(), 2u);
    EXPECT_EQ(ta.annotations[0].targetType, TypeAnnotationsAttr::kCast);
    EXPECT_EQ(ta.annotations[0].offset, 2u);          // 1 → 2（nop 插在其前）
    EXPECT_EQ(ta.annotations[0].typeArgumentIndex, 0u);
    ASSERT_EQ(ta.annotations[0].typePath.size(), 1u); // path 不携带 pc：原样
    EXPECT_EQ(ta.annotations[0].typePath[0].kind, 3u);
    EXPECT_EQ(ta.annotations[1].targetType, 0x43);
    EXPECT_EQ(ta.annotations[1].offset, 3u);          // 2 → 3
}

TEST(IrSessionTest, EditRelocatesTypeAnnotationsLocalVarRows)
{
    // localvar_target 行走 LVT 同款终点规则：行 {start=1,len=1} 的旧终点 2
    // 命中 pcmap；行 {start=0,len=3} 的旧终点 3==旧 code 长 → 钳到新长度。
    // 方法开头插 nop（old 0,1,2 → new 1,2,3，新长 4）。
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 codeIdx = cb.utf8("Code");
    const ca::u16 ritaIdx = cb.utf8("RuntimeVisibleTypeAnnotations");
    const ca::u16 tagIdx = cb.utf8("Lp/Tag;");
    const ca::u16 mIdx = cb.utf8("m");
    const ca::u16 voidIdx = cb.utf8("()V");
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setAccess(0x0021);
    cb.setVersion(52);

    Bytes lvInfo = u2b(2);                 // local_variable_table_length = 2
    {   // 行1 {start=1, len=1, slot=1}
        const Bytes r = u2x3(1, 1, 1);
        lvInfo.insert(lvInfo.end(), r.begin(), r.end());
    }
    {   // 行2 {start=0, len=3, slot=2}（终点触底旧 code 长）
        const Bytes r = u2x3(0, 3, 2);
        lvInfo.insert(lvInfo.end(), r.begin(), r.end());
    }
    const Bytes rita = taAttr({taEntry(TypeAnnotationsAttr::kLocalVar, lvInfo, tagIdx)});
    const Bytes code = {0x04, 0x57, 0xb1};
    std::vector<AttrSpec> methodAttrs;
    methodAttrs.push_back(AttrSpec{
        codeIdx, jbc::test::codeContent(1, 3, code, {}, {AttrSpec{ritaIdx, rita}})});
    cb.addMethod(0x0009, mIdx, voidIdx, std::move(methodAttrs));

    auto s = sessionOf(cb.build());
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    jbc::ir::CodeUnit& cu = *p.unwrap();
    CodeEditor ed(cu);
    ASSERT_TRUE(ed.insertBefore(cu.dom.order()[0], CodeEditor::simple(Op::NOP)).is_ok());
    ASSERT_TRUE(lowerMethod(*s, 0).is_ok());

    auto s2 = sessionOf(jbc::test::writeOk(s->backing()));
    ASSERT_NE(s2, nullptr);
    auto dc = s2->backing().decodeAttr(s2->backing().methods[0].attrs[0]);
    ASSERT_TRUE(dc.is_ok());
    auto& code2 = static_cast<CodeAttr&>(*dc.unwrap());
    auto dr = s2->backing().decodeAttr(code2.attrs[0]);
    ASSERT_TRUE(dr.is_ok());
    const auto& ta = static_cast<const TypeAnnotationsAttr&>(*dr.unwrap());
    ASSERT_EQ(ta.annotations.size(), 1u);
    ASSERT_EQ(ta.annotations[0].localVars.size(), 2u);
    EXPECT_EQ(ta.annotations[0].localVars[0].startPc, 2u);   // 1 → 2
    EXPECT_EQ(ta.annotations[0].localVars[0].length, 1u);    // 终点 2 → 3
    EXPECT_EQ(ta.annotations[0].localVars[1].startPc, 1u);   // 0 → 1
    EXPECT_EQ(ta.annotations[0].localVars[1].length, 3u);    // 终点触底钳新长 4
}

TEST(IrSessionTest, TypeAnnotationsNonOffsetTargetsSurviveRelocation)
{
    // 无 pc 目标（method return empty、catch 表下标）在布局改动中原样保留：
    // catch_index 是异常表条目下标而非 pc，lower 重建异常表保持顺序，不动。
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 codeIdx = cb.utf8("Code");
    const ca::u16 ritaIdx = cb.utf8("RuntimeInvisibleTypeAnnotations");
    const ca::u16 tagIdx = cb.utf8("Lp/Tag;");
    const ca::u16 mIdx = cb.utf8("m");
    const ca::u16 voidIdx = cb.utf8("()V");
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setAccess(0x0021);
    cb.setVersion(52);

    const Bytes rita = taAttr({
        taEntry(TypeAnnotationsAttr::kMethodReturn, {}, tagIdx),   // empty_target
        taEntry(TypeAnnotationsAttr::kCatch, u2b(0), tagIdx),      // catch_index=0
    });
    const Bytes code = {0x04, 0x57, 0xb1};
    std::vector<AttrSpec> methodAttrs;
    methodAttrs.push_back(AttrSpec{
        codeIdx, jbc::test::codeContent(1, 0, code, {}, {AttrSpec{ritaIdx, rita}})});
    cb.addMethod(0x0009, mIdx, voidIdx, std::move(methodAttrs));

    auto s = sessionOf(cb.build());
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    jbc::ir::CodeUnit& cu = *p.unwrap();
    CodeEditor ed(cu);
    ASSERT_TRUE(ed.insertAfter(cu.dom.order()[0], CodeEditor::simple(Op::NOP)).is_ok());
    ASSERT_TRUE(lowerMethod(*s, 0).is_ok());

    auto s2 = sessionOf(jbc::test::writeOk(s->backing()));
    ASSERT_NE(s2, nullptr);
    auto dc = s2->backing().decodeAttr(s2->backing().methods[0].attrs[0]);
    ASSERT_TRUE(dc.is_ok());
    auto& code2 = static_cast<CodeAttr&>(*dc.unwrap());
    auto dr = s2->backing().decodeAttr(code2.attrs[0]);
    ASSERT_TRUE(dr.is_ok());
    const auto& ta = static_cast<const TypeAnnotationsAttr&>(*dr.unwrap());
    ASSERT_EQ(ta.annotations.size(), 2u);
    EXPECT_EQ(ta.annotations[0].targetType, TypeAnnotationsAttr::kMethodReturn);
    EXPECT_EQ(ta.annotations[1].targetType, TypeAnnotationsAttr::kCatch);
    EXPECT_EQ(ta.annotations[1].catchIndex, 0u);
}

TEST(IrSessionTest, TypeAnnotationsBadOffsetFailsClosed)
{
    // CAST offset=5 不在任何指令起点 → RelocationBadTarget；全有或全无：
    // 元数据、code、同表 LNT、TA 全部保持原样（TA 槽保持 Decoded 不脏）。
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 codeIdx = cb.utf8("Code");
    const ca::u16 lntIdx = cb.utf8("LineNumberTable");
    const ca::u16 ritaIdx = cb.utf8("RuntimeInvisibleTypeAnnotations");
    const ca::u16 tagIdx = cb.utf8("Lp/Tag;");
    const ca::u16 mIdx = cb.utf8("m");
    const ca::u16 voidIdx = cb.utf8("()V");
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setAccess(0x0021);
    cb.setVersion(52);

    Bytes castInfo = u2b(5);       // 越界 offset：pcmap 必 miss
    castInfo.push_back(0x00);
    const Bytes rita = taAttr({taEntry(TypeAnnotationsAttr::kCast, castInfo, tagIdx)});
    const Bytes code = {0x04, 0x57, 0xb1};
    std::vector<AttrSpec> nested;
    nested.push_back(AttrSpec{lntIdx, {0x00, 0x01, 0x00, 0x00, 0x00, 0x01}});
    nested.push_back(AttrSpec{ritaIdx, rita});
    std::vector<AttrSpec> methodAttrs;
    methodAttrs.push_back(AttrSpec{codeIdx, jbc::test::codeContent(1, 0, code, {}, nested)});
    cb.addMethod(0x0009, mIdx, voidIdx, std::move(methodAttrs));

    const Bytes original = cb.build();
    auto s = sessionOf(original);
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    jbc::ir::CodeUnit& cu = *p.unwrap();
    CodeEditor ed(cu);
    ASSERT_TRUE(ed.insertAfter(cu.dom.order()[0], CodeEditor::simple(Op::NOP)).is_ok());

    const ca::u32 cpSize = s->backing().cp.size();
    const auto r = lowerMethod(*s, 0);
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), jbc::ir::LowerError::RelocationBadTarget);
    EXPECT_EQ(s->backing().cp.size(), cpSize);
    EXPECT_EQ(jbc::test::writeOk(s->backing()), original);

    auto dc = s->backing().decodeAttr(s->backing().methods[0].attrs[0]);
    ASSERT_TRUE(dc.is_ok());
    auto& codeAttr = static_cast<CodeAttr&>(*dc.unwrap());
    ASSERT_EQ(codeAttr.attrs.size(), 2u);
    EXPECT_EQ(codeAttr.attrs[0].state, AttrSlot::State::Decoded);   // LNT 未半改
    auto dr = s->backing().decodeAttr(codeAttr.attrs[1]);
    ASSERT_TRUE(dr.is_ok());
    const auto& ta = static_cast<const TypeAnnotationsAttr&>(*dr.unwrap());
    ASSERT_EQ(ta.annotations.size(), 1u);
    EXPECT_EQ(ta.annotations[0].offset, 5u);
    EXPECT_EQ(codeAttr.attrs[1].state, AttrSlot::State::Decoded);   // TA 未半改
}

TEST(IrSessionTest, EditRelocatesCharacterRangeTable)
{
    // CRT（javac -Xjcov 私有属性）行双列都是指令起始 pc：start_pc/end_pc
    // 各自精确命中 pcmap。iconst_1 后插 nop（old 0,1,2 → new 0,1,3）：
    // 行 {1,2} → {2,3}；行 {0,2} → {0,3}。char 列与 flags 不携带 pc，不动。
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 codeIdx = cb.utf8("Code");
    const ca::u16 crtIdx = cb.utf8("CharacterRangeTable");
    const ca::u16 mIdx = cb.utf8("m");
    const ca::u16 voidIdx = cb.utf8("()V");
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setAccess(0x0021);
    cb.setVersion(52);

    auto row = [](ca::u16 start, ca::u16 end) {
        // {u2 start, u2 end, u4 charStart, u4 charEnd, u2 flags}
        static const Bytes kChars = {0x00, 0x00, 0x0c, 0x09, 0x00, 0x00, 0x0c, 0x17};
        return cat(cat(u2b(start), u2b(end)), cat(kChars, u2b(1)));
    };
    Bytes crt = u2b(2);
    const Bytes r0 = row(1, 2);
    const Bytes r1 = row(0, 2);
    crt.insert(crt.end(), r0.begin(), r0.end());
    crt.insert(crt.end(), r1.begin(), r1.end());

    const Bytes code = {0x04, 0x57, 0xb1};
    std::vector<AttrSpec> methodAttrs;
    methodAttrs.push_back(AttrSpec{
        codeIdx, jbc::test::codeContent(1, 0, code, {}, {AttrSpec{crtIdx, crt}})});
    cb.addMethod(0x0009, mIdx, voidIdx, std::move(methodAttrs));

    auto s = sessionOf(cb.build());
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    jbc::ir::CodeUnit& cu = *p.unwrap();
    CodeEditor ed(cu);
    ASSERT_TRUE(ed.insertAfter(cu.dom.order()[0], CodeEditor::simple(Op::NOP)).is_ok());
    ASSERT_TRUE(lowerMethod(*s, 0).is_ok());

    const Bytes out = jbc::test::writeOk(s->backing());
    auto s2 = sessionOf(out);
    ASSERT_NE(s2, nullptr);
    auto dc = s2->backing().decodeAttr(s2->backing().methods[0].attrs[0]);
    ASSERT_TRUE(dc.is_ok());
    auto& code2 = static_cast<CodeAttr&>(*dc.unwrap());
    ASSERT_EQ(code2.attrs.size(), 1u);
    auto dr = s2->backing().decodeAttr(code2.attrs[0]);
    ASSERT_TRUE(dr.is_ok());
    EXPECT_EQ(code2.attrs[0].peek()->kind(), AttrKind::CharacterRangeTable);
    const auto& crt2 = static_cast<const CharacterRangeTableAttr&>(*dr.unwrap());
    ASSERT_EQ(crt2.rows.size(), 2u);
    EXPECT_EQ(crt2.rows[0].startPc, 2u);   // 1 → 2
    EXPECT_EQ(crt2.rows[0].endPc, 3u);     // 2 → 3
    EXPECT_EQ(crt2.rows[1].startPc, 0u);   // 0 → 0
    EXPECT_EQ(crt2.rows[1].endPc, 3u);     // 2 → 3
    EXPECT_EQ(crt2.rows[0].charStart, 0x0c09u);   // 非 pc 列不动
    EXPECT_EQ(crt2.rows[0].flags, 1u);
}

TEST(IrSessionTest, CharacterRangeTableBadPcFailsClosed)
{
    // CRT 行 end_pc=9 不在任何指令起点：CRT 已建模，因此是 RelocationBadTarget
    // （而不是未建模墙的 NotModeled）；全有或全无——code 与同表 LNT 原样。
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 codeIdx = cb.utf8("Code");
    const ca::u16 lntIdx = cb.utf8("LineNumberTable");
    const ca::u16 crtIdx = cb.utf8("CharacterRangeTable");
    const ca::u16 mIdx = cb.utf8("m");
    const ca::u16 voidIdx = cb.utf8("()V");
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setAccess(0x0021);
    cb.setVersion(52);

    Bytes crt = u2b(1);
    const Bytes r0 = cat(cat(u2b(0), u2b(9)),
                         cat(Bytes{0x00, 0x00, 0x0c, 0x09, 0x00, 0x00, 0x0c, 0x17}, u2b(1)));
    crt.insert(crt.end(), r0.begin(), r0.end());

    const Bytes code = {0x04, 0x57, 0xb1};
    std::vector<AttrSpec> nested;
    nested.push_back(AttrSpec{lntIdx, {0x00, 0x01, 0x00, 0x00, 0x00, 0x01}});
    nested.push_back(AttrSpec{crtIdx, crt});
    std::vector<AttrSpec> methodAttrs;
    methodAttrs.push_back(AttrSpec{codeIdx, jbc::test::codeContent(1, 0, code, {}, nested)});
    cb.addMethod(0x0009, mIdx, voidIdx, std::move(methodAttrs));

    const Bytes original = cb.build();
    auto s = sessionOf(original);
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    jbc::ir::CodeUnit& cu = *p.unwrap();
    CodeEditor ed(cu);
    ASSERT_TRUE(ed.insertAfter(cu.dom.order()[0], CodeEditor::simple(Op::NOP)).is_ok());

    const auto r = lowerMethod(*s, 0);
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), jbc::ir::LowerError::RelocationBadTarget);
    EXPECT_EQ(jbc::test::writeOk(s->backing()), original);

    auto dc = s->backing().decodeAttr(s->backing().methods[0].attrs[0]);
    ASSERT_TRUE(dc.is_ok());
    auto& codeAttr = static_cast<CodeAttr&>(*dc.unwrap());
    auto dl = s->backing().decodeAttr(codeAttr.attrs[0]);
    ASSERT_TRUE(dl.is_ok());
    const auto& lnt = static_cast<const LineNumberTableAttr&>(*dl.unwrap());
    ASSERT_EQ(lnt.rows.size(), 1u);
    EXPECT_EQ(lnt.rows[0].startPc, 0u);   // LNT 未半改
    EXPECT_EQ(codeAttr.attrs[0].state, AttrSlot::State::Decoded);
    EXPECT_EQ(codeAttr.attrs[1].state, AttrSlot::State::Decoded);
}

TEST(IrSessionTest, RelocatesCompactStackMapFrameDelta)
{
    const Bytes code = {0x00, 0x00, 0x00, 0xb1};   // return at old pc=3
    const Bytes smt  = {0x00, 0x01, 0x03};         // same_frame, delta=3
    const Bytes cls  = makeStackMapClass(code, smt);
    auto s = sessionOf(cls);
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    jbc::ir::CodeUnit& cu = *p.unwrap();
    CodeEditor ed(cu);
    ASSERT_TRUE(ed.insertAtStart(CodeEditor::simple(Op::NOP)).is_ok());
    ASSERT_TRUE(lowerMethod(*s, 0).is_ok());

    auto s2 = sessionOf(jbc::test::writeOk(s->backing()));
    ASSERT_NE(s2, nullptr);
    auto dc = s2->backing().decodeAttr(s2->backing().methods[0].attrs[0]);
    ASSERT_TRUE(dc.is_ok());
    auto& code2 = static_cast<CodeAttr&>(*dc.unwrap());
    auto ds = s2->backing().decodeAttr(code2.attrs[0]);
    ASSERT_TRUE(ds.is_ok());
    const auto& sm2 = static_cast<const StackMapTableAttr&>(*ds.unwrap());
    ASSERT_EQ(sm2.frames.size(), 1u);
    EXPECT_EQ(sm2.frames[0].type, 0x04);
    EXPECT_EQ(sm2.frames[0].offset, 4u);
}

TEST(IrSessionTest, PromotesStackMapFrameToExtendedWhenDeltaGrows)
{
    Bytes code(63, 0x00);
    code.push_back(0xb1);   // return at old pc=63
    const Bytes smt = {0x00, 0x01, 0x3f};   // same_frame, delta=63
    auto s = sessionOf(makeStackMapClass(code, smt));
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    jbc::ir::CodeUnit& cu = *p.unwrap();
    CodeEditor ed(cu);
    ASSERT_TRUE(ed.insertBefore(cu.dom.last(), CodeEditor::simple(Op::NOP)).is_ok());
    ASSERT_TRUE(lowerMethod(*s, 0).is_ok());

    auto s2 = sessionOf(jbc::test::writeOk(s->backing()));
    ASSERT_NE(s2, nullptr);
    auto dc = s2->backing().decodeAttr(s2->backing().methods[0].attrs[0]);
    ASSERT_TRUE(dc.is_ok());
    auto& code2 = static_cast<CodeAttr&>(*dc.unwrap());
    auto ds = s2->backing().decodeAttr(code2.attrs[0]);
    ASSERT_TRUE(ds.is_ok());
    const auto& sm2 = static_cast<const StackMapTableAttr&>(*ds.unwrap());
    ASSERT_EQ(sm2.frames.size(), 1u);
    EXPECT_EQ(sm2.frames[0].type, 0xfb);
    EXPECT_EQ(sm2.frames[0].offset, 64u);
}

TEST(IrSessionTest, RelocatesUninitializedStackMapOffset)
{
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 codeIdx = cb.utf8("Code");
    const ca::u16 smtIdx  = cb.utf8("StackMapTable");
    const ca::u16 mIdx = cb.utf8("m");
    const ca::u16 voidIdx = cb.utf8("()V");
    const ca::u16 clsIdx = cb.classRef(thisIdx);
    cb.setThis(clsIdx);
    cb.setSuper(cb.classRef(superIdx));
    cb.setAccess(0x0021);
    cb.setVersion(52);

    // new #class (pc 0, length 3); pop (pc 3); return (pc 4).
    const Bytes code = {0xbb, static_cast<ca::u8>(clsIdx >> 8),
                        static_cast<ca::u8>(clsIdx & 0xff), 0x57, 0xb1};
    // same_locals_1_stack_item_frame, delta=3, Uninitialized(offset=0).
    const Bytes smt = {0x00, 0x01, 0x43, 0x08, 0x00, 0x00};
    cb.addMethod(0x0009, mIdx, voidIdx,
                 {AttrSpec{codeIdx, jbc::test::codeContent(1, 0, code, {},
                                                            {AttrSpec{smtIdx, smt}})}});
    auto s = sessionOf(cb.build());
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    jbc::ir::CodeUnit& cu = *p.unwrap();
    CodeEditor ed(cu);
    ASSERT_TRUE(ed.insertAtStart(CodeEditor::simple(Op::NOP)).is_ok());
    ASSERT_TRUE(lowerMethod(*s, 0).is_ok());

    auto s2 = sessionOf(jbc::test::writeOk(s->backing()));
    ASSERT_NE(s2, nullptr);
    auto dc = s2->backing().decodeAttr(s2->backing().methods[0].attrs[0]);
    ASSERT_TRUE(dc.is_ok());
    auto& code2 = static_cast<CodeAttr&>(*dc.unwrap());
    auto ds = s2->backing().decodeAttr(code2.attrs[0]);
    ASSERT_TRUE(ds.is_ok());
    const auto& sm2 = static_cast<const StackMapTableAttr&>(*ds.unwrap());
    ASSERT_EQ(sm2.frames.size(), 1u);
    EXPECT_EQ(sm2.frames[0].type, 0x44);
    ASSERT_EQ(sm2.frames[0].stack.size(), 1u);
    EXPECT_EQ(sm2.frames[0].stack[0].tag, 0x08);
    EXPECT_EQ(sm2.frames[0].stack[0].cp, 1u);
}

TEST(IrSessionTest, BasicBlockView)
{
    auto s = sessionOf(makeEditableClass(false));
    auto p = promoteMethod(*s, 0);
    CodeEditor ed(*p.unwrap());
    // 顺序直落方法 = 单块。
    const auto blocks = ed.blocks();
    ASSERT_EQ(blocks.size(), 1u);
    EXPECT_EQ(blocks[0].insns.size(), 3u);
}

// ---------------------------------------------------------------------------
// 类级元数据提交（Tier A）：ClassUnit 类级字段 → backing 的唯一提交通道
// ---------------------------------------------------------------------------

/// 改类级元数据 + commitClassMetadata → 写出重读语义一致；直接改写不提交则
/// 静默不落 backing（不对称性的对照面）。
TEST(IrSessionTest, CommitClassMetadataLandsBackingAndRereads)
{
    auto s = sessionOf(makeEditableClass(false));
    ASSERT_NE(s, nullptr);
    ClassUnit& u = s->unit();
    const Bytes original = jbc::test::writeOk(s->backing());

    // 对照：直接改写高层字段不提交 → backing 不动（显式提交出活的理由）。
    u.access = 0x0031;
    EXPECT_EQ(s->backing().access, 0x0021);
    u.access = 0x0021;

    // 提交：改名/改访问/换父类/加接口/换版本。
    u.access = 0x0031;
    u.thisClass = u.symbols().intern("p/Renamed");
    u.superClass = u.symbols().intern("p/Base");
    u.interfaces.push_back(u.symbols().intern("java/io/Serializable"));
    u.version = (ca::u32{52} << 16) | ca::u32{3};
    ASSERT_TRUE(s->commitClassMetadata().is_ok());

    const Bytes out = jbc::test::writeOk(s->backing());
    EXPECT_NE(out, original);
    auto s2 = sessionOf(out);
    ASSERT_NE(s2, nullptr);
    EXPECT_EQ(s2->unit().access, 0x0031u);
    EXPECT_EQ(s2->unit().symbols().str(s2->unit().thisClass), "p/Renamed");
    EXPECT_EQ(s2->unit().symbols().str(s2->unit().superClass), "p/Base");
    ASSERT_EQ(s2->unit().interfaces.size(), 1u);
    EXPECT_EQ(s2->unit().symbols().str(s2->unit().interfaces[0]), "java/io/Serializable");
    EXPECT_EQ(s2->backing().minor, 3u);
    EXPECT_EQ(s2->backing().major, 52u);
}

/// super 空名提交为 0（= java/lang/Object 惯例），读回 superClass 名为空。
TEST(IrSessionTest, CommitClassMetadataEmptySuperWritesZero)
{
    auto s = sessionOf(makeEditableClass(false));
    ASSERT_NE(s, nullptr);
    ClassUnit& u = s->unit();
    EXPECT_NE(s->backing().superClass, 0u);   // 前置：makeEditableClass 有真父类
    u.superClass = u.symbols().intern("");    // 高层把「无父类」表为空名
    ASSERT_TRUE(s->commitClassMetadata().is_ok());
    EXPECT_EQ(s->backing().superClass, 0u);
}

/// CP 容量预检：提交失败时不留半截追加（先算后写）。
TEST(IrSessionTest, CommitClassMetadataCapacityRefusedWithoutPartialAppend)
{
    auto s = sessionOf(makeEditableClass(false));
    ASSERT_NE(s, nullptr);
    ClassUnit& u = s->unit();
    // access 超 u16：提交拒绝，CP 不动。
    u.access = 0x10000;
    const ca::u32 cpBefore = s->backing().cp.size();
    auto r = s->commitClassMetadata();
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), jbc::ir::LowerError::CpAppendFailed);
    EXPECT_EQ(s->backing().cp.size(), cpBefore);
}

// ---------------------------------------------------------------------------
// labelsAt：锚定标签查询（变换 pass 的旧栈 LabelNode 语义支点）
// ---------------------------------------------------------------------------

TEST(IrSessionTest, LabelsAtListsAnchoredLabelsBothRoles)
{
    // goto L; nop; L(return 前): iconst_1; pop; return
    //   - ControlFlow 标签锚定 nop（goto 的目标）
    //   - Boundary 标签锚定 return（手工创建）
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
    // goto +3; nop; iconst_1; pop; return
    const Bytes code = {0xa7, 0x00, 0x03, 0x00, 0x04, 0x57, 0xb1};
    std::vector<AttrSpec> methodAttrs;
    methodAttrs.push_back(AttrSpec{codeIdx, jbc::test::codeContent(0, 0, code)});
    cb.addMethod(0x0009, mIdx, voidIdx, std::move(methodAttrs));

    auto s = sessionOf(cb.build());
    auto p = promoteMethod(*s, 0);
    jbc::ir::CodeUnit& cu = *p.unwrap();

    const auto& order = cu.dom.order();
    const jbc::ir::InsnId gotoId = order[0];
    const jbc::ir::InsnId nopId = order[1];
    const jbc::ir::InsnId retId = order[4];

    // goto 的 ControlFlow 目标标签锚定 nop。
    const auto atNop = cu.dom.labelsAt(nopId);
    ASSERT_EQ(atNop.size(), 1u);
    const auto resolved = cu.dom.resolveLabel(atNop[0]);
    ASSERT_TRUE(resolved.is_ok());
    EXPECT_FALSE(resolved.unwrap().isEnd());
    EXPECT_EQ(resolved.unwrap().insn, nopId);
    // 该标签确实是 goto 的目标字段。
    const auto* g = cu.dom.peek(gotoId);
    ASSERT_NE(g, nullptr);
    EXPECT_EQ(g->target, atNop[0]);

    // 无锚定的指令返回空；手工 Boundary 标签可查询到。
    EXPECT_TRUE(cu.dom.labelsAt(gotoId).empty());
    auto boundary = cu.dom.createLabel(retId);
    ASSERT_TRUE(boundary.is_ok());
    const auto atRet = cu.dom.labelsAt(retId);
    ASSERT_EQ(atRet.size(), 1u);
    EXPECT_EQ(atRet[0], boundary.unwrap());

    // 无效句柄返回空（探测式，不抛错）。
    EXPECT_TRUE(cu.dom.labelsAt(jbc::ir::kNoInsn).empty());

    // 删除锚点后标签失效，查询不再返回（gen 递增句柄结构性死亡）。
    CodeEditor ed(cu);
    ASSERT_TRUE(ed.erase(nopId, CodeEditor::ErasePolicy::kRejectIfUsed).is_err());
}

TEST(IrSessionTest, MethodParametersAcceptsReceiverArity)
{
    // JVMS 4.7.24：MethodParameters 行数可为「描述符 arity」或「arity + 1」
    // （实例方法的接收者行，javac 对非静态方法恒带 this 行）。
    auto make = [](ca::u16 methodAccess) {
        ClassBuilder cb;
        const ca::u16 thisIdx  = cb.utf8("p/T");
        const ca::u16 superIdx = cb.utf8("java/lang/Object");
        const ca::u16 codeIdx  = cb.utf8("Code");
        const ca::u16 mIdx     = cb.utf8("m");
        const ca::u16 voidIdx  = cb.utf8("()V");
        cb.setThis(cb.classRef(thisIdx));
        cb.setSuper(cb.classRef(superIdx));
        cb.setAccess(0x0021);
        cb.setVersion(52);
        cb.addMethod(methodAccess, mIdx, voidIdx,
                     {AttrSpec{codeIdx, jbc::test::codeContent(0, 0, Bytes{0xb1})}});
        return sessionOf(cb.build());
    };

    // 实例方法 + 单接收者行：放行，写回可读且行名保真。
    {
        auto s = make(0x0001);
        ASSERT_NE(s, nullptr);
        const jbc::ir::MethodId id = s->unit().findMethodId("m", "()V");
        const jbc::ir::ParamNameRow rows[] = {{"this", 0}};
        auto r = s->attachMethodParameters(id, {rows, rows + 1});
        ASSERT_TRUE(r.is_ok()) << jbc::ir::to_string(r.unwrap_err());
        auto out = jbc::test::writeOk(s->backing());
        auto s2  = sessionOf(out);
        ASSERT_NE(s2, nullptr);
        ASSERT_FALSE(s2->backing().methods[0].attrs.empty());
        auto dc = s2->backing().decodeAttr(s2->backing().methods[0].attrs.back());
        ASSERT_TRUE(dc.is_ok());
        const auto* mp = ca::core::tag_cast::dyn_cast<const jbc::core::MethodParametersAttr>(
            dc.unwrap());
        ASSERT_NE(mp, nullptr);
        ASSERT_EQ(mp->rows.size(), 1u);
        EXPECT_EQ(s2->backing().cp.utf8Of(mp->rows[0].nameCp), "this");
    }
    // 实例方法 + arity+2 行：仍拒（InvalidSeed）。
    {
        auto s = make(0x0001);
        ASSERT_NE(s, nullptr);
        const jbc::ir::MethodId   id = s->unit().findMethodId("m", "()V");
        std::vector<jbc::ir::ParamNameRow> rows(2);
        auto r = s->attachMethodParameters(id, rows);
        EXPECT_TRUE(r.is_err() && r.unwrap_err() == jbc::ir::LowerError::InvalidSeed);
    }
    // 静态方法 + 接收者行：拒（静态方法无接收者）。
    {
        auto s = make(0x0009);
        ASSERT_NE(s, nullptr);
        const jbc::ir::MethodId   id = s->unit().findMethodId("m", "()V");
        std::vector<jbc::ir::ParamNameRow> rows(1);
        auto r = s->attachMethodParameters(id, rows);
        EXPECT_TRUE(r.is_err() && r.unwrap_err() == jbc::ir::LowerError::InvalidSeed);
    }
}
