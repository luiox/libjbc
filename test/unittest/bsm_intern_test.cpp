/// @file        bsm_intern_test.cpp
/// @brief       BSM 语义 intern 家族双模式门：
///
///   1. 纯模型门（无 JDK 也全跑）：句柄 / MethodType / Module / Package /
///      条件常量的去重 intern 与池形态走查（tag + 链路逐跳）；BootstrapMethods
///      缺则建 / 同形条目命中复用 / 域宽与锚点错误族；CP 容量边界拒下不留
///      半截；ChangeSet 记账；压缩收编后 BSM 集群语义保持。
///   2. JVM 真跑门（JDK 在位执行，缺席 skip）：BSM + 句柄条目的类形态自
///      JDK8 起即可承载（全版本）；条件常量类按 java.version 分判——JDK8
///      解析器不识 CONSTANT_Dynamic，拒收属环境事实而非缺陷（v55 的
///      UnsupportedClassVersionError 或 v55-以下形态都会发生，只断言非 ok）。
#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "jdk_fixture_gtest.hpp"
#include "test_class_bytes.hpp"
#include "jbc/jbc.hpp"

using namespace jbc::core;
namespace core = jbc::core;

using jbc::test::Bytes;
using jbc::test::writeOk;

using jbc::ir::BsmArg;
using jbc::ir::ClassSession;
using jbc::ir::LowerError;
using jbc::ir::MethodHandleSpec;
using jbc::ir::NewClassSeed;
using SessionPtr = std::shared_ptr<ClassSession>;

