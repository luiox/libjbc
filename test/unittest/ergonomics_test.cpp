/// @file        ergonomics_test.cpp
/// @brief       v0.4 人体工学补口测试（doc_ai/ergonomics.md）：
///        F1 hasCode/collectCodeUnits、F2 makeSessionFromBytes、F4 findMethod。
///
/// 全部用关内建的 ClassBuilder 字节构造（不依赖本机语料），畸形输入逐字节
/// 受控——这与 test_class_bytes.hpp 的测试哲学一致。
#include <gtest/gtest.h>

#include <memory>
#include <utility>
#include <vector>

#include "jbc/jbc.hpp"
#include "test_class_bytes.hpp"

using namespace jbc::core;
using jbc::ir::CodeUnit;
using jbc::ir::CodeUnitRef;
using jbc::ir::ClassSession;
using jbc::ir::PromoteError;
using jbc::test::AttrSpec;
using jbc::test::ClassBuilder;
using Bytes = std::vector<ca::u8>;

namespace {

/// 三个方法的类，覆盖 CodeVisit 的三种互斥状态：
///   [0] withCode   — 正常 Code（iconst_1; pop; return）
///   [1] abstractM  — ACC_ABSTRACT，无 Code 属性
///   [2] brokenCode — Code 属性存在但指令畸形（goto 跳进操作数中间）
Bytes makeThreeMethodClass()
{
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 codeIdx = cb.utf8("Code");
    const ca::u16 withCodeIdx = cb.utf8("withCode");
    const ca::u16 abstractIdx = cb.utf8("abstractM");
    const ca::u16 brokenIdx = cb.utf8("brokenCode");
    const ca::u16 voidIdx = cb.utf8("()V");

    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setAccess(0x0021);
    cb.setVersion(52);

    std::vector<AttrSpec> okAttrs;
    okAttrs.push_back(AttrSpec{codeIdx, jbc::test::codeContent(1, 0, {0x04, 0x57, 0xb1})});
    cb.addMethod(0x0009, withCodeIdx, voidIdx, std::move(okAttrs));

    // abstract 方法：只有 ACC_ABSTRACT，无任何属性。
    cb.addMethod(0x0400, abstractIdx, voidIdx);

    // 畸形 code：goto +1（0xa7 0x00 0x01）目标 pc=1 落在操作数中间，
    // importFrom 拒绝（BadTarget）→ promote 报 BadInstructions。
    // 结构本身合法（长度与声明一致），reader/lazy 解码都不拦——留给 promote。
    std::vector<AttrSpec> badAttrs;
    badAttrs.push_back(AttrSpec{codeIdx, jbc::test::codeContent(0, 0, {0xa7, 0x00, 0x01})});
    cb.addMethod(0x0009, brokenIdx, voidIdx, std::move(badAttrs));
    return cb.build();
}

std::shared_ptr<ClassSession> sessionOf(const Bytes& bytes)
{
    // 本文件的样板被 F2 结清：一步入口，不再 readClass→取 file→makeSession。
    auto r = jbc::ir::makeSessionFromBytes(ByteView{bytes.data(), bytes.size()});
    return r.is_ok() ? std::move(r).unwrap() : nullptr;
}

}   // namespace

// ===========================================================================
// F1：hasCode / collectCodeUnits——「无 Code」与「真失败」分流
// ===========================================================================

TEST(ErgonomicsF1Test, HasCodeDistinguishesNoCodeAndRange)
{
    auto s = sessionOf(makeThreeMethodClass());
    ASSERT_NE(s, nullptr);

    EXPECT_TRUE(jbc::ir::hasCode(*s, 0));    // withCode
    EXPECT_FALSE(jbc::ir::hasCode(*s, 1));   // abstractM：无 Code 属性
    EXPECT_TRUE(jbc::ir::hasCode(*s, 2));    // brokenCode：有 Code 属性（内容畸形是另一回事）

    // 越界：探测式 false，与 CodeDom::peek 返回 nullptr 同款纪律。
    EXPECT_FALSE(jbc::ir::hasCode(*s, 3));
    EXPECT_FALSE(jbc::ir::hasCode(*s, 1000));
}

