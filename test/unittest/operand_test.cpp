/// @file        operand_test.cpp
/// @brief       指令 CP 操作数语义层测试：读解析、去重 intern、端到端
///        插真指令（带 CP 常量/成员引用）、LDC 域宽护栏。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "jbc/jbc.hpp"
#include "corpus_util.hpp"
#include "test_class_bytes.hpp"

using namespace jbc::core;
using jbc::ir::ClassSession;
using jbc::ir::CodeEditor;
using jbc::ir::makeSession;
using jbc::ir::promoteMethod;
using jbc::ir::lowerMethod;
using Bytes = std::vector<ca::u8>;
using jbc::test::AttrSpec;
using jbc::test::ClassBuilder;

namespace {

/// 可变指令访问（peek 是只读的；get 走句柄校验的可变通道）。
jbc::ir::Insn* mutOf(jbc::ir::CodeDom& dom, const jbc::ir::InsnId& id)
{
    auto r = dom.get(id);
    return r.is_ok() ? r.unwrap() : nullptr;
}

std::shared_ptr<ClassSession> sessionOf(const Bytes& bytes)
{
    auto r = readClass(ByteView{bytes});
    if (r.is_err()) {
        return nullptr;
    }
    auto rr  = std::move(r).unwrap();
    auto ses = makeSession(std::move(rr.file));
    return ses.is_ok() ? std::move(ses).unwrap() : nullptr;
}

/// 一个带 CP 操作数指令的方法：ldc "hi"; getstatic System.out; swap; pop; pop;
/// invokevirtual println; new StringBuilder; pop; return。
Bytes makeOperandClass()
{
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 codeIdx = cb.utf8("Code");
    const ca::u16 mIdx = cb.utf8("m");
    const ca::u16 voidIdx = cb.utf8("()V");

    const ca::u16 hiIdx = cb.utf8("hi");
    const ca::u16 strIdx = cb.stringConst(hiIdx);
    const ca::u16 psNameIdx = cb.utf8("java/io/PrintStream");
    const ca::u16 psClsIdx = cb.classRef(psNameIdx);
    const ca::u16 outNameIdx = cb.utf8("out");
    const ca::u16 outDescIdx = cb.utf8("Ljava/io/PrintStream;");
    const ca::u16 outNtIdx = cb.nameAndType(outNameIdx, outDescIdx);
    const ca::u16 outRefIdx = cb.fieldref(cb.classRef(cb.utf8("java/lang/System")), outNtIdx);
    const ca::u16 plNameIdx = cb.utf8("println");
    const ca::u16 plDescIdx = cb.utf8("(Ljava/lang/String;)V");
    const ca::u16 plNtIdx = cb.nameAndType(plNameIdx, plDescIdx);
    const ca::u16 plRefIdx = cb.methodref(psClsIdx, plNtIdx);
    const ca::u16 sbIdx = cb.classRef(cb.utf8("java/lang/StringBuilder"));

    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setAccess(0x0021);
    cb.setVersion(52);

    // ldc str; getstatic out; swap; pop; pop; invokevirtual println;
    // new SB; pop; return   —— 栈每步平衡。
    Bytes code;
    code.push_back(0x12);
    code.push_back(static_cast<ca::u8>(strIdx & 0xFF));
    code.push_back(0xB2);
    code.push_back(static_cast<ca::u8>(outRefIdx >> 8));
    code.push_back(static_cast<ca::u8>(outRefIdx & 0xFF));
    code.push_back(0x5F);   // swap
    code.push_back(0x57);   // pop
    code.push_back(0x57);   // pop
    code.push_back(0xB6);
    code.push_back(static_cast<ca::u8>(plRefIdx >> 8));
    code.push_back(static_cast<ca::u8>(plRefIdx & 0xFF));
    code.push_back(0xBB);
    code.push_back(static_cast<ca::u8>(sbIdx >> 8));
    code.push_back(static_cast<ca::u8>(sbIdx & 0xFF));
    code.push_back(0x57);   // pop
    code.push_back(0xB1);   // return

    std::vector<AttrSpec> methodAttrs;
    methodAttrs.push_back(AttrSpec{codeIdx, jbc::test::codeContent(2, 0, code)});
    cb.addMethod(0x0009, mIdx, voidIdx, std::move(methodAttrs));
    return cb.build();
}

}   // namespace

