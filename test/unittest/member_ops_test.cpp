/// @file        member_ops_test.cpp
/// @brief       成员级增删、身份失效协议与从零建类的单元测试。
///
/// 全部用关内建的 ClassBuilder 字节构造（不依赖本机语料与 JDK）；JVM 真跑
/// 门在 member_move_test.cpp。测试矩阵：
///   - A3 方法：add（元数据 + 空 Code 体）/ remove（句柄失效 + 槽位复用
///     generation 自增 + 物理平移）/ adopt（跨会话搬移 + 语义重放）；
///   - A4 字段：FieldId 身份 / commitFieldMetadata / add/remove/adopt 同构；
///   - A5 从零：makeSession(NewClassSeed) 空类起步 + CP 引导 + 无源写出。
#include <gtest/gtest.h>

#include <memory>
#include <utility>
#include <vector>

#include "jbc/jbc.hpp"
#include "test_class_bytes.hpp"

using namespace jbc::core;
using jbc::ir::ClassSession;
using jbc::ir::FieldId;
using jbc::ir::kNoField;
using jbc::ir::kNoFieldId;
using jbc::ir::kNoMethod;
using jbc::ir::kNoMethodId;
using jbc::ir::LowerError;
using jbc::ir::MethodId;
using jbc::ir::PromoteError;
using jbc::test::AttrSpec;
using jbc::test::ClassBuilder;
using Bytes = std::vector<ca::u8>;

namespace {

/// 基准类 p/M：2 字段（alpha:I static / beta:J final static）+ 3 方法
/// （m()I 有体、n(I)I 有体、sketch()V abstract 无体）。
Bytes makeMemberClass()
{
    ClassBuilder cb;
    const ca::u16 thisIdx  = cb.utf8("p/M");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 codeIdx  = cb.utf8("Code");
    const ca::u16 aIdx     = cb.utf8("alpha");
    const ca::u16 iDesc    = cb.utf8("I");
    const ca::u16 bIdx     = cb.utf8("beta");
    const ca::u16 jDesc    = cb.utf8("J");
    const ca::u16 mIdx     = cb.utf8("m");
    const ca::u16 vDesc    = cb.utf8("()I");
    const ca::u16 nIdx     = cb.utf8("n");
    const ca::u16 iiDesc   = cb.utf8("(I)I");
    const ca::u16 sIdx     = cb.utf8("sketch");

    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setAccess(0x0021);
    cb.setVersion(52);

    cb.addField(0x000a, aIdx, iDesc);    // static int alpha（0x0008|0x0002）
    cb.addField(0x001a, bIdx, jDesc);    // static final long beta
    cb.addMethod(0x0009, mIdx, vDesc,
                 {AttrSpec{codeIdx, jbc::test::codeContent(1, 0, {0x04, 0xac})}});
    cb.addMethod(0x0009, nIdx, iiDesc,
                 {AttrSpec{codeIdx, jbc::test::codeContent(2, 1, {0x1a, 0x04, 0x60, 0xac})}});
    cb.addMethod(0x0401, sIdx, cb.utf8("()V"));   // public abstract，无 Code
    return cb.build();
}

/// 供搬移的供体类 d/D：static int stash + static int grab() { stash=7; return stash; }。
Bytes makeDonorClass()
{
    ClassBuilder cb;
    const ca::u16 thisIdx  = cb.utf8("d/D");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 codeIdx  = cb.utf8("Code");
    const ca::u16 stashIdx = cb.utf8("stash");
    const ca::u16 iDesc    = cb.utf8("I");
    const ca::u16 grabIdx  = cb.utf8("grab");
    const ca::u16 iDescRet = cb.utf8("()I");
    const ca::u16 natIdx   = cb.nameAndType(stashIdx, iDesc);
    const ca::u16 refIdx   = cb.fieldref(cb.classRef(thisIdx), natIdx);

    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setAccess(0x0021);
    cb.setVersion(52);

    cb.addField(0x0009, stashIdx, iDesc);
    // bipush 7; putstatic D.stash; getstatic D.stash; ireturn
    cb.addMethod(0x0009, grabIdx, iDescRet,
                 {AttrSpec{codeIdx, jbc::test::codeContent(1, 0, {0x10, 0x07, 0xb3,
                                                                  ca::u8(refIdx >> 8),
                                                                  ca::u8(refIdx),
                                                                  0xb2,
                                                                  ca::u8(refIdx >> 8),
                                                                  ca::u8(refIdx),
                                                                  0xac})}});
    return cb.build();
}

std::shared_ptr<ClassSession> sessionOf(const Bytes& bytes)
{
    auto r = jbc::ir::makeSessionFromBytes(ByteView{bytes.data(), bytes.size()});
    return r.is_ok() ? std::move(r).unwrap() : nullptr;
}

}   // namespace

// ===========================================================================
// A3：方法新增
// ===========================================================================

