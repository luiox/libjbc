/// @file        smt_build_test.cpp
/// @brief       StackMapTable 生成器测试。
///
/// 三部分：
/// 1. 单元——紧凑形态选择表驱动（same/same1/chop/append/full + delta 边界）、
///    cat2 展开、Uninit offset 落 new pc、CP intern 复用、无控制流空产出、
///    版本门、防御拒绝路径。
/// 2. javac oracle——对同源语料（loop/switch 扩展）剥原 SMT →
///    重生成 → maxes 覆盖 + lower 写回 → 组装写回 → 重读解码展开，与原 SMT
///    展开按公共前缀逐槽比对（口径：多槽/更精确不判分歧）。javac
///    缺席时 skip。
/// 3. JVM 真跑——javac 样本（异常/循环/switch/构造器链）+ 手拼 uninit
///    样本，J7 全链（剥 SMT + maxes 覆盖 + lower 写回 + 重生成 SMT）后
///    java 执行验收（整链端到端的最强门）。JDK 缺席时 skip。
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
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
using jbc::analysis::SmtBuildResult;
using jbc::analysis::VKind;
using jbc::analysis::VType;
using jbc::analysis::applyComputedMaxes;
using jbc::analysis::buildStackMapTable;
using jbc::analysis::smt_detail::chooseSmtFrame;
using jbc::analysis::smt_detail::smtEntriesOf;
using jbc::ir::ClassSession;
using jbc::ir::lowerMethod;
using jbc::ir::promoteMethod;
using jbc::test::AttrSpec;
using jbc::test::ClassBuilder;
using jbc::test::ExcSpec;
using jbc::test::StubResolver;
using jbc::test::entryLocalsOf;
using jbc::test::expandSmt;
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

using Ent     = jbc::core::StackMapTableAttr::VType;
using Entries = std::vector<Ent>;

Ent top() { return Ent{Ent::kTop, 0}; }
Ent ints() { return Ent{Ent::kInteger, 0}; }
Ent flt() { return Ent{Ent::kFloat, 0}; }
Ent long_() { return Ent{Ent::kLong, 0}; }
Ent obj(ca::u32 cp) { return Ent{Ent::kObject, cp}; }
Ent uninit(ca::u32 pc) { return Ent{Ent::kUninitialized, pc}; }

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

/// iconst_1; pop; return —— 无控制流最小载体。
std::shared_ptr<ClassSession> trivialClass(ca::u16 major = 52)
{
    ClassBuilder cb = baseClass(major);
    const ca::u16 codeIdx = cb.utf8("Code");
    const ca::u16 mIdx    = cb.utf8("m");
    const Bytes code      = {0x04, 0x57, 0xb1};   // iconst_1; pop; return
    std::vector<AttrSpec> ma;
    ma.push_back(AttrSpec{codeIdx, jbc::test::codeContent(1, 0, code)});
    cb.addMethod(0x0009, mIdx, cb.utf8("()V"), std::move(ma));
    return sessionOf(cb.build());
}

void put2(Bytes& b, ca::u16 v)
{
    b.push_back(static_cast<ca::u8>(v >> 8));
    b.push_back(static_cast<ca::u8>(v & 0xFF));
}

/// static m(String,int)：ifeq 与 goto 汇合于 pc 9（双臂同高，javac 可产出
/// 形态；分支臂自带 aconst_null 栈项）。
std::shared_ptr<ClassSession> branchMergeClass()
{
    ClassBuilder cb = baseClass();
    const ca::u16 codeIdx = cb.utf8("Code");
    const Bytes code = {0x1a,             // 0 iload_1
                        0x99, 0x00, 0x07, // 1 ifeq -> 8
                        0x2a,             // 4 aload_0
                        0xa7, 0x00, 0x04, // 5 goto -> 9
                        0x01,             // 8 aconst_null（分支臂栈项）
                        0x4d,             // 9 astore_2（合并点：[String] × [Null]）
                        0xb1};            // 10 return
    std::vector<AttrSpec> ma;
    ma.push_back(AttrSpec{codeIdx, jbc::test::codeContent(2, 3, code)});
    cb.addMethod(0x0009, cb.utf8("m"), cb.utf8("(Ljava/lang/String;I)V"), std::move(ma));
    return sessionOf(cb.build());
}

/// new 后分支再汇聚到同一 invokespecial：合并点携带 Uninitialized(offset)。
std::shared_ptr<ClassSession> newMergeClass()
{
    ClassBuilder cb = baseClass();
    const ca::u16 xCls  = cb.classRef(cb.utf8("p/X"));
    const ca::u16 xInit = cb.methodref(xCls, cb.nameAndType(cb.utf8("<init>"), cb.utf8("()V")));
    const ca::u16 codeIdx = cb.utf8("Code");

    Bytes code;
    code.push_back(0xbb);              // 0 new p/X
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
    code.push_back(0x00);              // 14 nop
    code.push_back(0x00);              // 15 nop
    code.push_back(0xb7);              // 16 invokespecial <init>（合并点）
    put2(code, xInit);
    code.push_back(0xb1);              // 19 return

    std::vector<AttrSpec> ma;
    ma.push_back(AttrSpec{codeIdx, jbc::test::codeContent(2, 2, code)});
    cb.addMethod(0x0009, cb.utf8("m"), cb.utf8("()V"), std::move(ma));
    return sessionOf(cb.build());
}

/// 两个异类引用在合并点汇合（CP intern 求解路径的载体）。
std::shared_ptr<ClassSession> refBranchMergeClass()
{
    ClassBuilder cb = baseClass();
    const ca::u16 aCls = cb.classRef(cb.utf8("p/A"));
    const ca::u16 bCls = cb.classRef(cb.utf8("p/B"));
    const ca::u16 aRef = cb.fieldref(aCls, cb.nameAndType(cb.utf8("f"), cb.utf8("Lp/A;")));
    const ca::u16 bRef = cb.fieldref(bCls, cb.nameAndType(cb.utf8("f"), cb.utf8("Lp/B;")));
    const ca::u16 codeIdx = cb.utf8("Code");

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
    cb.addMethod(0x0009, cb.utf8("m"), cb.utf8("()V"), std::move(ma));
    return sessionOf(cb.build());
}

