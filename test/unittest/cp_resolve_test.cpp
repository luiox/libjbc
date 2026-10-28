/// @file        cp_resolve_test.cpp
/// @brief       CP 读侧语义解析原语：成员引用一跳 / MethodHandle 三段 / 类名。
///
/// 两条构造路径各司其职：正例走 ClassBuilder 字节（readClass 内含
/// validateReferences，证明与真实解析产物兼容）；坏链走 ConstantPool 直构
/// （addXxx 家族不校验，恰好提供构造悬垂/坏 Utf8 链的自由度——这类输入
/// 根本过不了 readClass，却是池级原语必须安全表达失败的对象）。
#include <gtest/gtest.h>

#include <string_view>

#include "jbc/jbc.hpp"
#include "test_class_bytes.hpp"

using namespace jbc::core;

// ---- resolveMemberRef ----

/// 三族 tag 各一例（字节路径）：三段视图正确，out.tag 记原形。
TEST(CpResolveTest, MemberRefResolvesAllThreeTagFamilies)
{
    jbc::test::ClassBuilder cb;
    const ca::u16 thisIdx  = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 cls      = cb.classRef(thisIdx);
    const ca::u16 fNat     = cb.nameAndType(cb.utf8("field"), cb.utf8("I"));
    const ca::u16 mNat     = cb.nameAndType(cb.utf8("run"), cb.utf8("()V"));
    const ca::u16 iNat     = cb.nameAndType(cb.utf8("ifaceRun"), cb.utf8("()V"));
    const ca::u16 fref     = cb.fieldref(cls, fNat);
    const ca::u16 mref     = cb.methodref(cls, mNat);
    const ca::u16 iref     = cb.interfaceMethodref(cls, iNat);
    cb.setThis(cls);
    cb.setSuper(cb.classRef(superIdx));

    auto cf = jbc::test::readOk(cb.build());
    ASSERT_NE(cf, nullptr);

    struct Case
    {
        ca::u16 idx;
        const char* name;
        const char* desc;
        CpTag tag;
    };
    const Case cases[] = {
        {fref, "field", "I", CpTag::Fieldref},
        {mref, "run", "()V", CpTag::Methodref},
        {iref, "ifaceRun", "()V", CpTag::InterfaceMethodref},
    };
    for (const Case& c : cases) {
        MemberRefSem sem;
        ASSERT_TRUE(resolveMemberRef(cf->cp, c.idx, sem)) << "idx " << c.idx;
        EXPECT_EQ(sem.owner, "p/T");
        EXPECT_EQ(sem.name, c.name);
        EXPECT_EQ(sem.desc, c.desc);
        EXPECT_EQ(sem.tag, c.tag);
    }
}