TEST(MemberOpsA3Test, AddMethodProvisionsMetadataAndEmptyCode)
{
    auto s = sessionOf(makeMemberClass());
    ASSERT_NE(s, nullptr);
    const ca::usize methodsBefore = s->unit().methods.size();

    jbc::ir::MethodSeed seed;
    seed.name     = "added";
    seed.desc     = "()V";
    seed.access   = 0x0009;
    seed.withCode = true;
    seed.maxStack = 1;
    auto added    = s->addMethod(seed);
    ASSERT_TRUE(added.is_ok());
    const MethodId id = added.unwrap();
    EXPECT_EQ(s->unit().methods.size(), methodsBefore + 1);
    EXPECT_EQ(s->backing().methods.size(), methodsBefore + 1);
    EXPECT_EQ(s->unit().findMethodId("added", "()V"), id);
    EXPECT_TRUE(jbc::ir::hasCode(*s, s->unit().indexOf(id)));

    // 空 Code 体 promote 后经 CodeEditor 出活，lower 写回，重读可见。
    auto p = jbc::ir::promoteMethod(*s, id);
    ASSERT_TRUE(p.is_ok());
    EXPECT_EQ(p.unwrap()->dom.size(), 0u);
    {
        jbc::ir::CodeEditor ed(*p.unwrap());
        ASSERT_TRUE(ed.append(jbc::ir::CodeEditor::simple(Op::ICONST_1)).is_ok());
        ASSERT_TRUE(ed.append(jbc::ir::CodeEditor::simple(Op::IRETURN)).is_ok());
    }
    // desc 仍是 ()V 而体是 ireturn——测试只关心管线，JVM 门在 move 测试里
    // 用自洽样本；这里重读验证 lower 落盘即可。
    ASSERT_TRUE(jbc::ir::lowerMethod(*s, id).is_ok());
    auto bytes = jbc::test::writeOk(s->backing());
    auto s2    = sessionOf(bytes);
    ASSERT_NE(s2, nullptr);
    const MethodId id2 = s2->unit().findMethodId("added", "()V");
    ASSERT_NE(id2, kNoMethodId);
    auto p2 = jbc::ir::promoteMethod(*s2, id2);
    ASSERT_TRUE(p2.is_ok());
    EXPECT_EQ(p2.unwrap()->dom.size(), 2u);
    EXPECT_EQ(p2.unwrap()->maxStack, 1u);
}

TEST(MemberOpsA3Test, AddMethodRejectsDuplicateAndEmptySeed)
{
    auto s = sessionOf(makeMemberClass());
    ASSERT_NE(s, nullptr);

    jbc::ir::MethodSeed dup;
    dup.name   = "n";
    dup.desc   = "(I)I";
    dup.access = 0x0009;
    auto r     = s->addMethod(dup);
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), LowerError::InvalidSeed);

    jbc::ir::MethodSeed empty;
    empty.desc   = "()V";
    empty.access = 0x0009;
    ASSERT_TRUE(s->addMethod(empty).is_err());
    EXPECT_EQ(s->addMethod(empty).unwrap_err(), LowerError::InvalidSeed);
}

TEST(MemberOpsA3Test, AddMethodAbstractShapeHasNoCodeAttr)
{
    auto s = sessionOf(makeMemberClass());
    ASSERT_NE(s, nullptr);

    jbc::ir::MethodSeed seed;
    seed.name   = "sketch2";
    seed.desc   = "()V";
    seed.access = 0x0401;   // abstract：无体
    auto added  = s->addMethod(seed);
    ASSERT_TRUE(added.is_ok());
    EXPECT_FALSE(jbc::ir::hasCode(*s, s->unit().indexOf(added.unwrap())));
    auto p = jbc::ir::promoteMethod(*s, added.unwrap());
    ASSERT_TRUE(p.is_err());
    EXPECT_EQ(p.unwrap_err(), PromoteError::NoCode);
}

// ===========================================================================
// A3：方法删除与身份失效协议
// ===========================================================================

TEST(MemberOpsA3Test, RemoveMethodKeepsSiblingIdsAndShiftsBacking)
{
    auto s  = sessionOf(makeMemberClass());
    ASSERT_NE(s, nullptr);
    const MethodId idM      = s->unit().findMethodId("m", "()I");
    const MethodId idN      = s->unit().findMethodId("n", "(I)I");
    const MethodId idSketch = s->unit().findMethodId("sketch", "()V");
    ASSERT_NE(idM, kNoMethodId);
    ASSERT_NE(idN, kNoMethodId);
    ASSERT_NE(idSketch, kNoMethodId);

    ASSERT_TRUE(s->removeMethod(idN).is_ok());
    EXPECT_EQ(s->unit().methods.size(), 2u);
    EXPECT_EQ(s->backing().methods.size(), 2u);

    // 兄弟身份原样存活；被删身份失配。
    EXPECT_NE(s->unit().method(idM), nullptr);
    EXPECT_NE(s->unit().method(idSketch), nullptr);
    EXPECT_EQ(s->unit().method(idN), nullptr);
    EXPECT_EQ(s->unit().indexOf(idM), 0u);
    EXPECT_EQ(s->unit().indexOf(idSketch), 1u);

    // 物理位置平移后仍与身份对齐：按 backingIndex 读回的名字一致。
    for (const jbc::ir::MethodUnit& mu : s->unit().methods) {
        ASSERT_LT(mu.backingIndex, s->backing().methods.size());
        EXPECT_EQ(s->backing().nameOf(s->backing().methods[mu.backingIndex]),
                  s->unit().symbols().str(mu.name));
    }

    // 按 id 的 promote/lower 不受删除影响。
    auto p = jbc::ir::promoteMethod(*s, idM);
    ASSERT_TRUE(p.is_ok());
    ASSERT_TRUE(jbc::ir::lowerMethod(*s, idM).is_ok());

    auto bytes = jbc::test::writeOk(s->backing());
    auto s2    = sessionOf(bytes);
    ASSERT_NE(s2, nullptr);
    EXPECT_EQ(s2->unit().findMethod("n", "(I)I"), kNoMethod);
    EXPECT_NE(s2->unit().findMethod("m", "()I"), kNoMethod);
    EXPECT_NE(s2->unit().findMethod("sketch", "()V"), kNoMethod);
}