/// getstatic 后 return，handler 在 pc 5（catch Exception）。
std::shared_ptr<ClassSession> excHandlerClass()
{
    ClassBuilder cb = baseClass();
    const ca::u16 tCls   = cb.classRef(cb.utf8("p/T"));
    const ca::u16 gRef   = cb.fieldref(tCls, cb.nameAndType(cb.utf8("g"),
                                                            cb.utf8("Ljava/lang/Object;")));
    const ca::u16 excCls = cb.classRef(cb.utf8("java/lang/Exception"));
    const ca::u16 codeIdx = cb.utf8("Code");

    Bytes code;
    code.push_back(0xb2);              // 0 getstatic T.g
    put2(code, gRef);
    code.push_back(0x57);              // 3 pop
    code.push_back(0xb1);              // 4 return
    code.push_back(0x4b);              // 5 astore_0（handler）
    code.push_back(0xb1);              // 6 return

    std::vector<AttrSpec> ma;
    ma.push_back(AttrSpec{codeIdx, jbc::test::codeContent(1, 1, code,
                                                          {ExcSpec{0, 4, 5, excCls}})});
    cb.addMethod(0x0009, cb.utf8("m"), cb.utf8("()V"), std::move(ma));
    return sessionOf(cb.build());
}

/// Strict 计算 + 生成一步（合成用例公共前奏）。
jbc::analysis::SmtBuildResult buildOf(ClassSession& s, const MethodSig& sig,
                                      const StubResolver& res)
{
    auto p = promoteMethod(s, 0);
    if (p.is_err()) {
        ADD_FAILURE() << "promote failed";
        return {};
    }
    FrameEngine fe(s.backing().cp, *const_cast<StubResolver*>(&res), FramePolicy::Strict);
    std::vector<FrameAt> frames;
    const FrameSummary sum = fe.compute(*p.unwrap(), sig, frames);
    if (!sum.ok) {
        ADD_FAILURE() << "strict compute failed: " << jbc::analysis::to_string(sum.fail);
        return {};
    }
    auto r = buildStackMapTable(s, *p.unwrap(), frames);
    if (r.is_err()) {
        ADD_FAILURE() << "build failed: " << jbc::analysis::to_string(r.unwrap_err());
        return {};
    }
    return std::move(r).unwrap();
}

/// CP 中现成的属性名 Utf8（挂回 SMT 时复用既有条目，不追加重复 Utf8）。
ca::u32 smtNameCp(ClassFile& cf)
{
    for (ca::u32 i = 1; i < cf.cp.size(); ++i) {
        if (cf.cp.valid(i) && cf.cp.utf8Equals(i, "StackMapTable")) {
            return i;
        }
    }
    return cf.cp.addUtf8("StackMapTable");
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

/// 解码 Code 内全部嵌套属性后定位 SMT（只读；没有返回 nullptr）。
const StackMapTableAttr* findSmt(ClassFile& cf, MemberInfo& m)
{
    const ca::usize ci = decodeCodeSlot(cf, m);
    if (ci == static_cast<ca::usize>(-1)) {
        return nullptr;
    }
    auto& code = static_cast<CodeAttr&>(*m.attrs[ci].body);
    for (AttrSlot& nested : code.attrs) {
        (void)cf.decodeAttr(nested);   // 按内容识别前必须先解码（lazy）
        if (nested.peek() != nullptr && nested.peek()->kind() == AttrKind::StackMapTable) {
            return static_cast<const StackMapTableAttr*>(nested.peek());
        }
    }
    return nullptr;
}

/// 剥掉方法 Code 内的 SMT 嵌套槽（先解码再按 kind 识别；置脏走 mutate）。
/// 没有则无操作。返回是否剥了。
bool stripSmt(ClassFile& cf, MemberInfo& m)
{
    const ca::usize ci = decodeCodeSlot(cf, m);
    if (ci == static_cast<ca::usize>(-1)) {
        return false;
    }
    AttrSlot& codeSlot = m.attrs[ci];
    (void)cf.decodeAttr(codeSlot);
    auto& code = static_cast<CodeAttr&>(*codeSlot.mutate());
    for (AttrSlot& nested : code.attrs) {
        (void)cf.decodeAttr(nested);
    }
    const ca::usize before = code.attrs.size();
    code.attrs.erase(std::remove_if(code.attrs.begin(), code.attrs.end(),
                                    [](const AttrSlot& n) {
                                        return n.peek() != nullptr &&
                                               n.peek()->kind() == AttrKind::StackMapTable;
                                    }),
                     code.attrs.end());
    return code.attrs.size() != before;
}

MethodSig sigOf(const ClassSession& s, ca::usize i)
{
    const auto& mu = s.unit().methods[i];
    MethodSig sig;
    sig.isStatic  = (mu.access & 0x0008) != 0;
    sig.desc      = s.unit().symbols().str(mu.desc);
    sig.name      = s.unit().symbols().str(mu.name);
    sig.thisClass = s.unit().symbols().str(s.unit().thisClass);
    return sig;
}

/// J7 全链生成管线（oracle 与 JVM 探针共用，A1 收官门）：对会话内全部方法
/// 剥原 SMT → Strict 帧 → 重生成 → maxes 覆盖采用（applyComputedMaxes）→
/// lower 写回（脏检查分支落 backing）→ 挂回 SMT。返回失败原因（空 = 成功）。
struct RegenStats
{
    ca::usize methods = 0;    ///< 处理的带 Code 方法数
    ca::usize generated = 0;  ///< 重新附加 SMT 的方法数
    ca::usize empty = 0;      ///< 无控制流（needed=false）的方法数
};

std::string regenSmtInSession(ClassSession& s, StubResolver& res, RegenStats& st)
{
    ClassFile& cf = s.backing();
    for (ca::usize i = 0; i < cf.methods.size(); ++i) {
        MemberInfo& m = cf.methods[i];
        if (decodeCodeSlot(cf, m) == static_cast<ca::usize>(-1)) {
            continue;   // abstract/native
        }
        (void)stripSmt(cf, m);
        ++st.methods;

        auto p = promoteMethod(s, i);
        if (p.is_err()) {
            return std::string("promote failed: method ") + std::to_string(i);
        }
        const MethodSig sig = sigOf(s, i);
        FrameEngine fe(cf.cp, res, FramePolicy::Strict);
        std::vector<FrameAt> frames;
        const FrameSummary sum = fe.compute(*p.unwrap(), sig, frames);
        if (!sum.ok) {
            return std::string("strict compute failed (") + std::string(sig.name)
                + "): " + jbc::analysis::to_string(sum.fail);
        }
        auto r = buildStackMapTable(s, *p.unwrap(), frames);
        if (r.is_err()) {
            return std::string("build failed (") + std::string(sig.name) + "): "
                + jbc::analysis::to_string(r.unwrap_err());
        }
        auto built = std::move(r).unwrap();

        // J7 全链：maxes 覆盖（显式 API）→ lower 写回——布局未变，lower 不
        // 触碰指令与嵌套属性；maxes 变化走手动链路的同一脏检查分支（覆盖
        // 语义），与 JVM 真跑门共同锁「重算 maxes 可运行」。
        if (!applyComputedMaxes(*p.unwrap(), sum, sig)) {
            return std::string("maxes adopt failed (") + std::string(sig.name) + ")";
        }
        auto lw = lowerMethod(s, i);
        if (lw.is_err()) {
            return std::string("lower failed (") + std::string(sig.name) + "): "
                + jbc::ir::to_string(lw.unwrap_err());
        }
        if (!built.needed) {
            ++st.empty;
            continue;
        }
        // 挂回：lower 后重新定位 Code 槽，把再生 SMT 追加进去。
        const ca::usize ci = decodeCodeSlot(cf, m);
        if (ci == static_cast<ca::usize>(-1)) {
            return std::string("code slot lost: method ") + std::to_string(i);
        }
        auto& code = static_cast<CodeAttr&>(*m.attrs[ci].mutate());
        code.attrs.push_back(AttrSlot::makeNew(smtNameCp(cf), std::move(built.attr)));
        ++st.generated;
    }
    return {};
}

void spitBytes(const fs::path& p, const Bytes& bytes)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

}   // namespace

