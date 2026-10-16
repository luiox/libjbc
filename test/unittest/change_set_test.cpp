/// @file        change_set_test.cpp
/// @brief       E2 ChangeSet 协议单元测试（design.md §35）。
///
/// 全部用关内建 ClassBuilder 字节构造（不依赖本机语料与 JDK）。测试矩阵：
///   - 记账形状：从零建出 / 解析会话零起点 / 增删（方法+字段）/ adopt /
///     lower copy-through 不记 / lower 实改记 / 元数据实改记+未改不记 /
///     改名带 prior / 类级实改记；
///   - 折叠语义：add+remove → removals 桶 / remove+add → 存活+created /
///     改名 A→B→A → A 存活 B 消失；
///   - 等价性前提：记账面纯观察——同一批变换带/不带 ChangeSet 消费，写回
///     字节逐位一致。
#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "jbc/jbc.hpp"
#include "test_class_bytes.hpp"

using namespace jbc::core;
using jbc::core::Op;
using jbc::ir::ChangeKind;
using jbc::ir::ChangeRecord;
using jbc::ir::ClassChange;
using jbc::ir::ClassSession;
using jbc::ir::CodeEditor;
using jbc::ir::FieldSeed;
using jbc::ir::MemberKind;
using jbc::ir::MethodSeed;
using jbc::ir::NewClassSeed;
using jbc::test::AttrSpec;
using jbc::test::ClassBuilder;
using Bytes = std::vector<ca::u8>;

namespace {

/// 基准类 p/M：1 方法 m()I 有体（iconst_0; ireturn）+ 1 字段 alpha:I。
Bytes makeOneMemberClass()
{
    ClassBuilder cb;
    const ca::u16 thisIdx  = cb.utf8("p/M");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 codeIdx  = cb.utf8("Code");
    const ca::u16 mIdx     = cb.utf8("m");
    const ca::u16 vDesc    = cb.utf8("()I");
    const ca::u16 aIdx     = cb.utf8("alpha");
    const ca::u16 iDesc    = cb.utf8("I");

    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setAccess(0x0021);
    cb.setVersion(52);
    cb.addField(0x0002, aIdx, iDesc);
    cb.addMethod(0x0009, mIdx, vDesc,
                 {AttrSpec{codeIdx, jbc::test::codeContent(1, 0, {0x04, 0xac})}});
    return cb.build();
}

std::shared_ptr<ClassSession> sessionOf(const Bytes& bytes)
{
    auto r = jbc::ir::makeSessionFromBytes(ByteView{bytes.data(), bytes.size()});
    return r.is_ok() ? std::move(r).unwrap() : nullptr;
}

/// adopt 供体类 d/D：grab()I 有体（iconst_7; ireturn）——与目标类成员异名
/// （会话内名字+描述符唯一性预检挡同名挂入），无 CP 操作数（重放免池结构）。
Bytes makeDonorClass()
{
    ClassBuilder cb;
    const ca::u16 thisIdx  = cb.utf8("d/D");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 codeIdx  = cb.utf8("Code");
    const ca::u16 gIdx      = cb.utf8("grab");
    const ca::u16 iDesc     = cb.utf8("()I");

    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setAccess(0x0021);
    cb.setVersion(52);
    cb.addMethod(0x0009, gIdx, iDesc,
                 {AttrSpec{codeIdx, jbc::test::codeContent(1, 0, {0x08, 0xac})}});
    return cb.build();
}

bool hasRecord(const jbc::ir::ChangeSet& cs, ChangeKind kind, MemberKind member,
               std::string_view name = {}, std::string_view desc = {})
{
    for (const ChangeRecord& r : cs.records()) {
        if (r.kind != kind || r.member != member) continue;
        if (!name.empty() && r.name != name) continue;
        if (!desc.empty() && r.desc != desc) continue;
        return true;
    }
    return false;
}

}   // namespace

// ===========================================================================
// 记账形状
// ===========================================================================