TEST(ErgonomicsF1Test, CollectCodeUnitsSeparatesNoCodeFromFailure)
{
    auto s = sessionOf(makeThreeMethodClass());
    ASSERT_NE(s, nullptr);

    std::vector<jbc::ir::CodeVisit> visits = jbc::ir::collectCodeUnits(*s);

    ASSERT_EQ(visits.size(), 3u);

    // [0] 正常方法：Ok，指令数与源字节一致。
    EXPECT_EQ(visits[0].kind, jbc::ir::CodeVisit::Kind::Ok);
    EXPECT_EQ(visits[0].methodIdx, 0u);
    ASSERT_TRUE(visits[0].unit.get() != nullptr);
    EXPECT_EQ(visits[0].unit->dom.size(), 3u);
    EXPECT_EQ(visits[0].error, PromoteError::Ok);

    // [1] abstract：NoCode——与 Failed 不同的通道。
    EXPECT_EQ(visits[1].kind, jbc::ir::CodeVisit::Kind::NoCode);
    EXPECT_EQ(visits[1].methodIdx, 1u);
    EXPECT_EQ(visits[1].unit.get(), nullptr);

    // [2] 畸形指令：Failed（真失败），错误码明确——这就是旧写法
    //     `if (p.is_err()) continue;` 静默吞掉的那类。
    EXPECT_EQ(visits[2].kind, jbc::ir::CodeVisit::Kind::Failed);
    EXPECT_EQ(visits[2].methodIdx, 2u);
    EXPECT_EQ(visits[2].error, PromoteError::BadInstructions);
    EXPECT_EQ(visits[2].unit.get(), nullptr);
}

TEST(ErgonomicsF1Test, CollectCodeUnitsReusesPromotedUnit)
{
    auto s = sessionOf(makeThreeMethodClass());
    ASSERT_NE(s, nullptr);

    // 先 promote 方法 0 并持有句柄，再收集：已 promote 的方法必须复用
    // 现有 CodeUnit（重建会使既有句柄失效——promoteMethod 幂等语义在
    // 收集场景是陷阱，collectCodeUnits 刻意避开）。
    auto kept = jbc::ir::promoteMethod(*s, 0);
    ASSERT_TRUE(kept.is_ok());
    CodeUnit* raw = kept.unwrap().get();

    CodeUnitRef seenAgain;
    for (const auto& v : jbc::ir::collectCodeUnits(*s)) {
        if (v.methodIdx == 0) {
            seenAgain = v.unit;
        }
    }
    ASSERT_TRUE(seenAgain.get() != nullptr);
    EXPECT_EQ(seenAgain.get(), raw);   // 同一个对象，未重建
    EXPECT_EQ(kept.unwrap()->dom.size(), 3u);   // 原句柄仍有效可解引用
}

TEST(ErgonomicsF1Test, CollectCodeUnitsOnAbstractOnlyClassAllNoCode)
{
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/A");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 aIdx = cb.utf8("a");
    const ca::u16 bIdx = cb.utf8("b");
    const ca::u16 voidIdx = cb.utf8("()V");
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setAccess(0x0021);
    cb.setVersion(52);
    cb.addMethod(0x0400, aIdx, voidIdx);
    cb.addMethod(0x0401, bIdx, voidIdx);   // abstract | native：同样无 Code

    auto s = sessionOf(cb.build());
    ASSERT_NE(s, nullptr);

    ca::usize noCode = 0;
    for (const auto& v : jbc::ir::collectCodeUnits(*s)) {
        EXPECT_EQ(v.kind, jbc::ir::CodeVisit::Kind::NoCode);
        ++noCode;
    }
    EXPECT_EQ(noCode, 2u);
}

// ===========================================================================
// D6：collectCodeUnits 按族过滤——CodeCollectFilter 两维（指令族/属性族）
// ===========================================================================