TEST(MemberOpsA3Test, RemovedHandleInvalidationAndSlotReuseGeneration)
{
    auto s = sessionOf(makeMemberClass());
    ASSERT_NE(s, nullptr);
    const MethodId oldId = s->unit().findMethodId("n", "(I)I");
    ASSERT_NE(oldId, kNoMethodId);

    ASSERT_TRUE(s->removeMethod(oldId).is_ok());

    // 旧句柄在全部通道上显式失配，不是 UB。
    EXPECT_EQ(s->unit().method(oldId), nullptr);
    EXPECT_EQ(s->unit().indexOf(oldId), kNoMethod);
    EXPECT_EQ(s->unit().methodId(s->unit().indexOf(oldId)), kNoMethodId);
    auto c = s->commitMethodMetadata(oldId);
    ASSERT_TRUE(c.is_err());
    EXPECT_EQ(c.unwrap_err(), LowerError::InvalidHandle);
    auto p = jbc::ir::promoteMethod(*s, oldId);
    ASSERT_TRUE(p.is_err());
    EXPECT_EQ(p.unwrap_err(), PromoteError::InvalidHandle);
    auto l = jbc::ir::lowerMethod(*s, oldId);
    ASSERT_TRUE(l.is_err());
    EXPECT_EQ(l.unwrap_err(), LowerError::InvalidHandle);
    auto r = s->removeMethod(oldId);
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), LowerError::InvalidHandle);

    // 重建：新成员复用同槽，generation 自增——旧句柄不会复活。
    jbc::ir::MethodSeed seed;
    seed.name   = "n2";
    seed.desc   = "()I";
    seed.access = 0x0009;
    seed.withCode = true;
    seed.maxStack = 1;
    auto added = s->addMethod(seed);
    ASSERT_TRUE(added.is_ok());
    const MethodId newId = added.unwrap();
    EXPECT_EQ(newId.slot, oldId.slot);
    EXPECT_EQ(newId.generation, oldId.generation + 1);
    EXPECT_NE(newId, oldId);
    EXPECT_NE(s->unit().method(newId), nullptr);
    EXPECT_EQ(s->unit().method(oldId), nullptr);
}

// ===========================================================================
// A3：跨会话方法搬移（adopt）
// ===========================================================================

TEST(MemberOpsA3Test, AdoptMethodReplaysBodyAndInternsCp)
{
    auto donor = sessionOf(makeDonorClass());
    auto host  = sessionOf(makeMemberClass());
    ASSERT_NE(donor, nullptr);
    ASSERT_NE(host, nullptr);
    const MethodId donorGrab = donor->unit().findMethodId("grab", "()I");
    ASSERT_NE(donorGrab, kNoMethodId);
    const ca::u32 hostCpBefore = host->backing().cp.size();

    auto moved = host->adoptMethod(*donor, donorGrab);
    ASSERT_TRUE(moved.is_ok());
    const MethodId hostGrab = moved.unwrap();
    EXPECT_NE(host->unit().findMethodId("grab", "()I"), kNoMethodId);

    // 挂载点常量 intern 进宿主池（宿主池增长）。
    EXPECT_GT(host->backing().cp.size(), hostCpBefore);

    // 体随迁：指令形状与语义引用逐条可辨；owner 仍是供体名——重映射是
    // 消费者侧职责（ToolClassCombiner 场景），搬移本身逐字保真。
    auto p = jbc::ir::promoteMethod(*host, hostGrab);
    ASSERT_TRUE(p.is_ok());
    const jbc::ir::CodeUnit& cu = *p.unwrap();
    EXPECT_EQ(cu.dom.size(), 4u);
    std::vector<Op> ops;
    for (const jbc::ir::InsnId iid : cu.dom.order()) {
        const jbc::ir::Insn* in = cu.dom.peek(iid);
        ASSERT_NE(in, nullptr);
        ops.push_back(in->op);
    }
    EXPECT_EQ(ops, (std::vector<Op>{Op::BIPUSH, Op::PUTSTATIC, Op::GETSTATIC, Op::IRETURN}));
    ClassSession::MemberSem sem;
    ASSERT_TRUE(host->memberOf(*cu.dom.peek(cu.dom.order()[2]), sem));
    EXPECT_EQ(sem.kind, ClassSession::MemberSem::Kind::Field);
    EXPECT_EQ(sem.owner, "d/D");
    EXPECT_EQ(sem.name, "stash");
    EXPECT_EQ(sem.desc, "I");

    // 搬移入口已 lower 落盘：写回重读可见。
    auto bytes = jbc::test::writeOk(host->backing());
    auto s2    = sessionOf(bytes);
    ASSERT_NE(s2, nullptr);
    const MethodId again = s2->unit().findMethodId("grab", "()I");
    ASSERT_NE(again, kNoMethodId);
    auto p2 = jbc::ir::promoteMethod(*s2, again);
    ASSERT_TRUE(p2.is_ok());
    EXPECT_EQ(p2.unwrap()->dom.size(), 4u);
}

