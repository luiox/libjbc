/// @file        class_build_attrs_test.cpp
/// @brief       类构建挂载面双模式门：
///
///   1. 纯模型门（无 JDK 也全跑）：注解族四件 + 类/方法/字段简单注解从零
///      合成 → writeClass → 重读 → decodeAttr typed 面逐字段断言（形态 /
///      CP 指向 / 嵌套 / 数组 / 枚举 / 类字面量）；错误族门（重复挂载、
///      Code 拒收、u1 行数域、死句柄、坏规格、深度闸）；ChangeSet 记账；
///      压缩收编后语义保持。
///   2. JVM 真跑门（JDK 在位时执行，缺席 skip）：反射读回参数名/旗语、
///      参数注解全族、注解默认值、类级注解与 ConstantValue 静态常量。
///      枚举常量由真实 Java 源（bu/Op）经 extraJavaSources 同场编译——
///      合成注解与 javac 产物同 classpath 混合部署的互操作实证。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "jdk_fixture_gtest.hpp"
#include "test_class_bytes.hpp"
#include "jbc/jbc.hpp"

using namespace jbc::core;
using jbc::test::Bytes;
using jbc::test::readOk;
using jbc::test::writeOk;

using jbc::ir::ClassSession;
using jbc::ir::ConstValueSpec;
using jbc::ir::FieldSeed;
using jbc::ir::MethodSeed;
using jbc::ir::NewClassSeed;
using jbc::ir::ParamNameRow;
using jbc::ir::SemAnnotation;
using jbc::ir::SemElemValue;
using SessionPtr = std::shared_ptr<ClassSession>;

namespace fs = std::filesystem;