TEST(OperandTest, ReadPathResolvesSemantics)
{
    auto s = sessionOf(makeOperandClass());
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    const jbc::ir::CodeDom& dom = p.unwrap()->dom;

    ca::usize members = 0;
    ca::usize classes = 0;
    ca::usize consts  = 0;
    for (const jbc::ir::InsnId& id : dom.order()) {
        const jbc::ir::Insn* in = dom.peek(id);
        ASSERT_NE(in, nullptr);
        jbc::ir::ClassSession::MemberSem m;
        if (s->memberOf(*in, m)) {
            ++members;
            if (in->op == Op::GETSTATIC) {
                EXPECT_EQ(m.kind, jbc::ir::ClassSession::MemberSem::Kind::Field);
                EXPECT_EQ(m.owner, "java/lang/System");
                EXPECT_EQ(m.name, "out");
                EXPECT_EQ(m.desc, "Ljava/io/PrintStream;");
            } else if (in->op == Op::INVOKEVIRTUAL) {
                EXPECT_EQ(m.kind, jbc::ir::ClassSession::MemberSem::Kind::Method);
                EXPECT_EQ(m.owner, "java/io/PrintStream");
                EXPECT_EQ(m.name, "println");
                EXPECT_EQ(m.desc, "(Ljava/lang/String;)V");
            }
        }
        if (const std::string_view c = s->classOf(*in); !c.empty()) {
            ++classes;
            EXPECT_EQ(c, "java/lang/StringBuilder");
        }
        jbc::ir::ClassSession::ConstSem cv;
        if (s->constOf(*in, cv)) {
            ++consts;
            EXPECT_EQ(cv.kind, jbc::ir::ClassSession::ConstSem::Kind::String);
            EXPECT_EQ(cv.str, "hi");
        }
    }
    EXPECT_EQ(members, 2u);
    EXPECT_EQ(classes, 1u);
    EXPECT_EQ(consts, 1u);
}

TEST(OperandTest, InternDedupsAgainstExistingPool)
{
    auto s = sessionOf(makeOperandClass());
    ASSERT_NE(s, nullptr);
    const ca::u32 sizeBefore = s->backing().cp.size();

    // 已有内容 → 命中既有索引（幂等）且池不增长。
    EXPECT_EQ(s->internString("hi"), s->internString("hi"));
    EXPECT_EQ(s->internClass("java/lang/Object"), s->internClass("java/lang/Object"));
    EXPECT_EQ(s->internMethodRef("java/io/PrintStream", "println",
                                 "(Ljava/lang/String;)V"),
              s->internMethodRef("java/io/PrintStream", "println",
                                 "(Ljava/lang/String;)V"));
    EXPECT_EQ(s->backing().cp.size(), sizeBefore);   // 整链命中既有池

    // 新内容 → 追加且幂等（重复 intern 不再增长）。
    const ca::u32 f1 = s->internFieldRef("p/T", "tag", "I");
    const ca::u32 f2 = s->internFieldRef("p/T", "tag", "I");
    EXPECT_EQ(f1, f2);
    EXPECT_GT(s->backing().cp.size(), sizeBefore);
    const ca::u32 after = s->backing().cp.size();
    (void)s->internString("brand-new");
    EXPECT_EQ(s->internString("brand-new"), s->internString("brand-new"));
    (void)s->internInteger(7);
    EXPECT_EQ(s->internInteger(7), s->internInteger(7));
    EXPECT_GT(s->backing().cp.size(), after);
}

