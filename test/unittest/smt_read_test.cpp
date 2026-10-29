/// @file        smt_read_test.cpp
/// @brief       Preserve 读侧（SMT 语义帧视图）+ maxes 采用测试。
///
/// 四部分：
/// 1. 读侧链展开单测——same/append/chop/full 链算术、隐式初始帧（this 形态
///    + 描述符参数 + cat2 展开）、紧凑形态载荷校验、maxes 边界。
/// 2. 读侧防御面——chop 下溢 / locals/stack 超声明 maxes / tag-8 不落 new /
///    CP 条目坏 / 目标不对齐 / 形态标签与载荷矛盾，一律显式 Err。
/// 3. maxes 采用 API——覆盖语义（ok 才采用、u16 域外不采用）+ lower 写回
///    重读集成。
/// 4. javac oracle——读↔算对拍（读侧视图 vs Strict 计算帧，逐目标逐槽）与
///    maxes 对拍（重算 vs 原文件声明）。javac 缺席时 skip。
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
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
using jbc::analysis::SmtFrameView;
using jbc::analysis::SmtReadError;
using jbc::analysis::VKind;
using jbc::analysis::VType;
using jbc::analysis::applyComputedMaxes;
using jbc::analysis::initialLocalsOf;
using jbc::analysis::readFramesFromSmt;
using jbc::ir::ClassSession;
using jbc::ir::CodeUnit;
using jbc::ir::promoteMethod;
using jbc::test::AttrSpec;
using jbc::test::ClassBuilder;
using jbc::test::StubResolver;
using jbc::test::entryLocalsOf;
using jbc::test::headOf;
using jbc::test::indexAtPc;
using jbc::test::sameSlot;
using jbc::test::sessionOf;
using jbc::test::slurpFile;
using jbc::test::slotsText;
using jbc::test::trimmedSize;
using Bytes = std::vector<ca::u8>;

namespace fs = std::filesystem;