namespace {

constexpr ca::u32 kVersion8  = 52u << 16;
constexpr ca::u32 kVersion11 = 55u << 16;   ///< CONSTANT_Dynamic 的最低 classfile 版本

constexpr ca::u32 kAccPublic = 0x0001;
constexpr ca::u32 kAccSuper  = 0x0020;

const char kConcatOwner[] = "ju/invoke/StringConcatFactory";
const char kConcatName[]  = "makeConcatWithConstants";
const char kConcatDesc[]
    = "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/"
      "MethodType;)Ljava/lang/invoke/CallSite;";

template <typename R>
bool expectOk(const R& r, const char* what)
{
    if (r.is_ok()) {
        return true;
    }
    ADD_FAILURE() << what << " failed: " << jbc::ir::to_string(r.unwrap_err());
    return false;
}

SessionPtr seedSession(ca::u32 version, const char* name)
{
    NewClassSeed seed;
    seed.version   = version;
    seed.access    = kAccPublic | kAccSuper;
    seed.thisName  = name;
    seed.superName = "java/lang/Object";
    auto r         = jbc::ir::makeSession(seed);
    if (!expectOk(r, "makeSession")) {
        return nullptr;
    }
    return std::move(r).unwrap();
}

// ---- 合成规格小工厂（C++17 无指定初始化器，逐一装配） ----

MethodHandleSpec mhSpec(ca::u16 kind, std::string_view owner, std::string_view name,
                        std::string_view desc)
{
    MethodHandleSpec h;
    h.referenceKind = kind;
    h.owner         = owner;
    h.name          = name;
    h.desc          = desc;
    return h;
}

BsmArg argInt(ca::i64 v)
{
    BsmArg a;
    a.kind = BsmArg::Kind::Int;
    a.num  = v;
    return a;
}

BsmArg argStr(std::string_view s)
{
    BsmArg a;
    a.kind = BsmArg::Kind::String;
    a.str  = s;
    return a;
}

BsmArg argClass(std::string_view internal)
{
    BsmArg a;
    a.kind = BsmArg::Kind::Class;
    a.str  = internal;
    return a;
}

BsmArg argType(std::string_view desc)
{
    BsmArg a;
    a.kind = BsmArg::Kind::MethodType;
    a.str  = desc;
    return a;
}

BsmArg argHandle(const MethodHandleSpec& h)
{
    BsmArg a;
    a.kind   = BsmArg::Kind::MethodHandle;
    a.handle = h;
    return a;
}

BsmArg argDynamic(ca::u32 bsm, std::string_view name, std::string_view desc)
{
    BsmArg a;
    a.kind = BsmArg::Kind::Dynamic;
    a.bsm  = bsm;
    a.name = name;
    a.desc = desc;
    return a;
}

// ---- 池形态走查（链路逐跳；索引无关——压缩后原样复用） ----

bool utf8Is(const ClassFile& cf, ca::u32 idx, std::string_view expect)
{
    if (!cf.cp.valid(idx)) {
        ADD_FAILURE() << "utf8 idx invalid: " << idx;
        return false;
    }
    return cf.cp.at(idx).tag == CpTag::Utf8 && cf.cp.utf8Equals(idx, expect);
}

const char* tagName(CpTag t)
{
    switch (t) {
    case CpTag::Utf8: return "Utf8";
    case CpTag::Integer: return "Integer";
    case CpTag::Float: return "Float";
    case CpTag::Long: return "Long";
    case CpTag::Double: return "Double";
    case CpTag::Class: return "Class";
    case CpTag::String: return "String";
    case CpTag::Fieldref: return "Fieldref";
    case CpTag::Methodref: return "Methodref";
    case CpTag::InterfaceMethodref: return "InterfaceMethodref";
    case CpTag::NameAndType: return "NameAndType";
    case CpTag::MethodHandle: return "MethodHandle";
    case CpTag::MethodType: return "MethodType";
    case CpTag::Dynamic: return "Dynamic";
    case CpTag::InvokeDynamic: return "InvokeDynamic";
    case CpTag::Module: return "Module";
    case CpTag::Package: return "Package";
    }
    return "?";
}

bool entryIs(const ClassFile& cf, ca::u32 idx, CpTag expect, const char* what)
{
    if (!cf.cp.valid(idx)) {
        ADD_FAILURE() << what << ": idx invalid " << idx;
        return false;
    }
    const CpTag t = cf.cp.at(idx).tag;
    if (t != expect) {
        ADD_FAILURE() << what << ": expected " << tagName(expect) << ", got " << tagName(t);
        return false;
    }
    return true;
}

/// @brief MethodHandle 条目四跳形态：kind → ref tag → owner → (name, desc)。
bool walkHandle(const ClassFile& cf, ca::u32 mhIdx, ca::u32 kind, CpTag refTag,
                std::string_view owner, std::string_view name, std::string_view desc)
{
    if (!entryIs(cf, mhIdx, CpTag::MethodHandle, "handle")) {
        return false;
    }
    const ConstantPool::Entry& e = cf.cp.at(mhIdx);
    if (e.a != kind || !entryIs(cf, e.b, refTag, "handle ref")) {
        return false;
    }
    const ConstantPool::Entry& r = cf.cp.at(e.b);
    if (!entryIs(cf, r.a, CpTag::Class, "ref owner")) {
        return false;
    }
    if (!utf8Is(cf, cf.cp.at(r.a).a, owner)) {
        return false;
    }
    if (!entryIs(cf, r.b, CpTag::NameAndType, "ref nat")) {
        return false;
    }
    const ConstantPool::Entry& nat = cf.cp.at(r.b);
    return utf8Is(cf, nat.a, name) && utf8Is(cf, nat.b, desc);
}

/// @brief 条件常量条目形态：a = bootstrap 条目下标，b = NameAndType 链。
bool walkDynamic(const ClassFile& cf, ca::u32 idx, ca::u32 bsm, std::string_view name,
                 std::string_view desc)
{
    if (!entryIs(cf, idx, CpTag::Dynamic, "condy")) {
        return false;
    }
    const ConstantPool::Entry& e = cf.cp.at(idx);
    if (e.a != bsm || !entryIs(cf, e.b, CpTag::NameAndType, "condy nat")) {
        return false;
    }
    const ConstantPool::Entry& nat = cf.cp.at(e.b);
    return utf8Is(cf, nat.a, name) && utf8Is(cf, nat.b, desc);
}

/// @brief 类属性槽按名定位 + 按需解码为 typed 体（seekBsmAttr 的测试镜像）。
core::BootstrapMethodsAttr* bsmBodyOf(ClassSession& s)
{
    for (core::AttrSlot& slot : s.backing().attrs) {
        if (!s.backing().cp.utf8Equals(slot.nameCp, "BootstrapMethods")) {
            continue;
        }
        if (!slot.decoded()) {
            const auto r = s.backing().decodeAttr(slot);
            if (r.is_err()) {
                ADD_FAILURE() << "decodeAttr(BootstrapMethods) failed (ReadError)";
                return nullptr;
            }
        }
        if (slot.peek() == nullptr
            || slot.peek()->kind() != AttrKind::BootstrapMethods) {
            ADD_FAILURE() << "BootstrapMethods slot body kind mismatch";
            return nullptr;
        }
        return static_cast<core::BootstrapMethodsAttr*>(slot.body.get());
    }
    return nullptr;
}

std::vector<std::string> splitLines(const std::string& s)
{
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) {
            out.push_back(line);
        }
    }
    return out;
}

