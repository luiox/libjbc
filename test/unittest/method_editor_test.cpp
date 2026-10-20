/// @file        method_editor_test.cpp
/// @brief       Result-only 建方法门面（MethodEditor / addMethodWithBody）单测：
///        正常建体与重读、放弃编辑器留空体成员、append 错误不污染编辑器
///        （无 sticky 状态）、批量入口控制流前置拒绝与段/下标报告、种子
///        失败不留半截成员。
#include <gtest/gtest.h>

#include <memory>
#include <utility>
#include <vector>

#include "jbc/jbc.hpp"
#include "test_class_bytes.hpp"

using jbc::core::Op;
using jbc::ir::ClassSession;
using jbc::ir::CodeEditor;
using jbc::ir::EditError;
using jbc::ir::LowerError;
using jbc::ir::MethodBuildError;
using jbc::ir::MethodEditor;
using jbc::ir::MethodId;
using jbc::ir::MethodSeed;
using jbc::ir::NewClassSeed;
using jbc::test::Bytes;

namespace {

constexpr ca::u32 kVersion8       = 52u << 16;
constexpr ca::u32 kAccPublicStatic = 0x0009;

std::shared_ptr<ClassSession> newSession()
{
    NewClassSeed seed;
    seed.version   = kVersion8;
    seed.access    = 0x0021;
    seed.thisName  = "p/ME";
    seed.superName = "java/lang/Object";
    auto r         = jbc::ir::makeSession(seed);
    return r.is_ok() ? std::move(r).unwrap() : nullptr;
}

MethodSeed seedOf(const char* name, const char* desc)
{
    MethodSeed seed;
    seed.name     = name;
    seed.desc     = desc;
    seed.access   = kAccPublicStatic;
    seed.withCode = true;
    seed.maxStack = 2;
    return seed;
}

std::shared_ptr<ClassSession> reread(const std::shared_ptr<ClassSession>& s)
{
    const Bytes bytes = jbc::test::writeOk(s->backing());
    if (bytes.empty()) {
        return nullptr;
    }
    auto r = jbc::ir::makeSessionFromBytes(jbc::core::ByteView{bytes.data(), bytes.size()});
    return r.is_ok() ? std::move(r).unwrap() : nullptr;
}

}   // namespace

TEST(MethodEditorTest, AddMethodWithBodyLowersAndRereads)
{
    auto s = newSession();
    ASSERT_NE(s, nullptr);
    const ca::usize before = s->unit().methods.size();

    const std::vector<jbc::ir::Insn> body{
        CodeEditor::simple(Op::ICONST_1),
        CodeEditor::simple(Op::IRETURN),
    };
    auto r = jbc::ir::addMethodWithBody(*s, seedOf("one", "()I"), body);
    ASSERT_TRUE(r.is_ok());
    const MethodId id = r.unwrap();
    EXPECT_EQ(s->unit().methods.size(), before + 1);

    auto p = jbc::ir::promoteMethod(*s, id);
    ASSERT_TRUE(p.is_ok());
    auto u = std::move(p).unwrap();
    EXPECT_EQ(u->dom.size(), 2u);
    EXPECT_EQ(u->maxStack, 2u);

    auto s2 = reread(s);
    ASSERT_NE(s2, nullptr);
    const MethodId id2 = s2->unit().findMethodId("one", "()I");
    ASSERT_NE(id2, jbc::ir::kNoMethodId);
    auto p2 = jbc::ir::promoteMethod(*s2, id2);
    ASSERT_TRUE(p2.is_ok());
    auto u2 = std::move(p2).unwrap();
    ASSERT_EQ(u2->dom.size(), 2u);
    const jbc::ir::Insn* first = u2->dom.peek(u2->dom.order()[0]);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->op, Op::ICONST_1);
}

TEST(MethodEditorTest, EditorAppendsCommitsAndRereads)
{
    auto s = newSession();
    ASSERT_NE(s, nullptr);

    auto edR = MethodEditor::open(*s, seedOf("two", "()I"));
    ASSERT_TRUE(edR.is_ok());
    MethodEditor ed = std::move(edR).unwrap();
    EXPECT_EQ(ed.unit().maxStack, 2u);

    ASSERT_TRUE(ed.append(CodeEditor::simple(Op::ICONST_2)).is_ok());
    ASSERT_TRUE(ed.append(CodeEditor::simple(Op::IRETURN)).is_ok());
    EXPECT_EQ(ed.unit().dom.size(), 2u);
    ASSERT_TRUE(ed.commit().is_ok());

    auto s2 = reread(s);
    ASSERT_NE(s2, nullptr);
    const MethodId id2 = s2->unit().findMethodId("two", "()I");
    ASSERT_NE(id2, jbc::ir::kNoMethodId);
    auto p2 = jbc::ir::promoteMethod(*s2, id2);
    ASSERT_TRUE(p2.is_ok());
    EXPECT_EQ(p2.unwrap()->dom.size(), 2u);
}