/// 从零建出：ClassCreated 记账，类级查询 Created 优先。
TEST(ChangeSetTest, SeedSessionRecordsClassCreated)
{
    NewClassSeed seed;
    seed.thisName  = "q/N";
    seed.superName = "java/lang/Object";
    auto s = jbc::ir::makeSession(seed);
    ASSERT_TRUE(s.is_ok());
    const jbc::ir::ChangeSet& cs = s.unwrap()->changeSet();
    EXPECT_EQ(cs.size(), ca::usize{1});
    EXPECT_EQ(cs.classChange(), ClassChange::Created);
    EXPECT_TRUE(hasRecord(cs, ChangeKind::ClassCreated, MemberKind::None));
}

/// 解析会话零起点：未变换前台账为空。
TEST(ChangeSetTest, ParsedSessionStartsEmpty)
{
    auto s = sessionOf(makeOneMemberClass());
    ASSERT_NE(s, nullptr);
    EXPECT_TRUE(s->changeSet().empty());
    EXPECT_EQ(s->changeSet().classChange(), ClassChange::None);
    EXPECT_TRUE(s->changeSet().memberChanges().empty());
    EXPECT_TRUE(s->changeSet().removedMembers().empty());
}

/// 成员挂入：MemberAdded（方法+字段），折叠 created 置位。
TEST(ChangeSetTest, AddMemberRecords)
{
    auto s = sessionOf(makeOneMemberClass());
    ASSERT_NE(s, nullptr);

    MethodSeed ms;
    ms.name     = "added";
    ms.desc     = "()V";
    ms.access   = 0x0009;
    ms.withCode = true;
    ASSERT_TRUE(s->addMethod(ms).is_ok());

    FieldSeed fs;
    fs.name   = "gamma";
    fs.desc   = "J";
    fs.access = 0x0002;
    ASSERT_TRUE(s->addField(fs).is_ok());

    const jbc::ir::ChangeSet& cs = s->changeSet();
    EXPECT_TRUE(hasRecord(cs, ChangeKind::MemberAdded, MemberKind::Method, "added", "()V"));
    EXPECT_TRUE(hasRecord(cs, ChangeKind::MemberAdded, MemberKind::Field, "gamma", "J"));

    const auto ups = cs.memberChanges();
    ASSERT_EQ(ups.size(), ca::usize{2});
    bool sawMethod = false;
    bool sawField  = false;
    for (const auto& m : ups) {
        EXPECT_TRUE(m.created);
        EXPECT_FALSE(m.bodyChanged);
        if (m.member == MemberKind::Method && m.name == "added") sawMethod = true;
        if (m.member == MemberKind::Field && m.name == "gamma") sawField = true;
    }
    EXPECT_TRUE(sawMethod);
    EXPECT_TRUE(sawField);
    EXPECT_TRUE(cs.removedMembers().empty());
}

/// 成员摘除：MemberRemoved（方法+字段），折叠落在 removals 桶。
TEST(ChangeSetTest, RemoveMemberRecords)
{
    auto s = sessionOf(makeOneMemberClass());
    ASSERT_NE(s, nullptr);
    const jbc::ir::MethodId m = s->unit().findMethodId("m", "()I");
    ASSERT_FALSE(m == jbc::ir::kNoMethodId);
    const jbc::ir::FieldId f = s->unit().findFieldId("alpha");
    ASSERT_FALSE(f == jbc::ir::kNoFieldId);
    ASSERT_TRUE(s->removeMethod(m).is_ok());
    ASSERT_TRUE(s->removeField(f).is_ok());

    const jbc::ir::ChangeSet& cs = s->changeSet();
    EXPECT_TRUE(hasRecord(cs, ChangeKind::MemberRemoved, MemberKind::Method, "m", "()I"));
    EXPECT_TRUE(hasRecord(cs, ChangeKind::MemberRemoved, MemberKind::Field, "alpha", "I"));
    EXPECT_TRUE(cs.memberChanges().empty());

    const auto gone = cs.removedMembers();
    ASSERT_EQ(gone.size(), ca::usize{2});
}