TEST(OperandTest, EndToEndInstrumentWithCpOperands)
{
    const Bytes cls = makeOperandClass();
    auto s = sessionOf(cls);
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    jbc::ir::CodeUnit& cu = *p.unwrap();
    CodeEditor ed(cu);

    // 入口插「ldc "marked"; pop」——需要语义层 intern 出 String 常量，
    // 消费者不接触任何 CP 布局细节。
    const ca::u32 marked = s->internString("marked");
    ASSERT_TRUE(
        ed.insertBefore(cu.dom.order()[0], CodeEditor::loadConst(marked)).is_ok());
    ASSERT_TRUE(ed.insertAfter(cu.dom.order()[0], CodeEditor::simple(Op::POP)).is_ok());
    ASSERT_TRUE(lowerMethod(*s, 0).is_ok());

    auto s2 = sessionOf(jbc::test::writeOk(s->backing()));
    ASSERT_NE(s2, nullptr);
    auto p2 = promoteMethod(*s2, 0);
    ASSERT_TRUE(p2.is_ok());
    EXPECT_EQ(p2.unwrap()->dom.size(), cu.dom.size());

    // 重读：首条指令的语义就是 String "marked"。
    const jbc::ir::Insn* first = p2.unwrap()->dom.peek(p2.unwrap()->dom.order()[0]);
    ASSERT_NE(first, nullptr);
    jbc::ir::ClassSession::ConstSem cv;
    ASSERT_TRUE(s2->constOf(*first, cv));
    EXPECT_EQ(cv.kind, jbc::ir::ClassSession::ConstSem::Kind::String);
    EXPECT_EQ(cv.str, "marked");

    // 新写的 ldc 在重读会话里再次 intern 同一字符串 → 命中（写读一致）。
    EXPECT_EQ(s2->internString("marked"), first->cp);
}

TEST(OperandTest, LoadConstPicksWidthAutomatically)
{
    EXPECT_EQ(CodeEditor::loadConst(0x0A).op, Op::LDC);
    EXPECT_EQ(CodeEditor::loadConst(0xFF).op, Op::LDC);
    EXPECT_EQ(CodeEditor::loadConst(0x100).op, Op::LDC_W);
    EXPECT_EQ(CodeEditor::loadConst(0xFFFF).op, Op::LDC_W);
}

TEST(OperandRealClassTest, AllCpInsnsResolve)
{
    const jbc::test::corpus::CorpusSource src = jbc::test::corpus::resolveCorpus();
    if (src.stage == jbc::test::corpus::Stage::None) {
        GTEST_SKIP() << jbc::test::corpus::skipReason(src);
    }
    const std::vector<std::string> files = jbc::test::corpus::collectClassFiles(src.dir);
    if (files.empty()) {
        GTEST_SKIP() << jbc::test::corpus::skipReason(src);
    }

    ca::usize memberOk = 0;
    ca::usize classOk = 0;
    ca::usize constOk = 0;
    ca::usize unresolved = 0;
    for (const std::string& path : files) {
        std::ifstream in(path.c_str(), std::ios::binary);
        if (!in) {
            continue;
        }
        Bytes bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto s = sessionOf(bytes);
        if (s == nullptr) {
            continue;
        }
        for (ca::usize i = 0; i < s->unit().methods.size(); ++i) {
            auto p = promoteMethod(*s, i);
            if (p.is_err()) {
                continue;
            }
            for (const jbc::ir::InsnId& id : p.unwrap()->dom.order()) {
                const jbc::ir::Insn* in2 = p.unwrap()->dom.peek(id);
                if (in2 == nullptr) {
                    continue;
                }
                const Shape sh = shapeOf(in2->op);
                jbc::ir::ClassSession::MemberSem m;
                jbc::ir::ClassSession::ConstSem cv;
                const bool cpShaped = sh == Shape::CP1 || sh == Shape::CP2 ||
                                      sh == Shape::CP2Count || sh == Shape::CP2Zero2 ||
                                      sh == Shape::CP2Dims;
                if (s->memberOf(*in2, m)) {
                    ++memberOk;
                } else if (!s->classOf(*in2).empty()) {
                    ++classOk;   // new/anewarray/checkcast/instanceof/multianewarray
                } else if (s->constOf(*in2, cv)) {
                    ++constOk;
                } else if (cpShaped) {
                    ++unresolved;
                }
            }
        }
    }
    // 量级门按语料阶段分档：private-corpus（混淆产物，CP 指令面最宽）沿用历史标定；
    // 仓内自产语料（javac 规范产物）按自身量级设门槛。解析正确性门不分档：
    // 不允许任何解析不出的 CP 指令（multianewarray 已由 classOf 收口；
    // private-corpus 全语料 37398 条 unresolved=0 的口径，对任何语料都必须成立）。
    if (src.stage == jbc::test::corpus::Stage::Local || src.stage == jbc::test::corpus::Stage::Env) {
        EXPECT_GT(memberOk, 5000u);
        EXPECT_GT(classOk, 500u);
        EXPECT_GT(constOk, 1000u);
    } else {
        EXPECT_GT(memberOk, 1000u);
        EXPECT_GT(classOk, 100u);
        EXPECT_GT(constOk, 200u);
    }
    EXPECT_EQ(unresolved, 0u);
    std::printf("[operand-oracle] member=%zu class=%zu const=%zu unresolved=%zu\n",
                memberOk, classOk, constOk, unresolved);
}

