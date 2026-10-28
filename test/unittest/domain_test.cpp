/// @file        domain_test.cpp
/// @brief       值域搭车引擎测试：来源追踪 / 常量折叠 / 熔断 / 入参与异常播种。
///
/// 三个领域各 ~40 行——这就是「换值域不写 transfer 表」的实证：旧栈
/// analysis 换 Interpreter 要重写整张 per-opcode 表；这里只实现域钩子。
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "jbc/jbc.hpp"
#include "test_class_bytes.hpp"

using namespace jbc::core;
using jbc::analysis::DomainEngine;
using jbc::analysis::DomainResult;
using jbc::analysis::FrameSummary;
using jbc::analysis::IValueDomain;
using jbc::analysis::MethodSig;
using jbc::analysis::ShapeDomain;
using jbc::ir::ClassSession;
using jbc::ir::makeSession;
using jbc::ir::promoteMethod;
using Bytes = std::vector<ca::u8>;
using jbc::test::AttrSpec;
using jbc::test::ClassBuilder;
using jbc::test::ExcSpec;

namespace {

std::shared_ptr<ClassSession> sessionOf(const Bytes& bytes)
{
    auto cf = jbc::test::readOk(bytes);
    if (cf == nullptr) {
        return nullptr;
    }
    auto s = makeSession(std::move(cf));
    return s.is_ok() ? std::move(s).unwrap() : nullptr;
}

/// 单方法类：code + 可选异常表；返回 session。
std::shared_ptr<ClassSession> singleMethodClass(const Bytes& code,
                                                const std::vector<ExcSpec>& exc = {},
                                                ca::u16 maxStack = 4,
                                                ca::u16 maxLocals = 4)
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
    std::vector<AttrSpec> methodAttrs;
    methodAttrs.push_back(AttrSpec{codeIdx, jbc::test::codeContent(maxStack, maxLocals, code, exc)});
    cb.addMethod(0x0009, mIdx, voidIdx, std::move(methodAttrs));
    return sessionOf(cb.build());
}

/// order 下标 ←→ 帧 下标对齐；按 srcPc 定位指令在 order 中的位置。
ca::usize indexAtPc(const jbc::ir::CodeUnit& u, ca::u32 pc)
{
    const auto& order = u.dom.order();
    for (ca::usize i = 0; i < order.size(); ++i) {
        const jbc::ir::Insn* in = u.dom.peek(order[i]);
        if (in != nullptr && in->srcPc == static_cast<ca::i32>(pc)) {
            return i;
        }
    }
    return static_cast<ca::usize>(-1);
}

// ---------------------------------------------------------------------------
// 域一：单来源追踪（旧栈 source/destination interpreter 的最小内核）
// ---------------------------------------------------------------------------

struct Origin
{
    ca::u32 origin = 0;   // 产者指令槽位；哨兵见下
    bool known = false;
};

constexpr ca::u32 kParamOrigin = 0xFFFFFFFF;
constexpr ca::u32 kExcOrigin = 0xFFFFFFFE;

class OriginDomain final : public IValueDomain<Origin>
{
public:
    Origin top() override { return Origin{}; }

    Origin merge(const Origin& a, const Origin& b) override
    {
        if (a.known && b.known && a.origin == b.origin) {
            return a;
        }
        return Origin{};   // 来源不同/未知 → 未知（单调）
    }

    Origin newValue(const jbc::ir::Insn&, jbc::ir::InsnId at, jbc::analysis::VKind) override
    {
        return Origin{at.slot, true};
    }

    Origin newParameter(ca::u32, jbc::analysis::VKind) override
    {
        return Origin{kParamOrigin, true};
    }

    Origin newException(const jbc::ir::CodeUnit::Exc&) override
    {
        return Origin{kExcOrigin, true};
    }

    bool equal(const Origin& a, const Origin& b) override
    {
        return a.known == b.known && (!a.known || a.origin == b.origin);
    }
};