namespace {

using SmtAttr  = jbc::core::StackMapTableAttr;
using SmtEnt   = SmtAttr::VType;
using SmtFrame = SmtAttr::Frame;

SmtEnt intE() { return SmtEnt{SmtEnt::kInteger, 0}; }
SmtEnt objE(ca::u32 cp) { return SmtEnt{SmtEnt::kObject, cp}; }
SmtEnt uninitE(ca::u32 pc) { return SmtEnt{SmtEnt::kUninitialized, pc}; }

void put2(Bytes& b, ca::u16 v)
{
    b.push_back(static_cast<ca::u8>(v >> 8));
    b.push_back(static_cast<ca::u8>(v & 0xFF));
}

/// 单方法类骨架（版本可调）。
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

/// 解码方法 Code 属性并返回其槽下标；无 Code 返回 npos 惯例哨兵。
ca::usize decodeCodeSlot(ClassFile& cf, MemberInfo& m)
{
    for (ca::usize i = 0; i < m.attrs.size(); ++i) {
        auto b = cf.decodeAttr(m.attrs[i]);
        if (b.is_ok() && m.attrs[i].peek() != nullptr &&
            m.attrs[i].peek()->kind() == AttrKind::Code) {
            return i;
        }
    }
    return static_cast<ca::usize>(-1);
}

/// CP 中现成的属性名 Utf8（挂 SMT 时复用既有条目）。
ca::u32 smtNameCp(ClassFile& cf)
{
    for (ca::u32 i = 1; i < cf.cp.size(); ++i) {
        if (cf.cp.valid(i) && cf.cp.utf8Equals(i, "StackMapTable")) {
            return i;
        }
    }
    return cf.cp.addUtf8("StackMapTable");
}

/// 手拼 SMT 属性挂进方法 Code（读侧测试不经 codec 编码——保真形态直接
/// 可读；经 codec 的编码路径由 roundtrip/attr_extra 面覆盖）。返回属性引用。
SmtAttr& attachSmt(ClassFile& cf, MemberInfo& m)
{
    const ca::usize ci = decodeCodeSlot(cf, m);
    EXPECT_NE(ci, static_cast<ca::usize>(-1));
    auto& code = static_cast<CodeAttr&>(*m.attrs[ci].mutate());
    for (AttrSlot& nested : code.attrs) {
        (void)cf.decodeAttr(nested);
    }
    auto attr = std::make_unique<SmtAttr>();
    SmtAttr& ref = *attr;
    code.attrs.push_back(AttrSlot::makeNew(smtNameCp(cf), std::move(attr)));
    return ref;
}

/// static m(JLjava/lang/String;I)V：初始条目 [Long, String, Int]（槽展开
/// [Long, Top, String, Int]，4 槽）；分支目标落在 pc 2/6/8/10。链测试用
/// maxLocals=5（append 后 5 槽恰在边界）。
std::shared_ptr<ClassSession> paramsClass(ca::u16 maxStack = 2, ca::u16 maxLocals = 5)
{
    ClassBuilder cb = baseClass();
    const ca::u16 qCls    = cb.classRef(cb.utf8("demo/Q"));
    const ca::u16 codeIdx = cb.utf8("Code");

    Bytes code;
    code.push_back(0x01);              // 0 aconst_null
    code.push_back(0x57);              // 1 pop
    code.push_back(0x1d);              // 2 iload_3        <- same(2)
    code.push_back(0x99);              // 3 ifeq -> 6
    put2(code, 3);
    code.push_back(0x2c);              // 6 aload_2        <- append [Q]（delta 3）
    code.push_back(0x4d);              // 7 astore_2
    code.push_back(0xb1);              // 8 return         <- chop k=1（delta 1）
    code.push_back(0x00);              // 9 nop
    code.push_back(0xb1);              // 10 return        <- full（delta 1）

    std::vector<AttrSpec> ma;
    ma.push_back(AttrSpec{codeIdx, jbc::test::codeContent(maxStack, maxLocals, code)});
    cb.addMethod(0x0009, cb.utf8("m"), cb.utf8("(JLjava/lang/String;I)V"),
                 std::move(ma));
    return sessionOf(cb.build());
}

/// 实例构造器 <init>()V：初始条目 [UninitThis]（槽展开 1 槽，maxLocals
/// 边界样本）；目标 pc 0/2。
std::shared_ptr<ClassSession> ctorClass(ca::u16 maxStack = 1, ca::u16 maxLocals = 1)
{
    ClassBuilder cb = baseClass();
    const ca::u16 qCls    = cb.classRef(cb.utf8("demo/Q"));
    const ca::u16 codeIdx = cb.utf8("Code");

    Bytes code;
    code.push_back(0x2a);              // 0 aload_0        <- same(0)
    code.push_back(0x01);              // 1 aconst_null
    code.push_back(0x57);              // 2 pop            <- same1 [Q]（delta 2）
    code.push_back(0xb1);              // 3 return

    std::vector<AttrSpec> ma;
    ma.push_back(AttrSpec{codeIdx, jbc::test::codeContent(maxStack, maxLocals, code)});
    cb.addMethod(0x0002, cb.utf8("<init>"), cb.utf8("()V"), std::move(ma));
    return sessionOf(cb.build());
}

/// new + pop + return（pcs 0/3/4）：tag-8 载荷与不对齐目标的载体。
std::shared_ptr<ClassSession> newPopClass(ca::u16 maxStack = 2, ca::u16 maxLocals = 1)
{
    ClassBuilder cb = baseClass();
    const ca::u16 xCls    = cb.classRef(cb.utf8("p/X"));
    const ca::u16 codeIdx = cb.utf8("Code");

    Bytes code;
    code.push_back(0xbb);              // 0 new p/X
    put2(code, xCls);
    code.push_back(0x57);              // 3 pop
    code.push_back(0xb1);              // 4 return

    std::vector<AttrSpec> ma;
    ma.push_back(AttrSpec{codeIdx, jbc::test::codeContent(maxStack, maxLocals, code)});
    cb.addMethod(0x0009, cb.utf8("m"), cb.utf8("()V"), std::move(ma));
    return sessionOf(cb.build());
}

/// static m()V：零参数直线体（chop 下溢 / 不对齐目标的无参载体）。
std::shared_ptr<ClassSession> plainClass()
{
    ClassBuilder cb = baseClass();
    const ca::u16 codeIdx = cb.utf8("Code");
    const Bytes code      = {0x01, 0x57, 0xb1};   // 0 aconst_null; 1 pop; 2 return
    std::vector<AttrSpec> ma;
    ma.push_back(AttrSpec{codeIdx, jbc::test::codeContent(1, 0, code)});
    cb.addMethod(0x0009, cb.utf8("m"), cb.utf8("()V"), std::move(ma));
    return sessionOf(cb.build());
}

MethodSig sigOf(const ClassSession& s, ca::usize i)
{
    const auto& mu = s.unit().methods[i];
    MethodSig sig;
    sig.isStatic  = (mu.access & 0x0008) != 0;
    sig.desc      = s.unit().symbols().str(mu.desc);
    sig.name      = s.unit().symbols().str(mu.name);
    sig.thisClass = s.unit().symbols().str(s.unit().thisClass);
    return std::move(sig);
}

/// CP 中指定类名的 Class 条目索引；未命中返回 0（调用方 ASSERT）。
ca::u32 cpClassOf(const ClassSession& s, const char* name)
{
    const auto& cp = s.backing().cp;
    for (ca::u32 i = 1; i < cp.size(); ++i) {
        if (cp.valid(i) && cp.tagOf(i) == CpTag::Class && cp.utf8Equals(cp.at(i).a, name)) {
            return i;
        }
    }
    return 0u;
}

/// 读侧一步：promote + 读帧，Result 原样交付（调用方分支断言）。
ca::core::Result<std::vector<SmtFrameView>, SmtReadError>
readOf(ClassSession& s, ca::usize i)
{
    auto p = promoteMethod(s, i);
    if (p.is_err()) {
        ADD_FAILURE() << "promote failed";
        return ca::core::Err(SmtReadError::NoCode);
    }
    return readFramesFromSmt(s, *p.unwrap());
}

}   // namespace