size_t countMetadataRecords(const jbc::ir::ChangeSet& cs)
{
    size_t n = 0;
    for (const jbc::ir::ChangeRecord& r : cs.records()) {
        if (r.kind == jbc::ir::ChangeKind::ClassMetadata) {
            ++n;
        }
    }
    return n;
}

}   // namespace

// ---------------------------------------------------------------------------
// 纯模型门
// ---------------------------------------------------------------------------

TEST(BsmIntern, MethodHandleInternDedupAndShape)
{
    SessionPtr s = seedSession(kVersion8, "bu/B");
    ASSERT_NE(s, nullptr);

    const auto r1 = s->internMethodHandle(mhSpec(6, "ju/Math", "abs", "(I)I"));
    ASSERT_TRUE(expectOk(r1, "internMethodHandle"));
    const ca::u32 h1 = r1.unwrap();
    ASSERT_TRUE(walkHandle(s->backing(), h1, 6, CpTag::Methodref, "ju/Math", "abs", "(I)I"));

    const ca::u32 sizeAfter = s->backing().cp.size();
    const auto r2 = s->internMethodHandle(mhSpec(6, "ju/Math", "abs", "(I)I"));
    ASSERT_TRUE(expectOk(r2, "internMethodHandle(re)"));
    EXPECT_EQ(r2.unwrap(), h1);   // 命中既有句柄不膨胀
    EXPECT_EQ(s->backing().cp.size(), sizeAfter);

    // kind 不同即不同句柄条目（9 → InterfaceMethodref 形）。
    const auto r3 = s->internMethodHandle(mhSpec(9, "ju/If", "m", "(Ljava/lang/Object;)V"));
    ASSERT_TRUE(expectOk(r3, "internMethodHandle(if)"));
    ASSERT_TRUE(walkHandle(s->backing(), r3.unwrap(), 9, CpTag::InterfaceMethodref,
                           "ju/If", "m", "(Ljava/lang/Object;)V"));
    EXPECT_NE(r3.unwrap(), h1);

    // 字段形句柄：1..4 → Fieldref。
    const auto r4 = s->internMethodHandle(mhSpec(2, "bu/B", "f", "I"));
    ASSERT_TRUE(expectOk(r4, "internMethodHandle(getStatic)"));
    ASSERT_TRUE(walkHandle(s->backing(), r4.unwrap(), 2, CpTag::Fieldref, "bu/B", "f", "I"));

    // kind 域外：JVM 类解析即拒 → InvalidSeed，且不追加任何条目。
    const ca::u32 sizeKeep = s->backing().cp.size();
    for (const ca::u16 bad : {ca::u16{0}, ca::u16{10}, ca::u16{16}}) {
        const auto rb = s->internMethodHandle(mhSpec(bad, "x", "y", "(I)I"));
        EXPECT_TRUE(rb.is_err() && rb.unwrap_err() == LowerError::InvalidSeed) << bad;
    }
    EXPECT_EQ(s->backing().cp.size(), sizeKeep);
}

TEST(BsmIntern, MethodTypeModulePackageIntern)
{
    SessionPtr s = seedSession(kVersion8, "bu/B");
    ASSERT_NE(s, nullptr);

    const ca::u32 t1 = s->internMethodType("(I)V");
    ASSERT_TRUE(entryIs(s->backing(), t1, CpTag::MethodType, "mt"));
    EXPECT_TRUE(utf8Is(s->backing(), s->backing().cp.at(t1).a, "(I)V"));
    EXPECT_EQ(s->internMethodType("(I)V"), t1);

    // Module/Package：名同条目同（各自去重），互不混账。
    const ca::u32 m1 = s->internModule("ju/test/module");
    ASSERT_TRUE(entryIs(s->backing(), m1, CpTag::Module, "module"));
    EXPECT_TRUE(utf8Is(s->backing(), s->backing().cp.at(m1).a, "ju/test/module"));
    EXPECT_EQ(s->internModule("ju/test/module"), m1);

    const ca::u32 p1 = s->internPackage("ju/test/pkg");
    ASSERT_TRUE(entryIs(s->backing(), p1, CpTag::Package, "package"));
    EXPECT_TRUE(utf8Is(s->backing(), s->backing().cp.at(p1).a, "ju/test/pkg"));
    EXPECT_EQ(s->internPackage("ju/test/pkg"), p1);
    EXPECT_NE(p1, m1);
}