/// adopt：目标会话记 MemberAdded + MethodBody（重放体经 lower 落账）；
/// 源会话不记 remove（搬移是拷贝不是摘除）。供体成员与目标类异名（会话内
/// 名字+描述符唯一性预检会挡同名挂入）。
TEST(ChangeSetTest, AdoptRecordsOnTargetOnly)
{
    auto src = sessionOf(makeDonorClass());
    auto dst = sessionOf(makeOneMemberClass());
    ASSERT_NE(src, nullptr);
    ASSERT_NE(dst, nullptr);
    const jbc::ir::MethodId m = src->unit().findMethodId("grab", "()I");
    ASSERT_FALSE(m == jbc::ir::kNoMethodId);

    const auto adopted = dst->adoptMethod(*src, m);
    ASSERT_TRUE(adopted.is_ok());

    const jbc::ir::ChangeSet& dcs = dst->changeSet();
    EXPECT_TRUE(hasRecord(dcs, ChangeKind::MemberAdded, MemberKind::Method, "grab", "()I"));
    EXPECT_TRUE(hasRecord(dcs, ChangeKind::MethodBody, MemberKind::Method, "grab", "()I"));
    const auto ups = dcs.memberChanges();
    ASSERT_EQ(ups.size(), ca::usize{1});
    EXPECT_TRUE(ups[0].created);
    EXPECT_TRUE(ups[0].bodyChanged);
    EXPECT_TRUE(src->changeSet().empty());
}

/// lower copy-through 不记账：promote 后原样 lower，台账保持为空。
TEST(ChangeSetTest, LowerCopyThroughRecordsNothing)
{
    auto s = sessionOf(makeOneMemberClass());
    ASSERT_NE(s, nullptr);
    const jbc::ir::MethodId m = s->unit().findMethodId("m", "()I");
    ASSERT_TRUE(jbc::ir::promoteMethod(*s, m).is_ok());
    ASSERT_TRUE(jbc::ir::lowerMethod(*s, m).is_ok());
    EXPECT_TRUE(s->changeSet().empty());
}

/// lower 实改记账：插一条 NOP 强制布局改变 → MethodBody，折叠 bodyChanged。
TEST(ChangeSetTest, LowerAfterEditRecordsBody)
{
    auto s = sessionOf(makeOneMemberClass());
    ASSERT_NE(s, nullptr);
    const jbc::ir::MethodId m = s->unit().findMethodId("m", "()I");
    auto p = jbc::ir::promoteMethod(*s, m);
    ASSERT_TRUE(p.is_ok());
    CodeEditor ed(*p.unwrap().get());
    ASSERT_TRUE(ed.insertAtStart(CodeEditor::simple(Op::NOP)).is_ok());
    ASSERT_TRUE(jbc::ir::lowerMethod(*s, m).is_ok());

    const jbc::ir::ChangeSet& cs = s->changeSet();
    EXPECT_TRUE(hasRecord(cs, ChangeKind::MethodBody, MemberKind::Method, "m", "()I"));
    const auto ups = cs.memberChanges();
    ASSERT_EQ(ups.size(), ca::usize{1});
    EXPECT_FALSE(ups[0].created);
    EXPECT_TRUE(ups[0].bodyChanged);
    EXPECT_FALSE(ups[0].metadataChanged);
}