// ---------------------------------------------------------------------------
// 链展开单测（same/append/chop/full + 隐式初始帧 + cat2）
// ---------------------------------------------------------------------------

TEST(SmtReadChain, AbsentAttributeYieldsEmptyViews)
{
    auto s = plainClass();
    ASSERT_NE(s, nullptr);
    auto r = readOf(*s, 0);
    ASSERT_FALSE(r.is_err());
    EXPECT_TRUE(std::move(r).unwrap().empty());   // 属性缺席 = 合法缺席，空视图不报错
}

TEST(SmtReadChain, FourFormChainDecodesWithInitialFrameExpansion)
{
    auto s = paramsClass();
    ASSERT_NE(s, nullptr);
    const ca::u32 q = cpClassOf(*s, "demo/Q");
    ASSERT_NE(q, 0u);

    SmtAttr& smt = attachSmt(s->backing(), s->backing().methods[0]);
    smt.frames.push_back(SmtFrame{2, 2, {}, {}});            // same(2)
    smt.frames.push_back(SmtFrame{252, 3, {objE(q)}, {}});   // append 1
    smt.frames.push_back(SmtFrame{250, 1, {}, {}});          // chop 1
    smt.frames.push_back(SmtFrame{255, 1, {intE(), objE(q)}, {intE()}});   // full

    auto r = readOf(*s, 0);
    ASSERT_FALSE(r.is_err());
    const auto views = std::move(r).unwrap();
    ASSERT_EQ(views.size(), 4u);

    // 初始帧（隐式链头）= [Long, String, Int] 条目 → 槽展开 [Long, Top,
    // String, Int]；same 保持。
    EXPECT_EQ(views[0].pc, 2u);
    EXPECT_TRUE(views[0].frame.reachable);
    EXPECT_FALSE(views[0].at == jbc::ir::kNoInsn);
    ASSERT_EQ(views[0].frame.locals.size(), 4u);
    EXPECT_TRUE(sameSlot(views[0].frame.locals[0], VType{VKind::Long, 0, {}}));
    EXPECT_TRUE(sameSlot(views[0].frame.locals[1], VType{VKind::Top, 0, {}}));
    EXPECT_TRUE(sameSlot(views[0].frame.locals[2], VType::ref("java/lang/String")));
    EXPECT_TRUE(sameSlot(views[0].frame.locals[3], VType{VKind::Int, 0, {}}));
    EXPECT_TRUE(views[0].frame.stack.empty());

    // append 1（demo/Q）：条目接在链尾，槽展开后 Q 落 slot 4。
    EXPECT_EQ(views[1].pc, 6u);
    ASSERT_EQ(views[1].frame.locals.size(), 5u);
    EXPECT_TRUE(sameSlot(views[1].frame.locals[4], VType::ref("demo/Q")));

    // chop 1：回到初始 4 槽。
    EXPECT_EQ(views[2].pc, 8u);
    ASSERT_EQ(views[2].frame.locals.size(), 4u);
    EXPECT_TRUE(sameSlot(views[2].frame.locals[0], VType{VKind::Long, 0, {}}));

    // full：整体替换 + 栈载荷（槽语义交付）。
    EXPECT_EQ(views[3].pc, 10u);
    ASSERT_EQ(views[3].frame.locals.size(), 2u);
    EXPECT_TRUE(sameSlot(views[3].frame.locals[0], VType{VKind::Int, 0, {}}));
    EXPECT_TRUE(sameSlot(views[3].frame.locals[1], VType::ref("demo/Q")));
    ASSERT_EQ(views[3].frame.stack.size(), 1u);
    EXPECT_TRUE(sameSlot(views[3].frame.stack[0], VType{VKind::Int, 0, {}}));
}