// ---------------------------------------------------------------------------
// 域二：int 常量折叠（dataflow/死代码消除的最小内核）
// ---------------------------------------------------------------------------

struct Const
{
    ca::i32 val = 0;
    bool known = false;
};

class ConstDomain final : public IValueDomain<Const>
{
public:
    Const top() override { return Const{}; }

    Const merge(const Const& a, const Const& b) override
    {
        if (a.known && b.known && a.val == b.val) {
            return a;
        }
        return Const{};
    }

    Const newValue(const jbc::ir::Insn& in, jbc::ir::InsnId, jbc::analysis::VKind shape) override
    {
        // 只折 int 常量族；其余（含 ldc，cp 需读表——本演示域从简）未知。
        if (shape != jbc::analysis::VKind::Int) {
            return Const{};
        }
        switch (in.op) {
        case Op::ICONST_M1: return Const{-1, true};
        case Op::ICONST_0: return Const{0, true};
        case Op::ICONST_1: return Const{1, true};
        case Op::ICONST_2: return Const{2, true};
        case Op::ICONST_3: return Const{3, true};
        case Op::ICONST_4: return Const{4, true};
        case Op::ICONST_5: return Const{5, true};
        case Op::BIPUSH:
        case Op::SIPUSH: return Const{in.a, true};
        default: return Const{};
        }
    }

    Const newParameter(ca::u32, jbc::analysis::VKind) override { return Const{}; }
    Const newException(const jbc::ir::CodeUnit::Exc&) override { return Const{}; }

    Const binary(Op op, const Const& a, const Const& b, jbc::analysis::VKind,
                 const jbc::ir::Insn&, jbc::ir::InsnId) override
    {
        if (!a.known || !b.known) {
            return Const{};
        }
        switch (op) {
        case Op::IADD: return Const{a.val + b.val, true};
        case Op::ISUB: return Const{a.val - b.val, true};
        case Op::IMUL: return Const{a.val * b.val, true};
        case Op::IAND: return Const{a.val & b.val, true};
        case Op::IOR: return Const{a.val | b.val, true};
        case Op::IXOR: return Const{a.val ^ b.val, true};
        case Op::ISHL: return Const{a.val << (b.val & 31), true};
        case Op::ISHR: return Const{a.val >> (b.val & 31), true};
        default: return Const{};   // 除法等本演示域不折
        }
    }

    Const unary(Op op, const Const& a, jbc::analysis::VKind, const jbc::ir::Insn&,
                jbc::ir::InsnId) override
    {
        if (!a.known) {
            return Const{};
        }
        switch (op) {
        case Op::INEG: return Const{-a.val, true};
        case Op::I2B: return Const{static_cast<ca::i32>(static_cast<ca::i8>(a.val)), true};
        case Op::I2C: return Const{a.val & 0xFFFF, true};
        case Op::I2S: return Const{static_cast<ca::i32>(static_cast<ca::i16>(a.val)), true};
        default: return Const{};
        }
    }

    // loadLocal/storeLocal 用默认恒等：常量穿透 store→load。

    bool equal(const Const& a, const Const& b) override
    {
        return a.known == b.known && (!a.known || a.val == b.val);
    }
};

// ---------------------------------------------------------------------------
// 域三：非单调域（故意破坏格——验证熔断兜底）
// ---------------------------------------------------------------------------

struct Bad
{
    ca::u32 n = 0;
};

class BadDomain final : public IValueDomain<Bad>
{
public:
    Bad top() override { return Bad{}; }
    /// 每次合并结果必变 → sameState 永假 → 回边永久重访 → 熔断。
    Bad merge(const Bad& a, const Bad& b) override { return Bad{a.n + b.n + 1}; }
    Bad newValue(const jbc::ir::Insn&, jbc::ir::InsnId, jbc::analysis::VKind) override
    {
        return Bad{};
    }
    Bad newParameter(ca::u32, jbc::analysis::VKind) override { return Bad{}; }
    Bad newException(const jbc::ir::CodeUnit::Exc&) override { return Bad{}; }
    bool equal(const Bad& a, const Bad& b) override { return a.n == b.n; }
};

}   // namespace