// ---------------------------------------------------------------------------
// ASM 对位：改写 pass / 前向跳 / 宽度升格 / insertAtStart
// ---------------------------------------------------------------------------

TEST(OperandTest, AsmStyleRewritePass)
{
    // ASM 等价物：遍历指令，把 println 调用改成 print 调用
    // (insn.name = "print")。
    auto s = sessionOf(makeOperandClass());
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    jbc::ir::CodeUnit& cu = *p.unwrap();
    CodeEditor ed(cu);

    ca::usize rewritten = 0;
    for (const jbc::ir::InsnId& id : cu.dom.order()) {
        jbc::ir::Insn* in = mutOf(cu.dom, id);
        if (in == nullptr) {
            continue;
        }
        jbc::ir::ClassSession::MemberSem m;
        if (s->memberOf(*in, m) && m.name == "println") {
            // 一步改写；kind 不匹配（对 invoke 调 setMember(Field)）会 false。
            ASSERT_TRUE(s->setMember(*in, m.kind, m.owner, "print", m.desc));
            ++rewritten;
        }
    }
    ASSERT_EQ(rewritten, 1u);
    ASSERT_TRUE(lowerMethod(*s, 0).is_ok());

    auto s2 = sessionOf(jbc::test::writeOk(s->backing()));
    ASSERT_NE(s2, nullptr);
    auto p2 = promoteMethod(*s2, 0);
    ASSERT_TRUE(p2.is_ok());
    for (const jbc::ir::InsnId& id : p2.unwrap()->dom.order()) {
        const jbc::ir::Insn* in = p2.unwrap()->dom.peek(id);
        if (in != nullptr && in->op == Op::INVOKEVIRTUAL) {
            jbc::ir::ClassSession::MemberSem m;
            ASSERT_TRUE(s2->memberOf(*in, m));
            EXPECT_EQ(m.name, "print");
            EXPECT_EQ(m.owner, "java/io/PrintStream");
        }
    }
    // 改写后 setMember 幂等（同内容 intern 命中同索引）。
    jbc::ir::Insn* anyInvoke = nullptr;
    for (const jbc::ir::InsnId& id : p2.unwrap()->dom.order()) {
        jbc::ir::Insn* in = mutOf(p2.unwrap()->dom, id);
        if (in != nullptr && in->op == Op::INVOKEVIRTUAL) {
            anyInvoke = in;
        }
    }
    ASSERT_NE(anyInvoke, nullptr);
    EXPECT_FALSE(s2->setMember(*anyInvoke,
                               jbc::ir::ClassSession::MemberSem::Kind::Field, "a", "b",
                               "I"));   // kind 与 op 族不符 → false 且不改写
    const ca::u32 cpBefore = anyInvoke->cp;
    EXPECT_TRUE(s2->setMember(*anyInvoke,
                              jbc::ir::ClassSession::MemberSem::Kind::Method,
                              "java/io/PrintStream", "print",
                              "(Ljava/lang/String;)V"));
    EXPECT_EQ(anyInvoke->cp, cpBefore);   // 同内容 → 同索引
}