/// 元数据粒度：仅 access 实改 → MemberMetadata（prior 为空）；未改重复提交
/// 不记。body 旗标不置位。
TEST(ChangeSetTest, MetadataChangeRecordsWithEmptyPrior)
{
    auto s = sessionOf(makeOneMemberClass());
    ASSERT_NE(s, nullptr);
    const jbc::ir::MethodId m = s->unit().findMethodId("m", "()I");
    jbc::ir::MethodUnit* mu = s->unit().method(m);
    ASSERT_NE(mu, nullptr);
    mu->access = 0x0001;   // public static → public
    ASSERT_TRUE(s->commitMethodMetadata(m).is_ok());
    // 未改重复提交：无新记录。
    ASSERT_TRUE(s->commitMethodMetadata(m).is_ok());

    const jbc::ir::ChangeSet& cs = s->changeSet();
    ASSERT_EQ(cs.size(), ca::usize{1});
    const ChangeRecord& r = cs.records()[0];
    EXPECT_EQ(r.kind, ChangeKind::MemberMetadata);
    EXPECT_EQ(r.member, MemberKind::Method);
    EXPECT_EQ(r.name, "m");
    EXPECT_TRUE(r.priorName.empty());

    const auto ups = cs.memberChanges();
    ASSERT_EQ(ups.size(), ca::usize{1});
    EXPECT_TRUE(ups[0].metadataChanged);
    EXPECT_FALSE(ups[0].bodyChanged);
}

/// 改名：MemberMetadata 带 prior；折叠 = 旧身份消失 + 新身份存活
/// （metadataChanged，created 继承原代）。
TEST(ChangeSetTest, RenameRecordsPriorIdentity)
{
    auto s = sessionOf(makeOneMemberClass());
    ASSERT_NE(s, nullptr);
    const jbc::ir::MethodId m = s->unit().findMethodId("m", "()I");
    jbc::ir::MethodUnit* mu = s->unit().method(m);
    ASSERT_NE(mu, nullptr);
    mu->name  = s->unit().symbols().intern("renamed");
    ASSERT_TRUE(s->commitMethodMetadata(m).is_ok());

    const jbc::ir::ChangeSet& cs = s->changeSet();
    ASSERT_EQ(cs.size(), ca::usize{1});
    const ChangeRecord& r = cs.records()[0];
    EXPECT_EQ(r.kind, ChangeKind::MemberMetadata);
    EXPECT_EQ(r.name, "renamed");
    EXPECT_EQ(r.priorName, "m");
    EXPECT_EQ(r.priorDesc, "()I");

    const auto ups = cs.memberChanges();
    ASSERT_EQ(ups.size(), ca::usize{1});
    EXPECT_EQ(ups[0].name, "renamed");
    EXPECT_TRUE(ups[0].metadataChanged);
    EXPECT_FALSE(ups[0].created);

    const auto gone = cs.removedMembers();
    ASSERT_EQ(gone.size(), ca::usize{1});
    EXPECT_EQ(gone[0].name, "m");
    EXPECT_EQ(gone[0].desc, "()I");
}

/// 类级粒度：unit.access 实改 + commitClassMetadata → ClassMetadata；
/// 未改重复提交不记。
TEST(ChangeSetTest, ClassMetadataChangeRecords)
{
    auto s = sessionOf(makeOneMemberClass());
    ASSERT_NE(s, nullptr);
    s->unit().access = 0x0031;
    ASSERT_TRUE(s->commitClassMetadata().is_ok());
    ASSERT_TRUE(s->commitClassMetadata().is_ok());
    const jbc::ir::ChangeSet& cs = s->changeSet();
    ASSERT_EQ(cs.size(), ca::usize{1});
    EXPECT_EQ(cs.records()[0].kind, ChangeKind::ClassMetadata);
    EXPECT_EQ(cs.classChange(), ClassChange::Metadata);
}

// ===========================================================================
// 折叠语义（§35.3）
// ===========================================================================

/// add 后 remove 同键：终态消失 → 只落 removals 桶，存活侧不出现。
TEST(ChangeSetTest, FoldAddThenRemoveVanishes)
{
    auto s = sessionOf(makeOneMemberClass());
    ASSERT_NE(s, nullptr);
    MethodSeed ms;
    ms.name   = "temp";
    ms.desc   = "()V";
    ms.access = 0x0009;
    const auto added = s->addMethod(ms);
    ASSERT_TRUE(added.is_ok());
    ASSERT_TRUE(s->removeMethod(added.unwrap()).is_ok());

    EXPECT_TRUE(s->changeSet().memberChanges().empty());
    const auto gone = s->changeSet().removedMembers();
    ASSERT_EQ(gone.size(), ca::usize{1});
    EXPECT_EQ(gone[0].name, "temp");
}