/// 拒绝族（直构池）：cpIdx 非 ref 三族、悬垂索引、链上任一跳 tag 不符或缺
/// Utf8（含缺 name / 缺 desc 两个方向）；失败不触碰 out。
TEST(CpResolveTest, MemberRefRejectsBrokenChains)
{
    ConstantPool cp;
    const ca::u32 nameIdx = cp.addUtf8("f");
    const ca::u32 descIdx = cp.addUtf8("I");
    const ca::u32 intIdx  = cp.addInteger(5);
    const ca::u32 clsIdx  = cp.addClass(cp.addUtf8("p/T"));
    const ca::u32 natIdx  = cp.addNameAndType(nameIdx, descIdx);
    const ca::u32 good    = cp.addRef(CpTag::Methodref, clsIdx, natIdx);

    MemberRefSem sem;
    ASSERT_TRUE(resolveMemberRef(cp, good, sem));
    EXPECT_EQ(sem.owner, "p/T");
    EXPECT_EQ(sem.name, "f");
    EXPECT_EQ(sem.desc, "I");
    EXPECT_EQ(sem.tag, CpTag::Methodref);

    // cpIdx 自身不是引用三族。
    EXPECT_FALSE(resolveMemberRef(cp, natIdx, sem));
    EXPECT_FALSE(resolveMemberRef(cp, nameIdx, sem));
    EXPECT_FALSE(resolveMemberRef(cp, intIdx, sem));

    // class_index 侧：越界、0 号槽、指向非 Class、Class.a 非 Utf8。
    EXPECT_FALSE(resolveMemberRef(cp, cp.addRef(CpTag::Fieldref, 9999, natIdx), sem));
    EXPECT_FALSE(resolveMemberRef(cp, cp.addRef(CpTag::Fieldref, 0, natIdx), sem));
    EXPECT_FALSE(resolveMemberRef(cp, cp.addRef(CpTag::Fieldref, natIdx, natIdx), sem));
    EXPECT_FALSE(resolveMemberRef(cp, cp.addRef(CpTag::Fieldref, cp.addClass(intIdx), natIdx), sem));

    // NameAndType 侧：越界、缺 name（a 非 Utf8）、缺 desc（b 非 Utf8）。
    EXPECT_FALSE(resolveMemberRef(cp, cp.addRef(CpTag::Fieldref, clsIdx, 8888), sem));
    EXPECT_FALSE(resolveMemberRef(cp, cp.addRef(CpTag::Fieldref, clsIdx, cp.addNameAndType(intIdx, descIdx)), sem));
    EXPECT_FALSE(resolveMemberRef(cp, cp.addRef(CpTag::Fieldref, clsIdx, cp.addNameAndType(nameIdx, intIdx)), sem));

    // 索引面：0 号槽 / 越界。
    EXPECT_FALSE(resolveMemberRef(cp, 0, sem));
    EXPECT_FALSE(resolveMemberRef(cp, 9999, sem));

    // 失败不触碰 out：哨兵四字段原样。
    MemberRefSem sent;
    sent.tag  = CpTag::InterfaceMethodref;
    sent.name = "keep";
    (void)resolveMemberRef(cp, natIdx, sent);
    EXPECT_EQ(sent.tag, CpTag::InterfaceMethodref);
    EXPECT_EQ(sent.name, "keep");
    EXPECT_TRUE(sent.owner.empty());
    EXPECT_TRUE(sent.desc.empty());
}

// ---- resolveHandleCp ----

/// 全部九种 reference_kind（字节路径）：三段 + kind + isInterface 逐项正确；
/// kind 6/7 配 InterfaceMethodref 是 JVMS v52+ 合法交叉形态（javac 产出），
/// isInterface 按引用条目实际 tag 置位。
TEST(CpResolveTest, HandleResolvesAllNineKinds)
{
    jbc::test::ClassBuilder cb;
    const ca::u16 thisIdx  = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 cls      = cb.classRef(thisIdx);
    const ca::u16 fNat     = cb.nameAndType(cb.utf8("field"), cb.utf8("I"));
    const ca::u16 mNat     = cb.nameAndType(cb.utf8("run"), cb.utf8("()V"));
    const ca::u16 ctorNat  = cb.nameAndType(cb.utf8("<init>"), cb.utf8("(Ljava/lang/String;)V"));
    const ca::u16 iNat     = cb.nameAndType(cb.utf8("ifaceRun"), cb.utf8("()V"));
    const ca::u16 fref     = cb.fieldref(cls, fNat);
    const ca::u16 mref     = cb.methodref(cls, mNat);
    const ca::u16 ctorRef  = cb.methodref(cls, ctorNat);
    const ca::u16 iref     = cb.interfaceMethodref(cls, iNat);
    cb.setThis(cls);
    cb.setSuper(cb.classRef(superIdx));

    struct Case
    {
        ca::u16 mh;
        ca::u32 kind;
        const char* name;
        const char* desc;
        bool iface;
    };
    // 逐一装配（C++17 无指定初始化器）：handle 索引在初始化内分配。
    const Case cases[] = {
        {cb.methodHandle(1, fref), 1, "field", "I", false},
        {cb.methodHandle(2, fref), 2, "field", "I", false},
        {cb.methodHandle(3, fref), 3, "field", "I", false},
        {cb.methodHandle(4, fref), 4, "field", "I", false},
        {cb.methodHandle(5, mref), 5, "run", "()V", false},
        {cb.methodHandle(6, mref), 6, "run", "()V", false},
        {cb.methodHandle(7, mref), 7, "run", "()V", false},
        {cb.methodHandle(8, ctorRef), 8, "<init>", "(Ljava/lang/String;)V", false},
        {cb.methodHandle(9, iref), 9, "ifaceRun", "()V", true},
        {cb.methodHandle(6, iref), 6, "ifaceRun", "()V", true},
        {cb.methodHandle(7, iref), 7, "ifaceRun", "()V", true},
    };

    auto cf = jbc::test::readOk(cb.build());
    ASSERT_NE(cf, nullptr);

    for (const Case& c : cases) {
        HandleSem sem;
        ASSERT_TRUE(resolveHandleCp(cf->cp, c.mh, sem)) << "kind " << c.kind;
        EXPECT_EQ(sem.owner, "p/T");
        EXPECT_EQ(sem.name, c.name);
        EXPECT_EQ(sem.desc, c.desc);
        EXPECT_EQ(sem.kind, c.kind);
        EXPECT_EQ(sem.isInterface, c.iface) << "kind " << c.kind;
    }
}