TEST(OperandTest, SetMemberRecomputesInvokeinterfaceCount)
{
    ClassBuilder cb;
    const ca::u16 thisIdx  = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 codeIdx  = cb.utf8("Code");
    const ca::u16 mIdx     = cb.utf8("m");
    const ca::u16 voidIdx  = cb.utf8("()V");

    const ca::u16 ifaceClsIdx = cb.classRef(cb.utf8("p/I"));
    const ca::u16 ntIdx = cb.nameAndType(cb.utf8("run"), cb.utf8("(I)V"));
    const ca::u16 refIdx = cb.interfaceMethodref(ifaceClsIdx, ntIdx);

    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setAccess(0x0021);
    cb.setVersion(52);

    // aload_0; invokeinterface #ref count=2 0; return（(I)V → 1 参数槽 + this）
    Bytes code = {0x2a, 0xb9,
                  static_cast<ca::u8>(refIdx >> 8), static_cast<ca::u8>(refIdx & 0xFF),
                  0x02, 0x00, 0xb1};
    std::vector<AttrSpec> methodAttrs;
    methodAttrs.push_back(AttrSpec{codeIdx, jbc::test::codeContent(1, 1, code)});
    cb.addMethod(0x0001, mIdx, voidIdx, std::move(methodAttrs));

    auto s = sessionOf(cb.build());
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    jbc::ir::CodeUnit& cu = *p.unwrap();

    jbc::ir::Insn* in = nullptr;
    for (const jbc::ir::InsnId& id : cu.dom.order()) {
        jbc::ir::Insn* cur = mutOf(cu.dom, id);
        if (cur != nullptr && cur->op == Op::INVOKEINTERFACE) {
            in = cur;
        }
    }
    ASSERT_NE(in, nullptr);
    jbc::ir::ClassSession::MemberSem m;
    ASSERT_TRUE(s->memberOf(*in, m));
    EXPECT_EQ(m.desc, "(I)V");
    ASSERT_EQ(in->a, 2);   // 1 参数槽 + this

    // 改指 (IJ)V：参数槽 1+2=3，count 必须重算为 4——只改 cp 不改 count 会
    // 产出 JVM 可拒绝的 class。
    ASSERT_TRUE(s->setMember(*in, jbc::ir::ClassSession::MemberSem::Kind::InterfaceMethod,
                             "p/I", "run", "(IJ)V"));
    EXPECT_EQ(in->a, 4);

    // 非法描述符 → false 且指令原样（fail-closed，与 op 族不符同一纪律）。
    const ca::u32 cpBefore = in->cp;
    EXPECT_FALSE(s->setMember(*in, jbc::ir::ClassSession::MemberSem::Kind::InterfaceMethod,
                              "p/I", "run", "not-a-desc"));
    EXPECT_EQ(in->cp, cpBefore);
    EXPECT_EQ(in->a, 4);

    // lower → 写回 → 重读：count 字节与新描述符一致。
    ASSERT_TRUE(lowerMethod(*s, 0).is_ok());
    auto s2 = sessionOf(jbc::test::writeOk(s->backing()));
    ASSERT_NE(s2, nullptr);
    auto p2 = promoteMethod(*s2, 0);
    ASSERT_TRUE(p2.is_ok());
    jbc::ir::Insn* in2 = nullptr;
    for (const jbc::ir::InsnId& id : p2.unwrap()->dom.order()) {
        const jbc::ir::Insn* cur = p2.unwrap()->dom.peek(id);
        if (cur != nullptr && cur->op == Op::INVOKEINTERFACE) {
            in2 = mutOf(p2.unwrap()->dom, id);
        }
    }
    ASSERT_NE(in2, nullptr);
    jbc::ir::ClassSession::MemberSem m2;
    ASSERT_TRUE(s2->memberOf(*in2, m2));
    EXPECT_EQ(m2.owner, "p/I");
    EXPECT_EQ(m2.name, "run");
    EXPECT_EQ(m2.desc, "(IJ)V");
    EXPECT_EQ(in2->a, 4);
}