// ---------------------------------------------------------------------------
// 来源追踪
// ---------------------------------------------------------------------------

TEST(DomainOriginTest, OriginSurvivesStoreLoad)
{
    // new A; astore_1; aload_1; ifnonnull → 栈顶值的来源 = new 指令。
    Bytes code;
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
        code = Bytes{
            0xbb, static_cast<ca::u8>(objIdx >> 8), static_cast<ca::u8>(objIdx & 0xFF),   // 0 new
            0x4c,                                                                              // 3 astore_1
            0x2b,                                                                              // 4 aload_1
            0xc7, 0x00, 0x06,                                                                  // 5 ifnonnull -> 11
            0x00, 0x00, 0x00,                                                                  // 8-10 nop
            0xb1,                                                                              // 11 return
        };
        std::vector<AttrSpec> methodAttrs;
        methodAttrs.push_back(AttrSpec{codeIdx, jbc::test::codeContent(1, 2, code)});
        cb.addMethod(0x0009, mIdx, voidIdx, std::move(methodAttrs));
        code = cb.build();
    }
    auto s = sessionOf(code);
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    const jbc::ir::CodeUnit& u = *p.unwrap();

    const auto& order = u.dom.order();
    ca::u32 newSlot = 0;
    bool found = false;
    for (const jbc::ir::InsnId& id : order) {
        const jbc::ir::Insn* in = u.dom.peek(id);
        if (in != nullptr && in->op == Op::NEW) {
            newSlot = id.slot;
            found = true;
        }
    }
    ASSERT_TRUE(found);

    OriginDomain dom;
    DomainEngine<Origin> eng(s->backing().cp, dom);
    DomainResult<Origin> r = eng.analyze(u);
    ASSERT_TRUE(r.summary.ok);

    const ca::usize idx = indexAtPc(u, 5);   // ifnonnull 的入口帧
    ASSERT_LT(idx, r.frames.size());
    ASSERT_TRUE(r.frames[idx].reachable);
    ASSERT_EQ(r.frames[idx].stack.size(), 1u);
    EXPECT_TRUE(r.frames[idx].stack[0].val.known);
    EXPECT_EQ(r.frames[idx].stack[0].val.origin, newSlot);
}

TEST(DomainOriginTest, DifferentOriginsMergeToUnknown)
{
    // 两路 new 不同指令 → 汇合后来源未知（单来源域的正确退化）。
    Bytes code;
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
        code = Bytes{
            0xbb, static_cast<ca::u8>(objIdx >> 8), static_cast<ca::u8>(objIdx & 0xFF),   // 0  new A
            0x4c,                                                                              // 3  astore_1
            0xbb, static_cast<ca::u8>(objIdx >> 8), static_cast<ca::u8>(objIdx & 0xFF),   // 4  new A
            0x4d,                                                                              // 7  astore_2
            0x03,                                                                              // 8  iconst_0
            0x99, 0x00, 0x07,                                                                  // 9  ifeq -> 16
            0x2b,                                                                              // 12 aload_1
            0xa7, 0x00, 0x04,                                                                  // 13 goto -> 17
            0x2c,                                                                              // 16 aload_2
            0x57,                                                                              // 17 pop
            0xb1,                                                                              // 18 return
        };
        std::vector<AttrSpec> methodAttrs;
        methodAttrs.push_back(AttrSpec{codeIdx, jbc::test::codeContent(1, 3, code)});
        cb.addMethod(0x0009, mIdx, voidIdx, std::move(methodAttrs));
        code = cb.build();
    }
    auto s = sessionOf(code);
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    const jbc::ir::CodeUnit& u = *p.unwrap();

    OriginDomain dom;
    DomainEngine<Origin> eng(s->backing().cp, dom);
    DomainResult<Origin> r = eng.analyze(u);
    ASSERT_TRUE(r.summary.ok);

    const ca::usize idx = indexAtPc(u, 17);   // pop 的入口帧：两路汇合
    ASSERT_LT(idx, r.frames.size());
    ASSERT_TRUE(r.frames[idx].reachable);
    ASSERT_EQ(r.frames[idx].stack.size(), 1u);
    EXPECT_FALSE(r.frames[idx].stack[0].val.known);
}