TEST(SmtReadChain, CtorInitialFrameCarriesUninitThisAtMaxLocalsBoundary)
{
    auto s = ctorClass();   // maxLocals = 1（仅 this 槽）：边界即合法
    ASSERT_NE(s, nullptr);
    const ca::u32 q = cpClassOf(*s, "demo/Q");
    ASSERT_NE(q, 0u);

    SmtAttr& smt = attachSmt(s->backing(), s->backing().methods[0]);
    smt.frames.push_back(SmtFrame{0, 0, {}, {}});              // same(0)
    smt.frames.push_back(SmtFrame{65, 1, {}, {objE(q)}});      // same1 delta 1（pc 2）

    auto r = readOf(*s, 0);
    ASSERT_FALSE(r.is_err());
    const auto views = std::move(r).unwrap();
    ASSERT_EQ(views.size(), 2u);

    // 链头 this 槽 = UninitThis（构造器口径，与 Strict 播种同源）；1 槽恰
    // 等 maxLocals——边界不溢出。
    EXPECT_EQ(views[0].pc, 0u);
    ASSERT_EQ(views[0].frame.locals.size(), 1u);
    EXPECT_EQ(views[0].frame.locals[0].k, VKind::UninitThis);
    EXPECT_TRUE(views[0].frame.stack.empty());

    // same1：栈 1 条目（恰等 maxStack 边界），locals 链上保持。
    EXPECT_EQ(views[1].pc, 2u);
    ASSERT_EQ(views[1].frame.stack.size(), 1u);
    EXPECT_TRUE(sameSlot(views[1].frame.stack[0], VType::ref("demo/Q")));
    ASSERT_EQ(views[1].frame.locals.size(), 1u);
    EXPECT_EQ(views[1].frame.locals[0].k, VKind::UninitThis);
}

TEST(SmtReadChain, UninitEntryMapsToNewPcPayload)
{
    auto s = newPopClass();
    ASSERT_NE(s, nullptr);
    SmtAttr& smt = attachSmt(s->backing(), s->backing().methods[0]);
    smt.frames.push_back(SmtFrame{255, 3, {}, {uninitE(0)}});   // full @pc3

    auto r = readOf(*s, 0);
    ASSERT_FALSE(r.is_err());
    const auto views = std::move(r).unwrap();
    ASSERT_EQ(views.size(), 1u);
    EXPECT_EQ(views[0].pc, 3u);
    ASSERT_EQ(views[0].frame.stack.size(), 1u);
    EXPECT_EQ(views[0].frame.stack[0].k, VKind::Uninitialized);
    EXPECT_EQ(views[0].frame.stack[0].uninitOffset(), 0u);
}

TEST(SmtReadChain, InitialLocalsMatchesEngineSeedingShape)
{
    // initialLocalsOf 与测试设施 entryLocalsOf（引擎播种口径）同源同义。
    const MethodSig ctor{false, "(JI)V", "<init>", "p/T"};
    const auto a = initialLocalsOf(ctor);
    const auto b = entryLocalsOf(ctor);
    ASSERT_EQ(a.size(), b.size());
    for (ca::usize i = 0; i < a.size(); ++i) {
        EXPECT_TRUE(sameSlot(a[i], b[i])) << "slot " << i;
    }
    EXPECT_EQ(a.size(), 3u);   // UninitThis + long + int（cat2 条目口径）
    EXPECT_EQ(a[0].k, VKind::UninitThis);
    EXPECT_EQ(a[1].k, VKind::Long);
    EXPECT_EQ(a[2].k, VKind::Int);

    // static + 引用参数：精确类名进条目。
    const MethodSig stat{true, "(Ljava/lang/String;[I)V", "m", ""};
    const auto c = initialLocalsOf(stat);
    ASSERT_EQ(c.size(), 2u);
    EXPECT_TRUE(sameSlot(c[0], VType::ref("java/lang/String")));
    EXPECT_TRUE(sameSlot(c[1], VType::ref("[I")));
}

// ---------------------------------------------------------------------------
// 防御面（无法保真则显式 Err，不猜）
// ---------------------------------------------------------------------------