/// remove 后 add 同键（删旧挂新）：终态存活 → 只落存活桶且 created 置位
/// （中间代历史不外泄）。
TEST(ChangeSetTest, FoldRemoveThenAddRevives)
{
    auto s = sessionOf(makeOneMemberClass());
    ASSERT_NE(s, nullptr);
    const jbc::ir::MethodId m = s->unit().findMethodId("m", "()I");
    ASSERT_TRUE(s->removeMethod(m).is_ok());
    MethodSeed ms;
    ms.name     = "m";
    ms.desc     = "()I";
    ms.access   = 0x0009;
    ms.withCode = true;
    ASSERT_TRUE(s->addMethod(ms).is_ok());

    const auto ups = s->changeSet().memberChanges();
    ASSERT_EQ(ups.size(), ca::usize{1});
    EXPECT_EQ(ups[0].name, "m");
    EXPECT_TRUE(ups[0].created);
    EXPECT_TRUE(s->changeSet().removedMembers().empty());
}

/// 改名 A→B 再 B→A：终态 A 存活（metadataChanged），B 消失。
TEST(ChangeSetTest, FoldRenameAtoBtoA)
{
    auto s = sessionOf(makeOneMemberClass());
    ASSERT_NE(s, nullptr);
    const jbc::ir::MethodId m = s->unit().findMethodId("m", "()I");
    jbc::ir::MethodUnit* mu = s->unit().method(m);
    ASSERT_NE(mu, nullptr);
    mu->name = s->unit().symbols().intern("mid");
    ASSERT_TRUE(s->commitMethodMetadata(m).is_ok());
    mu->name = s->unit().symbols().intern("m");
    ASSERT_TRUE(s->commitMethodMetadata(m).is_ok());

    const auto ups = s->changeSet().memberChanges();
    ASSERT_EQ(ups.size(), ca::usize{1});
    EXPECT_EQ(ups[0].name, "m");
    EXPECT_TRUE(ups[0].metadataChanged);
    const auto gone = s->changeSet().removedMembers();
    ASSERT_EQ(gone.size(), ca::usize{1});
    EXPECT_EQ(gone[0].name, "mid");
}

// ===========================================================================
// 观察面不动字节（§35.4）
// ===========================================================================

/// 同一批变换：带 ChangeSet 消费与纯变换的写回字节逐位一致——记账是纯观察。
TEST(ChangeSetTest, RecordingDoesNotAlterWrittenBytes)
{
    const Bytes original = makeOneMemberClass();
    const auto transform = [](ClassSession& s) {
        const jbc::ir::MethodId m = s.unit().findMethodId("m", "()I");
        auto p = jbc::ir::promoteMethod(s, m);
        if (p.is_err()) return false;
        CodeEditor ed(*p.unwrap().get());
        if (!ed.insertAtStart(CodeEditor::simple(Op::NOP)).is_ok()) return false;
        if (jbc::ir::lowerMethod(s, m).is_err()) return false;
        MethodSeed ms;
        ms.name   = "extra";
        ms.desc   = "()V";
        ms.access = 0x0008;
        return s.addMethod(ms).is_ok();
    };

    auto observed = sessionOf(original);
    auto silent   = sessionOf(original);
    ASSERT_NE(observed, nullptr);
    ASSERT_NE(silent, nullptr);
    ASSERT_TRUE(transform(*observed));
    ASSERT_TRUE(transform(*silent));
    EXPECT_FALSE(observed->changeSet().empty());

    auto wb = jbc::core::writeClass(observed->backing());
    auto ws = jbc::core::writeClass(silent->backing());
    ASSERT_TRUE(wb.is_ok());
    ASSERT_TRUE(ws.is_ok());
    const Bytes& b = wb.unwrap();
    const Bytes& c = ws.unwrap();
    ASSERT_EQ(b.size(), c.size());
    EXPECT_TRUE(0 == std::memcmp(b.data(), c.data(), b.size()));
}