TEST(BsmIntern, AppendBirthReuseDedupeAndShape)
{
    SessionPtr s = seedSession(kVersion8, "bu/B");
    ASSERT_NE(s, nullptr);
    const std::vector<BsmArg> args = {argInt(42), argStr("hi")};

    const auto r0 = s->appendBootstrapMethod(mhSpec(6, kConcatOwner, kConcatName, kConcatDesc),
                                             args);
    ASSERT_TRUE(expectOk(r0, "append(birth)"));
    EXPECT_EQ(r0.unwrap(), 0u);

    core::BootstrapMethodsAttr* b0 = bsmBodyOf(*s);
    ASSERT_NE(b0, nullptr);
    ASSERT_EQ(b0->entries.size(), 1u);
    const ca::u32 hIdx = b0->entries[0].methodRef;
    EXPECT_TRUE(walkHandle(s->backing(), hIdx, 6, CpTag::Methodref, kConcatOwner,
                           kConcatName, kConcatDesc));
    ASSERT_EQ(b0->entries[0].args.size(), 2u);
    ASSERT_TRUE(entryIs(s->backing(), b0->entries[0].args[0], CpTag::Integer, "arg0"));
    EXPECT_EQ(s->backing().cp.at(b0->entries[0].args[0]).ival, 42);
    ASSERT_TRUE(entryIs(s->backing(), b0->entries[0].args[1], CpTag::String, "arg1"));
    ASSERT_TRUE(utf8Is(s->backing(), s->backing().cp.at(b0->entries[0].args[1]).a, "hi"));
    // 属性诞生即单例：同帐名槽位至多一条。
    size_t slotCount = 0;
    for (const core::AttrSlot& slot : s->backing().attrs) {
        if (s->backing().cp.utf8Equals(slot.nameCp, "BootstrapMethods")) {
            ++slotCount;
        }
    }
    EXPECT_EQ(slotCount, 1u);

    // 同形复用：命中不追加、不记账。
    const ca::u32 cpKeep = s->backing().cp.size();
    const auto r1 = s->appendBootstrapMethod(mhSpec(6, kConcatOwner, kConcatName, kConcatDesc),
                                             args);
    ASSERT_TRUE(expectOk(r1, "append(reuse)"));
    EXPECT_EQ(r1.unwrap(), 0u);
    EXPECT_EQ(s->backing().cp.size(), cpKeep);
    EXPECT_EQ(bsmBodyOf(*s)->entries.size(), 1u);

    // 变形条目正常追加；同形条目命中各自的旧位。
    const auto r2 = s->appendBootstrapMethod(mhSpec(6, kConcatOwner, kConcatName, kConcatDesc),
                                             {argInt(42), argInt(43)});
    ASSERT_TRUE(expectOk(r2, "append(distinct)"));
    EXPECT_EQ(r2.unwrap(), 1u);
    const auto r3 = s->appendBootstrapMethod(mhSpec(6, kConcatOwner, kConcatName, kConcatDesc),
                                             {argInt(42), argInt(43)});
    ASSERT_TRUE(expectOk(r3, "append(reuse2)"));
    EXPECT_EQ(r3.unwrap(), 1u);
    EXPECT_EQ(bsmBodyOf(*s)->entries.size(), 2u);

    // 记账：实改（诞生 + 追加）各一条，纯命中不记账。
    EXPECT_EQ(countMetadataRecords(s->changeSet()), 2u);
}

TEST(BsmIntern, AppendErrorFamily)
{
    SessionPtr s = seedSession(kVersion8, "bu/B");
    ASSERT_NE(s, nullptr);

    // 句柄 kind 域外。
    const auto r0 = s->appendBootstrapMethod(mhSpec(0, "x", "y", "z"), {});
    EXPECT_TRUE(r0.is_err() && r0.unwrap_err() == LowerError::InvalidSeed);

    // 空 String/Class/MethodType 规格。
    const ca::u32 sizeKeep = s->backing().cp.size();
    const auto r1 = s->appendBootstrapMethod(mhSpec(6, "a", "b", "(I)V"), {argStr("")});
    EXPECT_TRUE(r1.is_err() && r1.unwrap_err() == LowerError::InvalidSeed);
    const auto r2 = s->appendBootstrapMethod(mhSpec(6, "a", "b", "(I)V"), {argClass("")});
    EXPECT_TRUE(r2.is_err() && r2.unwrap_err() == LowerError::InvalidSeed);
    const auto r3 = s->appendBootstrapMethod(mhSpec(6, "a", "b", "(I)V"), {argType("")});
    EXPECT_TRUE(r3.is_err() && r3.unwrap_err() == LowerError::InvalidSeed);
    EXPECT_EQ(s->backing().cp.size(), sizeKeep);

    // attr 缺位而引条件常量：锚点缺失（JVM 类解析同拒形态），拒下不留半截。
    const auto r4 = s->appendBootstrapMethod(mhSpec(6, "a", "b", "(I)V"),
                                             {argDynamic(0, "mk", "I")});
    EXPECT_TRUE(r4.is_err() && r4.unwrap_err() == LowerError::InvalidSeed);
    EXPECT_EQ(s->backing().cp.size(), sizeKeep);
    EXPECT_EQ(bsmBodyOf(*s), nullptr);
}