namespace {

/// 防御面共用：读必须以指定错误码失败（不交付视图）。
void expectReadFails(ClassSession& s, SmtReadError expect)
{
    auto r = readOf(s, 0);
    ASSERT_TRUE(r.is_err()) << "expected failure " << jbc::analysis::to_string(expect);
    EXPECT_EQ(r.unwrap_err(), expect);
}

}   // namespace

TEST(SmtReadGuard, LocalsOverflowRejected)
{
    auto s = ctorClass(1, 0);   // maxLocals=0：链头 this 槽 1 槽即溢出
    ASSERT_NE(s, nullptr);
    SmtAttr& smt = attachSmt(s->backing(), s->backing().methods[0]);
    smt.frames.push_back(SmtFrame{0, 0, {}, {}});
    expectReadFails(*s, SmtReadError::LocalsOverflow);
}

TEST(SmtReadGuard, StackOverflowRejected)
{
    auto s = ctorClass(0, 1);   // maxStack=0：same1 栈 1 槽即溢出
    ASSERT_NE(s, nullptr);
    const ca::u32 q = cpClassOf(*s, "demo/Q");
    ASSERT_NE(q, 0u);
    SmtAttr& smt = attachSmt(s->backing(), s->backing().methods[0]);
    smt.frames.push_back(SmtFrame{0, 0, {}, {}});
    smt.frames.push_back(SmtFrame{66, 2, {}, {objE(q)}});
    expectReadFails(*s, SmtReadError::StackOverflow);
}

TEST(SmtReadGuard, ChopUnderflowRejected)
{
    auto s = plainClass();   // 零参数：链头空条目，chop k=1 即下溢
    ASSERT_NE(s, nullptr);
    SmtAttr& smt = attachSmt(s->backing(), s->backing().methods[0]);
    smt.frames.push_back(SmtFrame{250, 0, {}, {}});
    expectReadFails(*s, SmtReadError::LocalsUnderflow);
}

TEST(SmtReadGuard, FrameNotAlignedRejected)
{
    auto s = newPopClass();   // pc 0 是 3 字节 new：pc 1 不是指令边界
    ASSERT_NE(s, nullptr);
    SmtAttr& smt = attachSmt(s->backing(), s->backing().methods[0]);
    smt.frames.push_back(SmtFrame{1, 1, {}, {}});
    expectReadFails(*s, SmtReadError::FrameNotAligned);
}

TEST(SmtReadGuard, UninitPayloadNotOnNewRejected)
{
    auto s = newPopClass();
    ASSERT_NE(s, nullptr);
    SmtAttr& smt = attachSmt(s->backing(), s->backing().methods[0]);
    smt.frames.push_back(SmtFrame{255, 3, {}, {uninitE(999)}});
    expectReadFails(*s, SmtReadError::UninitNotOnNew);
}

TEST(SmtReadGuard, BadObjectCpRejected)
{
    auto s = newPopClass();
    ASSERT_NE(s, nullptr);
    SmtAttr& smt = attachSmt(s->backing(), s->backing().methods[0]);
    smt.frames.push_back(SmtFrame{255, 3, {}, {objE(9999)}});
    expectReadFails(*s, SmtReadError::BadCpEntry);
}

TEST(SmtReadGuard, TagPayloadContradictionsRejected)
{
    // same 帧带栈载荷 / same1 缺栈载荷 / append 条数不符 / 保留标签。
    auto base = [] {
        auto s = ctorClass();
        if (s == nullptr) {
            ADD_FAILURE() << "session";
        }
        return s;
    };
    {
        auto s = base();
        SmtAttr& smt = attachSmt(s->backing(), s->backing().methods[0]);
        smt.frames.push_back(SmtFrame{1, 0, {}, {intE()}});   // same 带栈项
        expectReadFails(*s, SmtReadError::BadFramePayload);
    }
    {
        auto s = base();
        SmtAttr& smt = attachSmt(s->backing(), s->backing().methods[0]);
        smt.frames.push_back(SmtFrame{66, 2, {}, {}});   // same1 缺栈项
        expectReadFails(*s, SmtReadError::BadFramePayload);
    }
    {
        auto s = base();
        SmtAttr& smt = attachSmt(s->backing(), s->backing().methods[0]);
        smt.frames.push_back(
            SmtFrame{252, 2, {intE(), intE()}, {}});   // append k=1 载荷 2 条
        expectReadFails(*s, SmtReadError::BadFramePayload);
    }
    {
        auto s = base();
        SmtAttr& smt = attachSmt(s->backing(), s->backing().methods[0]);
        smt.frames.push_back(SmtFrame{200, 0, {}, {}});   // 保留标签 128..246
        expectReadFails(*s, SmtReadError::BadFramePayload);
    }
}