namespace {

/// 七个方法的类，覆盖过滤器全部分面：
///   [0] invokeM   — invokestatic（Invoke 族）
///   [1] branchM   — goto（Branch 族）
///   [2] fieldM    — getstatic（FieldAccess 族）
///   [3] plainM    — nop; return（不属任何族）
///   [4] smtM      — nop; return + 嵌套 StackMapTable（attrKinds 维度命中）
///   [5] abstractM — 无 Code（NoCode 分流，受 includeNoCode 管）
///   [6] brokenM   — Code 存在但指令畸形（Failed 分流，恒保留）
Bytes makeFamilyClass()
{
    ClassBuilder cb;
    const ca::u16 thisIdx  = cb.utf8("p/F");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 codeIdx  = cb.utf8("Code");
    const ca::u16 smtIdx   = cb.utf8("StackMapTable");
    const ca::u16 sysIdx   = cb.utf8("java/lang/System");
    const ca::u16 outIdx   = cb.utf8("out");
    const ca::u16 psIdx    = cb.utf8("Ljava/io/PrintStream;");
    const ca::u16 runIdx   = cb.utf8("run");
    const ca::u16 voidIdx  = cb.utf8("()V");
    const ca::u16 selfCls  = cb.classRef(thisIdx);
    const ca::u16 selfNat  = cb.nameAndType(runIdx, voidIdx);
    const ca::u16 sysCls   = cb.classRef(sysIdx);
    const ca::u16 fieldNat = cb.nameAndType(outIdx, psIdx);
    const ca::u16 mrefIdx  = cb.methodref(selfCls, selfNat);
    const ca::u16 frefIdx  = cb.fieldref(sysCls, fieldNat);

    cb.setThis(selfCls);
    cb.setSuper(cb.classRef(superIdx));
    cb.setAccess(0x0021);
    cb.setVersion(52);

    auto operand2 = [](ca::u16 idx) {
        return std::pair<ca::u8, ca::u8>{static_cast<ca::u8>(idx >> 8),
                                         static_cast<ca::u8>(idx & 0xff)};
    };

    const auto [mhi, mlo] = operand2(mrefIdx);
    std::vector<AttrSpec> invokeAttrs;
    invokeAttrs.push_back(
        AttrSpec{codeIdx, jbc::test::codeContent(0, 0, {0xb8, mhi, mlo, 0x57, 0xb1})});
    cb.addMethod(0x0009, runIdx, voidIdx, std::move(invokeAttrs));

    std::vector<AttrSpec> branchAttrs;
    // goto +0：目标 = 下一条指令（return），结构合法。
    branchAttrs.push_back(AttrSpec{codeIdx, jbc::test::codeContent(0, 0, {0xa7, 0x00, 0x00, 0xb1})});
    cb.addMethod(0x0009, cb.utf8("branchM"), voidIdx, std::move(branchAttrs));

    const auto [fhi, flo] = operand2(frefIdx);
    std::vector<AttrSpec> fieldAttrs;
    fieldAttrs.push_back(
        AttrSpec{codeIdx, jbc::test::codeContent(0, 0, {0xb2, fhi, flo, 0x57, 0xb1})});
    cb.addMethod(0x0009, cb.utf8("fieldM"), voidIdx, std::move(fieldAttrs));

    std::vector<AttrSpec> plainAttrs;
    plainAttrs.push_back(AttrSpec{codeIdx, jbc::test::codeContent(0, 0, {0x00, 0xb1})});
    cb.addMethod(0x0009, cb.utf8("plainM"), voidIdx, std::move(plainAttrs));

    // 嵌套 StackMapTable：0 条目（最小合法体）。
    std::vector<AttrSpec> smtNested;
    smtNested.push_back(AttrSpec{smtIdx, Bytes{0x00, 0x00}});
    std::vector<AttrSpec> smtAttrs;
    smtAttrs.push_back(AttrSpec{codeIdx, jbc::test::codeContent(0, 0, {0x00, 0xb1}, {}, smtNested)});
    cb.addMethod(0x0009, cb.utf8("smtM"), voidIdx, std::move(smtAttrs));

    cb.addMethod(0x0400, cb.utf8("abstractM"), voidIdx);

    std::vector<AttrSpec> badAttrs;
    badAttrs.push_back(AttrSpec{codeIdx, jbc::test::codeContent(0, 0, {0xa7, 0x00, 0x01})});
    cb.addMethod(0x0009, cb.utf8("brokenM"), voidIdx, std::move(badAttrs));
    return cb.build();
}

/// 按过滤器收集命中的 methodIdx 列表（保序）。
std::vector<ca::usize> idxOf(std::shared_ptr<ClassSession>& s,
                             const jbc::ir::CodeCollectFilter& f)
{
    std::vector<ca::usize> out;
    for (const auto& v : jbc::ir::collectCodeUnits(*s, f)) {
        out.push_back(v.methodIdx);
    }
    return out;
}

}   // namespace