TEST(MemberOpsA3Test, AdoptMethodRejectsSelfSourceAndDeadHandle)
{
    auto donor = sessionOf(makeDonorClass());
    auto host  = sessionOf(makeMemberClass());
    ASSERT_NE(donor, nullptr);
    ASSERT_NE(host, nullptr);

    const MethodId hostOwn = host->unit().findMethodId("m", "()I");
    ASSERT_NE(hostOwn, kNoMethodId);
    auto self = host->adoptMethod(*host, hostOwn);
    ASSERT_TRUE(self.is_err());
    EXPECT_EQ(self.unwrap_err(), LowerError::InvalidHandle);

    MethodId ghost = donor->unit().findMethodId("grab", "()I");
    ASSERT_NE(ghost, kNoMethodId);
    ASSERT_TRUE(donor->removeMethod(ghost).is_ok());
    auto dead = host->adoptMethod(*donor, ghost);
    ASSERT_TRUE(dead.is_err());
    EXPECT_EQ(dead.unwrap_err(), LowerError::InvalidHandle);
}

/// 分支体供体类 d/B（回归钉死  分支体重放双缺陷，M 线 member_move
/// 夹具零分支缺口的无 JDK 单测面）：
///   - biz(I)I：for 循环后向 goto + 前向 if_icmpge + tableswitch + ldc +
///     异常表（catch-all）——三读门误伤分支族与占位标签被 append 拒收
///     两缺陷任一在场都必败；
///   - pick(I)I：lookupswitch——钉 key 表随迁（丢 keys 则 switch 形状
///     校验 BadOperand）。
Bytes makeBranchyDonorClass()
{
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("d/B");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 codeIdx  = cb.utf8("Code");
    const ca::u16 bizIdx   = cb.utf8("biz");
    const ca::u16 pickIdx  = cb.utf8("pick");
    const ca::u16 iiDesc   = cb.utf8("(I)I");
    const ca::u16 c100Idx  = cb.integer(100);

    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setAccess(0x0021);
    cb.setVersion(52);

    // biz 指令布局（分支偏移逐条手工核算，目标全部落在指令起点）：
    //   0 iconst_0; 1 istore_1; 2 iconst_0; 3 istore_2
    //   4 iload_2 (Lloop); 5 iload_0; 6 if_icmpge +13(→19); 9 iload_1;
    //   10 iload_2; 11 iadd; 12 istore_1; 13 iinc 2,1; 16 goto -12(→4)
    //   19 iload_0 (Lafter); 20 iconst_1; 21 iand;
    //   22 tableswitch{default→56, 0→44, 1→50}（24 起对齐）
    //   44 ldc 100; 46 istore_1; 47 goto +9(→56);
    //   50 iinc 1,10; 53 goto +3(→56); 56 iload_1 (Ltail); 57 ireturn;
    //   58 pop (Lh); 59 iload_1; 60 ireturn —— 异常表 [19,44)→58 catch-all
    const Bytes biz = {
        0x03, 0x3c, 0x03, 0x3d,             // 0-3
        0x1c, 0x1a, 0xa2, 0x00, 0x0d,       // 4-8  Lloop; if_icmpge → 19
        0x1b, 0x1c, 0x60, 0x3c,             // 9-12
        0x84, 0x02, 0x01,                   // 13-15 iinc 2,1
        0xa7, 0xff, 0xf4,                   // 16-18 goto → 4
        0x1a, 0x04, 0x7e,                   // 19-21 Lafter; x&1
        0xaa, 0x00,                         // 22 tableswitch + 对齐填充
        0x00, 0x00, 0x00, 0x22,             //   default → 56
        0x00, 0x00, 0x00, 0x00,             //   low = 0
        0x00, 0x00, 0x00, 0x01,             //   high = 1
        0x00, 0x00, 0x00, 0x16,             //   case 0 → 44
        0x00, 0x00, 0x00, 0x1c,             //   case 1 → 50
        0x12, static_cast<ca::u8>(c100Idx), // 44 ldc 100
        0x3c,                               // 46 istore_1
        0xa7, 0x00, 0x09,                   // 47 goto → 56
        0x84, 0x01, 0x0a,                   // 50 iinc 1,10
        0xa7, 0x00, 0x03,                   // 53 goto → 56
        0x1b, 0xac,                         // 56-57 Ltail; ireturn
        0x57, 0x1b, 0xac,                   // 58-60 Lh: pop; iload_1; ireturn
    };
    cb.addMethod(0x0009, bizIdx, iiDesc,
                 {AttrSpec{codeIdx, jbc::test::codeContent(2, 3, biz,
                                                          {{19, 44, 58, 0}})}});

    // pick 指令布局：
    //   0 iload_0; 1 lookupswitch{default→38, 1→28, 9→32}（4 起对齐）
    //   28 iconst_1 (L1); 29 goto +8(→37); 32 bipush 9 (L9); 34 goto +3(→37);
    //   37 ireturn (Lret); 38 iconst_m1 (Ldef); 39 ireturn
    const Bytes pick = {
        0x1a,                               // 0
        0xab, 0x00, 0x00,                   // 1 lookupswitch + 对齐填充
        0x00, 0x00, 0x00, 0x25,             //   default → 38
        0x00, 0x00, 0x00, 0x02,             //   npairs = 2
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x1b,   //   1 → 28
        0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x1f,   //   9 → 32
        0x04,                               // 28 L1 iconst_1
        0xa7, 0x00, 0x08,                   // 29 goto → 37
        0x10, 0x09,                         // 32 L9 bipush 9
        0xa7, 0x00, 0x03,                   // 34 goto → 37
        0xac,                               // 37 Lret ireturn
        0x02, 0xac,                         // 38-39 Ldef iconst_m1; ireturn
    };
    cb.addMethod(0x0009, pickIdx, iiDesc,
                 {AttrSpec{codeIdx, jbc::test::codeContent(1, 1, pick)}});
    return cb.build();
}