// ---------------------------------------------------------------------------
// maxes 采用 API（覆盖裁决）
// ---------------------------------------------------------------------------

TEST(MaxesApi, OkSummaryOverwritesBothFields)
{
    CodeUnit u;
    u.maxStack  = 2;
    u.maxLocals = 3;
    const MethodSig sig{true, "()V", "m", ""};   // static 无参：地板 0
    FrameSummary sum;
    sum.ok        = true;
    sum.maxStack  = 5;
    sum.maxLocals = 7;
    EXPECT_TRUE(applyComputedMaxes(u, sum, sig));
    EXPECT_EQ(u.maxStack, 5u);
    EXPECT_EQ(u.maxLocals, 7u);
}

TEST(MaxesApi, ParameterFloorEntersAdoptedLocals)
{
    // 引擎 maxLocals 只计 store 触达：只读参数的方法观测值低于 verifier
    // 要求——采用值并入参数槽地板（this + 描述符参数）。
    CodeUnit u;
    u.maxStack  = 1;
    u.maxLocals = 0;
    const MethodSig ctor{false, "(JI)V", "<init>", "p/T"};   // 地板 = 1 + 3 = 4
    FrameSummary sum;
    sum.ok        = true;
    sum.maxStack  = 2;
    sum.maxLocals = 0;   // 无任何 store 的观测值
    EXPECT_TRUE(applyComputedMaxes(u, sum, ctor));
    EXPECT_EQ(u.maxStack, 2u);
    EXPECT_EQ(u.maxLocals, 4u);
}

TEST(MaxesApi, HandlerFloorKeepsExceptionStack)
{
    // 带异常表：handler 入口栈恒有异常值，maxStack 地板 1（空 CodeUnit +
    // 一条占位异常表项即可测地板判定）。
    CodeUnit cu;
    cu.maxStack  = 0;
    cu.maxLocals = 1;
    cu.exc.push_back(CodeUnit::Exc{});

    const MethodSig sig{true, "()V", "m", ""};
    FrameSummary sum;
    sum.ok        = true;
    sum.maxStack  = 0;
    sum.maxLocals = 1;
    EXPECT_TRUE(applyComputedMaxes(cu, sum, sig));
    EXPECT_EQ(cu.maxStack, 1u);
    EXPECT_EQ(cu.maxLocals, 1u);
}

TEST(MaxesApi, FailedSummaryNotAdopted)
{
    CodeUnit u;
    u.maxStack  = 2;
    u.maxLocals = 3;
    FrameSummary bad;
    bad.ok        = false;
    bad.fail      = FrameSummary::Fail::TypeConflict;
    bad.maxStack  = 9;
    bad.maxLocals = 9;
    const MethodSig sig{true, "()V", "m", ""};
    EXPECT_FALSE(applyComputedMaxes(u, bad, sig));   // 失败计算无真值
    EXPECT_EQ(u.maxStack, 2u);
    EXPECT_EQ(u.maxLocals, 3u);
}

TEST(MaxesApi, BeyondU16NotAdopted)
{
    CodeUnit u;
    u.maxStack  = 2;
    u.maxLocals = 3;
    FrameSummary sum;
    sum.ok        = true;
    sum.maxStack  = 0x10000;
    sum.maxLocals = 7;
    const MethodSig sig{true, "()V", "m", ""};
    EXPECT_FALSE(applyComputedMaxes(u, sum, sig));   // u16 域外截断即伪造
    EXPECT_EQ(u.maxStack, 2u);
    EXPECT_EQ(u.maxLocals, 3u);
}