TEST(BsmIntern, DynamicInternAnchoring)
{
    SessionPtr s = seedSession(kVersion8, "bu/B");
    ASSERT_NE(s, nullptr);

    // 无 BSM 属性：拒绝且池不涨。
    const ca::u32 sizeKeep = s->backing().cp.size();
    const auto r0 = s->internDynamic(0, "mk", "Ljava/lang/String;");
    EXPECT_TRUE(r0.is_err() && r0.unwrap_err() == LowerError::InvalidSeed);
    EXPECT_EQ(s->backing().cp.size(), sizeKeep);

    // 挂一条 BSM 条目，锚住下标 0。
    ASSERT_TRUE(expectOk(s->appendBootstrapMethod(mhSpec(6, kConcatOwner, kConcatName,
                                                  kConcatDesc),
                                                  {argInt(1)}),
                         "append#0"));
    const auto r1 = s->internDynamic(0, "mk", "Ljava/lang/String;");
    ASSERT_TRUE(expectOk(r1, "condy"));
    const ca::u32 c1 = r1.unwrap();
    ASSERT_TRUE(walkDynamic(s->backing(), c1, 0, "mk", "Ljava/lang/String;"));

    ca::u32 cpKeep = s->backing().cp.size();
    const auto r2 = s->internDynamic(0, "mk", "Ljava/lang/String;");
    ASSERT_TRUE(expectOk(r2, "condy(re)"));
    EXPECT_EQ(r2.unwrap(), c1);   // 同 (bsm, 名, 描述符) 去重
    EXPECT_EQ(s->backing().cp.size(), cpKeep);

    // 锚到另一条目 → 不同常量条目。
    ASSERT_TRUE(expectOk(s->appendBootstrapMethod(mhSpec(6, kConcatOwner, kConcatName,
                                                  kConcatDesc),
                                                  {argInt(2)}),
                         "append#1"));
    const auto r3 = s->internDynamic(1, "mk", "Ljava/lang/String;");
    ASSERT_TRUE(expectOk(r3, "condy#1"));
    EXPECT_NE(r3.unwrap(), c1);
    ASSERT_TRUE(walkDynamic(s->backing(), r3.unwrap(), 1, "mk", "Ljava/lang/String;"));
    cpKeep = s->backing().cp.size();   // append#1 自身的合法涨池已然入账

    // 下标越界（= 条目数 /= 大值）：锚点缺失。
    const auto r4 = s->internDynamic(2, "mk", "I");
    EXPECT_TRUE(r4.is_err() && r4.unwrap_err() == LowerError::InvalidSeed);
    const auto r5 = s->internDynamic(0x7FFFu, "mk", "I");
    EXPECT_TRUE(r5.is_err() && r5.unwrap_err() == LowerError::InvalidSeed);
    EXPECT_EQ(s->backing().cp.size(), cpKeep);

    // 条件常量作 BSM 静态参数（绑定既有条目下标；(bsm, 名, 型) 元组不同即
    // 不同常量——"inner"/"I" 与 c1 的 "mk"/"Ljava/lang/String;" 恒异条目）。
    const auto r6 = s->appendBootstrapMethod(mhSpec(6, kConcatOwner, kConcatName, kConcatDesc),
                                             {argDynamic(0, "inner", "I")});
    ASSERT_TRUE(expectOk(r6, "append(condy-arg)"));
    ASSERT_EQ(bsmBodyOf(*s)->entries[2].args.size(), 1u);
    const ca::u32 argCondy = bsmBodyOf(*s)->entries[2].args[0];
    ASSERT_TRUE(walkDynamic(s->backing(), argCondy, 0, "inner", "I"));
    const auto r6b = s->internDynamic(0, "inner", "I");
    ASSERT_TRUE(expectOk(r6b, "condy(inner re)"));
    EXPECT_EQ(r6b.unwrap(), argCondy);   // 同形再 intern 命中同一条目

    // 越界锚点的条件参数：在预检期拒下（末位条目数即上界）。
    cpKeep = s->backing().cp.size();
    const auto r7 = s->appendBootstrapMethod(mhSpec(6, kConcatOwner, kConcatName, kConcatDesc),
                                             {argDynamic(99, "inner", "I")});
    EXPECT_TRUE(r7.is_err() && r7.unwrap_err() == LowerError::InvalidSeed);
    EXPECT_EQ(s->backing().cp.size(), cpKeep);
}