// ---------------------------------------------------------------------------
// 常量折叠
// ---------------------------------------------------------------------------

TEST(DomainConstTest, FoldArithmetic)
{
    // bipush 6; bipush 7; imul; ifeq → ifeq 入口栈顶 = 已知 42。
    const Bytes code = {
        0x10, 0x06,       // 0 bipush 6
        0x10, 0x07,       // 2 bipush 7
        0x68,             // 4 imul
        0x99, 0x00, 0x05, // 5 ifeq -> 10
        0x00, 0x00,       // 8-9 nop
        0xb1,             // 10 return
    };
    auto s = singleMethodClass(code);
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    const jbc::ir::CodeUnit& u = *p.unwrap();

    ConstDomain dom;
    DomainEngine<Const> eng(s->backing().cp, dom);
    DomainResult<Const> r = eng.analyze(u);
    ASSERT_TRUE(r.summary.ok);

    const ca::usize idx = indexAtPc(u, 5);
    ASSERT_LT(idx, r.frames.size());
    ASSERT_EQ(r.frames[idx].stack.size(), 1u);
    ASSERT_TRUE(r.frames[idx].stack[0].val.known);
    EXPECT_EQ(r.frames[idx].stack[0].val.val, 42);
}

TEST(DomainConstTest, PropagateThroughLocal)
{
    // iconst_2; istore_1; iload_1; iconst_3; iadd; ifeq
    // → iadd 入口栈 = [2, 3]（load 穿透）；ifeq 入口栈顶 = 5。
    const Bytes code = {
        0x05,             // 0 iconst_2
        0x3c,             // 1 istore_1
        0x1b,             // 2 iload_1
        0x06,             // 3 iconst_3
        0x60,             // 4 iadd
        0x99, 0x00, 0x06, // 5 ifeq -> 11
        0x00, 0x00, 0x00, // 8-10 nop
        0xb1,             // 11 return
    };
    auto s = singleMethodClass(code);
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    const jbc::ir::CodeUnit& u = *p.unwrap();

    ConstDomain dom;
    DomainEngine<Const> eng(s->backing().cp, dom);
    DomainResult<Const> r = eng.analyze(u);
    ASSERT_TRUE(r.summary.ok);

    const ca::usize atIadd = indexAtPc(u, 4);
    ASSERT_LT(atIadd, r.frames.size());
    ASSERT_EQ(r.frames[atIadd].stack.size(), 2u);
    ASSERT_TRUE(r.frames[atIadd].stack[0].val.known);
    ASSERT_TRUE(r.frames[atIadd].stack[1].val.known);
    EXPECT_EQ(r.frames[atIadd].stack[0].val.val, 2);
    EXPECT_EQ(r.frames[atIadd].stack[1].val.val, 3);

    const ca::usize atIf = indexAtPc(u, 5);
    ASSERT_TRUE(r.frames[atIf].stack[0].val.known);
    EXPECT_EQ(r.frames[atIf].stack[0].val.val, 5);
}

// ---------------------------------------------------------------------------
// cat2 载荷回归：long 值经 binary / store / load 往返不丢（popValue 行序）
// ---------------------------------------------------------------------------

struct LongVal
{
    ca::i64 val = 0;
    bool known = false;
};

class LongPayloadDomain final : public IValueDomain<LongVal>
{
public:
    LongVal top() override { return LongVal{}; }