// ---------------------------------------------------------------------------
// 紧凑形态选择（表驱动，JVMS 4.7.4 全形态 + 边界 delta）
// ---------------------------------------------------------------------------

namespace {

struct FormCase
{
    const char* name;
    ca::u32 delta;
    Entries prev;
    Entries locals;
    Entries stack;
    ca::u8 expectType;
    Entries expectLocals;   ///< 期望的帧载荷 locals（full/append；空 = 不查）
    Entries expectStack;    ///< 期望的帧载荷 stack（full/same1；空 = 不查）
};

}   // namespace

TEST(SmtBuildForm, CompactFormTableDriven)
{
    const std::vector<FormCase> cases = {
        // same 家族：locals 与链上前帧一致、栈空。
        {"same_delta0", 0, {ints()}, {ints()}, {}, 0, {}, {}},
        {"same_delta63", 63, {ints(), flt()}, {ints(), flt()}, {}, 63, {}, {}},
        {"same_ext_delta64", 64, {ints()}, {ints()}, {}, 251, {}, {}},
        {"same_empty", 5, {}, {}, {}, 5, {}, {}},
        // same1 家族：locals 一致、栈恰 1 条目。
        {"same1_delta0", 0, {ints()}, {ints()}, {obj(7)}, 64, {}, {}},
        {"same1_delta63", 63, {ints()}, {ints()}, {long_()}, 127, {}, {}},
        {"same1_ext_delta64", 64, {ints()}, {ints()}, {uninit(3)}, 247, {}, {}},
        // chop：栈空且 locals 恰为去掉末尾 k（1..3）条。
        {"chop_k1", 4, {ints(), ints()}, {ints()}, {}, 250, {}, {}},
        {"chop_k2", 4, {ints(), ints(), ints()}, {ints()}, {}, 249, {}, {}},
        {"chop_k3", 4, {ints(), ints(), ints(), ints()}, {ints()}, {}, 248, {}, {}},
        {"chop_k4_overflows_to_full", 4, {ints(), ints(), ints(), ints(), ints()},
         {ints()}, {}, 255, {ints()}, {}},
        {"chop_not_prefix_falls_to_full", 4, {ints(), obj(1)}, {obj(1), ints()}, {}, 255,
         {obj(1), ints()}, {}},
        {"chop_with_stack_falls_to_full", 4, {ints(), ints()}, {ints()}, {ints()}, 255,
         {ints()}, {ints()}},
        // append：栈空且 locals 恰为末尾追加 k（1..3）条。
        {"append_k1", 4, {ints()}, {ints(), flt()}, {}, 252, {flt()}, {}},
        {"append_k2", 4, {ints()}, {ints(), flt(), long_()}, {}, 253, {flt(), long_()},
         {}},
        {"append_k3", 4, {}, {ints(), flt(), long_()}, {}, 254,
         {ints(), flt(), long_()}, {}},
        {"append_k4_overflows_to_full", 4, {}, {ints(), flt(), long_(), ints()}, {}, 255,
         {ints(), flt(), long_(), ints()}, {}},
        {"append_with_stack_falls_to_full", 4, {ints()}, {ints(), flt()}, {ints()}, 255,
         {ints(), flt()}, {ints()}},
        // cat2：单条目（Long/Double 不带垫底）。
        {"cat2_append_long", 4, {}, {long_()}, {}, 252, {long_()}, {}},
        {"cat2_chop_long", 4, {long_()}, {}, {}, 250, {}, {}},
        {"cat2_same_long", 4, {long_()}, {long_()}, {}, 4, {}, {}},
        // full：中部变化只能整体替换。
        {"full_mid_change", 4, {ints(), obj(1)}, {ints(), obj(2)}, {ints()}, 255,
         {ints(), obj(2)}, {ints()}},
        {"full_delta0_mid_change", 0, {ints()}, {flt()}, {}, 255, {flt()}, {}},
    };

    for (const FormCase& c : cases) {
        const jbc::core::StackMapTableAttr::Frame f =
            chooseSmtFrame(c.delta, c.prev, c.locals, c.stack);
        EXPECT_EQ(f.type, c.expectType) << c.name;
        EXPECT_EQ(f.offset, c.delta) << c.name;
        if (c.expectType == 255 || f.type == 252 || f.type == 253 || f.type == 254) {
            EXPECT_EQ(f.locals, c.expectLocals) << c.name;
        }
        if (!c.expectStack.empty()) {
            EXPECT_EQ(f.stack, c.expectStack) << c.name;
        }
    }
}

// ---------------------------------------------------------------------------
// cat2 展开（槽语义 → verification_type 条目）
// ---------------------------------------------------------------------------