TEST(MethodEditorTest, AbandonedEditorKeepsMountedEmptyMember)
{
    auto s = newSession();
    ASSERT_NE(s, nullptr);
    const ca::usize before = s->unit().methods.size();

    {
        auto edR = MethodEditor::open(*s, seedOf("abandoned", "()I"));
        ASSERT_TRUE(edR.is_ok());
        MethodEditor ed = std::move(edR).unwrap();
        ASSERT_TRUE(ed.append(CodeEditor::simple(Op::ICONST_1)).is_ok());
        // 不 commit：编辑随编辑器销毁丢弃，成员本身不回收。
    }

    EXPECT_EQ(s->unit().methods.size(), before + 1);
    const MethodId id = s->unit().findMethodId("abandoned", "()I");
    ASSERT_NE(id, jbc::ir::kNoMethodId);
    auto p = jbc::ir::promoteMethod(*s, id);
    ASSERT_TRUE(p.is_ok());
    EXPECT_EQ(p.unwrap()->dom.size(), 0u);
}

TEST(MethodEditorTest, AppendErrorIsImmediateAndDoesNotPoison)
{
    auto s = newSession();
    ASSERT_NE(s, nullptr);
    auto edR = MethodEditor::open(*s, seedOf("resilient", "()I"));
    ASSERT_TRUE(edR.is_ok());
    MethodEditor ed = std::move(edR).unwrap();

    // 未绑定标签的分支：append 当场返回 Err，指令不进 DOM。
    auto bad = ed.append(CodeEditor::branch(Op::GOTO, jbc::ir::kNoLabel));
    ASSERT_TRUE(bad.is_err());
    EXPECT_EQ(bad.unwrap_err(), EditError::InvalidHandle);
    EXPECT_EQ(ed.unit().dom.size(), 0u);

    // 编辑器没有失败态：后续 append 照常工作。
    ASSERT_TRUE(ed.append(CodeEditor::simple(Op::ICONST_3)).is_ok());
    ASSERT_TRUE(ed.append(CodeEditor::simple(Op::IRETURN)).is_ok());
    ASSERT_TRUE(ed.commit().is_ok());

    auto s2 = reread(s);
    ASSERT_NE(s2, nullptr);
    const MethodId id2 = s2->unit().findMethodId("resilient", "()I");
    ASSERT_NE(id2, jbc::ir::kNoMethodId);
    auto p2 = jbc::ir::promoteMethod(*s2, id2);
    ASSERT_TRUE(p2.is_ok());
    EXPECT_EQ(p2.unwrap()->dom.size(), 2u);
}

TEST(MethodEditorTest, BatchRejectsControlFlowBeforeMounting)
{
    auto s = newSession();
    ASSERT_NE(s, nullptr);
    const ca::usize before = s->unit().methods.size();

    const std::vector<jbc::ir::Insn> body{
        CodeEditor::simple(Op::ICONST_1),
        CodeEditor::branch(Op::GOTO, jbc::ir::kNoLabel),
    };
    auto r = jbc::ir::addMethodWithBody(*s, seedOf("bad", "()V"), body);
    ASSERT_TRUE(r.is_err());
    const MethodBuildError e = r.unwrap_err();
    EXPECT_EQ(e.stage, MethodBuildError::Stage::Edit);
    EXPECT_EQ(e.edit, EditError::BadOperand);
    EXPECT_EQ(e.insnIndex, 1u);

    // 在前置检查就拒绝：成员表与 backing 都零变化。
    EXPECT_EQ(s->unit().methods.size(), before);
    EXPECT_EQ(s->backing().methods.size(), before);
}

TEST(MethodEditorTest, SeedFailuresLeaveNoMember)
{
    auto s = newSession();
    ASSERT_NE(s, nullptr);
    const ca::usize before = s->unit().methods.size();

    MethodSeed noCode = seedOf("noCode", "()V");
    noCode.withCode   = false;

    auto batch = jbc::ir::addMethodWithBody(*s, noCode, {CodeEditor::simple(Op::RETURN)});
    ASSERT_TRUE(batch.is_err());
    EXPECT_EQ(batch.unwrap_err().stage, MethodBuildError::Stage::Add);
    EXPECT_EQ(batch.unwrap_err().lower, LowerError::InvalidSeed);

    auto opened = MethodEditor::open(*s, noCode);
    ASSERT_TRUE(opened.is_err());
    EXPECT_EQ(opened.unwrap_err().stage, MethodBuildError::Stage::Add);
    EXPECT_EQ(opened.unwrap_err().lower, LowerError::InvalidSeed);

    ASSERT_TRUE(jbc::ir::addMethodWithBody(*s, seedOf("dup", "()V"),
                                           {CodeEditor::simple(Op::RETURN)})
                    .is_ok());
    const ca::usize afterFirst = s->unit().methods.size();
    auto dup = jbc::ir::addMethodWithBody(*s, seedOf("dup", "()V"),
                                          {CodeEditor::simple(Op::RETURN)});
    ASSERT_TRUE(dup.is_err());
    EXPECT_EQ(dup.unwrap_err().stage, MethodBuildError::Stage::Add);
    EXPECT_EQ(dup.unwrap_err().lower, LowerError::InvalidSeed);
    EXPECT_EQ(s->unit().methods.size(), afterFirst);

    EXPECT_EQ(s->unit().methods.size(), before + 1);
}