    LongVal merge(const LongVal& a, const LongVal& b) override
    {
        return a.known && b.known && a.val == b.val ? a : LongVal{};
    }

    LongVal newValue(const jbc::ir::Insn& in, jbc::ir::InsnId,
                     jbc::analysis::VKind shape) override
    {
        if (shape != jbc::analysis::VKind::Long) {
            return LongVal{};
        }
        switch (in.op) {
        case Op::LCONST_0: return LongVal{0, true};
        case Op::LCONST_1: return LongVal{1, true};
        default: return LongVal{};
        }
    }

    LongVal newParameter(ca::u32, jbc::analysis::VKind) override { return LongVal{}; }
    LongVal newException(const jbc::ir::CodeUnit::Exc&) override { return LongVal{}; }

    LongVal binary(Op op, const LongVal& a, const LongVal& b, jbc::analysis::VKind k,
                   const jbc::ir::Insn&, jbc::ir::InsnId) override
    {
        if (!a.known || !b.known || k != jbc::analysis::VKind::Long) {
            return LongVal{};
        }
        switch (op) {
        case Op::LADD: return LongVal{a.val + b.val, true};
        case Op::LSUB: return LongVal{a.val - b.val, true};
        default: return LongVal{};
        }
    }

    bool equal(const LongVal& a, const LongVal& b) override
    {
        return a.known == b.known && (!a.known || a.val == b.val);
    }

    ca::u8 width(const LongVal& v) override { return v.known ? 2 : 1; }
};

TEST(DomainLongTest, Cat2PayloadSurvivesBinaryAndLocalRoundtrip)
{
    // lconst_1; lconst_1; ladd; lstore_1; lload_1; lconst_1; ladd; lreturn
    // → lreturn 入口栈 = [底槽, 值(3)]：值行载荷穿过了两次 binary 和
    //   store/load 往返（修复前 popValue 取到底槽行，载荷恒丢）。
    const Bytes code = {
        0x0a,             // 0 lconst_1
        0x0a,             // 1 lconst_1
        0x61,             // 2 ladd
        0x3f,             // 3 lstore_1
        0x1e,             // 4 lload_1
        0x0a,             // 5 lconst_1
        0x61,             // 6 ladd
        0xad,             // 7 lreturn
    };
    auto s = singleMethodClass(code);
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    const jbc::ir::CodeUnit& u = *p.unwrap();

    LongPayloadDomain dom;
    DomainEngine<LongVal> eng(s->backing().cp, dom);
    DomainResult<LongVal> r = eng.analyze(u);
    ASSERT_TRUE(r.summary.ok);

    const ca::usize atRet = indexAtPc(u, 7);
    ASSERT_LT(atRet, r.frames.size());
    ASSERT_TRUE(r.frames[atRet].reachable);
    ASSERT_EQ(r.frames[atRet].stack.size(), 2u);   // 底槽 + 值行
    EXPECT_FALSE(r.frames[atRet].stack[0].val.known);
    ASSERT_TRUE(r.frames[atRet].stack[1].val.known);
    EXPECT_EQ(r.frames[atRet].stack[1].val.val, 3);
}

TEST(DomainConstTest, InegFold)
{
    // iconst_5; ineg; ifeq → 栈顶 = -5。
    const Bytes code = {
        0x08,             // 0 iconst_5
        0x74,             // 1 ineg
        0x99, 0x00, 0x05, // 2 ifeq -> 7
        0x00, 0x00,       // 5-6 nop
        0xb1,             // 7 return
    };
    auto s = singleMethodClass(code);
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    const jbc::ir::CodeUnit& u = *p.unwrap();

    ConstDomain dom;
    DomainEngine<Const> eng(s->backing().cp, dom);
    DomainResult<Const> r = eng.analyze(u);
    ASSERT_TRUE(r.summary.ok);

    const ca::usize idx = indexAtPc(u, 2);
    ASSERT_TRUE(r.frames[idx].stack[0].val.known);
    EXPECT_EQ(r.frames[idx].stack[0].val.val, -5);
}