namespace {

constexpr ca::u32 kVersion8 = 52u << 16;

constexpr ca::u32 kAccPublic     = 0x0001;
constexpr ca::u32 kAccStatic     = 0x0008;
constexpr ca::u32 kAccFinal      = 0x0010;
constexpr ca::u32 kAccSuper      = 0x0020;
constexpr ca::u32 kAccInterface  = 0x0200;
constexpr ca::u32 kAccAbstract   = 0x0400;
constexpr ca::u32 kAccAnnotation = 0x2000;

constexpr ca::u16 kParameterFinal     = 0x0010;   ///< MethodParameters ACC_FINAL
constexpr ca::u16 kParameterSynthetic = 0x1000;   ///< MethodParameters ACC_SYNTHETIC

const char kAnnoDesc[]  = "Lbu/Anno;";
const char kInvisDesc[] = "Lbu/Ivis;";

/// 通用 hopeful：EXPECT 单点记账，构建路径据此短路（后续 unwrap 只在
/// 成功分支继续）。
template <typename R>
bool expectOk(const R& r, const char* what)
{
    if (r.is_ok()) {
        return true;
    }
    ADD_FAILURE() << what << " failed: " << jbc::ir::to_string(r.unwrap_err());
    return false;
}

/// 从零 seed 会话（this/super 建骨架，名字内部名形态；注解类需追加
/// java/lang/annotation/Annotation 接口——javac 的 @interface 语义糖）。
SessionPtr seedSession(ca::u32 access, const char* thisName,
                       std::vector<std::string_view> interfaces = {})
{
    NewClassSeed seed;
    seed.version   = kVersion8;
    seed.access    = access;
    seed.thisName  = thisName;
    seed.superName = "java/lang/Object";
    seed.interfaces = std::move(interfaces);
    auto r         = jbc::ir::makeSession(seed);
    if (!expectOk(r, "makeSession")) {
        return nullptr;
    }
    return std::move(r).unwrap();
}

// ---- 合成规格小工厂（值语义，成员逐一装配；C++17 无指定初始化器） ----

SemElemValue svNum(SemElemValue::Form form, ca::i64 num)
{
    SemElemValue e;
    e.form = form;
    e.num  = num;
    return e;
}

SemElemValue svStr(std::string_view str)
{
    SemElemValue e;
    e.form = SemElemValue::Form::String;
    e.str  = str;
    return e;
}

SemElemValue svClass(std::string_view name)
{
    SemElemValue e;
    e.form = SemElemValue::Form::Class;
    e.str  = name;
    return e;
}

SemElemValue svEnum(std::string_view typeDesc, std::string_view constName)
{
    SemElemValue e;
    e.form     = SemElemValue::Form::Enum;
    e.enumType = typeDesc;
    e.str      = constName;
    return e;
}

SemElemValue svDblBits(ca::u64 bits)
{
    SemElemValue e;
    e.form  = SemElemValue::Form::Double;
    e.dbits = bits;
    return e;
}

SemElemValue svArr(std::vector<SemElemValue> items)
{
    SemElemValue e;
    e.form  = SemElemValue::Form::Array;
    e.array = std::move(items);
    return e;
}

SemElemValue svNested(SemAnnotation anno)
{
    SemElemValue e;
    e.form   = SemElemValue::Form::Nested;
    e.nested = std::make_shared<SemAnnotation>(std::move(anno));
    return e;
}

SemAnnotation spec(std::string_view typeDesc)
{
    SemAnnotation a;
    a.typeDesc = typeDesc;
    return a;
}

SemAnnotation spec(std::string_view typeDesc,
                   std::vector<std::pair<std::string_view, SemElemValue>> values)
{
    SemAnnotation a;
    a.typeDesc = typeDesc;
    a.values   = std::move(values);
    return a;
}

ConstValueSpec cvInt(ca::i64 num)
{
    ConstValueSpec v;
    v.kind = ConstValueSpec::Kind::Int;
    v.num  = num;
    return v;
}

ConstValueSpec cvStr(std::string_view str)
{
    ConstValueSpec v;
    v.kind = ConstValueSpec::Kind::String;
    v.str  = str;
    return v;
}

/// 注解类 bu/Anno 的合成：default 面（int/String/int[]/long/double/enum/
/// Class 全形态）+ 类级 @Retention(RUNTIME)（'e' 元素直达 JDK meta-注解）。
/// 注解方法皆抽象（withCode=false），CP 只有 this/super 引导。
SessionPtr buildAnnoSession()
{
    SessionPtr s = seedSession(
        kAccPublic | kAccInterface | kAccAbstract | kAccAnnotation, "bu/Anno",
        {"java/lang/annotation/Annotation"});
    if (s == nullptr) {
        return nullptr;
    }
    auto mi    = s->addMethod({"i", "()I", kAccPublic | kAccAbstract});
    auto mhow  = s->addMethod({"how", "()Ljava/lang/String;", kAccPublic | kAccAbstract});
    auto mtags = s->addMethod({"tags", "()[I", kAccPublic | kAccAbstract});
    auto mlong = s->addMethod({"longV", "()J", kAccPublic | kAccAbstract});
    auto mdbl  = s->addMethod({"dbl", "()D", kAccPublic | kAccAbstract});
    auto mcls  = s->addMethod({"cls", "()Ljava/lang/Class;", kAccPublic | kAccAbstract});
    auto mop   = s->addMethod({"op", "()Lbu/Op;", kAccPublic | kAccAbstract});
    auto mnest = s->addMethod({"nested", "()Lbu/Anno;", kAccPublic | kAccAbstract});
    if (!expectOk(mi, "add i") || !expectOk(mhow, "add how") || !expectOk(mtags, "add tags")
        || !expectOk(mlong, "add longV") || !expectOk(mdbl, "add dbl")
        || !expectOk(mcls, "add cls") || !expectOk(mop, "add op")
        || !expectOk(mnest, "add nested")) {
        return s;
    }
    if (!expectOk(
            s->attachAnnotationDefault(mi.unwrap(), svNum(SemElemValue::Form::Int, 7)),
            "default i")
        || !expectOk(s->attachAnnotationDefault(mhow.unwrap(), svStr("hello")),
                     "default how")
        || !expectOk(s->attachAnnotationDefault(
                         mtags.unwrap(),
                         svArr({svNum(SemElemValue::Form::Int, 9),
                                svNum(SemElemValue::Form::Int, 8)})),
                     "default tags")
        || !expectOk(s->attachAnnotationDefault(
                         mlong.unwrap(),
                         svNum(SemElemValue::Form::Long, 123456789012345LL)),
                     "default longV")
        || !expectOk(s->attachAnnotationDefault(mdbl.unwrap(),
                                                svDblBits(0x4004000000000000ull)),
                     "default dbl")
        || !expectOk(s->attachAnnotationDefault(mcls.unwrap(), svClass("I")),
                     "default cls")
        || !expectOk(
            s->attachAnnotationDefault(mop.unwrap(), svEnum("Lbu/Op;", "PLUS")),
            "default op")
        || !expectOk(s->attachClassAnnotations(
                         true,
                         {spec("Ljava/lang/annotation/Retention;",
                               {{"value",
                                 svEnum("Ljava/lang/annotation/RetentionPolicy;",
                                        "RUNTIME")}})}),
                     "class retention")) {
        return s;
    }
    return s;
}

/// 宿主类 bu/C：抽象方法 f(String)I 挂 MethodParameters + 参数注解（全形态
/// 元素 + 嵌套注解）+ 方法级可见/不可见注解；类级可见注解；恒值字段 K/S2。
SessionPtr buildHostSession()
{
    SessionPtr s = seedSession(kAccPublic | kAccAbstract | kAccSuper, "bu/C");
    if (s == nullptr) {
        return nullptr;
    }
    auto mf = s->addMethod({"f", "(Ljava/lang/String;)I", kAccPublic | kAccAbstract});
    if (!expectOk(mf, "add f")) {
        return s;
    }
    const auto f = mf.unwrap();
    if (!expectOk(s->attachMethodParameters(f, {ParamNameRow{"n0", kParameterFinal}}),
                  "method parameters")) {
        return s;
    }
    const SemAnnotation inner = spec(kAnnoDesc, {{"i", svNum(SemElemValue::Form::Int, 3)}});
    const SemAnnotation pa    = spec(kAnnoDesc,
                                  {
                                      {"i", svNum(SemElemValue::Form::Int, 7)},
                                      {"how", svStr("hello")},
                                      {"tags", svArr({svNum(SemElemValue::Form::Int, 9),
                                                      svNum(SemElemValue::Form::Int, 8)})},
                                      {"longV",
                                       svNum(SemElemValue::Form::Long, 123456789012345LL)},
                                      {"dbl", svDblBits(0x4004000000000000ull)},
                                      {"cls", svClass("I")},
                                      {"op", svEnum("Lbu/Op;", "PLUS")},
                                      {"nested", svNested(inner)},
                                  });
    if (!expectOk(
            s->attachParameterAnnotations(f, true, {std::vector<SemAnnotation>{pa}}),
            "parameter annotations")
        || !expectOk(s->attachAnnotations(
                         f, true,
                         {spec(kAnnoDesc, {{"i", svNum(SemElemValue::Form::Int, 1)}})}),
                     "method visible")
        || !expectOk(s->attachAnnotations(f, false, {spec(kInvisDesc)}),
                     "method invisible")
        || !expectOk(s->attachClassAnnotations(
                         true,
                         {spec(kAnnoDesc,
                               {{"how", svStr("alpha")},
                                {"i", svNum(SemElemValue::Form::Int, 1701)}})}),
                     "class visible")) {
        return s;
    }
    auto fk = s->addField({"K", "I", kAccPublic | kAccStatic | kAccFinal});
    auto fm = s->addField({"S2", "Ljava/lang/String;", kAccPublic | kAccStatic | kAccFinal});
    if (!expectOk(fk, "field K") || !expectOk(fm, "field S2")) {
        return s;
    }
    if (!expectOk(s->attachConstantValue(fk.unwrap(), cvInt(42)), "cv K")
        || !expectOk(s->attachConstantValue(fm.unwrap(), cvStr("SS")), "cv S2")) {
        return s;
    }
    return s;
}

/// 顶层字节（writeOk 失败自带记账）。
Bytes bytesOf(const SessionPtr& s)
{
    if (s == nullptr) {
        return {};
    }
    return writeOk(s->backing());
}

/// decodeAttr 后取 typed 体（失败记账返回 nullptr）。
template <typename T>
const T* decodeTyped(ClassFile& cf, AttrSlot* slot)
{
    if (slot == nullptr) {
        ADD_FAILURE() << "attr slot not found";
        return nullptr;
    }
    auto d = cf.decodeAttr(*slot);
    if (!d.is_ok()) {
        ADD_FAILURE() << "decodeAttr failed";
        return nullptr;
    }
    return static_cast<const T*>(slot->peek());
}

/// 成员（方法/字段）上按名字找属性槽；memberName/memberDesc 双匹配定位。
AttrSlot* memberAttrSlot(ClassFile& cf, bool isMethod, std::string_view memberName,
                         std::string_view memberDesc, std::string_view attrName)
{
    auto& members = isMethod ? cf.methods : cf.fields;
    for (MemberInfo& m : members) {
        if (!cf.cp.utf8Equals(m.nameCp, memberName)
            || !cf.cp.utf8Equals(m.descCp, memberDesc)) {
            continue;
        }
        for (AttrSlot& slot : m.attrs) {
            if (cf.cp.utf8Equals(slot.nameCp, attrName)) {
                return &slot;
            }
        }
    }
    return nullptr;
}

/// 类属性表按名字找槽。
AttrSlot* classAttrSlot(ClassFile& cf, std::string_view attrName)
{
    for (AttrSlot& slot : cf.attrs) {
        if (cf.cp.utf8Equals(slot.nameCp, attrName)) {
            return &slot;
        }
    }
    return nullptr;
}

/// CP 语义快捷读：Integer 值 / Utf8 直索引与 String、Class 条目名比对 /
/// Wide 位模式值。
ca::i32 intAt(const ClassFile& cf, ca::u32 cp)
{
    return cf.cp.at(cp).ival;
}

/// 直接 Utf8 索引内容比对（typeCp / typeNameCp / constNameCp / row.nameCp）。
bool utf8Eq(const ClassFile& cf, ca::u32 idx, std::string_view lit)
{
    return cf.cp.utf8Equals(idx, lit);
}

/// String/Class 条目的转引内容比对（条目 .a = Utf8 名索引）。
bool strEq(const ClassFile& cf, ca::u32 cp, std::string_view lit)
{
    return cf.cp.utf8Equals(cf.cp.at(cp).a, lit);
}

bool bitsAt(const ClassFile& cf, ca::u32 cp, ca::u64 expect)
{
    return static_cast<ca::u64>(cf.cp.at(cp).lval) == expect;
}

/// 输出按行分成 vector（归一化后的 stdout 逐行比对入口）。
std::vector<std::string> splitLines(const std::string& s)
{
    std::vector<std::string> out;
    std::string cur;
    for (const char c : s) {
        if (c == '\n') {
            if (!cur.empty()) {
                out.push_back(cur);
            }
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) {
        out.push_back(cur);
    }
    return out;
}

}   // namespace

// ---------------------------------------------------------------------------
// 纯模型门
// ---------------------------------------------------------------------------

TEST(ClassBuildAttrs, AnnoClassSynthReadback)
{
    const Bytes bytes = bytesOf(buildAnnoSession());
    ASSERT_FALSE(bytes.empty());
    // 重读解码：类级注解 + 七个 default 全 typed 落位。
    auto cf = readOk(bytes);
    ASSERT_NE(cf, nullptr);
    // 类级 @Retention(RUNTIME)。
    const auto* ra = decodeTyped<AnnotationsAttr>(*cf, classAttrSlot(*cf, "RuntimeVisibleAnnotations"));
    ASSERT_NE(ra, nullptr);
    ASSERT_EQ(ra->annos.size(), 1u);
    EXPECT_TRUE(utf8Eq(*cf, ra->annos[0].typeCp, "Ljava/lang/annotation/Retention;"));
    ASSERT_EQ(ra->annos[0].values.size(), 1u);
    const AnnotationsAttr::ElemValue& rv = ra->annos[0].values[0].second;
    EXPECT_EQ(rv.tag, 'e');
    EXPECT_TRUE(utf8Eq(*cf, rv.typeNameCp, "Ljava/lang/annotation/RetentionPolicy;"));
    EXPECT_TRUE(utf8Eq(*cf, rv.constNameCp, "RUNTIME"));
    // i → 'I' 7。
    const auto* di = decodeTyped<AnnotationDefaultAttr>(
        *cf, memberAttrSlot(*cf, true, "i", "()I", "AnnotationDefault"));
    ASSERT_NE(di, nullptr);
    EXPECT_EQ(di->defaultValue.tag, 'I');
    EXPECT_EQ(intAt(*cf, di->defaultValue.cp), 7);
    // how → 's' "hello"。
    const auto* dhow = decodeTyped<AnnotationDefaultAttr>(
        *cf, memberAttrSlot(*cf, true, "how", "()Ljava/lang/String;", "AnnotationDefault"));
    ASSERT_NE(dhow, nullptr);
    EXPECT_EQ(dhow->defaultValue.tag, 's');
    EXPECT_TRUE(utf8Eq(*cf, dhow->defaultValue.cp, "hello"));   // 's' → Utf8 直接
    // tags → '[' 两项 'I' 9/8。
    const auto* dtags = decodeTyped<AnnotationDefaultAttr>(
        *cf, memberAttrSlot(*cf, true, "tags", "()[I", "AnnotationDefault"));
    ASSERT_NE(dtags, nullptr);
    ASSERT_EQ(dtags->defaultValue.tag, '[');
    ASSERT_EQ(dtags->defaultValue.items.size(), 2u);
    EXPECT_EQ(intAt(*cf, dtags->defaultValue.items[0].cp), 9);
    EXPECT_EQ(intAt(*cf, dtags->defaultValue.items[1].cp), 8);
    // longV → 'J'（Wide 条目主槽值）。
    const auto* dlong = decodeTyped<AnnotationDefaultAttr>(
        *cf, memberAttrSlot(*cf, true, "longV", "()J", "AnnotationDefault"));
    ASSERT_NE(dlong, nullptr);
    EXPECT_EQ(dlong->defaultValue.tag, 'J');
    EXPECT_EQ(static_cast<ca::i64>(cf->cp.at(dlong->defaultValue.cp).lval),
              123456789012345LL);
    // dbl → 'D' 位模式。
    const auto* ddbl = decodeTyped<AnnotationDefaultAttr>(
        *cf, memberAttrSlot(*cf, true, "dbl", "()D", "AnnotationDefault"));
    ASSERT_NE(ddbl, nullptr);
    EXPECT_EQ(ddbl->defaultValue.tag, 'D');
    EXPECT_TRUE(bitsAt(*cf, ddbl->defaultValue.cp, 0x4004000000000000ull));
    // cls → 'c'，const_value_index 指向 Utf8（类型描述符语义，javac 同形）。
    const auto* dcls = decodeTyped<AnnotationDefaultAttr>(
        *cf,
        memberAttrSlot(*cf, true, "cls", "()Ljava/lang/Class;", "AnnotationDefault"));
    ASSERT_NE(dcls, nullptr);
    EXPECT_EQ(dcls->defaultValue.tag, 'c');
    EXPECT_EQ(cf->cp.tagOf(dcls->defaultValue.cp), CpTag::Utf8);
    EXPECT_TRUE(utf8Eq(*cf, dcls->defaultValue.cp, "I"));
    // op → 'e' 枚举；typeName/constName 双槽。
    const auto* dop = decodeTyped<AnnotationDefaultAttr>(
        *cf, memberAttrSlot(*cf, true, "op", "()Lbu/Op;", "AnnotationDefault"));
    ASSERT_NE(dop, nullptr);
    EXPECT_EQ(dop->defaultValue.tag, 'e');
    EXPECT_TRUE(utf8Eq(*cf, dop->defaultValue.typeNameCp, "Lbu/Op;"));
    EXPECT_TRUE(utf8Eq(*cf, dop->defaultValue.constNameCp, "PLUS"));
    // 未触碰槽的例行重写恒等（合成体重编码确定性 + copy-through）。
    EXPECT_EQ(writeOk(*cf), bytes);
}

TEST(ClassBuildAttrs, HostSynthReadbackAndCompact)
{
    SessionPtr s = buildHostSession();
    ASSERT_NE(s, nullptr);
    const Bytes raw = bytesOf(s);
    ASSERT_FALSE(raw.empty());
    // 注解族四件在压缩封闭集：合成类可压缩（收编），写回重读语义保持。
    ASSERT_TRUE(jbc::ir::compactConstantPool(*s).is_ok());
    const Bytes compacted = bytesOf(s);
    ASSERT_FALSE(compacted.empty());

    for (const Bytes& variant : {raw, compacted}) {
        auto cf = readOk(variant);
        ASSERT_NE(cf, nullptr);
        // 参数注解：一条注解 + 全形态元素 + 嵌套。
        const auto* pva = decodeTyped<ParameterAnnotationsAttr>(
            *cf, memberAttrSlot(*cf, true, "f", "(Ljava/lang/String;)I",
                                "RuntimeVisibleParameterAnnotations"));
        ASSERT_NE(pva, nullptr);
        ASSERT_EQ(pva->parameters.size(), 1u);
        ASSERT_EQ(pva->parameters[0].size(), 1u);
        const AnnotationsAttr::Annotation& a = pva->parameters[0][0];
        EXPECT_TRUE(utf8Eq(*cf, a.typeCp, kAnnoDesc));
        ASSERT_EQ(a.values.size(), 8u);
        EXPECT_EQ(a.values[0].second.tag, 'I');                       // i=7
        EXPECT_EQ(intAt(*cf, a.values[0].second.cp), 7);
        EXPECT_EQ(a.values[1].second.tag, 's');                       // how=hello
        EXPECT_TRUE(utf8Eq(*cf, a.values[1].second.cp, "hello"));   // 's' → Utf8
        ASSERT_EQ(a.values[2].second.tag, '[');                       // tags
        ASSERT_EQ(a.values[2].second.items.size(), 2u);
        EXPECT_EQ(intAt(*cf, a.values[2].second.items[0].cp), 9);
        EXPECT_EQ(intAt(*cf, a.values[2].second.items[1].cp), 8);
        EXPECT_EQ(a.values[3].second.tag, 'J');                       // longV
        EXPECT_EQ(static_cast<ca::i64>(cf->cp.at(a.values[3].second.cp).lval),
                  123456789012345LL);
        EXPECT_EQ(a.values[4].second.tag, 'D');                       // dbl
        EXPECT_TRUE(bitsAt(*cf, a.values[4].second.cp, 0x4004000000000000ull));
        EXPECT_EQ(a.values[5].second.tag, 'c');                       // cls → int.class
        EXPECT_TRUE(utf8Eq(*cf, a.values[5].second.cp, "I"));
        EXPECT_EQ(a.values[6].second.tag, 'e');                       // op enum
        EXPECT_TRUE(utf8Eq(*cf, a.values[6].second.typeNameCp, "Lbu/Op;"));
        EXPECT_TRUE(utf8Eq(*cf, a.values[6].second.constNameCp, "PLUS"));
        ASSERT_NE(a.values[7].second.nested, nullptr);                // nested @Anno
        EXPECT_TRUE(utf8Eq(*cf, a.values[7].second.nested->typeCp, kAnnoDesc));
        ASSERT_EQ(a.values[7].second.nested->values.size(), 1u);
        EXPECT_EQ(intAt(*cf, a.values[7].second.nested->values[0].second.cp), 3);
        // 参数名/旗语：n0(final)，行数 = 描述符 arity（硬契约，见 attr_build 头注）。
        const auto* mp = decodeTyped<MethodParametersAttr>(
            *cf, memberAttrSlot(*cf, true, "f", "(Ljava/lang/String;)I",
                                "MethodParameters"));
        ASSERT_NE(mp, nullptr);
        ASSERT_EQ(mp->rows.size(), 1u);
        EXPECT_TRUE(utf8Eq(*cf, mp->rows[0].nameCp, "n0"));
        EXPECT_EQ(mp->rows[0].accessFlags, kParameterFinal);
        // 方法级可见注解：@Anno(i=1)。
        const auto* mva = decodeTyped<AnnotationsAttr>(
            *cf, memberAttrSlot(*cf, true, "f", "(Ljava/lang/String;)I",
                                "RuntimeVisibleAnnotations"));
        ASSERT_NE(mva, nullptr);
        ASSERT_EQ(mva->annos.size(), 1u);
        EXPECT_TRUE(utf8Eq(*cf, mva->annos[0].typeCp, kAnnoDesc));
        EXPECT_EQ(intAt(*cf, mva->annos[0].values[0].second.cp), 1);
        // 不可见名独立挂载成功（同名可见/不可见互不冲突）。
        const auto* mia = decodeTyped<AnnotationsAttr>(
            *cf, memberAttrSlot(*cf, true, "f", "(Ljava/lang/String;)I",
                                "RuntimeInvisibleAnnotations"));
        ASSERT_NE(mia, nullptr);
        EXPECT_TRUE(utf8Eq(*cf, mia->annos[0].typeCp, kInvisDesc));
        // 类级注解：@Anno(how="alpha", i=1701)。
        const auto* cva = decodeTyped<AnnotationsAttr>(
            *cf, classAttrSlot(*cf, "RuntimeVisibleAnnotations"));
        ASSERT_NE(cva, nullptr);
        ASSERT_EQ(cva->annos.size(), 1u);
        ASSERT_EQ(cva->annos[0].values.size(), 2u);
        EXPECT_TRUE(utf8Eq(*cf, cva->annos[0].values[0].second.cp, "alpha"));
        EXPECT_EQ(intAt(*cf, cva->annos[0].values[1].second.cp), 1701);
        // 恒值字段：int 42 / String "SS"。
        const auto* ck = decodeTyped<ConstantValueAttr>(
            *cf, memberAttrSlot(*cf, false, "K", "I", "ConstantValue"));
        ASSERT_NE(ck, nullptr);
        EXPECT_EQ(intAt(*cf, ck->constantValueCp), 42);
        const auto* cs2 = decodeTyped<ConstantValueAttr>(
            *cf, memberAttrSlot(*cf, false, "S2", "Ljava/lang/String;", "ConstantValue"));
        ASSERT_NE(cs2, nullptr);
        EXPECT_EQ(cf->cp.tagOf(cs2->constantValueCp), CpTag::String);
        EXPECT_TRUE(strEq(*cf, cs2->constantValueCp, "SS"));
    }
}

TEST(ClassBuildAttrs, MountAndSpecFailures)
{
    // ---- 重复挂载：单例规则收口（与 LNT 多条的例外对照）。
    {
        SessionPtr s = buildHostSession();
        ASSERT_NE(s, nullptr);
        const auto f = s->unit().findMethodId("f", "(Ljava/lang/String;)I");
        const auto fk = s->unit().findFieldId("K", "I");
        ASSERT_FALSE(f == jbc::ir::kNoMethodId);
        ASSERT_FALSE(fk == jbc::ir::kNoFieldId);
        EXPECT_EQ(s->attachConstantValue(fk, cvInt(1)).unwrap_err(),
                  jbc::ir::LowerError::AlreadyMounted);
        // arity 失配先行于单例判（规格形状是更深一层的失败面）。非静态方法
        // 的「arity+1」接收者形态合法（JVMS 4.7.24）：2 行先过 arity、撞上
        // 单例判；越界到 arity+2 才是 InvalidSeed。
        EXPECT_EQ(s->attachMethodParameters(f, {ParamNameRow{"b0", 0}, ParamNameRow{"b1", 0}})
                      .unwrap_err(),
                  jbc::ir::LowerError::AlreadyMounted);
        EXPECT_EQ(s->attachMethodParameters(
                      f,
                      {ParamNameRow{"b0", 0}, ParamNameRow{"b1", 0}, ParamNameRow{"b2", 0}})
                      .unwrap_err(),
                  jbc::ir::LowerError::InvalidSeed);
        EXPECT_EQ(s->attachMethodParameters(f, {ParamNameRow{"b2", 0}}).unwrap_err(),
                  jbc::ir::LowerError::AlreadyMounted);
        EXPECT_EQ(s->attachAnnotations(f, true, {spec(kAnnoDesc)}).unwrap_err(),
                  jbc::ir::LowerError::AlreadyMounted);
        // 不可见名已由构建期挂上，同名重复同样显式拒绝。
        EXPECT_EQ(s->attachAnnotations(f, false, {spec(kInvisDesc)}).unwrap_err(),
                  jbc::ir::LowerError::AlreadyMounted);
        EXPECT_EQ(s->attachParameterAnnotations(f, true, {{spec(kAnnoDesc)}}).unwrap_err(),
                  jbc::ir::LowerError::AlreadyMounted);
        EXPECT_EQ(s->attachAnnotationDefault(f, svNum(SemElemValue::Form::Int, 9))
                      .is_ok(),
                  true);
        EXPECT_EQ(s->attachAnnotationDefault(f, svNum(SemElemValue::Form::Int, 10))
                      .unwrap_err(),
                  jbc::ir::LowerError::AlreadyMounted);
        // Code 拒收（方法体归 promote→lower 管径）。
        EXPECT_EQ(s->attachAttr(f, "Code", std::make_unique<RawAttr>(Bytes{}))
                      .unwrap_err(),
                  jbc::ir::LowerError::BadMount);
        // LNT 条许多条：挂两条成功。
        EXPECT_TRUE(s->attachAttr(f, "LineNumberTable", std::make_unique<RawAttr>(Bytes{1}))
                        .is_ok());
        EXPECT_TRUE(
            s->attachAttr(f, "LineNumberTable", std::make_unique<RawAttr>(Bytes{2}))
                .is_ok());
    }
    // ---- u1 行数域 + 死句柄。
    {
        SessionPtr s = seedSession(kAccPublic | kAccSuper, "bu/B");
        ASSERT_NE(s, nullptr);
        auto mf = s->addMethod({"m", "(I)V", kAccPublic | kAccAbstract});
        if (!expectOk(mf, "add m")) {
            return;
        }
        const auto m = mf.unwrap();
        // parameters_count / num_parameters 是 u1（域宽先行于 arity 语义）。
        std::vector<ParamNameRow> tooMany(256, ParamNameRow{"", 0});
        EXPECT_EQ(s->attachMethodParameters(m, tooMany).unwrap_err(),
                  jbc::ir::LowerError::DomainOverflow);
        std::vector<std::vector<SemAnnotation>> g256(
            256, std::vector<SemAnnotation>{});
        EXPECT_EQ(s->attachParameterAnnotations(m, true, g256).unwrap_err(),
                  jbc::ir::LowerError::DomainOverflow);
        // arity 硬契约：行数/参数组数超出「描述符参数个数 + 接收者行」上限
        // → InvalidSeed（m 非静态，2 行是合法接收者形态，会真的落位）。
        EXPECT_EQ(s->attachMethodParameters(
                      m,
                      {ParamNameRow{"a", 0}, ParamNameRow{"b", 0}, ParamNameRow{"c", 0}})
                      .unwrap_err(),
                  jbc::ir::LowerError::InvalidSeed);
        EXPECT_EQ(s->attachParameterAnnotations(m, true, {}).unwrap_err(),
                  jbc::ir::LowerError::InvalidSeed);
        // 合法单行（无名 + SYNTHETIC → name_index 0）与合法单组落位。
        EXPECT_TRUE(
            s->attachMethodParameters(m, {ParamNameRow{"", kParameterSynthetic}})
                .is_ok());
        EXPECT_TRUE(s->attachParameterAnnotations(m, true, {{spec(kInvisDesc)}})
                        .is_ok());
        // 摘除后旧句柄结构性失配。
        EXPECT_TRUE(s->removeMethod(m).is_ok());
        EXPECT_EQ(s->attachMethodParameters(m, {}).unwrap_err(),
                  jbc::ir::LowerError::InvalidHandle);
    }
    // ---- 坏规格 + 深度闸（类级挂载免建成员；全新会话保证探针合法序）。
    {
        SessionPtr s = seedSession(kAccPublic | kAccSuper, "bu/B");
        ASSERT_NE(s, nullptr);
        // type_index 非 L;类场域（首字符/尾字符双向）。
        EXPECT_EQ(s->attachClassAnnotations(true, {spec("I")}).unwrap_err(),
                  jbc::ir::LowerError::InvalidSeed);
        EXPECT_EQ(s->attachClassAnnotations(true, {spec("Lbu/Anno;X")}).unwrap_err(),
                  jbc::ir::LowerError::InvalidSeed);   // 尾字符非 ';'
        // 空元素名 / 空数组 / 枚举类型名非描述符。
        EXPECT_EQ(s->attachClassAnnotations(
                      true, {spec(kAnnoDesc, {{"", svNum(SemElemValue::Form::Int, 0)}})})
                      .unwrap_err(),
                  jbc::ir::LowerError::InvalidSeed);
        EXPECT_EQ(s->attachClassAnnotations(
                      true, {spec(kAnnoDesc, {{"v", svArr({})}})})
                      .unwrap_err(),
                  jbc::ir::LowerError::InvalidSeed);
        EXPECT_EQ(s->attachClassAnnotations(
                      true,
                      {spec(kAnnoDesc, {{"v", svEnum("bu/Op", "PLUS")}})})
                      .unwrap_err(),
                  jbc::ir::LowerError::InvalidSeed);
        // 深度闸：200 层嵌套注解规格 → count 遍历即拒绝（InvalidSeed）。
        SemAnnotation deep = spec(kAnnoDesc, {{"i", svNum(SemElemValue::Form::Int, 1)}});
        for (int i = 0; i < 200; ++i) {
            deep = spec(kAnnoDesc, {{"k", svNested(deep)}});
        }
        EXPECT_EQ(s->attachClassAnnotations(true, {deep}).unwrap_err(),
                  jbc::ir::LowerError::InvalidSeed);
        // 不合法规格不落半截：换合法规格仍能挂载成功（属性表未被污染）。
        EXPECT_TRUE(s->attachClassAnnotations(true, {spec(kAnnoDesc)}).is_ok());
    }
}

TEST(ClassBuildAttrs, ChangeSetBookkeeping)
{
    SessionPtr s = buildHostSession();
    ASSERT_NE(s, nullptr);
    const jbc::ir::ChangeSet& cs = s->changeSet();
    // 类级 seed 会话从零建出 → Created 优先。
    EXPECT_EQ(cs.classChange(), jbc::ir::ClassChange::Created);
    const auto members = cs.memberChanges();
    bool sawMethodF = false;
    for (const auto& mc : members) {
        EXPECT_TRUE(mc.created);
        EXPECT_TRUE(mc.metadataChanged);   // 挂载即实改
        if (mc.member == jbc::ir::MemberKind::Method) {
            EXPECT_TRUE(mc.desc == "(Ljava/lang/String;)I");
            sawMethodF = mc.name == "f";
        } else {
            EXPECT_TRUE(mc.name == "K" || mc.name == "S2");
        }
    }
    EXPECT_TRUE(sawMethodF);
    // 原始记录面：最后一条是 S2 的 MemberMetadata（挂载序记账）。
    ASSERT_FALSE(cs.records().empty());
    EXPECT_EQ(cs.records().back().kind, jbc::ir::ChangeKind::MemberMetadata);
    EXPECT_TRUE(cs.records().back().name == "S2");
}

// ---------------------------------------------------------------------------
// JVM 真跑门（JDK 在位执行，缺席 skip）
// ---------------------------------------------------------------------------

TEST(ClassBuildAttrs, JvmReflectionGate)
{
    const Bytes annoBytes = bytesOf(buildAnnoSession());
    ASSERT_FALSE(annoBytes.empty());
    SessionPtr sh = buildHostSession();
    ASSERT_NE(sh, nullptr);
    ASSERT_TRUE(jbc::ir::compactConstantPool(*sh).is_ok());
    const Bytes hostBytes = bytesOf(sh);
    ASSERT_FALSE(hostBytes.empty());

    // bu/Op 枚举用真实 Java 源同场编译（合成注解 ↔ javac 产物互操作）。
    const fs::path opSrc =
        fs::temp_directory_path() / ("Op.java");   // public 类型必须与文件名同形
    {
        std::ofstream out(opSrc, std::ios::binary | std::ios::trunc);
        out << "package bu;\npublic enum Op { PLUS, MINUS }\n";
    }
    test::jdk::RunnerSpec spec;
    spec.classes                        = {{"bu/Anno", annoBytes}, {"bu/C", hostBytes}};
    spec.extraJavaSources               = {opSrc};
    spec.runnerBody                     = R"java(
try {
java.lang.reflect.Method f = bu.C.class.getMethod("f", String.class);
java.lang.reflect.Parameter[] ps = f.getParameters();
System.out.println("pn0=" + ps[0].getName());
System.out.println("pn0f=" + ((ps[0].getModifiers() & 0x10) != 0));
bu.Anno anno = (bu.Anno) f.getParameterAnnotations()[0][0];
System.out.println("ai=" + anno.i());
System.out.println("ahow=" + anno.how());
System.out.println("atags=" + java.util.Arrays.toString(anno.tags()));
System.out.println("along=" + anno.longV());
System.out.println("adbl=" + anno.dbl());
System.out.println("acls=" + (anno.cls() == int.class));
System.out.println("aop=" + anno.op());
System.out.println("ainner=" + anno.nested().i());
System.out.println("mai=" + f.getAnnotation(bu.Anno.class).i());
System.out.println("cahow=" + bu.C.class.getAnnotation(bu.Anno.class).how());
System.out.println("cai=" + bu.C.class.getAnnotation(bu.Anno.class).i());
System.out.println("di=" + bu.Anno.class.getMethod("i").getDefaultValue());
System.out.println("dhow=" + bu.Anno.class.getMethod("how").getDefaultValue());
System.out.println("dtags="
    + java.util.Arrays.toString(
        (int[]) bu.Anno.class.getMethod("tags").getDefaultValue()));
System.out.println("dop=" + bu.Anno.class.getMethod("op").getDefaultValue());
System.out.println(
    "dcls=" + (bu.Anno.class.getMethod("cls").getDefaultValue() == int.class));
System.out.println("ddbl=" + bu.Anno.class.getMethod("dbl").getDefaultValue());
System.out.println("dk=" + bu.C.class.getDeclaredField("K").get(null));
System.out.println("ds=" + bu.C.class.getDeclaredField("S2").get(null));
} catch (Throwable t) {
t.printStackTrace();
}
)java";

    std::string got;
    if (!test::jdk::runRunnerChecked(test::jdk::probeJdk(), spec, &got)) {
        return;
    }
    const std::vector<std::string> lines = splitLines(got);
    const std::vector<std::string> expect = {
        "pn0=n0",      "pn0f=true",
        "ai=7",        "ahow=hello",  "atags=[9, 8]", "along=123456789012345",
        "adbl=2.5",    "acls=true",   "aop=PLUS",     "ainner=3",
        "mai=1",       "cahow=alpha", "cai=1701",
        "di=7",        "dhow=hello",  "dtags=[9, 8]", "dop=PLUS",
        "dcls=true",   "ddbl=2.5",
        "dk=42",       "ds=SS",
    };
    ASSERT_EQ(lines.size(), expect.size()) << "full output:\n" << got;
    for (size_t i = 0; i < expect.size(); ++i) {
        EXPECT_EQ(lines[i], expect[i]);
    }
}