TEST(SmtBuildEntries, Cat2CollapsesToOneEntry)
{
    auto s = trivialClass();
    ASSERT_NE(s, nullptr);

    // long 值槽 + 垫底槽 → 单条目（tag 4）。
    auto r = smtEntriesOf(*s, {VType{VKind::Long, 0, {}}, VType{VKind::Top, 0, {}}});
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.unwrap(), Entries({long_()}));

    // double 同理（tag 3）。
    auto r2 = smtEntriesOf(*s, {VType{VKind::Double, 0, {}}, VType{VKind::Top, 0, {}}});
    ASSERT_TRUE(r2.is_ok());
    EXPECT_EQ(r2.unwrap(), Entries({Ent{Ent::kDouble, 0}}));

    // cat1 原样；中部 Top 保留。
    auto r3 = smtEntriesOf(*s, {VType{VKind::Int, 0, {}}, VType{VKind::Top, 0, {}},
                                VType{VKind::Float, 0, {}}});
    ASSERT_TRUE(r3.is_ok());
    EXPECT_EQ(r3.unwrap(), Entries({ints(), top(), flt()}));
}

TEST(SmtBuildEntries, Cat2WithoutFillerRejected)
{
    auto s = trivialClass();
    ASSERT_NE(s, nullptr);

    // long 值槽后没有垫底槽：帧不自洽，拒绝。
    auto r = smtEntriesOf(*s, {VType{VKind::Long, 0, {}}});
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), SmtBuildError::BottomInFrame);

    // 垫底槽被占用：同上。
    auto r2 = smtEntriesOf(*s, {VType{VKind::Long, 0, {}}, VType{VKind::Int, 0, {}}});
    ASSERT_TRUE(r2.is_err());
    EXPECT_EQ(r2.unwrap_err(), SmtBuildError::BottomInFrame);
}

/// 引擎栈上 cat2 的交付口径：长值跨分支目标（合并点栈上持有 long）时，
/// 交付帧的栈与 SMT 条目空间同序——值槽在前、Top 垫底在后。引擎内部
/// [垫底行, 值行] 的存储布局不得泄漏到 FrameAt（泄漏形态在 smtEntriesOf
/// 处会以 BottomInFrame 被拒，B 批 junk 合并点样本即此暴露）。
TEST(SmtBuildEntries, Cat2OnStackDeliveredValueFirst)
{
    ClassBuilder cb = baseClass();

    Bytes code;
    code.push_back(0x0a);              // 0 lconst_1
    code.push_back(0xa7);              // 1 goto -> 8
    put2(code, 7);
    code.push_back(0x0a);              // 4 lconst_1
    code.push_back(0x00);              // 5 nop
    code.push_back(0x00);              // 6 nop
    code.push_back(0x00);              // 7 nop
    code.push_back(0x3f);              // 8 lstore_0（合并点：栈上持有 long）
    code.push_back(0xb1);              // 9 return

    std::vector<AttrSpec> ma;
    ma.push_back(AttrSpec{cb.utf8("Code"), jbc::test::codeContent(2, 2, code)});
    cb.addMethod(0x0009, cb.utf8("m"), cb.utf8("()V"), std::move(ma));
    auto s = sessionOf(cb.build());
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());

    StubResolver res;
    FrameEngine fe(s->backing().cp, res, FramePolicy::Strict);
    std::vector<FrameAt> frames;
    const FrameSummary sum =
        fe.compute(*p.unwrap(), MethodSig{true, "()V", "m", ""}, frames);
    ASSERT_TRUE(sum.ok) << jbc::analysis::to_string(sum.fail);

    // 合并点交付：[Long, Top]（值槽在前）。
    const ca::usize atMerge = indexAtPc(*p.unwrap(), 8);
    ASSERT_NE(atMerge, static_cast<ca::usize>(-1));
    ASSERT_EQ(frames[atMerge].stack.size(), 2u);
    EXPECT_EQ(frames[atMerge].stack[0].k, VKind::Long);
    EXPECT_EQ(frames[atMerge].stack[1].k, VKind::Top);

    // lstore 后：值落 local0/local1，同一序（既有口径不变）。
    const ca::usize atRet = indexAtPc(*p.unwrap(), 9);
    ASSERT_GE(frames[atRet].locals.size(), 2u);
    EXPECT_EQ(frames[atRet].locals[0].k, VKind::Long);
    EXPECT_EQ(frames[atRet].locals[1].k, VKind::Top);

    // 生成器直接接受该帧（端到端自洽：交付序 = 条目空间序）。
    auto built = buildStackMapTable(*s, *p.unwrap(), frames);
    ASSERT_TRUE(built.is_ok());
    EXPECT_TRUE(std::move(built).unwrap().needed);
}

TEST(SmtBuildEntries, RefInternsClassAndBottomRejected)
{
    auto s = trivialClass();
    ASSERT_NE(s, nullptr);
    const ca::u32 before = s->backing().cp.size();

    auto r = smtEntriesOf(*s, {VType::ref("p/Q")});
    ASSERT_TRUE(r.is_ok());
    const Entries got = r.unwrap();
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].tag, Ent::kObject);
    ASSERT_TRUE(s->backing().cp.valid(got[0].cp));
    EXPECT_EQ(s->backing().cp.utf8Of(s->backing().cp.at(got[0].cp).a), "p/Q");
    // 名字 intern 走 session 语义面：utf8 + class 两条新增。
    EXPECT_EQ(s->backing().cp.size(), before + 2);

    // Bottom / 无名引用：防御拒绝。
    auto rb = smtEntriesOf(*s, {VType{VKind::Bottom, 0, {}}});
    ASSERT_TRUE(rb.is_err());
    EXPECT_EQ(rb.unwrap_err(), SmtBuildError::BottomInFrame);
    auto rn = smtEntriesOf(*s, {VType::ref("")});
    ASSERT_TRUE(rn.is_err());
    EXPECT_EQ(rn.unwrap_err(), SmtBuildError::BottomInFrame);
}

// ---------------------------------------------------------------------------
// 生成 API：目标集合 / 版本门 / 空产出 / CP 落位
// ---------------------------------------------------------------------------

TEST(SmtBuildApi, NoControlFlowYieldsNothing)
{
    auto s = trivialClass();
    ASSERT_NE(s, nullptr);
    const SmtBuildResult r = buildOf(*s, MethodSig{true, "()V", "m", ""}, StubResolver{});
    EXPECT_FALSE(r.needed);
    EXPECT_EQ(r.attr, nullptr);
    EXPECT_EQ(r.frameCount, 0u);
}