TEST(MemberOpsA3Test, AdoptMethodReplaysBranchyBodyWithTargetsAndExc)
{
    auto donor = sessionOf(makeBranchyDonorClass());
    auto host  = sessionOf(makeMemberClass());
    ASSERT_NE(donor, nullptr);
    ASSERT_NE(host, nullptr);

    // 双缺陷回归门：含分支方法体的 adopt 此前在 CP 三读门（分支族误伤）
    // 或占位标签 append 校验处必败 Err(BadInstructions)。
    const MethodId donorBiz = donor->unit().findMethodId("biz", "(I)I");
    ASSERT_NE(donorBiz, kNoMethodId);
    auto movedBiz = host->adoptMethod(*donor, donorBiz);
    ASSERT_TRUE(movedBiz.is_ok()) << "分支体 adopt 必须成功";
    const MethodId hostBiz = movedBiz.unwrap();

    const MethodId donorPick = donor->unit().findMethodId("pick", "(I)I");
    ASSERT_NE(donorPick, kNoMethodId);
    auto movedPick = host->adoptMethod(*donor, donorPick);
    ASSERT_TRUE(movedPick.is_ok()) << "lookupswitch 体 adopt 必须成功";
    const MethodId hostPick = movedPick.unwrap();

    // biz 体逐条保真：27 条指令，前向/后向分支与 switch 目标全部落到
    // 对应指令，异常表边界与 catch-all（catchCp == 0）随迁。
    auto p = jbc::ir::promoteMethod(*host, hostBiz);
    ASSERT_TRUE(p.is_ok());
    const jbc::ir::CodeUnit& cu = *p.unwrap();
    const std::vector<jbc::ir::InsnId>& order = cu.dom.order();
    ASSERT_EQ(order.size(), 27u);
    const std::vector<Op> wantOps = {
        Op::ICONST_0, Op::ISTORE_1, Op::ICONST_0, Op::ISTORE_2,
        Op::ILOAD_2,  Op::ILOAD_0,  Op::IF_ICMPGE,
        Op::ILOAD_1,  Op::ILOAD_2,  Op::IADD,     Op::ISTORE_1,
        Op::IINC,     Op::GOTO,
        Op::ILOAD_0,  Op::ICONST_1, Op::IAND,     Op::TABLESWITCH,
        Op::LDC,      Op::ISTORE_1, Op::GOTO,
        Op::IINC,     Op::GOTO,
        Op::ILOAD_1,  Op::IRETURN,
        Op::POP,      Op::ILOAD_1,  Op::IRETURN,
    };
    for (std::size_t i = 0; i < wantOps.size(); ++i) {
        const jbc::ir::Insn* in = cu.dom.peek(order[i]);
        ASSERT_NE(in, nullptr);
        ASSERT_EQ(in->op, wantOps[i]) << "biz 指令 #" << i;
    }

    // 前向（if_icmpge → Lafter）与后向（goto → Lloop）分支目标。
    const jbc::ir::Insn* ifge = cu.dom.peek(order[6]);
    ASSERT_NE(ifge, nullptr);
    auto after = cu.dom.resolveLabel(ifge->target);
    ASSERT_TRUE(after.is_ok());
    EXPECT_EQ(after.unwrap().insn, order[13]) << "if_icmpge 目标应为 Lafter(iload_0)";
    const jbc::ir::Insn* backGoto = cu.dom.peek(order[12]);
    ASSERT_NE(backGoto, nullptr);
    auto loop = cu.dom.resolveLabel(backGoto->target);
    ASSERT_TRUE(loop.is_ok());
    EXPECT_EQ(loop.unwrap().insn, order[4]) << "goto 目标应为 Lloop(iload_2)";

    // tableswitch：default 与 case 表全部重锚；tableswitch 无 key 表。
    const jbc::ir::Insn* sw = cu.dom.peek(order[16]);
    ASSERT_NE(sw, nullptr);
    ASSERT_EQ(sw->targets.size(), 2u);
    EXPECT_TRUE(sw->keys.empty());
    auto swDefault = cu.dom.resolveLabel(sw->target);
    ASSERT_TRUE(swDefault.is_ok());
    EXPECT_EQ(swDefault.unwrap().insn, order[22]) << "default 应为 Ltail(iload_1)";
    auto swCase0 = cu.dom.resolveLabel(sw->targets[0]);
    ASSERT_TRUE(swCase0.is_ok());
    EXPECT_EQ(swCase0.unwrap().insn, order[17]) << "case 0 应为 ldc";
    auto swCase1 = cu.dom.resolveLabel(sw->targets[1]);
    ASSERT_TRUE(swCase1.is_ok());
    EXPECT_EQ(swCase1.unwrap().insn, order[20]) << "case 1 应为 iinc 1,10";

    // 异常表：[Lafter, ldc) → handler(pop)，catch-all 保持 0 号槽。
    ASSERT_EQ(cu.exc.size(), 1u);
    auto excStart = cu.dom.resolveLabel(cu.exc[0].start);
    ASSERT_TRUE(excStart.is_ok());
    EXPECT_EQ(excStart.unwrap().insn, order[13]);
    auto excEnd = cu.dom.resolveLabel(cu.exc[0].end);
    ASSERT_TRUE(excEnd.is_ok());
    EXPECT_EQ(excEnd.unwrap().insn, order[17]);
    auto excHandler = cu.dom.resolveLabel(cu.exc[0].handler);
    ASSERT_TRUE(excHandler.is_ok());
    EXPECT_EQ(excHandler.unwrap().insn, order[24]);
    EXPECT_EQ(cu.exc[0].catchCp, 0u);

    // pick 体：lookupswitch key 表随迁（丢 keys 即形状校验 BadOperand），
    // default 与两个 case 目标逐一对位。
    auto pPick = jbc::ir::promoteMethod(*host, hostPick);
    ASSERT_TRUE(pPick.is_ok());
    const jbc::ir::CodeUnit& cuPick = *pPick.unwrap();
    const std::vector<jbc::ir::InsnId>& pickOrder = cuPick.dom.order();
    ASSERT_EQ(pickOrder.size(), 9u);
    const jbc::ir::Insn* lsw = cuPick.dom.peek(pickOrder[1]);
    ASSERT_NE(lsw, nullptr);
    ASSERT_EQ(lsw->op, Op::LOOKUPSWITCH);
    ASSERT_EQ(lsw->keys.size(), 2u);
    EXPECT_EQ(lsw->keys[0], 1);
    EXPECT_EQ(lsw->keys[1], 9);
    ASSERT_EQ(lsw->targets.size(), 2u);
    auto pickCase1 = cuPick.dom.resolveLabel(lsw->targets[0]);
    ASSERT_TRUE(pickCase1.is_ok());
    EXPECT_EQ(pickCase1.unwrap().insn, pickOrder[2]) << "case 1 应为 iconst_1";
    auto pickCase9 = cuPick.dom.resolveLabel(lsw->targets[1]);
    ASSERT_TRUE(pickCase9.is_ok());
    EXPECT_EQ(pickCase9.unwrap().insn, pickOrder[4]) << "case 9 应为 bipush 9";
    auto pickDefault = cuPick.dom.resolveLabel(lsw->target);
    ASSERT_TRUE(pickDefault.is_ok());
    EXPECT_EQ(pickDefault.unwrap().insn, pickOrder[7]) << "default 应为 iconst_m1";

    // 写回重读：lower 后的分支体完整落盘、可再升格。
    auto bytes = jbc::test::writeOk(host->backing());
    auto s2    = sessionOf(bytes);
    ASSERT_NE(s2, nullptr);
    const MethodId bizAgain = s2->unit().findMethodId("biz", "(I)I");
    ASSERT_NE(bizAgain, kNoMethodId);
    auto p2 = jbc::ir::promoteMethod(*s2, bizAgain);
    ASSERT_TRUE(p2.is_ok());
    EXPECT_EQ(p2.unwrap()->dom.size(), 27u);
    EXPECT_EQ(p2.unwrap()->exc.size(), 1u);
}