TEST(MaxesApi, AdoptedMaxesFlowToLowerWriteBackAndReread)
{
    // 显式 API → CodeUnit → lower 脏检查分支 → backing → 写出重读：
    // 手动链路的上游补全（本测试锁全链）。
    auto s = paramsClass(2, 4);   // 文件声明 maxStack=2
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    CodeUnit& cu = *p.unwrap();
    ASSERT_EQ(cu.maxStack, 2u);
    ASSERT_EQ(cu.maxLocals, 4u);

    StubResolver res;
    FrameEngine fe(s->backing().cp, res, FramePolicy::Strict);
    const FrameSummary sum = fe.compute(cu, sigOf(*s, 0));
    ASSERT_TRUE(sum.ok);
    ASSERT_EQ(sum.maxStack, 1u);    // 直线体实际栈深 1
    ASSERT_EQ(sum.maxLocals, 3u);   // 引擎口径：只计 store 触达（astore_2 → 槽 3）

    // 采用 = 覆盖 + 参数槽地板（static J+String+I = 4）：原 2 → 1（栈）、
    // locals 观测 3 由地板抬到 4。
    EXPECT_TRUE(applyComputedMaxes(cu, sum, sigOf(*s, 0)));
    EXPECT_EQ(cu.maxStack, 1u);
    EXPECT_EQ(cu.maxLocals, 4u);
    auto lw = jbc::ir::lowerMethod(*s, 0);
    ASSERT_TRUE(lw.is_ok());

    // 写出重读：maxes 持久化（lower 脏检查分支以 maxesChanged 触发）。
    auto w = jbc::core::writeClass(s->backing());
    ASSERT_TRUE(w.is_ok());
    auto s2 = sessionOf(std::move(w).unwrap());
    ASSERT_NE(s2, nullptr);
    const ca::usize ci = decodeCodeSlot(s2->backing(), s2->backing().methods[0]);
    ASSERT_NE(ci, static_cast<ca::usize>(-1));
    const auto& code2 = static_cast<const CodeAttr&>(*s2->backing().methods[0].attrs[ci].peek());
    EXPECT_EQ(code2.maxStack, 1u);
    EXPECT_EQ(code2.maxLocals, 4u);
}

// ---------------------------------------------------------------------------
// javac oracle：读↔算对拍 + maxes 对拍
// ---------------------------------------------------------------------------

namespace {

/// 读↔算逐槽对拍（口径：locals 尾部 Top 修剪后公共前缀一致 + stack
/// 全量一致；javac 声明投影的截尾不判分歧）。
void compareReadVsCompute(const char* ctx, const SmtFrameView& v, const FrameAt& c,
                          ca::usize& checked, ca::usize& mismatch)
{
    const std::vector<VType> readLocals =
        headOf(v.frame.locals, trimmedSize(v.frame.locals));
    const std::vector<VType> compLocals = headOf(c.locals, trimmedSize(c.locals));
    const ca::usize n = readLocals.size() < compLocals.size() ? readLocals.size()
                                                              : compLocals.size();
    const bool localsOk = std::equal(compLocals.begin(),
                                     compLocals.begin() + static_cast<std::ptrdiff_t>(n),
                                     readLocals.begin(), sameSlot);
    const bool stackOk =
        c.stack.size() == v.frame.stack.size()
        && std::equal(c.stack.begin(), c.stack.end(), v.frame.stack.begin(), sameSlot);
    if (!localsOk || !stackOk) {
        ++mismatch;
        ADD_FAILURE() << ctx << " @" << v.pc << "\n  read   locals "
                      << slotsText(readLocals) << "\n  compute locals "
                      << slotsText(compLocals) << "\n  read   stack "
                      << slotsText(v.frame.stack) << "\n  compute stack "
                      << slotsText(c.stack);
        return;
    }
    ++checked;
}

/// 遍历语料：每类每方法回调（方法下标 + 会话）。返回处理的类数。
template <typename Fn>
ca::usize forEachCorpusMethod(const fs::path& root, Fn&& fn)
{
    ca::usize classes = 0;
    std::error_code iterEc;
    for (fs::recursive_directory_iterator it(root / "classes", iterEc), end;
         it != end && !iterEc; it.increment(iterEc)) {
        if (!it->is_regular_file() || it->path().extension() != ".class") {
            continue;
        }
        auto s = sessionOf(slurpFile(it->path()));
        if (s == nullptr) {
            ADD_FAILURE() << it->path();
            continue;
        }
        ++classes;
        for (ca::usize i = 0; i < s->unit().methods.size(); ++i) {
            fn(*s, i);
        }
    }
    return classes;
}

}   // namespace