TEST(SmtBuildApi, EmptyMethodYieldsNothing)
{
    // 最小合法方法体（单条 return，0 栈 0 局部）：无合并点 → 不产出属性。
    // 真正的空指令体是非法 classfile（JVM 要求至少一条 return），promote
    // 侧已 fail-loud，生成器不会见到该形态。
    ClassBuilder cb = baseClass();
    const ca::u16 codeIdx = cb.utf8("Code");
    const Bytes code = {0xb1};   // return
    std::vector<AttrSpec> ma;
    ma.push_back(AttrSpec{codeIdx, jbc::test::codeContent(0, 0, code)});
    cb.addMethod(0x0009, cb.utf8("m"), cb.utf8("()V"), std::move(ma));
    auto s = sessionOf(cb.build());
    ASSERT_NE(s, nullptr);
    const SmtBuildResult r = buildOf(*s, MethodSig{true, "()V", "m", ""}, StubResolver{});
    EXPECT_FALSE(r.needed);
    EXPECT_EQ(r.attr, nullptr);
    EXPECT_EQ(r.frameCount, 0u);
}

TEST(SmtBuildApi, VersionGateBelowFifty)
{
    auto s = trivialClass(49);
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    StubResolver res;
    FrameEngine fe(s->backing().cp, res, FramePolicy::Strict);
    std::vector<FrameAt> frames;
    const FrameSummary sum =
        fe.compute(*p.unwrap(), MethodSig{true, "()V", "m", ""}, frames);
    ASSERT_TRUE(sum.ok);

    auto r = buildStackMapTable(*s, *p.unwrap(), frames);
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), SmtBuildError::ClassTooOld);
}

TEST(SmtBuildApi, FramesNotAlignedRejected)
{
    auto s = branchMergeClass();
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    StubResolver res;
    FrameEngine fe(s->backing().cp, res, FramePolicy::Strict);
    std::vector<FrameAt> frames;
    ASSERT_TRUE(fe.compute(*p.unwrap(), MethodSig{true, "(Ljava/lang/String;I)V", "m", ""},
                           frames)
                    .ok);

    auto r = buildStackMapTable(*s, *p.unwrap(), {});
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), SmtBuildError::FramesNotOk);
}

/// try 范围只覆盖不可抛指令（iload/ifeq/goto）+ handler 段首 nop 的形态。
/// JVM 校验器对范围内全部有帧指令做 handler 可指派性检查，handler 入口恒
/// 须有帧；引擎若仅从可抛指令发异常边，此形态下 handler 不可达，SMT 缺
/// handler 帧（JVM 加载期 VerifyError）。handler 本体只有异常边进入。
/// anyCatch=false 时 catch_type 指向真实 Class 条目，true 时为 0（any）。
std::shared_ptr<ClassSession> deadTryClass(bool anyCatch)
{
    ClassBuilder cb = baseClass();
    const ca::u32 catchCp =
        anyCatch ? 0 : static_cast<ca::u32>(cb.classRef(cb.utf8("java/lang/Exception")));
    const ca::u16 codeIdx = cb.utf8("Code");
    const Bytes code = {
        0x04,             // 0  iconst_1
        0x3b,             // 1  istore_0
        0x1a,             // 2  iload_0            ← try 起（不可抛）
        0x99, 0x00, 0x09, // 3  ifeq -> 12         （范围内，不可抛）
        0xa7, 0x00, 0x06, // 6  goto -> 12         ← try 止（开区间）
        0x00,             // 9  nop                ← handler 段首
        0x57,             // 10 pop（消耗 throwable）
        0xb1,             // 11 return
        0xb1,             // 12 return
    };
    std::vector<AttrSpec> ma;
    ma.push_back(AttrSpec{
        codeIdx,
        jbc::test::codeContent(1, 1, code, {ExcSpec{2, 9, 9, static_cast<ca::u16>(catchCp)}})});
    cb.addMethod(0x0009, cb.utf8("m"), cb.utf8("(I)V"), std::move(ma));
    return sessionOf(cb.build());
}

TEST(SmtBuildApi, HandlerFrameEmittedForFramedCoveredInsns)
{
    const MethodSig sig{true, "(I)V", "m", ""};
    const auto check = [&](ClassSession& s) {
        const SmtBuildResult r = buildOf(s, sig, StubResolver{});
        ASSERT_TRUE(r.needed);
        ASSERT_NE(r.attr, nullptr);
        const auto full =
            expandSmt(*r.attr, entryLocalsOf(sig), s.backing().cp);
        const auto it = std::find_if(full.begin(), full.end(),
                                     [](const jbc::test::FullFrame& f) { return f.offset == 9; });
        ASSERT_NE(it, full.end()) << "handler entry frame missing at pc 9";
        ASSERT_EQ(it->stack.size(), 1u);
        EXPECT_EQ(it->stack[0].k, VKind::Ref);
    };

    // catch_type 非零变体。
    {
        auto s = deadTryClass(false);
        ASSERT_NE(s, nullptr);
        check(*s);
    }
    // catch_type 0（any）变体：handler 帧同样恒在。
    {
        auto s = deadTryClass(true);
        ASSERT_NE(s, nullptr);
        check(*s);
    }
}

TEST(SmtBuildApi, BranchMergeFrameIsSame1WithInternedObject)
{
    auto s = branchMergeClass();
    ASSERT_NE(s, nullptr);
    const SmtBuildResult r =
        buildOf(*s, MethodSig{true, "(Ljava/lang/String;I)V", "m", ""}, StubResolver{});
    ASSERT_TRUE(r.needed);
    ASSERT_NE(r.attr, nullptr);
    // 目标集 = ifeq 目标 pc 8（空栈到达、locals 同初始帧 → same(8)）+
    // goto 目标 pc 9（合并点）。
    ASSERT_EQ(r.attr->frames.size(), 2u);
    EXPECT_EQ(r.attr->frames[0].type, 8u);
    EXPECT_EQ(r.attr->frames[0].offset, 8u);

    // 合并点 pc 9：双臂同高（javac 可产出形态）→ same1(64+8)；
    // Ref{String} × Null → Ref{String}（null 万能引用底，与 Top 支配正交）。
    const auto& f = r.attr->frames[1];
    EXPECT_EQ(f.type, 64);     // same1 家族，tag = 64 + delta（delta 0）
    EXPECT_EQ(f.offset, 0u);   // 9 - 8 - 1
    ASSERT_EQ(f.stack.size(), 1u);
    EXPECT_EQ(f.stack[0].tag, Ent::kObject);
    EXPECT_EQ(s->backing().cp.utf8Of(s->backing().cp.at(f.stack[0].cp).a),
              "java/lang/String");
}