// ===========================================================================
// A4：字段身份、元数据提交与增删
// ===========================================================================

TEST(MemberOpsA4Test, FieldIdIdentityAndFind)
{
    auto s = sessionOf(makeMemberClass());
    ASSERT_NE(s, nullptr);
    const jbc::ir::ClassUnit& u = s->unit();

    const FieldId idAlpha = u.findFieldId("alpha");
    ASSERT_NE(idAlpha, kNoFieldId);
    EXPECT_EQ(u.indexOf(idAlpha), 0u);
    EXPECT_EQ(u.fieldId(0), idAlpha);
    EXPECT_EQ(u.fieldId(99), kNoFieldId);
    EXPECT_NE(u.field(idAlpha), nullptr);
    EXPECT_EQ(u.symbols().str(u.field(idAlpha)->name), "alpha");

    EXPECT_EQ(u.findField("beta"), 1u);
    EXPECT_EQ(u.findField("alpha", "I"), 0u);
    EXPECT_EQ(u.findField("alpha", "J"), kNoField);
    EXPECT_EQ(u.findFieldId("missing"), kNoFieldId);
    EXPECT_EQ(u.field(kNoFieldId), nullptr);
}

TEST(MemberOpsA4Test, CommitFieldMetadataSurvivesReorder)
{
    auto s = sessionOf(makeMemberClass());
    ASSERT_NE(s, nullptr);
    const FieldId id = s->unit().findFieldId("alpha");
    ASSERT_NE(id, kNoFieldId);
    auto* fu = s->unit().field(id);
    ASSERT_NE(fu, nullptr);

    fu->name   = s->unit().symbols().intern("gamma");
    fu->access = 0x000a;
    std::swap(s->unit().fields[0], s->unit().fields[1]);

    ASSERT_TRUE(s->commitFieldMetadata(id).is_ok());
    auto bytes = jbc::test::writeOk(s->backing());
    auto s2    = sessionOf(bytes);
    ASSERT_NE(s2, nullptr);
    EXPECT_NE(s2->unit().findField("gamma", "I"), kNoField);
    EXPECT_EQ(s2->unit().findField("alpha"), kNoField);
    const jbc::ir::FieldUnit* g = s2->unit().field(s2->unit().findFieldId("gamma"));
    ASSERT_NE(g, nullptr);
    EXPECT_EQ(g->access, 0x000au);
}