TEST(BsmIntern, CpAppendBoundaryNoHalfMount)
{
    SessionPtr s = seedSession(kVersion8, "bu/B");
    ASSERT_NE(s, nullptr);

    // 填池：留出的余位 < 一条 BSM 集群所需（utf8 三件 + object 链 + 句柄 +
    // attr 名 + 参数），令容量预检只能在拒与过之间二选一。
    ca::u32 i = 0;
    while (s->backing().cp.size() < 0xFFFAu) {
        const std::string tag = "pad/" + std::to_string(i++);
        s->internUtf8(tag);
    }
    const ca::u32 sizeKeep = s->backing().cp.size();
    const size_t attrKeep  = s->backing().attrs.size();

    const auto r = s->appendBootstrapMethod(mhSpec(6, kConcatOwner, kConcatName, kConcatDesc),
                                            {argInt(1), argInt(2)});
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), LowerError::CpAppendFailed);
    // 拒下不留半截：池与属性表纹丝不动。
    EXPECT_EQ(s->backing().cp.size(), sizeKeep);
    EXPECT_EQ(s->backing().attrs.size(), attrKeep);
    EXPECT_EQ(bsmBodyOf(*s), nullptr);
    EXPECT_EQ(countMetadataRecords(s->changeSet()), 0u);
}