TEST(ErgonomicsD6Test, DefaultFilterMatchesNoFilter)
{
    auto s = sessionOf(makeFamilyClass());
    ASSERT_NE(s, nullptr);

    const std::vector<jbc::ir::CodeVisit> full = jbc::ir::collectCodeUnits(*s);
    const std::vector<jbc::ir::CodeVisit> def =
        jbc::ir::collectCodeUnits(*s, jbc::ir::CodeCollectFilter{});
    ASSERT_EQ(full.size(), 7u);
    ASSERT_EQ(full.size(), def.size());
    for (ca::usize i = 0; i < full.size(); ++i) {
        EXPECT_EQ(full[i].kind, def[i].kind);
        EXPECT_EQ(full[i].methodIdx, def[i].methodIdx);
        EXPECT_EQ(full[i].unit.get(), def[i].unit.get());
    }
}

TEST(ErgonomicsD6Test, InsnFamilyBitClassifiesAndExcludes)
{
    using jbc::ir::insnFamilyBit;
    EXPECT_NE(insnFamilyBit(Op::INVOKESTATIC), 0u);
    EXPECT_NE(insnFamilyBit(Op::GOTO), 0u);
    EXPECT_NE(insnFamilyBit(Op::GETFIELD), 0u);
    EXPECT_NE(insnFamilyBit(Op::LDC), 0u);
    // 常规数据搬运不属任何族（族 = 调用方关心的「重」指令）。
    EXPECT_EQ(insnFamilyBit(Op::NOP), 0u);
    EXPECT_EQ(insnFamilyBit(Op::RETURN), 0u);
    EXPECT_EQ(insnFamilyBit(Op::ILOAD), 0u);
    // 四族独立位，互不重叠。
    EXPECT_EQ(insnFamilyBit(Op::INVOKESTATIC) & insnFamilyBit(Op::GOTO), 0u);
    EXPECT_EQ(insnFamilyBit(Op::GETFIELD) & insnFamilyBit(Op::LDC), 0u);
}

TEST(ErgonomicsD6Test, InsnFamilyFilterSelectsMatchingMethods)
{
    auto s = sessionOf(makeFamilyClass());
    ASSERT_NE(s, nullptr);
    using jbc::ir::CodeCollectFilter;
    using jbc::ir::InsnFamily;
    const auto bit = [](InsnFamily f) {
        return 1u << static_cast<ca::u32>(f);
    };

    // 命中方法 + NoCode/Failed 分流（后者不受指令族过滤）。
    CodeCollectFilter f;
    f.insnFamilies = bit(InsnFamily::Invoke);
    EXPECT_EQ(idxOf(s, f), (std::vector<ca::usize>{0, 5, 6}));

    f.insnFamilies = bit(InsnFamily::Branch);
    EXPECT_EQ(idxOf(s, f), (std::vector<ca::usize>{1, 5, 6}));

    f.insnFamilies = bit(InsnFamily::FieldAccess);
    EXPECT_EQ(idxOf(s, f), (std::vector<ca::usize>{2, 5, 6}));

    // 无 LDC/NEW：CpConst 无命中——只剩分流记录。
    f.insnFamilies = bit(InsnFamily::CpConst);
    EXPECT_EQ(idxOf(s, f), (std::vector<ca::usize>{5, 6}));

    // 掩码可并：Invoke | Branch。
    f.insnFamilies = bit(InsnFamily::Invoke) | bit(InsnFamily::Branch);
    EXPECT_EQ(idxOf(s, f), (std::vector<ca::usize>{0, 1, 5, 6}));

    // 命中的 Ok 记录句柄可用：指令数与源一致。
    f.insnFamilies = bit(InsnFamily::Invoke);
    const auto visits = jbc::ir::collectCodeUnits(*s, f);
    ASSERT_EQ(visits.size(), 3u);
    ASSERT_TRUE(visits[0].unit.get() != nullptr);
    EXPECT_EQ(visits[0].unit->dom.size(), 3u);
}

TEST(ErgonomicsD6Test, AttrKindFilterSelectsByNestedAttr)
{
    auto s = sessionOf(makeFamilyClass());
    ASSERT_NE(s, nullptr);
    using jbc::ir::CodeCollectFilter;
    const auto smtBit = 1u << static_cast<ca::u32>(AttrKind::StackMapTable);
    const auto rawBit = 1u << static_cast<ca::u32>(AttrKind::Raw);

    CodeCollectFilter f;
    f.attrKinds = smtBit;
    EXPECT_EQ(idxOf(s, f), (std::vector<ca::usize>{4, 5, 6}));

    // 嵌套属性全是 SMT：Raw 不命中。
    f.attrKinds = rawBit;
    EXPECT_EQ(idxOf(s, f), (std::vector<ca::usize>{5, 6}));
}