/// 汇合单边赋值槽 → 陈述 Top（裁决口径的常规边直测）：只在分支
/// 臂赋值的 local，汇合帧按 verifier 合并陈述 Top（append 载荷 [top]）。
TEST(SmtBuildApi, MixedJoinSlotAppendsTop)
{
    ClassBuilder cb = baseClass();
    const ca::u16 codeIdx = cb.utf8("Code");
    const Bytes code = {0x1a,             // 0 iload_1
                        0x99, 0x00, 0x05, // 1 ifeq -> 6
                        0x2a,             // 4 aload_0（仅此臂赋 local2）
                        0x4d,             // 5 astore_2
                        0xb1};            // 6 return（汇合：local2 单边赋值）
    std::vector<AttrSpec> ma;
    ma.push_back(AttrSpec{codeIdx, jbc::test::codeContent(1, 3, code)});
    cb.addMethod(0x0009, cb.utf8("m"), cb.utf8("(Ljava/lang/String;I)V"), std::move(ma));
    auto s = sessionOf(cb.build());
    ASSERT_NE(s, nullptr);

    const SmtBuildResult r =
        buildOf(*s, MethodSig{true, "(Ljava/lang/String;I)V", "m", ""}, StubResolver{});
    ASSERT_TRUE(r.needed);
    ASSERT_NE(r.attr, nullptr);
    ASSERT_EQ(r.attr->frames.size(), 1u);
    const auto& f = r.attr->frames[0];
    EXPECT_EQ(f.type, 252u);   // append 1
    EXPECT_EQ(f.offset, 6u);
    ASSERT_EQ(f.locals.size(), 1u);
    EXPECT_EQ(f.locals[0].tag, Ent::kTop);
}

TEST(SmtBuildApi, UninitPayloadLandsOnNewPc)
{
    auto s = newMergeClass();
    ASSERT_NE(s, nullptr);
    const SmtBuildResult r =
        buildOf(*s, MethodSig{true, "()V", "m", ""}, StubResolver{});
    ASSERT_TRUE(r.needed);
    // 目标集合 = ifeq 目标 pc 13 + goto 目标 pc 16（两处都需帧）。
    ASSERT_EQ(r.attr->frames.size(), 2u);

    // 合并点 pc 16：locals/stack 携带 Uninitialized(offset=0)——0 号指令是 new。
    // 帧载荷里的 offset 是编码口径的 delta（首帧 = pc，其后 = 相邻差 - 1）。
    const auto& f = r.attr->frames[1];
    EXPECT_EQ(f.offset, 2u);   // 16 - 13 - 1
    ASSERT_EQ(f.locals.size(), 2u);
    EXPECT_EQ(f.locals[0].tag, Ent::kTop);
    EXPECT_EQ(f.locals[1].tag, Ent::kUninitialized);
    EXPECT_EQ(f.locals[1].cp, 0u);   // new 的 pc
    ASSERT_EQ(f.stack.size(), 2u);
    EXPECT_EQ(f.stack[0].tag, Ent::kUninitialized);
    EXPECT_EQ(f.stack[0].cp, 0u);
    EXPECT_EQ(f.stack[1].tag, Ent::kUninitialized);
    EXPECT_EQ(f.stack[1].cp, 0u);
    // 首帧（pc 13）的 locals 同样携带该 Uninitialized 条目。
    const auto& f0 = r.attr->frames[0];
    EXPECT_EQ(f0.offset, 13u);
    ASSERT_EQ(f0.locals.size(), 2u);
    EXPECT_EQ(f0.locals[1].tag, Ent::kUninitialized);
    EXPECT_EQ(f0.locals[1].cp, 0u);
}

TEST(SmtBuildApi, UninitPayloadNotOnNewRejected)
{
    auto s = newMergeClass();
    ASSERT_NE(s, nullptr);
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    StubResolver res;
    FrameEngine fe(s->backing().cp, res, FramePolicy::Strict);
    std::vector<FrameAt> frames;
    ASSERT_TRUE(fe.compute(*p.unwrap(), MethodSig{true, "()V", "m", ""}, frames).ok);
    // 篡改载荷为不存在的 pc：生成器必须拒绝而不是编出指向虚无的条目。
    frames[indexAtPc(*p.unwrap(), 13)].locals[1] = VType::uninitNew(999);
    auto r = buildStackMapTable(*s, *p.unwrap(), frames);
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), SmtBuildError::TargetNotInsn);
}

TEST(SmtBuildApi, ObjectInternReusesExistingEntries)
{
    auto s = refBranchMergeClass();
    ASSERT_NE(s, nullptr);
    StubResolver res;
    res.parent["p/A"] = "p/M";
    res.parent["p/B"] = "p/M";
    res.parent["p/M"] = "java/lang/Object";
    const ca::u32 before = s->backing().cp.size();

    const SmtBuildResult r = buildOf(*s, MethodSig{true, "()V", "m", ""}, res);
    ASSERT_TRUE(r.needed);
    // 目标集合 = ifeq 目标 pc 10 + goto 目标 pc 13。pc 10 的帧是 getstatic
    // 执行前的到达态（栈空、locals 同初始帧 → same(10)）。
    ASSERT_EQ(r.attr->frames.size(), 2u);
    const auto& f0 = r.attr->frames[0];
    EXPECT_EQ(f0.offset, 10u);
    EXPECT_EQ(f0.type, 10u);
    EXPECT_TRUE(f0.stack.empty());
    const auto& f = r.attr->frames[1];
    EXPECT_EQ(f.offset, 2u);   // 13 - 10 - 1
    ASSERT_EQ(f.stack.size(), 1u);
    EXPECT_EQ(f.stack[0].tag, Ent::kObject);
    EXPECT_EQ(s->backing().cp.utf8Of(s->backing().cp.at(f.stack[0].cp).a), "p/M");
    // p/A、p/B 的 Class 条目已在池里（指令操作数）；新增只有 p/M 的 utf8+class。
    EXPECT_EQ(s->backing().cp.size(), before + 2);

    // 同会话重复生成：既有条目全部复用，CP 不再增长。
    const SmtBuildResult r2 = buildOf(*s, MethodSig{true, "()V", "m", ""}, res);
    ASSERT_TRUE(r2.needed);
    EXPECT_EQ(s->backing().cp.size(), before + 2);
    EXPECT_EQ(r2.attr->frames[1].stack[0].cp, f.stack[0].cp);
}