TEST(MemberOpsA4Test, AddRemoveFieldGenerationInvalidation)
{
    auto s = sessionOf(makeMemberClass());
    ASSERT_NE(s, nullptr);
    const ca::usize before = s->unit().fields.size();

    jbc::ir::FieldSeed seed;
    seed.name   = "delta";
    seed.desc   = "I";
    seed.access = 0x000a;
    auto added  = s->addField(seed);
    ASSERT_TRUE(added.is_ok());
    const FieldId id = added.unwrap();
    EXPECT_EQ(s->unit().fields.size(), before + 1);
    EXPECT_EQ(s->backing().fields.size(), before + 1);
    EXPECT_EQ(s->unit().findFieldId("delta", "I"), id);

    // 重名拒绝。
    jbc::ir::FieldSeed dup;
    dup.name   = "delta";
    dup.desc   = "I";
    dup.access = 0x000a;
    auto r     = s->addField(dup);
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), LowerError::InvalidSeed);

    ASSERT_TRUE(s->removeField(id).is_ok());
    EXPECT_EQ(s->unit().fields.size(), before);
    EXPECT_EQ(s->backing().fields.size(), before);
    EXPECT_EQ(s->unit().field(id), nullptr);
    auto c = s->commitFieldMetadata(id);
    ASSERT_TRUE(c.is_err());
    EXPECT_EQ(c.unwrap_err(), LowerError::InvalidHandle);
    ASSERT_TRUE(s->removeField(id).is_err());

    // 槽位复用 + generation 自增。
    jbc::ir::FieldSeed seed2;
    seed2.name   = "delta2";
    seed2.desc   = "J";
    seed2.access = 0x000a;
    auto added2  = s->addField(seed2);
    ASSERT_TRUE(added2.is_ok());
    EXPECT_EQ(added2.unwrap().slot, id.slot);
    EXPECT_EQ(added2.unwrap().generation, id.generation + 1);
    EXPECT_EQ(s->unit().field(id), nullptr);
    EXPECT_NE(s->unit().field(added2.unwrap()), nullptr);
}

TEST(MemberOpsA4Test, AdoptFieldCarriesMetadata)
{
    auto donor = sessionOf(makeDonorClass());
    auto host  = sessionOf(makeMemberClass());
    ASSERT_NE(donor, nullptr);
    ASSERT_NE(host, nullptr);
    const FieldId donorStash = donor->unit().findFieldId("stash");
    ASSERT_NE(donorStash, kNoFieldId);

    auto moved = host->adoptField(*donor, donorStash);
    ASSERT_TRUE(moved.is_ok());
    const FieldId hostStash = moved.unwrap();
    EXPECT_NE(host->unit().findFieldId("stash", "I"), kNoFieldId);
    const jbc::ir::FieldUnit* fu = host->unit().field(hostStash);
    ASSERT_NE(fu, nullptr);
    EXPECT_EQ(fu->access, 0x0009u);   // public static 原样随迁

    auto self = host->adoptField(*host, hostStash);
    ASSERT_TRUE(self.is_err());
    EXPECT_EQ(self.unwrap_err(), LowerError::InvalidHandle);

    auto bytes = jbc::test::writeOk(host->backing());
    auto s2    = sessionOf(bytes);
    ASSERT_NE(s2, nullptr);
    EXPECT_NE(s2->unit().findField("stash", "I"), kNoField);
}