TEST(BsmIntern, CompactKeepsBsmClusterSemantics)
{
    SessionPtr s = seedSession(kVersion11, "bu/B");
    ASSERT_NE(s, nullptr);

    // 参与压缩的长驻集群全部挂在 BSM 条目上（方法形句柄 + 多形参数含
    // MethodType/句柄参数/Class/条件常量）——BSM 引用是压缩标活的真实可达
    // 源（预指令面下唯一）；孤件常量会被死常量消除收掉，属压缩语义。
    const ca::u32 mtPre = s->internMethodType("(J)Ljava/lang/String;");
    const auto b0 = s->appendBootstrapMethod(
        mhSpec(6, kConcatOwner, kConcatName, kConcatDesc), {argInt(-7), argStr("zc")});
    const auto b1 = s->appendBootstrapMethod(
        mhSpec(6, "bu/Op", "factory", "(J)Ljava/lang/String;"),
        {argType("(J)Ljava/lang/String;"), argHandle(mhSpec(6, "bu/A", "m", "(I)I")),
         argClass("bu/A")});
    ASSERT_TRUE(expectOk(b0, "append#0") && expectOk(b1, "append#1"));
    const auto dy = s->internDynamic(0, "mk", "Ljava/lang/String;");
    ASSERT_TRUE(expectOk(dy, "condy"));
    const auto b2 = s->appendBootstrapMethod(
        mhSpec(6, kConcatOwner, kConcatName, kConcatDesc),
        {argDynamic(0, "mk", "Ljava/lang/String;")});
    ASSERT_TRUE(expectOk(b2, "append#2"));
    const auto hIdx = s->internMethodHandle(mhSpec(6, kConcatOwner, kConcatName, kConcatDesc));
    ASSERT_TRUE(expectOk(hIdx, "handle"));
    ASSERT_TRUE(walkHandle(s->backing(), hIdx.unwrap(), 6, CpTag::Methodref, kConcatOwner,
                           kConcatName, kConcatDesc));
    ASSERT_TRUE(walkDynamic(s->backing(), dy.unwrap(), 0, "mk", "Ljava/lang/String;"));
    ASSERT_TRUE(entryIs(s->backing(), mtPre, CpTag::MethodType, "mt"));
    EXPECT_EQ(bsmBodyOf(*s)->entries.size(), 3u);

    ASSERT_TRUE(jbc::ir::compactConstantPool(*s).is_ok());

    // 压缩重排 CP：旧局部索引全部失效（压缩前快照不留用），同语义规格再
    // intern 命中的即是新池规范索引；重 intern 以 SemIndex 重建命中、池
    // 不膨胀，逐跳走链断言集群形态与 BSM 条目内容原样。
    const ca::u32 sizeKeep = s->backing().cp.size();
    const auto h2 = s->internMethodHandle(mhSpec(6, kConcatOwner, kConcatName, kConcatDesc));
    ASSERT_TRUE(expectOk(h2, "handle(post)"));
    EXPECT_EQ(s->backing().cp.size(), sizeKeep);
    ASSERT_TRUE(walkHandle(s->backing(), h2.unwrap(), 6, CpTag::Methodref, kConcatOwner,
                           kConcatName, kConcatDesc));

    const auto t2 = s->internMethodType("(J)Ljava/lang/String;");
    ASSERT_EQ(t2, mtPre);   // 存活于 b1 的 MethodType 参数：原地编号未变
    ASSERT_TRUE(entryIs(s->backing(), t2, CpTag::MethodType, "mt-after"));
    EXPECT_TRUE(utf8Is(s->backing(), s->backing().cp.at(t2).a, "(J)Ljava/lang/String;"));
    EXPECT_EQ(s->backing().cp.size(), sizeKeep);

    const auto dy2 = s->internDynamic(0, "mk", "Ljava/lang/String;");
    ASSERT_TRUE(expectOk(dy2, "condy(post)"));
    ASSERT_TRUE(walkDynamic(s->backing(), dy2.unwrap(), 0, "mk", "Ljava/lang/String;"));
    EXPECT_EQ(s->backing().cp.size(), sizeKeep);

    // BSM 属性体（压缩重编码后经 typed 解析）：条目数与三形条目的内容。
    core::BootstrapMethodsAttr* b = bsmBodyOf(*s);
    ASSERT_NE(b, nullptr);
    ASSERT_EQ(b->entries.size(), 3u);
    ASSERT_TRUE(walkHandle(s->backing(), b->entries[0].methodRef, 6, CpTag::Methodref,
                           kConcatOwner, kConcatName, kConcatDesc));
    ASSERT_EQ(b->entries[0].args.size(), 2u);
    ASSERT_TRUE(entryIs(s->backing(), b->entries[0].args[0], CpTag::Integer, "post arg0"));
    EXPECT_EQ(s->backing().cp.at(b->entries[0].args[0]).ival, -7);
    ASSERT_TRUE(entryIs(s->backing(), b->entries[0].args[1], CpTag::String, "post arg1"));
    ASSERT_TRUE(utf8Is(s->backing(), s->backing().cp.at(b->entries[0].args[1]).a, "zc"));
    ASSERT_TRUE(walkHandle(s->backing(), b->entries[1].methodRef, 6, CpTag::Methodref,
                           "bu/Op", "factory", "(J)Ljava/lang/String;"));
    ASSERT_EQ(b->entries[1].args.size(), 3u);
    ASSERT_EQ(b->entries[1].args[0], t2);
    ASSERT_TRUE(walkHandle(s->backing(), b->entries[1].args[1], 6, CpTag::Methodref,
                           "bu/A", "m", "(I)I"));
    ASSERT_TRUE(entryIs(s->backing(), b->entries[1].args[2], CpTag::Class, "post cls"));
    ASSERT_TRUE(utf8Is(s->backing(), s->backing().cp.at(b->entries[1].args[2]).a, "bu/A"));
    ASSERT_TRUE(walkHandle(s->backing(), b->entries[2].methodRef, 6, CpTag::Methodref,
                           kConcatOwner, kConcatName, kConcatDesc));
    ASSERT_EQ(b->entries[2].args[0], dy2.unwrap());
}

TEST(BsmIntern, CompactDropsOrphanConstants)
{
    // 孤件 Module/Package/MethodType/condy（无任何消费者）：压缩按死常量
    // 消除语义收掉；再 intern 走重新追加（索引全新、形态正确）；BSM 锚点
    // 在 BSM 属性内存活，条件常量的重 intern 仍受其约束。
    SessionPtr s = seedSession(kVersion11, "bu/B");
    ASSERT_NE(s, nullptr);
    (void)s->internMethodType("(I)V");
    (void)s->internModule("ju/test/module");
    (void)s->internPackage("ju/test/pkg");
    const auto e0 = s->appendBootstrapMethod(mhSpec(6, kConcatOwner, kConcatName, kConcatDesc),
                                             {argInt(1)});
    ASSERT_TRUE(expectOk(e0, "append#0"));
    const auto orphan = s->internDynamic(0, "mk", "Ljava/lang/String;");
    ASSERT_TRUE(expectOk(orphan, "orphan condy"));
    const ca::u32 sizeLive = s->backing().cp.size();

    ASSERT_TRUE(jbc::ir::compactConstantPool(*s).is_ok());
    EXPECT_LT(s->backing().cp.size(), sizeLive);   // 孤件收敛池缩

    const auto t3 = s->internMethodType("(I)V");
    ASSERT_TRUE(entryIs(s->backing(), t3, CpTag::MethodType, "mt re"));
    const auto m3 = s->internModule("ju/test/module");
    ASSERT_TRUE(entryIs(s->backing(), m3, CpTag::Module, "module re"));
    const auto p3 = s->internPackage("ju/test/pkg");
    ASSERT_TRUE(entryIs(s->backing(), p3, CpTag::Package, "package re"));
    const auto o2 = s->internDynamic(0, "mk", "Ljava/lang/String;");
    ASSERT_TRUE(expectOk(o2, "orphan condy re"));
    ASSERT_TRUE(walkDynamic(s->backing(), o2.unwrap(), 0, "mk", "Ljava/lang/String;"))
        << "BSM 条目锚点（下标 0）在压缩中存活，重 intern 正常落位（孤件）";
}