TEST(ErgonomicsD6Test, TwoDimensionsIntersect)
{
    auto s = sessionOf(makeFamilyClass());
    ASSERT_NE(s, nullptr);
    using jbc::ir::CodeCollectFilter;
    using jbc::ir::InsnFamily;
    const auto smtBit = 1u << static_cast<ca::u32>(AttrKind::StackMapTable);

    // Branch 指令与嵌套 SMT 无一方法兼有：两维同时给 → 空集（分流除外）。
    CodeCollectFilter f;
    f.insnFamilies = 1u << static_cast<ca::u32>(InsnFamily::Branch);
    f.attrKinds    = smtBit;
    EXPECT_EQ(idxOf(s, f), (std::vector<ca::usize>{5, 6}));
}

TEST(ErgonomicsD6Test, IncludeNoCodeFalseKeepsFailed)
{
    auto s = sessionOf(makeFamilyClass());
    ASSERT_NE(s, nullptr);
    using jbc::ir::CodeCollectFilter;
    using jbc::ir::CodeVisit;
    using jbc::ir::InsnFamily;

    CodeCollectFilter f;
    f.includeNoCode = false;
    f.insnFamilies  = 1u << static_cast<ca::u32>(InsnFamily::Invoke);
    const auto visits = jbc::ir::collectCodeUnits(*s, f);
    ASSERT_EQ(visits.size(), 2u);
    EXPECT_EQ(visits[0].methodIdx, 0u);
    EXPECT_EQ(visits[0].kind, CodeVisit::Kind::Ok);
    // Failed 恒保留——过滤不复活 F1 要堵的「静默吞失败」。
    EXPECT_EQ(visits[1].methodIdx, 6u);
    EXPECT_EQ(visits[1].kind, CodeVisit::Kind::Failed);
    EXPECT_EQ(visits[1].error, PromoteError::BadInstructions);
}

// ===========================================================================
// F2：makeSessionFromBytes——一步从字节建会话
// ===========================================================================

TEST(ErgonomicsF2Test, OneStepBuildsSameSessionAsThreeStep)
{
    const Bytes cls = makeThreeMethodClass();
    auto r = jbc::ir::makeSessionFromBytes(ByteView{cls.data(), cls.size()});
    ASSERT_TRUE(r.is_ok());
    auto s = std::move(r).unwrap();

    // 骨架物化与三步版一致。
    EXPECT_EQ(s->unit().symbols().str(s->unit().thisClass), "p/T");
    ASSERT_EQ(s->unit().methods.size(), 3u);
    EXPECT_EQ(s->unit().symbols().str(s->unit().methods[0].name), "withCode");
    EXPECT_EQ(s->unit().symbols().str(s->unit().methods[0].desc), "()V");
    EXPECT_TRUE(s->backing().hasSource());   // 默认保留源字节（copy-through 前提）

    // 产物可用性一致：promote / 遍历照常。
    auto p = jbc::ir::promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    EXPECT_EQ(p.unwrap()->dom.size(), 3u);
}

TEST(ErgonomicsF2Test, ReportsReadStageErrorWithCode)
{
    // 坏魔数：错误落在 Read 段，具体码保留（不被跨族映射吞掉）。
    const Bytes badMagic = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    auto r = jbc::ir::makeSessionFromBytes(ByteView{badMagic.data(), badMagic.size()});
    ASSERT_TRUE(r.is_err());
    const jbc::ir::SessionError e = r.unwrap_err();
    EXPECT_EQ(e.stage, jbc::ir::SessionError::Stage::Read);
    EXPECT_EQ(e.read, ReadError::BadMagic);
    EXPECT_EQ(e.session, PromoteError::Ok);

    // 空输入：连魔数都读不完 → Truncated。
    auto r2 = jbc::ir::makeSessionFromBytes(ByteView{});
    ASSERT_TRUE(r2.is_err());
    EXPECT_EQ(r2.unwrap_err().read, ReadError::Truncated);
}