// ===========================================================================
// A5：从零建类
// ===========================================================================

TEST(MemberOpsA5Test, MakeSessionFromSeedRoundtripWritesWithoutSource)
{
    jbc::ir::NewClassSeed seed;
    seed.version   = (52u << 16);
    seed.access    = 0x0021;
    seed.thisName  = "q/N";
    seed.superName = "java/lang/Object";
    auto r         = jbc::ir::makeSession(seed);
    ASSERT_TRUE(r.is_ok());
    std::shared_ptr<ClassSession> s = std::move(r).unwrap();

    EXPECT_TRUE(s->unit().fields.empty());
    EXPECT_TRUE(s->unit().methods.empty());
    EXPECT_EQ(s->unit().version, 52u << 16);
    EXPECT_EQ(s->unit().access, 0x0021u);
    EXPECT_EQ(s->unit().symbols().str(s->unit().thisClass), "q/N");
    EXPECT_EQ(s->unit().symbols().str(s->unit().superClass), "java/lang/Object");
    // CP 引导：占位 0 + this{Utf8,Class} + super{Utf8,Class} = 5。
    EXPECT_EQ(s->backing().cp.size(), 5u);
    EXPECT_EQ(s->backing().thisName(), "q/N");
    EXPECT_EQ(s->backing().superName(), "java/lang/Object");

    ASSERT_TRUE(s->commitClassMetadata().is_ok());
    // 无源字节的合成模型可写出（writer 从零分支）。
    auto bytes = jbc::test::writeOk(s->backing());
    auto s2    = sessionOf(bytes);
    ASSERT_NE(s2, nullptr);
    EXPECT_EQ(s2->backing().thisName(), "q/N");
    EXPECT_EQ(s2->backing().superName(), "java/lang/Object");
    EXPECT_EQ(s2->backing().major, 52u);
    EXPECT_EQ(s2->backing().access, 0x0021u);
    EXPECT_TRUE(s2->backing().methods.empty());
}

TEST(MemberOpsA5Test, MakeSessionRejectsInvalidSeed)
{
    jbc::ir::NewClassSeed noThis;
    noThis.superName = "java/lang/Object";
    auto r           = jbc::ir::makeSession(noThis);
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), PromoteError::InvalidSeed);

    jbc::ir::NewClassSeed badAccess;
    badAccess.thisName = "q/B";
    badAccess.access   = 0x10000u;
    auto r2            = jbc::ir::makeSession(badAccess);
    ASSERT_TRUE(r2.is_err());
    EXPECT_EQ(r2.unwrap_err(), PromoteError::InvalidSeed);
}

TEST(MemberOpsA5Test, FreshSessionHostsMemberBuildPipeline)
{
    jbc::ir::NewClassSeed seed;
    seed.thisName  = "q/Built";
    seed.superName = "java/lang/Object";
    auto r         = jbc::ir::makeSession(seed);
    ASSERT_TRUE(r.is_ok());
    std::shared_ptr<ClassSession> s = std::move(r).unwrap();

    jbc::ir::MethodSeed main;
    main.name     = "main";
    main.desc     = "([Ljava/lang/String;)V";
    main.access   = 0x0009;
    main.withCode = true;
    main.maxStack = 2;
    main.maxLocals = 1;
    auto added    = s->addMethod(main);
    ASSERT_TRUE(added.is_ok());
    const MethodId id = added.unwrap();

    auto p = jbc::ir::promoteMethod(*s, id);
    ASSERT_TRUE(p.is_ok());
    {
        jbc::ir::CodeEditor ed(*p.unwrap());
        const ca::u32 outIdx = s->internFieldRef("java/lang/System", "out",
                                                 "Ljava/io/PrintStream;");
        const ca::u32 msgIdx = s->internString("fresh-built");
        const ca::u32 printIdx = s->internMethodRef("java/io/PrintStream", "println",
                                                    "(Ljava/lang/String;)V");
        ASSERT_TRUE(ed.append(jbc::ir::CodeEditor::cpInsn(Op::GETSTATIC, outIdx)).is_ok());
        ASSERT_TRUE(ed.append(jbc::ir::CodeEditor::loadConst(msgIdx)).is_ok());
        ASSERT_TRUE(ed.append(jbc::ir::CodeEditor::cpInsn(Op::INVOKEVIRTUAL, printIdx)).is_ok());
        ASSERT_TRUE(ed.append(jbc::ir::CodeEditor::simple(Op::RETURN)).is_ok());
    }
    ASSERT_TRUE(jbc::ir::lowerMethod(*s, id).is_ok());
    ASSERT_TRUE(s->commitClassMetadata().is_ok());

    auto bytes = jbc::test::writeOk(s->backing());
    auto s2    = sessionOf(bytes);
    ASSERT_NE(s2, nullptr);
    const MethodId id2 = s2->unit().findMethodId("main", "([Ljava/lang/String;)V");
    ASSERT_NE(id2, kNoMethodId);
    auto p2 = jbc::ir::promoteMethod(*s2, id2);
    ASSERT_TRUE(p2.is_ok());
    EXPECT_EQ(p2.unwrap()->dom.size(), 4u);
}