TEST(SmtBuildApi, HandlerFrameCarriesExceptionStack)
{
    auto s = excHandlerClass();
    ASSERT_NE(s, nullptr);
    const SmtBuildResult r = buildOf(*s, MethodSig{true, "()V", "m", ""}, StubResolver{});
    ASSERT_TRUE(r.needed);
    ASSERT_EQ(r.attr->frames.size(), 1u);

    // handler pc 5：locals 与初始帧一致（空）+ 栈 1 条目 → same1(64+5)。
    const auto& f = r.attr->frames[0];
    EXPECT_EQ(f.type, 69u);
    EXPECT_EQ(f.offset, 5u);
    ASSERT_EQ(f.stack.size(), 1u);
    EXPECT_EQ(f.stack[0].tag, Ent::kObject);
    EXPECT_EQ(s->backing().cp.utf8Of(s->backing().cp.at(f.stack[0].cp).a),
              "java/lang/Exception");
}

// ---------------------------------------------------------------------------
// javac oracle：剥原 SMT → 重生成 → 写回 → 重读解码展开 → 公共前缀逐槽比对
// ---------------------------------------------------------------------------

namespace {

/// 公共前缀逐槽比对（口径：多槽/更精确不判分歧；oracle 双侧都经
/// fromSmtTag 展开，cls 恒 0，逐槽即比 tag 语义）。
void compareExpanded(const char* ctx,
                     const std::vector<jbc::test::FullFrame>& expect,
                     const std::vector<jbc::test::FullFrame>& actual,
                     ca::usize& checked, ca::usize& mismatch)
{
    if (expect.size() != actual.size()) {
        ++mismatch;
        ADD_FAILURE() << ctx << ": frame count " << actual.size() << " != "
                      << expect.size();
        return;
    }
    for (ca::usize i = 0; i < expect.size(); ++i) {
        if (expect[i].offset != actual[i].offset) {
            ++mismatch;
            ADD_FAILURE() << ctx << ": frame " << i << " offset " << actual[i].offset
                          << " != " << expect[i].offset;
            continue;
        }
        ++checked;
        const std::vector<VType> expLocals =
            headOf(expect[i].locals, trimmedSize(expect[i].locals));
        const std::vector<VType> gotLocals =
            headOf(actual[i].locals, trimmedSize(actual[i].locals));
        const ca::usize nLocals =
            gotLocals.size() < expLocals.size() ? gotLocals.size() : expLocals.size();
        const bool localsOk =
            std::equal(gotLocals.begin(),
                       gotLocals.begin() + static_cast<std::ptrdiff_t>(nLocals),
                       expLocals.begin(), sameSlot);
        const bool stackOk = actual[i].stack.size() == expect[i].stack.size() &&
                             std::equal(actual[i].stack.begin(), actual[i].stack.end(),
                                        expect[i].stack.begin(), sameSlot);
        if (!localsOk || !stackOk) {
            ++mismatch;
            ADD_FAILURE() << ctx << " @" << expect[i].offset << "\n  expect locals "
                          << slotsText(expLocals) << "\n  actual locals "
                          << slotsText(gotLocals) << "\n  expect stack "
                          << slotsText(expect[i].stack) << "\n  actual stack "
                          << slotsText(actual[i].stack);
        }
    }
}

}   // namespace

TEST(SmtBuildOracle, RegeneratedSmtMatchesOriginal)
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

    std::error_code iterEc;
    for (fs::recursive_directory_iterator it(root / "classes", iterEc), end;
         it != end && !iterEc; it.increment(iterEc)) {
        if (!it->is_regular_file() || it->path().extension() != ".class") {
            continue;
        }
        const fs::path clsPath = it->path();
        const std::string clsName = clsPath.stem().string();
        auto s1 = sessionOf(slurpFile(clsPath));
        ASSERT_NE(s1, nullptr) << clsPath;

        for (ca::usize i = 0; i < s1->unit().methods.size(); ++i) {
            MemberInfo& m1 = s1->backing().methods[i];
            const MethodSig sig = sigOf(*s1, i);
            const StackMapTableAttr* orig = findSmt(s1->backing(), m1);
            if (orig == nullptr) {
                continue;   // 无分支直线方法：javac 也不产 SMT
            }
            ++methods;
            const std::vector<jbc::test::FullFrame> expect =
                expandSmt(*orig, entryLocalsOf(sig), s1->backing().cp);
            ASSERT_FALSE(expect.empty()) << clsName << "." << sig.name;

            // 剥原 SMT → 写回。
            ASSERT_TRUE(stripSmt(s1->backing(), m1)) << clsName;
            auto w1 = writeClass(s1->backing());
            ASSERT_TRUE(w1.is_ok()) << clsName;

            // 重读 → Strict 帧 → 重生成（管线全走一遍）。
            auto s2 = sessionOf(std::move(w1).unwrap());
            ASSERT_NE(s2, nullptr) << clsName;
            RegenStats st;
            const std::string err = regenSmtInSession(*s2, res, st);
            ASSERT_TRUE(err.empty()) << clsName << ": " << err;
            EXPECT_EQ(st.methods, s2->backing().methods.size());

            auto w2 = writeClass(s2->backing());
            ASSERT_TRUE(w2.is_ok()) << clsName;

            // 重读解码：生成链经真实编码器落盘后必须展开回一致的全帧。
            auto s3 = sessionOf(std::move(w2).unwrap());
            ASSERT_NE(s3, nullptr) << clsName;
            const StackMapTableAttr* gen =
                findSmt(s3->backing(), s3->backing().methods[i]);
            ASSERT_NE(gen, nullptr) << clsName << "." << sig.name << ": SMT lost";
            const std::vector<jbc::test::FullFrame> actual =
                expandSmt(*gen, entryLocalsOf(sig), s3->backing().cp);
            const std::string ctx = clsName + "." + std::string(sig.name);
            compareExpanded(ctx.c_str(), expect, actual, framesChecked, mismatch);
        }
    }
    std::printf("[smt-build-oracle] methods=%zu framesChecked=%zu mismatch=%zu\n",
                methods, framesChecked, mismatch);
    EXPECT_GE(methods, 10u);   // 语料护栏（帧语料 + loop/switch 扩展）
    EXPECT_GE(framesChecked, 30u);
    EXPECT_EQ(mismatch, 0u);   // 验收口径：重生成与 javac 原产物公共前缀一致
}

// ---------------------------------------------------------------------------
// JVM 真跑：重生成 SMT 的类必须在真实 JVM 上加载执行
// ---------------------------------------------------------------------------