TEST(ErgonomicsF2Test, HonorsReadOptions)
{
    const Bytes cls = makeThreeMethodClass();
    ReadOptions opt;
    opt.preserveSourceBytes = false;   // 扫描形态：省一份字节拷贝
    auto r = jbc::ir::makeSessionFromBytes(ByteView{cls.data(), cls.size()}, opt);
    ASSERT_TRUE(r.is_ok());
    auto s = std::move(r).unwrap();
    EXPECT_FALSE(s->backing().hasSource());
    // 骨架仍完整：成员名/描述符已物化进符号池。
    EXPECT_EQ(s->unit().symbols().str(s->unit().methods[2].name), "brokenCode");
}

// ===========================================================================
// F4：ClassUnit::findMethod——按名字/描述符定位方法
// ===========================================================================

namespace {

/// 重载类：方法 [0] m()V、[1] m(I)V、[2] n()V，另有与 m 同名的字段 int m;
/// （字段在独立容器里，不干扰方法查找）。
Bytes makeOverloadClass()
{
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/O");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 codeIdx = cb.utf8("Code");
    const ca::u16 mIdx = cb.utf8("m");
    const ca::u16 nIdx = cb.utf8("n");
    const ca::u16 voidIdx = cb.utf8("()V");
    const ca::u16 intVoidIdx = cb.utf8("(I)V");
    const ca::u16 iIdx = cb.utf8("I");

    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setAccess(0x0021);
    cb.setVersion(52);
    cb.addField(0x0002, mIdx, iIdx);

    std::vector<AttrSpec> attrs;
    attrs.push_back(AttrSpec{codeIdx, jbc::test::codeContent(0, 0, {0xb1})});
    cb.addMethod(0x0009, mIdx, voidIdx, attrs);
    cb.addMethod(0x0009, mIdx, intVoidIdx, attrs);
    cb.addMethod(0x0009, nIdx, voidIdx, attrs);
    return cb.build();
}

}   // namespace

TEST(ErgonomicsF4Test, FindMethodByNameAndDescriptor)
{
    auto s = sessionOf(makeOverloadClass());
    ASSERT_NE(s, nullptr);
    const jbc::ir::ClassUnit& u = s->unit();

    // 按名字：命中下标最小者（重载不遮蔽）。
    EXPECT_EQ(u.findMethod("m"), 0u);
    // 名字 + 描述符：分辨重载。
    EXPECT_EQ(u.findMethod("m", "()V"), 0u);
    EXPECT_EQ(u.findMethod("m", "(I)V"), 1u);
    EXPECT_EQ(u.findMethod("n", "()V"), 2u);
    // 未命中：命名哨兵 kNoMethod（替代裸魔法值，同 kNoInsn 惯例）。
    EXPECT_EQ(u.findMethod("nope"), jbc::ir::kNoMethod);
    EXPECT_EQ(u.findMethod("m", "()Z"), jbc::ir::kNoMethod);   // 名字命中、描述符不匹配
    EXPECT_EQ(u.findMethod(""), jbc::ir::kNoMethod);
}

TEST(ErgonomicsF4Test, FindMethodIndexFeedsPromoteAndLower)
{
    auto s = sessionOf(makeOverloadClass());
    ASSERT_NE(s, nullptr);
    const jbc::ir::ClassUnit& u = s->unit();

    // 找到的下标直接喂 promote/lower——这是本 API 的目标用法。
    const ca::usize overload = u.findMethod("m", "(I)V");
    ASSERT_NE(overload, jbc::ir::kNoMethod);
    auto p = jbc::ir::promoteMethod(*s, overload);
    ASSERT_TRUE(p.is_ok());
    EXPECT_EQ(p.unwrap()->dom.size(), 1u);   // 单条 return
    EXPECT_TRUE(jbc::ir::lowerMethod(*s, overload).is_ok());   // 无变化 → copy-through
}