// ---------------------------------------------------------------------------
// 入参播种 / 异常播种 / 熔断
// ---------------------------------------------------------------------------

TEST(DomainSigTest, ParameterSeedingLayout)
{
    // return-only 方法；按签名播种：
    //   实例 (Ljava/lang/String;J)V → [this, str, long-lo, long-bottom] = 4 行
    //   静态 (IJ)V                   → [int, long-lo, long-bottom]      = 3 行
    const Bytes code = {0xb1};
    auto s = singleMethodClass(code, {}, 1, 5);
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    const jbc::ir::CodeUnit& u = *p.unwrap();

    ShapeDomain dom;
    DomainEngine<jbc::analysis::VType> eng(s->backing().cp, dom);

    DomainResult<jbc::analysis::VType> r1 =
        eng.analyze(u, MethodSig{false, "(Ljava/lang/String;J)V"});
    ASSERT_TRUE(r1.summary.ok);
    ASSERT_FALSE(r1.frames.empty());
    ASSERT_EQ(r1.frames[0].locals.size(), 4u);
    EXPECT_EQ(r1.frames[0].locals[0].val.k, jbc::analysis::VKind::Ref);    // this
    EXPECT_EQ(r1.frames[0].locals[1].val.k, jbc::analysis::VKind::Ref);    // String
    EXPECT_EQ(r1.frames[0].locals[2].val.k, jbc::analysis::VKind::Long);
    EXPECT_TRUE(r1.frames[0].locals[3].bottom);                            // cat2 底槽

    DomainResult<jbc::analysis::VType> r2 = eng.analyze(u, MethodSig{true, "(IJ)V"});
    ASSERT_TRUE(r2.summary.ok);
    ASSERT_EQ(r2.frames[0].locals.size(), 3u);
    EXPECT_EQ(r2.frames[0].locals[0].val.k, jbc::analysis::VKind::Int);
    EXPECT_EQ(r2.frames[0].locals[1].val.k, jbc::analysis::VKind::Long);
    EXPECT_TRUE(r2.frames[0].locals[2].bottom);

    // 无 sig 重载：不播种（与 FrameEngine 等价形态）。
    DomainResult<jbc::analysis::VType> r3 = eng.analyze(u);
    ASSERT_TRUE(r3.summary.ok);
    EXPECT_TRUE(r3.frames[0].locals.empty());
}

TEST(DomainSigTest, ExceptionEntrySeeded)
{
    // aconst_null; athrow + catch(any)@2 → 处理器 astore_1 的入口栈上
    // 恰有一个 throwable（无播种则 astore 必然下溢）。
    const Bytes code = {
        0x01,   // 0 aconst_null
        0xbf,   // 1 athrow
        0x4c,   // 2 astore_1（handler）
        0xb1,   // 3 return
    };
    std::vector<ExcSpec> exc;
    exc.push_back(ExcSpec{0, 2, 2, 0});
    auto s = singleMethodClass(code, exc, 1, 2);
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    const jbc::ir::CodeUnit& u = *p.unwrap();

    ShapeDomain dom;
    DomainEngine<jbc::analysis::VType> eng(s->backing().cp, dom);
    DomainResult<jbc::analysis::VType> r = eng.analyze(u);
    ASSERT_TRUE(r.summary.ok) << jbc::analysis::to_string(r.summary.fail);

    const ca::usize idx = indexAtPc(u, 2);
    ASSERT_LT(idx, r.frames.size());
    ASSERT_TRUE(r.frames[idx].reachable);
    ASSERT_EQ(r.frames[idx].stack.size(), 1u);
    EXPECT_EQ(r.frames[idx].stack[0].val.k, jbc::analysis::VKind::Ref);
}

