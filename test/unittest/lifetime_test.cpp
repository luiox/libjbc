/// @file        lifetime_test.cpp
/// @brief       寿命接管测试：保活句柄（CodeUnitRef / *SemRef）把会话寿命
///        带在身上——会话变量销毁后视图仍安全；句柄全部销毁后会话真的
///        归零（weak_ptr 两侧验证：不欠也不漏）。
#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "jbc/jbc.hpp"
#include "test_class_bytes.hpp"

using namespace jbc::core;
using jbc::ir::makeSession;
using jbc::ir::promoteMethod;
using Bytes = std::vector<ca::u8>;
using jbc::test::AttrSpec;
using jbc::test::ClassBuilder;

namespace {

/// getstatic System.out + invokevirtual println(String) + return。
Bytes makeClass()
{
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 codeIdx = cb.utf8("Code");
    const ca::u16 mIdx = cb.utf8("m");
    const ca::u16 voidIdx = cb.utf8("()V");

    const ca::u16 hiIdx = cb.utf8("hi");
    const ca::u16 strIdx = cb.stringConst(hiIdx);
    const ca::u16 outNtIdx = cb.nameAndType(cb.utf8("out"), cb.utf8("Ljava/io/PrintStream;"));
    const ca::u16 outRefIdx =
        cb.fieldref(cb.classRef(cb.utf8("java/lang/System")), outNtIdx);
    const ca::u16 plNtIdx =
        cb.nameAndType(cb.utf8("println"), cb.utf8("(Ljava/lang/String;)V"));
    const ca::u16 plRefIdx = cb.methodref(cb.classRef(cb.utf8("java/io/PrintStream")), plNtIdx);

    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setAccess(0x0021);
    cb.setVersion(52);

    // ldc "hi"; getstatic out; invokevirtual println; return（栈平衡）。
    Bytes code;
    code.push_back(0x12);
    code.push_back(static_cast<ca::u8>(strIdx & 0xFF));
    code.push_back(0xB2);
    code.push_back(static_cast<ca::u8>(outRefIdx >> 8));
    code.push_back(static_cast<ca::u8>(outRefIdx & 0xFF));
    code.push_back(0xB6);
    code.push_back(static_cast<ca::u8>(plRefIdx >> 8));
    code.push_back(static_cast<ca::u8>(plRefIdx & 0xFF));
    code.push_back(0xB1);

    std::vector<AttrSpec> methodAttrs;
    methodAttrs.push_back(AttrSpec{codeIdx, jbc::test::codeContent(2, 0, code)});
    cb.addMethod(0x0009, mIdx, voidIdx, std::move(methodAttrs));
    return cb.build();
}

std::shared_ptr<jbc::ir::ClassSession> openSession(const Bytes& bytes)
{
    auto r = readClass(ByteView{bytes});
    if (r.is_err()) {
        return nullptr;
    }
    auto s = makeSession(std::move(std::move(r).unwrap().file));
    return s.is_ok() ? std::move(s).unwrap() : nullptr;
}

}   // namespace

TEST(LifetimeTest, KeepaliveHandlesOutliveSessionVariable)
{
    jbc::ir::CodeUnitRef unit;
    jbc::ir::MemberSemRef member;
    jbc::ir::ConstSemRef constant;
    jbc::ir::ClassSemRef cls;
    std::weak_ptr<jbc::ir::ClassSession> watch;

    {
        auto s = openSession(makeClass());
        ASSERT_NE(s, nullptr);
        watch = s;

        auto p = promoteMethod(*s, 0);
        ASSERT_TRUE(p.is_ok());
        unit = p.unwrap();

        const jbc::ir::Insn* ldc = unit->dom.peek(unit->dom.first());
        ASSERT_NE(ldc, nullptr);
        const jbc::ir::Insn* call = unit->dom.peek(unit->dom.order()[2]);
        ASSERT_NE(call, nullptr);
        const jbc::ir::Insn* newInsn = unit->dom.peek(unit->dom.order()[0]);
        (void)newInsn;

        member   = s->memberSemRef(*call);
        constant = s->constSemRef(*ldc);
        cls      = s->classSemRef(*call);   // 无类操作数：name 为空，但保活成立

        // 会话变量在这里销毁——句柄必须独立保活。
        ASSERT_FALSE(watch.expired());
    }

    // 会话变量已亡：保活句柄使会话仍存活，视图/指针全部安全。
    EXPECT_FALSE(watch.expired());
    EXPECT_EQ(member.sem.owner, "java/io/PrintStream");
    EXPECT_EQ(member.sem.name, "println");
    EXPECT_EQ(constant.c.kind, jbc::ir::ClassSession::ConstSem::Kind::String);
    EXPECT_EQ(constant.c.str, "hi");
    EXPECT_EQ(unit->dom.size(), 4u);   // CodeUnit 指针可解引用
    EXPECT_TRUE(cls.name.empty());     // invokevirtual 无类操作数

    // 句柄全部销毁 → 会话归零（接管是精确的：不欠寿命也不漏引用）。
    unit   = jbc::ir::CodeUnitRef{};
    member = jbc::ir::MemberSemRef{};
    constant = jbc::ir::ConstSemRef{};
    cls    = jbc::ir::ClassSemRef{};
    EXPECT_TRUE(watch.expired());
}

TEST(LifetimeTest, KeepaliveSupportsCrossScopeEditing)
{
    // 消费者把 promote 句柄存出作用域、之后继续编辑——寿命接管下不需要
    // 手动保活会话（旧式裸指针此时已悬垂）。
    jbc::ir::CodeUnitRef unit;
    {
        auto s = openSession(makeClass());
        ASSERT_NE(s, nullptr);
        auto p = promoteMethod(*s, 0);
        ASSERT_TRUE(p.is_ok());
        unit = p.unwrap();
    }
    // 会话变量已销毁：经句柄编辑 + 帧计算（默认 resolver 一步构造）。
    jbc::ir::CodeEditor ed(*unit);
    ASSERT_TRUE(ed.insertAtStart(jbc::ir::CodeEditor::simple(Op::NOP)).is_ok());
    jbc::analysis::FrameEngine fe(unit.session()->backing().cp);
    const auto sum = fe.compute(*unit);
    EXPECT_TRUE(sum.ok);
    EXPECT_EQ(unit->dom.size(), 5u);
}

TEST(LifetimeTest, FrameEngineDefaultCtorMatchesInjected)
{
    auto s = openSession(makeClass());
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());

    jbc::analysis::ShapeResolver res;
    jbc::analysis::FrameEngine feInjected(s->backing().cp, res);
    jbc::analysis::FrameEngine feDefault(s->backing().cp);
    const auto a = feInjected.compute(*p.unwrap());
    const auto b = feDefault.compute(*p.unwrap());
    EXPECT_EQ(a.ok, b.ok);
    EXPECT_EQ(a.maxStack, b.maxStack);
    EXPECT_EQ(a.maxLocals, b.maxLocals);
}