TEST(ErgonomicsF4Test, MethodIdSurvivesIrReorderAndCodeAttrReorder)
{
    auto s = sessionOf(makeOverloadClass());
    ASSERT_NE(s, nullptr);
    const jbc::ir::MethodId id = s->unit().findMethodId("m", "(I)V");
    ASSERT_NE(id, jbc::ir::kNoMethodId);

    auto p = jbc::ir::promoteMethod(*s, id);
    ASSERT_TRUE(p.is_ok());

    // 逻辑方法顺序变化不应改变 stable id，也不应把 lower 指到另一条物理方法。
    std::swap(s->unit().methods[0], s->unit().methods[1]);

    // Code 槽的位置也不是身份：在它前面插入一个新属性，lower 必须重新查找。
    const ca::u16 extraName = static_cast<ca::u16>(s->backing().cp.addUtf8("Extra"));
    auto& attrs = s->backing().methods[1].attrs;
    attrs.insert(attrs.begin(),
                 AttrSlot::makeNew(extraName, std::make_unique<RawAttr>(Bytes{0xde, 0xad})));

    jbc::ir::CodeEditor ed(*p.unwrap());
    ASSERT_TRUE(ed.insertAtStart(jbc::ir::CodeEditor::simple(Op::NOP)).is_ok());
    ASSERT_TRUE(jbc::ir::lowerMethod(*s, id).is_ok());

    auto out = jbc::test::writeOk(s->backing());
    auto s2 = sessionOf(out);
    ASSERT_NE(s2, nullptr);
    const jbc::ir::MethodId id2 = s2->unit().findMethodId("m", "(I)V");
    auto p2 = jbc::ir::promoteMethod(*s2, id2);
    ASSERT_TRUE(p2.is_ok());
    EXPECT_EQ(p2.unwrap()->dom.size(), 2u);
}

TEST(ErgonomicsF4Test, MaxesHandEditFeedsLowerWriteBack)
{
    // A2 契约（手动链路）：手改 CodeUnit 的 maxes 后，lower 的脏检查分支把
    // 新值写回 Code 属性（自动采用帧引擎重算值是另一条待接线链路）。
    auto s = sessionOf(makeOverloadClass());
    ASSERT_NE(s, nullptr);
    const jbc::ir::MethodId id = s->unit().findMethodId("m", "(I)V");
    ASSERT_NE(id, jbc::ir::kNoMethodId);
    auto p = jbc::ir::promoteMethod(*s, id);
    ASSERT_TRUE(p.is_ok());

    auto& cu = *p.unwrap();
    jbc::ir::CodeEditor ed(cu);
    ASSERT_TRUE(ed.insertAtStart(jbc::ir::CodeEditor::simple(Op::NOP)).is_ok());
    cu.maxStack  = 9;
    cu.maxLocals = 7;
    ASSERT_TRUE(jbc::ir::lowerMethod(*s, id).is_ok());

    auto bytes = jbc::test::writeOk(s->backing());
    auto s2 = sessionOf(bytes);
    ASSERT_NE(s2, nullptr);
    const jbc::ir::MethodId id2 = s2->unit().findMethodId("m", "(I)V");
    ASSERT_NE(id2, jbc::ir::kNoMethodId);
    auto p2 = jbc::ir::promoteMethod(*s2, id2);
    ASSERT_TRUE(p2.is_ok());
    EXPECT_EQ(p2.unwrap()->maxStack, 9u);
    EXPECT_EQ(p2.unwrap()->maxLocals, 7u);
}

TEST(ErgonomicsF4Test, MethodMetadataCommitUsesStableMethodId)
{
    auto s = sessionOf(makeOverloadClass());
    ASSERT_NE(s, nullptr);

    const jbc::ir::MethodId id = s->unit().findMethodId("m", "(I)V");
    ASSERT_NE(id, jbc::ir::kNoMethodId);
    auto method = s->unit().method(id);
    ASSERT_NE(method, nullptr);

    // Mutate the high-level metadata, then reorder both views.  The commit must
    // still update the member selected by MethodId rather than a physical index.
    method->name = s->unit().symbols().intern("renamed");
    method->desc = s->unit().symbols().intern("(J)V");
    method->access = 0x0001u;
    std::swap(s->unit().methods[0], s->unit().methods[1]);

    ASSERT_TRUE(s->commitMethodMetadata(id).is_ok());
    auto bytes = jbc::test::writeOk(s->backing());
    auto reread = sessionOf(bytes);
    ASSERT_NE(reread, nullptr);
    EXPECT_NE(reread->unit().findMethod("renamed", "(J)V"), jbc::ir::kNoMethod);
    EXPECT_EQ(reread->unit().findMethod("m", "(I)V"), jbc::ir::kNoMethod);
}