TEST(DomainSigTest, HandlerSeedMergesWithLateNormalEdge)
{
    // 处理器入口的正常边晚于种子处理到达（入口块之后的深路径才跳进
    // handler）：种子不得抢先单跑——旧实现把 [exc] 单槽状态在 analyze 入口
    // 无条件入队，handler 体系在正常边并入前先以残缺帧执行，直链上需要
    // 双操作数的 iand 当场 Underflow。正确语义是 verifier 口径：入口帧 =
    // 全部入边汇合。
    const Bytes code = {
        0x04,             // 0  iconst_1          ; [1]
        0xa7, 0x00, 0x03, // 1  goto 4
        0x05,             // 4  iconst_2          ; [1,2]
        0xa7, 0x00, 0x03, // 5  goto 8（跳进 handler）
        0x3b,             // 8  istore_0（handler） ；种子路径 [exc]→[]
        0x02,             // 9  iconst_m1         ; [-1]
        0x7e,             // 10 iand              ; 旧实现：Underflow@10
        0x3c,             // 11 istore_1
        0xb1,             // 12 return
    };
    std::vector<ExcSpec> exc;
    exc.push_back(ExcSpec{0, 8, 8, 0});
    auto s = singleMethodClass(code, exc, 2, 2);
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    const jbc::ir::CodeUnit& u = *p.unwrap();

    ShapeDomain dom;
    DomainEngine<jbc::analysis::VType> eng(s->backing().cp, dom);
    DomainResult<jbc::analysis::VType> r = eng.analyze(u);
    ASSERT_TRUE(r.summary.ok) << jbc::analysis::to_string(r.summary.fail);

    // handler 入口 = 异常种子 ⊔ 正常边 [1,2]：栈深取深侧（2 行），值并成 top。
    const ca::usize idx = indexAtPc(u, 8);
    ASSERT_LT(idx, r.frames.size());
    ASSERT_TRUE(r.frames[idx].reachable);
    ASSERT_EQ(r.frames[idx].stack.size(), 2u);
    EXPECT_EQ(r.frames[idx].stack[0].val.k, jbc::analysis::VKind::Bottom);
    EXPECT_EQ(r.frames[idx].stack[1].val.k, jbc::analysis::VKind::Int);
}

TEST(DomainSigTest, HandlerSeedKeepsFallthroughEdge)
{
    // 顺序边直落处理器入口（fake-try 连续体：handler 就是 try 之后的自然
    // 续体）：截断会把唯一真实入边丢掉，种子单跑同样在 iand 下溢。基础档
    // 不截断——handler 入口按普通汇合点参与数据流。
    const Bytes code = {
        0x04,   // 0 iconst_1          ; [1]
        0x05,   // 1 iconst_2          ; [1,2]
        0x00,   // 2 nop               ; try 范围终点
        0x3b,   // 3 istore_0（handler）
        0x02,   // 4 iconst_m1         ; [-1]
        0x7e,   // 5 iand              ; 旧实现：Underflow@5
        0x3c,   // 6 istore_1
        0xb1,   // 7 return
    };
    std::vector<ExcSpec> exc;
    exc.push_back(ExcSpec{0, 3, 3, 0});
    auto s = singleMethodClass(code, exc, 2, 2);
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    const jbc::ir::CodeUnit& u = *p.unwrap();

    ShapeDomain dom;
    DomainEngine<jbc::analysis::VType> eng(s->backing().cp, dom);
    DomainResult<jbc::analysis::VType> r = eng.analyze(u);
    ASSERT_TRUE(r.summary.ok) << jbc::analysis::to_string(r.summary.fail);

    const ca::usize idx = indexAtPc(u, 3);
    ASSERT_LT(idx, r.frames.size());
    ASSERT_TRUE(r.frames[idx].reachable);
    // 入口 = 异常种子(Ref) ⊔ 顺序边 [Int,Int]：栈深取深侧 2 行；第 0 行
    // Ref⊔Int 冲突落 Bottom，第 1 行双侧同为 Int 保持。
    ASSERT_EQ(r.frames[idx].stack.size(), 2u);
    EXPECT_EQ(r.frames[idx].stack[0].val.k, jbc::analysis::VKind::Bottom);
    EXPECT_EQ(r.frames[idx].stack[1].val.k, jbc::analysis::VKind::Int);
}