// ---------------------------------------------------------------------------
// JVM 真跑门（JDK 在位执行，缺席 skip）
// ---------------------------------------------------------------------------

TEST(BsmIntern, JvmLoadGate)
{
    // S1：v52 + BSM 属性两条（句柄参数与多形参数）——句柄/MethodType/BSM 家族
    //     自 JDK7+ 即为合法类形态，全域断言可装载。
    SessionPtr s1 = seedSession(kVersion8, "bu/S1");
    ASSERT_NE(s1, nullptr);
    const auto e0 = s1->appendBootstrapMethod(mhSpec(6, kConcatOwner, kConcatName, kConcatDesc),
                                              {argInt(1), argClass("bu/A")});
    const auto e1 = s1->appendBootstrapMethod(mhSpec(9, "bu/I", "run", "()V"),
                                              {argStr("v"), argHandle(mhSpec(6, "bu/A", "m",
                                                                                      "(I)I"))});
    ASSERT_TRUE(expectOk(e0, "append#0") && expectOk(e1, "append#1"));
    const Bytes s1Bytes = writeOk(s1->backing());
    ASSERT_FALSE(s1Bytes.empty());

    // S2：v55 + 同构条目集 + 条件常量——CONSTANT_Dynamic 需 JDK11+ 解析；
    //     JDK8 在 UnsupportedClassVersionError（v55）先行拒绝，两条路径都记
    //     非 ok，实现按 java.version 分判，不硬编异常名。
    SessionPtr s2 = seedSession(kVersion11, "bu/S2");
    ASSERT_NE(s2, nullptr);
    const auto f0 = s2->appendBootstrapMethod(mhSpec(6, kConcatOwner, kConcatName, kConcatDesc),
                                              {argInt(9)});
    ASSERT_TRUE(expectOk(f0, "append#0"));
    const auto dy = s2->internDynamic(0, "mk", "Ljava/lang/String;");
    ASSERT_TRUE(expectOk(dy, "condy"));
    const Bytes s2Bytes = writeOk(s2->backing());
    ASSERT_FALSE(s2Bytes.empty());

    test::jdk::RunnerSpec spec;
    spec.classes = {{"bu/S1", s1Bytes}, {"bu/S2", s2Bytes}};
    spec.runnerBody = R"java(
try {
    Class.forName("bu.S1");
    System.out.println("S1=ok");
} catch (Throwable t) {
    System.out.println("S1=" + t.getClass().getSimpleName());
}
try {
    Class.forName("bu.S2");
    System.out.println("S2=ok");
} catch (Throwable t) {
    System.out.println("S2=" + t.getClass().getSimpleName());
}
System.out.println("v=" + System.getProperty("java.version"));
)java";

    std::string got;
    if (!test::jdk::runRunnerChecked(test::jdk::probeJdk(), spec, &got)) {
        return;   // SKIP / FAIL 已在 checked 入口记账
    }
    const std::vector<std::string> lines = splitLines(got);
    ASSERT_EQ(lines.size(), 3u) << "full output:\n" << got;
    EXPECT_EQ(lines[0], "S1=ok");
    const bool jdk8 = lines[2].rfind("v=1.", 0) == 0;
    if (jdk8) {
        EXPECT_FALSE(lines[1].rfind("S2=ok", 0) == 0)
            << "v55/condy 不应在 JDK8 上承载；version=" << lines[2];
    } else {
        EXPECT_EQ(lines[1], "S2=ok") << "version=" << lines[2];
    }
}