/// 拒绝族（直构池）：kind=0/10 域外、悬垂 ref 索引、ref 非 ref 三族（NaT/Utf8）、
/// kind-tag 不匹配（字段类 kind 配方法 ref、9 配 Methodref、6 配 Fieldref）、
/// cpIdx 非 MethodHandle / 0 号槽 / 越界、链上 Class.a 非 Utf8。
TEST(CpResolveTest, HandleRejectsInvalidShape)
{
    ConstantPool cp;
    const ca::u32 nameIdx = cp.addUtf8("run");
    const ca::u32 descIdx = cp.addUtf8("()V");
    const ca::u32 clsIdx  = cp.addClass(cp.addUtf8("p/T"));
    const ca::u32 natIdx  = cp.addNameAndType(nameIdx, descIdx);
    const ca::u32 fref    = cp.addRef(CpTag::Fieldref, clsIdx, natIdx);
    const ca::u32 mref    = cp.addRef(CpTag::Methodref, clsIdx, natIdx);

    HandleSem sem;

    // kind 域外：0 与 10（严格口径，不做「非字段一律按方法」兜底）。
    EXPECT_FALSE(resolveHandleCp(cp, cp.addMethodHandle(0, mref), sem));
    EXPECT_FALSE(resolveHandleCp(cp, cp.addMethodHandle(10, mref), sem));

    // 悬垂 ref 索引：0 号槽与越界。
    EXPECT_FALSE(resolveHandleCp(cp, cp.addMethodHandle(6, 0), sem));
    EXPECT_FALSE(resolveHandleCp(cp, cp.addMethodHandle(6, 9999), sem));

    // ref 不在引用三族：NaT 与 Utf8。
    EXPECT_FALSE(resolveHandleCp(cp, cp.addMethodHandle(6, natIdx), sem));
    EXPECT_FALSE(resolveHandleCp(cp, cp.addMethodHandle(6, nameIdx), sem));

    // kind-tag 不匹配。
    EXPECT_FALSE(resolveHandleCp(cp, cp.addMethodHandle(1, mref), sem));
    EXPECT_FALSE(resolveHandleCp(cp, cp.addMethodHandle(6, fref), sem));
    EXPECT_FALSE(resolveHandleCp(cp, cp.addMethodHandle(9, mref), sem));

    // cpIdx 自身不是 MethodHandle；0 号槽 / 越界。
    EXPECT_FALSE(resolveHandleCp(cp, natIdx, sem));
    EXPECT_FALSE(resolveHandleCp(cp, nameIdx, sem));
    EXPECT_FALSE(resolveHandleCp(cp, 0, sem));
    EXPECT_FALSE(resolveHandleCp(cp, 9999, sem));

    // 失败不触碰 out：kind 域外时不得残留半截语义。
    sem.kind        = 42;
    sem.isInterface = true;
    (void)resolveHandleCp(cp, cp.addMethodHandle(0, mref), sem);
    EXPECT_EQ(sem.kind, 42u);
    EXPECT_TRUE(sem.isInterface);

    // 链上坏 Utf8：Class.a 指向 Integer——handle 与成员引用一跳同拒。
    ConstantPool cp2;
    const ca::u32 intIdx  = cp2.addInteger(7);
    const ca::u32 badCls  = cp2.addClass(intIdx);
    const ca::u32 nat2    = cp2.addNameAndType(cp2.addUtf8("run"), cp2.addUtf8("()V"));
    const ca::u32 badRef  = cp2.addRef(CpTag::Methodref, badCls, nat2);
    MemberRefSem msem;
    EXPECT_FALSE(resolveHandleCp(cp2, cp2.addMethodHandle(6, badRef), sem));
    EXPECT_FALSE(resolveMemberRef(cp2, badRef, msem));
}