namespace {

fs::path probeResource()
{
    return fs::path("test_resource") / "libjbc" / "smt_probe";
}

/// 手拼 UninitProbe.run：new 后分支再汇聚到同一 \<init\>——真实帧携带
/// Uninitialized(offset) 条目（javac --release 8 产不出该形态，合成补位）。
/// 执行结果 = "U"。
Bytes buildUninitProbe()
{
    ClassBuilder cb;
    const ca::u16 selfUtf  = cb.utf8("demo/UninitProbe");
    const ca::u16 superUtf = cb.utf8("java/lang/Object");
    const ca::u16 sbUtf    = cb.utf8("java/lang/StringBuilder");
    const ca::u16 sbCls    = cb.classRef(sbUtf);
    const ca::u16 selfCls  = cb.classRef(selfUtf);
    const ca::u16 superCls = cb.classRef(superUtf);
    const ca::u16 initNat =
        cb.nameAndType(cb.utf8("<init>"), cb.utf8("(Ljava/lang/String;)V"));
    const ca::u16 toStringNat =
        cb.nameAndType(cb.utf8("toString"), cb.utf8("()Ljava/lang/String;"));
    const ca::u16 sbInit  = cb.methodref(sbCls, initNat);
    const ca::u16 sbToStr = cb.methodref(sbCls, toStringNat);
    const ca::u16 strU    = cb.stringConst(cb.utf8("U"));
    const ca::u16 codeIdx = cb.utf8("Code");

    cb.setThis(selfCls);
    cb.setSuper(superCls);
    cb.setAccess(0x0021);
    cb.setVersion(52);

    Bytes code;
    code.push_back(0xbb);              // 0 new StringBuilder
    put2(code, sbCls);
    code.push_back(0x59);              // 3 dup
    code.push_back(0x4b);              // 4 astore_0
    code.push_back(0x04);              // 5 iconst_1
    code.push_back(0x99);              // 6 ifeq -> 11
    put2(code, 5);
    code.push_back(0x01);              // 9 aconst_null
    code.push_back(0x57);              // 10 pop
    code.push_back(0x2a);              // 11 aload_0（合并点：locals/stack 携带 uninit@0）
    code.push_back(0x12);              // 12 ldc "U"
    code.push_back(static_cast<ca::u8>(strU));
    code.push_back(0xb7);              // 14 invokespecial <init>
    put2(code, sbInit);
    code.push_back(0x2a);              // 17 aload_0
    code.push_back(0xb6);              // 18 invokevirtual toString
    put2(code, sbToStr);
    code.push_back(0xb0);              // 21 areturn

    std::vector<AttrSpec> ma;
    ma.push_back(AttrSpec{codeIdx, jbc::test::codeContent(3, 1, code)});
    cb.addMethod(0x0009, cb.utf8("run"), cb.utf8("()Ljava/lang/String;"), std::move(ma));
    return cb.build();
}

}   // namespace

TEST(SmtBuildJvmProbe, FullChainRegeneratedClassesRunOnJvm)
{
    if (!jbc::test::jdkPresent()) {
        GTEST_SKIP() << "JDK not on PATH";
    }
    const fs::path genDir = fs::path("test") / "libjbc_smt_probe" / "regen";

    // 1) javac 编译探针样本（异常/循环/switch/构造器链）。
    {
        const fs::path srcDir = probeResource() / "demo";
        std::error_code ec;
        if (!fs::exists(srcDir, ec) || ec) {
            GTEST_SKIP() << "probe samples missing";
        }
        fs::remove_all(genDir, ec);
        fs::create_directories(genDir, ec);
        std::string files;
        for (fs::directory_iterator it(srcDir), end; it != end; ++it) {
            if (it->is_regular_file() && it->path().extension() == ".java") {
                files += " ";
                files += it->path().string();
            }
        }
        const std::string cmd = "javac --release 8 -encoding UTF-8 -d " + genDir.string()
            + files + " > nul 2>&1";
        ASSERT_EQ(std::system(cmd.c_str()), 0) << "javac probe compile failed";
    }

    StubResolver res;
    res.seedJdkParents();

    // 2) 逐类：剥原 SMT → Strict 帧 → maxes 覆盖 + lower 写回 → 重生成 →
    //    挂回 → 写出。
    ca::usize classes = 0;
    ca::usize generated = 0;
    std::error_code iterEc;
    for (fs::recursive_directory_iterator it(genDir, iterEc), end;
         it != end && !iterEc; it.increment(iterEc)) {
        if (!it->is_regular_file() || it->path().extension() != ".class") {
            continue;
        }
        const Bytes bytes = slurpFile(it->path());
        auto s = sessionOf(bytes);
        ASSERT_NE(s, nullptr) << it->path();
        RegenStats st;
        const std::string err = regenSmtInSession(*s, res, st);
        ASSERT_TRUE(err.empty()) << it->path() << ": " << err;
        generated += st.generated;
        auto w = writeClass(s->backing());
        ASSERT_TRUE(w.is_ok()) << it->path();
        spitBytes(it->path(), std::move(w).unwrap());
        ++classes;
    }
    EXPECT_GE(classes, 3u);
    EXPECT_GE(generated, 4u);   // 异常/循环/switch/构造器链的帧载体

    // 3) 手拼 UninitProbe：tag-8（Uninitialized）真实帧走同一条管线。
    {
        auto s = sessionOf(buildUninitProbe());
        ASSERT_NE(s, nullptr);
        RegenStats st;
        const std::string err = regenSmtInSession(*s, res, st);
        ASSERT_TRUE(err.empty()) << err;
        EXPECT_EQ(st.generated, 1u);
        auto w = writeClass(s->backing());
        ASSERT_TRUE(w.is_ok());
        spitBytes(genDir / "demo" / "UninitProbe.class", std::move(w).unwrap());
    }

    // 4) JVM 真跑：verifier（读我们生成的 SMT）+ 执行一次过。
    const fs::path runOut = fs::path("test") / "libjbc_smt_probe_run.txt";
    const std::string cmd = "java -cp " + genDir.string() + " demo.Main > "
        + runOut.string() + " 2>&1";
    ASSERT_EQ(std::system(cmd.c_str()), 0) << "java run failed";
    const Bytes raw = slurpFile(runOut);
    const std::string out(raw.begin(), raw.end());
#ifdef _WIN32
    constexpr std::string_view kEol = "\r\n";
#else
    constexpr std::string_view kEol = "\n";
#endif
    const std::string expect = std::string("exc=6 exc2=30") + std::string(kEol)
        + "tbl=10 50 -1" + std::string(kEol) + "lkp=2 -1" + std::string(kEol)
        + "ctor=4" + std::string(kEol) + "make=demo.ProbeCtor" + std::string(kEol)
        + "uninit=U" + std::string(kEol);
    EXPECT_EQ(out, expect) << "full-chain rewrite (SMT + maxes) changed behavior "
                              "(or verifier rejected)";
}