TEST(OperandTest, ForwardJumpPatternWithSetTarget)
{
    // 句柄式设计没有「先建标签再前向跳」；前向跳的标准模式是：
    // 分支先指向插入锚点，目标代码插完后 setTarget 精确改写。
    // 构造 if (v==0) goto T; iconst_1; pop; T: return —— T 原指向锚点 return。
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
    // iconst_0; goto +6(过 then 块到 return)
    const Bytes code = {0x03, 0xa7, 0x00, 0x05, 0x04, 0x57, 0xb1};
    std::vector<AttrSpec> methodAttrs;
    methodAttrs.push_back(AttrSpec{codeIdx, jbc::test::codeContent(1, 0, code)});
    cb.addMethod(0x0009, mIdx, voidIdx, std::move(methodAttrs));

    auto s = sessionOf(cb.build());
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    jbc::ir::CodeUnit& cu = *p.unwrap();
    CodeEditor ed(cu);

    const auto retId = cu.dom.last();          // return（then 块要跳过 pop 的目标）
    const auto gotoId = cu.dom.order()[1];     // 原 goto：本测试把它改成条件跳
    const auto retLabel = cu.dom.createLabel(retId);
    ASSERT_TRUE(retLabel.is_ok());
    // 目标代码已在（return 是既有指令）——直接改写分支：
    // goto → ifne（弹 1），分支与直落都到 return，语义仍可 lower。
    ASSERT_TRUE(ed.setTarget(gotoId, retLabel.unwrap()).is_ok());
    {
        jbc::ir::Insn* g = mutOf(cu.dom, gotoId);
        ASSERT_NE(g, nullptr);
        g->op = Op::IFNE;   // 改 opcode 本体（保持目标句柄）
    }
    ASSERT_TRUE(lowerMethod(*s, 0).is_ok());
    auto s2 = sessionOf(jbc::test::writeOk(s->backing()));
    ASSERT_NE(s2, nullptr);
    auto p2 = promoteMethod(*s2, 0);
    ASSERT_TRUE(p2.is_ok());
    const jbc::ir::Insn* g2 = p2.unwrap()->dom.peek(p2.unwrap()->dom.order()[1]);
    ASSERT_NE(g2, nullptr);
    EXPECT_EQ(g2->op, Op::IFNE);
    const auto target2 = p2.unwrap()->dom.resolveLabel(g2->target);
    ASSERT_TRUE(target2.is_ok());
    ASSERT_FALSE(target2.unwrap().isEnd());
    const jbc::ir::Insn* tgt = p2.unwrap()->dom.peek(target2.unwrap().insn);
    ASSERT_NE(tgt, nullptr);
    EXPECT_EQ(tgt->op, Op::RETURN);
}

TEST(OperandTest, SetStringUpgradesLdcWidth)
{
    auto s = sessionOf(makeOperandClass());
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    jbc::ir::CodeUnit& cu = *p.unwrap();

    // 找到原 ldc "hi"（小池，必为 LDC 形态）。
    jbc::ir::Insn* ldc = nullptr;
    for (const jbc::ir::InsnId& id : cu.dom.order()) {
        jbc::ir::Insn* in = mutOf(cu.dom, id);
        if (in != nullptr && in->op == Op::LDC) {
            ldc = in;
            break;
        }
    }
    ASSERT_NE(ldc, nullptr);

    // 灌大池（> 0xFF）再改写字符串 → op 必须自动升格 LDC_W。
    std::string wide(64, 'x');
    for (int i = 0; i < 300; ++i) {
        (void)s->internString(wide + std::to_string(i));
    }
    ASSERT_TRUE(s->setString(*ldc, "replaced-long-marker"));
    EXPECT_EQ(ldc->op, Op::LDC_W);

    ASSERT_TRUE(lowerMethod(*s, 0).is_ok());
    auto s2 = sessionOf(jbc::test::writeOk(s->backing()));
    ASSERT_NE(s2, nullptr);
    auto p2 = promoteMethod(*s2, 0);
    ASSERT_TRUE(p2.is_ok());
    jbc::ir::ClassSession::ConstSem cv;
    bool found = false;
    for (const jbc::ir::InsnId& id : p2.unwrap()->dom.order()) {
        const jbc::ir::Insn* in = p2.unwrap()->dom.peek(id);
        if (in != nullptr && in->op == Op::LDC_W && s2->constOf(*in, cv)) {
            EXPECT_EQ(cv.str, "replaced-long-marker");
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST(OperandTest, InsertAtStartAndFirstLast)
{
    auto s = sessionOf(makeOperandClass());
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    jbc::ir::CodeUnit& cu = *p.unwrap();
    CodeEditor ed(cu);

    EXPECT_NE(cu.dom.first(), jbc::ir::kNoInsn);
    EXPECT_NE(cu.dom.last(), jbc::ir::kNoInsn);
    const ca::usize sizeBefore = cu.dom.size();
    ASSERT_TRUE(ed.insertAtStart(CodeEditor::simple(Op::NOP)).is_ok());
    EXPECT_EQ(cu.dom.size(), sizeBefore + 1);
    const jbc::ir::Insn* firstIn = cu.dom.peek(cu.dom.first());
    ASSERT_NE(firstIn, nullptr);
    EXPECT_EQ(firstIn->op, Op::NOP);
}