// ---- resolveClassName ----

/// 正例（字节路径）+ 非法面：非 Class 条目、0 号槽、越界、Class.a 非 Utf8。
TEST(CpResolveTest, ResolveClassName)
{
    jbc::test::ClassBuilder cb;
    const ca::u16 thisIdx  = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 thisCls  = cb.classRef(thisIdx);
    const ca::u16 superCls = cb.classRef(superIdx);
    cb.setThis(thisCls);
    cb.setSuper(superCls);

    auto cf = jbc::test::readOk(cb.build());
    ASSERT_NE(cf, nullptr);

    EXPECT_EQ(resolveClassName(cf->cp, thisCls), "p/T");
    EXPECT_EQ(resolveClassName(cf->cp, superCls), "java/lang/Object");

    // 非法面：空 view 表达失败（与 utf8Of 的探测语义同款）。
    EXPECT_EQ(resolveClassName(cf->cp, thisIdx), std::string_view{});
    EXPECT_EQ(resolveClassName(cf->cp, 0), std::string_view{});
    EXPECT_EQ(resolveClassName(cf->cp, 9999), std::string_view{});

    ConstantPool cp;
    const ca::u32 badCls = cp.addClass(cp.addInteger(3));
    EXPECT_EQ(resolveClassName(cp, badCls), std::string_view{});
}

// ---- 宽池续槽与坏条目隔离 ----

/// 宽条目（Long 双槽）的续槽不可解析；坏条目（kind=0 handle）不影响同池
/// 其他条目的解析。
TEST(CpResolveTest, WideSlotAndBadEntryDoNotPoisonNeighbors)
{
    ConstantPool cp;
    const ca::u32 longIdx = cp.addLong(0x0102030405060708LL);
    ASSERT_FALSE(cp.valid(longIdx + 1));   // 前置：续槽不可用

    const ca::u32 clsIdx     = cp.addClass(cp.addUtf8("p/T"));
    const ca::u32 natIdx     = cp.addNameAndType(cp.addUtf8("run"), cp.addUtf8("()V"));
    const ca::u32 mref       = cp.addRef(CpTag::Methodref, clsIdx, natIdx);
    const ca::u32 badHandle  = cp.addMethodHandle(0, mref);
    const ca::u32 goodHandle = cp.addMethodHandle(6, mref);

    HandleSem hsem;
    EXPECT_FALSE(resolveHandleCp(cp, badHandle, hsem));   // 坏条目失败
    ASSERT_TRUE(resolveHandleCp(cp, goodHandle, hsem));   // 好条目照常
    EXPECT_EQ(hsem.owner, "p/T");
    EXPECT_EQ(hsem.name, "run");
    EXPECT_EQ(hsem.kind, 6u);

    // 续槽与宽主槽不解析成任何语义。
    MemberRefSem msem;
    EXPECT_FALSE(resolveMemberRef(cp, longIdx, msem));
    EXPECT_FALSE(resolveMemberRef(cp, longIdx + 1, msem));
    EXPECT_FALSE(resolveHandleCp(cp, longIdx, hsem));

    // 同池的好成员引用不受坏 handle 影响。
    ASSERT_TRUE(resolveMemberRef(cp, mref, msem));
    EXPECT_EQ(msem.name, "run");
}