TEST(DomainDiagTest, FailReportsFaultingInstruction)
{
    // pop 在空栈上：Underflow 的现场定位（failAt）必须是 pop 本条。
    const Bytes code = {
        0x57,   // 0 pop（栈空 → Underflow）
        0xb1,   // 1 return
    };
    auto s = singleMethodClass(code);
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    const jbc::ir::CodeUnit& u = *p.unwrap();

    ShapeDomain dom;
    DomainEngine<jbc::analysis::VType> eng(s->backing().cp, dom);
    DomainResult<jbc::analysis::VType> r = eng.analyze(u);
    ASSERT_FALSE(r.summary.ok);
    EXPECT_EQ(r.summary.fail, FrameSummary::Fail::Underflow);
    const ca::usize idx = indexAtPc(u, 0);
    ASSERT_LT(idx, u.dom.order().size());
    EXPECT_TRUE(r.summary.failAt == u.dom.order()[idx]);
}

TEST(DomainDiagTest, RetMarksSubroutineFact)
{
    // jsr; return; L: astore_1; ret 1 —— 引擎不建模子程序：ok 仍真（best-effort
    // 完成），但 hasSubroutine 显式标记 + degraded 计数，"算不动"可查询。
    const Bytes code = {
        0xa8, 0x00, 0x04,   // 0 jsr -> 4
        0xb1,               // 3 return
        0x4c,               // 4 astore_1
        0xa9, 0x01,         // 5 ret 1
    };
    auto s = singleMethodClass(code, {}, 2, 1);
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    const jbc::ir::CodeUnit& u = *p.unwrap();

    ShapeDomain dom;
    DomainEngine<jbc::analysis::VType> eng(s->backing().cp, dom);
    DomainResult<jbc::analysis::VType> r = eng.analyze(u);
    ASSERT_TRUE(r.summary.ok) << jbc::analysis::to_string(r.summary.fail);
    EXPECT_TRUE(r.summary.hasSubroutine);
    EXPECT_GT(r.summary.degraded, 0u);
}

TEST(DomainFuseTest, NonMonotoneDomainFuses)
{
    // 计数循环（iload/ifge/iinc/goto 回边）：单调域收敛；非单调域熔断。
    const Bytes code = {
        0x03,             // 0  iconst_0
        0x3b,             // 1  istore_0
        0x1a,             // 2  iload_0          ← 环头
        0x9e, 0x00, 0x0b, // 3  ifge -> 14
        0x84, 0x00, 0x01, // 6  iinc 0,1
        0xa7, 0xff, 0xf9, // 9  goto -> 2
        0x00, 0x00,       // 12-13 nop
        0xb1,             // 14 return
    };
    auto s = singleMethodClass(code, {}, 2, 1);
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    const jbc::ir::CodeUnit& u = *p.unwrap();

    ShapeDomain shapeDom;
    DomainEngine<jbc::analysis::VType> shapeEng(s->backing().cp, shapeDom);
    const DomainResult<jbc::analysis::VType> sr = shapeEng.analyze(u);
    EXPECT_TRUE(sr.summary.ok) << jbc::analysis::to_string(sr.summary.fail);

    BadDomain badDom;
    DomainEngine<Bad> badEng(s->backing().cp, badDom);
    const DomainResult<Bad> br = badEng.analyze(u);
    EXPECT_FALSE(br.summary.ok);
    EXPECT_EQ(br.summary.fail, FrameSummary::Fail::NoFixpoint);
    EXPECT_TRUE(br.frames.empty());   // 失败不交付半成品帧
}