TEST(SmtReadOracle, ReadSideMatchesStrictCompute)
{
    if (!jbc::test::oracleToolPresent("javac")) {
        GTEST_SKIP() << "javac not present";
    }
    const fs::path root = fs::path("test") / "libjbc_smt_oracle";
    ASSERT_TRUE(jbc::test::compileOracleSamples(root));

    StubResolver res;
    res.seedJdkParents();
    res.parent["demo/CtorUninit"] = "demo/CtorBase";

    ca::usize methods = 0;
    ca::usize framesChecked = 0;
    ca::usize mismatch = 0;

    forEachCorpusMethod(root, [&](ClassSession& s, ca::usize i) {
        const MethodSig sig = sigOf(s, i);
        auto p = promoteMethod(s, i);
        ASSERT_TRUE(p.is_ok());
        CodeUnit& cu = *p.unwrap();

        // 读侧：属性缺席（无控制流直线方法）跳过。
        auto rr = readFramesFromSmt(s, cu);
        ASSERT_FALSE(rr.is_err()) << "read failed: "
                                  << jbc::analysis::to_string(rr.unwrap_err());
        const std::vector<SmtFrameView> views = std::move(rr).unwrap();
        if (views.empty()) {
            return;
        }
        ++methods;

        // 算侧：Strict 计算帧（播种口径与链头同源）。
        FrameEngine fe(s.backing().cp, res, FramePolicy::Strict);
        std::vector<FrameAt> frames;
        const FrameSummary sum = fe.compute(cu, sig, frames);
        ASSERT_TRUE(sum.ok) << sig.name << ": " << jbc::analysis::to_string(sum.fail);

        // 逐目标逐槽对拍：读侧每个帧目标在算侧必可达且同形。
        const std::string ctx = std::string(s.unit().symbols().str(s.unit().thisClass))
            + "." + std::string(sig.name);
        for (const SmtFrameView& v : views) {
            const ca::usize idx = indexAtPc(cu, v.pc);
            ASSERT_NE(idx, static_cast<ca::usize>(-1)) << ctx << " pc " << v.pc;
            ASSERT_TRUE(frames[idx].reachable) << ctx << " pc " << v.pc;
            compareReadVsCompute(ctx.c_str(), v, frames[idx], framesChecked, mismatch);
        }
    });

    std::printf(
        "[smt-read-oracle] methods=%zu framesChecked=%zu mismatch=%zu\n", methods,
        framesChecked, mismatch);
    EXPECT_GE(methods, 10u);         // 语料护栏（与生成器 oracle 同源）
    EXPECT_GE(framesChecked, 30u);
    EXPECT_EQ(mismatch, 0u);         // 验收口径：读侧视图与计算帧一致
}

TEST(MaxesOracle, RecomputedMaxesMatchJavac)
{
    if (!jbc::test::oracleToolPresent("javac")) {
        GTEST_SKIP() << "javac not present";
    }
    const fs::path root = fs::path("test") / "libjbc_smt_oracle";
    ASSERT_TRUE(jbc::test::compileOracleSamples(root));

    StubResolver res;
    res.seedJdkParents();
    res.parent["demo/CtorUninit"] = "demo/CtorBase";

    ca::usize methods  = 0;
    ca::usize equalAll = 0;   // stack 与 locals 全等
    ca::usize geAny    = 0;   // 重算 ≥ 原值且至少一项严格更大
    ca::usize ltStack  = 0;   // 重算 < 原值（应为 0）
    ca::usize ltLocals = 0;

    forEachCorpusMethod(root, [&](ClassSession& s, ca::usize i) {
        auto p = promoteMethod(s, i);
        ASSERT_TRUE(p.is_ok());
        CodeUnit& cu = *p.unwrap();
        const ca::u32 origStack  = cu.maxStack;
        const ca::u32 origLocals = cu.maxLocals;

        FrameEngine fe(s.backing().cp, res, FramePolicy::Strict);
        const MethodSig sig = sigOf(s, i);
        const FrameSummary sum = fe.compute(cu, sig);
        ASSERT_TRUE(sum.ok) << sig.name << ": " << jbc::analysis::to_string(sum.fail);
        ++methods;

        // 采用值口径（覆盖 + 地板）落回 CodeUnit 后与文件声明对拍。
        ASSERT_TRUE(applyComputedMaxes(cu, sum, sig));
        if (cu.maxStack < origStack) {
            ++ltStack;
            ADD_FAILURE() << "maxStack recompute < declared: method " << sig.name;
        }
        if (cu.maxLocals < origLocals) {
            ++ltLocals;
            ADD_FAILURE() << "maxLocals recompute < declared: method " << sig.name;
        }
        if (cu.maxStack == origStack && cu.maxLocals == origLocals) {
            ++equalAll;
        } else if (cu.maxStack >= origStack && cu.maxLocals >= origLocals) {
            ++geAny;
        }
    });

    std::printf("[maxes-oracle] methods=%zu equal=%zu ge=%zu ltStack=%zu "
                "ltLocals=%zu\n",
                methods, equalAll, geAny, ltStack, ltLocals);
    EXPECT_GE(methods, 15u);
    EXPECT_EQ(ltStack, 0u);    // javac 产物口径：重算不低于声明
    EXPECT_EQ(ltLocals, 0u);
}
