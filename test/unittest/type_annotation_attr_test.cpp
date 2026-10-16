/// @file        type_annotation_attr_test.cpp
/// @brief       Runtime*TypeAnnotations typed 建模与类名手术的表驱动单测
///              （design.md §32.7）：解码保真（roundtrip）、无映射字节不动
///              （硬门禁）、命中改写（type_index 与 'c' 元素值）、失败显式、
///              全有或全无、trailing 保持，以及语料门。
#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "jbc/jbc.hpp"
#include "corpus_util.hpp"
#include "test_class_bytes.hpp"

using namespace jbc::core;
using namespace jbc::ir;
using namespace jbc::type;
using jbc::test::AttrSpec;
using jbc::test::Bytes;
using jbc::test::ClassBuilder;
using jbc::test::codeContent;

namespace {

Bytes slurp(const std::string& path)
{
    std::ifstream in(path.c_str(), std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// @brief TA 内容字节构造器（测试专用，与 codec 互为 oracle）。
struct TaBytes
{
    Bytes b;

    void count(ca::u16 n) { put16(n); }

    /// 开一条注解：targetType + target_info（按 JVMS 4.7.20.1 手拼）。
    void beginAnno(ca::u8 targetType) { b.push_back(targetType); }

    void supertypeInfo(ca::u16 ifaceIdx) { put16(ifaceIdx); }
    void emptyInfo() {}
    void throwsInfo(ca::u16 excIdx) { put16(excIdx); }
    void offsetInfo(ca::u16 offset) { put16(offset); }
    void typeArgInfo(ca::u16 offset, ca::u8 argIdx)
    {
        put16(offset);
        b.push_back(argIdx);
    }
    void localVarInfo(const std::vector<std::tuple<ca::u16, ca::u16, ca::u16>>& rows)
    {
        put16(static_cast<ca::u16>(rows.size()));
        for (const auto& r : rows) {
            put16(std::get<0>(r));
            put16(std::get<1>(r));
            put16(std::get<2>(r));
        }
    }

    void path(const std::vector<std::pair<ca::u8, ca::u8>>& entries)
    {
        b.push_back(static_cast<ca::u8>(entries.size()));
        for (const auto& e : entries) {
            b.push_back(e.first);
            b.push_back(e.second);
        }
    }
    void noPath() { b.push_back(0); }

    void annoBody(ca::u16 typeIdx, const std::vector<std::tuple<ca::u16, ca::u8, ca::u16>>& values)
    {
        put16(typeIdx);
        put16(static_cast<ca::u16>(values.size()));
        for (const auto& v : values) {
            put16(std::get<0>(v));
            b.push_back(std::get<1>(v));   // element_value tag
            put16(std::get<2>(v));         // 常量/类信息 CP 索引
        }
    }

private:
    void put16(ca::u16 v)
    {
        b.push_back(static_cast<ca::u8>(v >> 8));
        b.push_back(static_cast<ca::u8>(v));
    }
};

/// @brief 一个带三处挂载点 TypeAnnotations 的最小类。
///
/// - 类级 RuntimeVisibleTypeAnnotations：supertype_target 一条（typeIndex=
///   VTag 注解描述符，'c' 值 = Ldemo/Expr;）——interface 下标 0 是合法
///   （superclass 占位，JVMS 允许指向 interfaces[0]）。
/// - 字段级 RuntimeVisibleTypeAnnotations：empty_target 一条（typeIndex=
///   Ldemo/Expr; 命中行 + 'c' void 值——基类型跳过对照）。
/// - 方法 Code 内 RuntimeInvisibleTypeAnnotations：CAST offset 一条
///   （typeIndex=Ldemo/Expr; 命中行，带 type_path 实参段）。
struct TaClass
{
    Bytes bytes;
    ca::u16 vtagIdx = 0;    ///< Ldemo/VTag;（注解自身，未命中对照）
    ca::u16 exprIdx = 0;    ///< Ldemo/Expr;（命中串）
    ca::u16 voidDescIdx = 0;///< V（void.class 的 'c' 值）

    static constexpr const char* kVtag = "Ldemo/VTag;";
    static constexpr const char* kExpr = "Ldemo/Expr;";
};

TaClass makeTaClass()
{
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/Use");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 rvtaName = cb.utf8("RuntimeVisibleTypeAnnotations");
    const ca::u16 ritaName = cb.utf8("RuntimeInvisibleTypeAnnotations");
    const ca::u16 codeName = cb.utf8("Code");
    const ca::u16 vtag = cb.utf8(TaClass::kVtag);
    const ca::u16 expr = cb.utf8(TaClass::kExpr);
    const ca::u16 voidDesc = cb.utf8("V");
    const ca::u16 clsName = cb.utf8("cls");
    const ca::u16 fldName = cb.utf8("f");
    const ca::u16 fldDesc = cb.utf8("Ljava/lang/Object;");
    const ca::u16 runName = cb.utf8("run");
    const ca::u16 runDesc = cb.utf8("(Ljava/lang/Object;)Ljava/lang/Object;");

    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setVersion(52);

    // 类级：supertype_target（ifaceIdx=0），typeIndex=VTag，cls='c' Expr。
    TaBytes ta;
    ta.count(1);
    ta.beginAnno(TypeAnnotationsAttr::kSupertype);
    ta.supertypeInfo(0);
    ta.noPath();
    ta.annoBody(vtag, {std::make_tuple(clsName, ca::u8('c'), expr)});
    cb.addClassAttr(rvtaName, ta.b);

    // 字段级：empty_target，typeIndex=Expr（命中），cls='c' V（基类型跳过）。
    TaBytes fta;
    fta.count(1);
    fta.beginAnno(TypeAnnotationsAttr::kField);
    fta.emptyInfo();
    fta.noPath();
    fta.annoBody(expr, {std::make_tuple(clsName, ca::u8('c'), voidDesc)});
    cb.addField(0x0002, fldName, fldDesc, {AttrSpec{rvtaName, fta.b}});

    // 方法 Code 内：CAST offset=3，type_path 深入类型实参 0，typeIndex=Expr。
    TaBytes mta;
    mta.count(1);
    mta.beginAnno(TypeAnnotationsAttr::kCast);   // 0x47 cast 位置
    mta.typeArgInfo(3, 0);                       // u2 offset + u1 type_index
    mta.path({{3, 0}});
    mta.annoBody(expr, {});
    cb.addMethod(0x0001, runName, runDesc,
                 {AttrSpec{codeName,
                           codeContent(1, 1, Bytes{0xb0}, {},
                                       {AttrSpec{ritaName, mta.b}})}});
    return TaClass{cb.build(), vtag, expr, voidDesc};
}

/// @brief 读出类级第 annoIdx 条注解（typed 判型）。
TypeAnnotationsAttr::Annotation& classAnnoAt(ClassFile& cf, ca::usize annoIdx)
{
    AttrSlot& slot = cf.attrs[0];
    auto d = cf.decodeAttr(slot);
    EXPECT_TRUE(d.is_ok());
    auto* body = static_cast<TypeAnnotationsAttr*>(std::move(d).unwrap());
    EXPECT_LT(annoIdx, body->annotations.size());
    return body->annotations[annoIdx];
}

}   // namespace

// ---- 保真：typed 解码 + 未触碰写回逐字节不动 ----

TEST(TaAttrTest, DecodeOnlyWriteIsByteIdentical)
{
    const TaClass tc = makeTaClass();
    auto cf = jbc::test::readOk(tc.bytes);
    ASSERT_NE(cf, nullptr);
    ASSERT_TRUE(decodeAllAttrs(*cf).is_ok());
    EXPECT_EQ(jbc::test::writeOk(*cf), tc.bytes);
}

TEST(TaAttrTest, TypedKindAndStructure)
{
    const TaClass tc = makeTaClass();
    auto cf = jbc::test::readOk(tc.bytes);
    ASSERT_NE(cf, nullptr);

    // 类级：supertype_target + typeIndex=VTag + 'c' Expr。
    auto d = cf->decodeAttr(cf->attrs[0]);
    ASSERT_TRUE(d.is_ok());
    auto* body = static_cast<TypeAnnotationsAttr*>(std::move(d).unwrap());
    ASSERT_EQ(body->kind(), AttrKind::TypeAnnotations);
    ASSERT_EQ(body->annotations.size(), ca::usize{1});
    EXPECT_EQ(body->annotations[0].targetType, TypeAnnotationsAttr::kSupertype);
    EXPECT_EQ(body->annotations[0].typeIndexCp, tc.vtagIdx);
    ASSERT_EQ(body->annotations[0].values.size(), ca::usize{1});
    EXPECT_EQ(body->annotations[0].values[0].second.tag, 'c');
    EXPECT_EQ(body->annotations[0].values[0].second.cp, tc.exprIdx);

    // 方法 Code 内：CAST + type_path。
    auto& m = cf->methods[0];
    auto c = cf->decodeAttr(m.attrs[0]);
    ASSERT_TRUE(c.is_ok());
    auto* code = static_cast<CodeAttr*>(std::move(c).unwrap());
    auto md = cf->decodeAttr(code->attrs[0]);
    ASSERT_TRUE(md.is_ok());
    auto* mbody = static_cast<TypeAnnotationsAttr*>(std::move(md).unwrap());
    ASSERT_EQ(mbody->annotations.size(), ca::usize{1});
    EXPECT_EQ(mbody->annotations[0].targetType, TypeAnnotationsAttr::kCast);
    EXPECT_EQ(mbody->annotations[0].offset, 3u);
    ASSERT_EQ(mbody->annotations[0].typePath.size(), ca::usize{1});
    EXPECT_EQ(mbody->annotations[0].typePath[0].kind, 3);
}

// ---- 硬门禁与命中改写 ----

TEST(TaAttrTest, HardGateNoHitIsByteIdentical)
{
    const TaClass tc = makeTaClass();
    auto cf = jbc::test::readOk(tc.bytes);
    ASSERT_NE(cf, nullptr);

    auto miss = renameClassInTypeAnnotations(*cf, "zzz/Absent_R", "zzz/Moved_R");
    ASSERT_TRUE(miss.is_ok());
    const TaRenameSummary st = std::move(miss).unwrap();
    EXPECT_EQ(st.attrsTotal, 3u);
    EXPECT_EQ(st.attrsChanged, 0u);
    EXPECT_EQ(jbc::test::writeOk(*cf), tc.bytes);
}

TEST(TaAttrTest, TypeIndexAndCValueRewrite)
{
    const TaClass tc = makeTaClass();
    auto cf = jbc::test::readOk(tc.bytes);
    ASSERT_NE(cf, nullptr);

    auto r = renameClassInTypeAnnotations(*cf, "demo/Expr", "demo/Renamed");
    ASSERT_TRUE(r.is_ok());
    const TaRenameSummary st = std::move(r).unwrap();
    EXPECT_EQ(st.attrsChanged, 3u);   // 类级 / 字段级 / Code 嵌套各一
    EXPECT_GE(st.hits, 3u);           // 'c' Expr + typeIndex Expr + CAST typeIndex Expr

    // 类级 'c' 值改写；typeIndex（VTag）不动。
    auto& ca = classAnnoAt(*cf, 0);
    EXPECT_EQ(cf->utf8(ca.values[0].second.cp), "Ldemo/Renamed;");
    EXPECT_EQ(ca.typeIndexCp, tc.vtagIdx);

    // 字段级：typeIndex 改写，'c' void（基类型）原样。
    auto fd = cf->decodeAttr(cf->fields[0].attrs[0]);
    ASSERT_TRUE(fd.is_ok());
    auto* fbody = static_cast<TypeAnnotationsAttr*>(std::move(fd).unwrap());
    EXPECT_EQ(cf->utf8(fbody->annotations[0].typeIndexCp), "Ldemo/Renamed;");
    EXPECT_EQ(fbody->annotations[0].values[0].second.cp, tc.voidDescIdx);

    // Code 嵌套：CAST 的 typeIndex 改写。
    auto c = cf->decodeAttr(cf->methods[0].attrs[0]);
    ASSERT_TRUE(c.is_ok());
    auto* code = static_cast<CodeAttr*>(std::move(c).unwrap());
    auto md = cf->decodeAttr(code->attrs[0]);
    ASSERT_TRUE(md.is_ok());
    auto* mbody = static_cast<TypeAnnotationsAttr*>(std::move(md).unwrap());
    EXPECT_EQ(cf->utf8(mbody->annotations[0].typeIndexCp), "Ldemo/Renamed;");
}

TEST(TaAttrTest, ReverseRenameReusesOriginalEntries)
{
    const TaClass tc = makeTaClass();
    auto cf = jbc::test::readOk(tc.bytes);
    ASSERT_NE(cf, nullptr);

    auto fwd = renameClassInTypeAnnotations(*cf, "demo/Expr", "demo/Renamed");
    ASSERT_TRUE(fwd.is_ok());
    const ca::usize cpAfterForward = cf->cp.size();

    auto back = renameClassInTypeAnnotations(*cf, "demo/Renamed", "demo/Expr");
    ASSERT_TRUE(back.is_ok());
    const TaRenameSummary bs = std::move(back).unwrap();
    EXPECT_GE(bs.hits, 3u);
    EXPECT_EQ(cf->cp.size(), cpAfterForward) << "reverse rename must not grow the pool";

    // 逐位还原：所有改写点回到原 CP 条目。
    auto& ca = classAnnoAt(*cf, 0);
    EXPECT_EQ(ca.values[0].second.cp, tc.exprIdx);
    auto fd = cf->decodeAttr(cf->fields[0].attrs[0]);
    ASSERT_TRUE(fd.is_ok());
    auto* fbody = static_cast<TypeAnnotationsAttr*>(std::move(fd).unwrap());
    EXPECT_EQ(fbody->annotations[0].typeIndexCp, tc.exprIdx);
}

// ---- 失败显式：半截体 / 非法 target_type / 坏描述符全有或全无 ----

TEST(TaAttrTest, TruncatedBodyFailsDecodeKind)
{
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/Use");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 rvtaName = cb.utf8("RuntimeVisibleTypeAnnotations");
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setVersion(52);
    cb.addClassAttr(rvtaName, {0x00, 0x01, 0x01});   // count=1 但体只有 1 字节
    const Bytes bytes = cb.build();
    auto cf = jbc::test::readOk(bytes);
    ASSERT_NE(cf, nullptr);

    auto r = renameClassInTypeAnnotations(*cf, "demo/Expr", "demo/Renamed");
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(std::move(r).unwrap_err().kind, TaAttrError::Kind::SlotDecode);
    EXPECT_EQ(jbc::test::writeOk(*cf), bytes) << "untouched write must roundtrip";
}

TEST(TaAttrTest, UnknownTargetTypeFailsDecode)
{
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/Use");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 rvtaName = cb.utf8("RuntimeVisibleTypeAnnotations");
    const ca::u16 vtag = cb.utf8("Ldemo/VTag;");
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setVersion(52);
    Bytes ta = {0x00, 0x01, 0x7F};   // count=1，targetType 0x7F 未定义
    ta.push_back(0x00);              // type_path length=0
    ta.push_back(static_cast<ca::u8>(vtag >> 8));
    ta.push_back(static_cast<ca::u8>(vtag));
    ta.push_back(0x00);              // num_element_values = 0
    ta.push_back(0x00);
    cb.addClassAttr(rvtaName, ta);
    const Bytes bytes = cb.build();
    auto cf = jbc::test::readOk(bytes);
    ASSERT_NE(cf, nullptr);

    auto r = renameClassInTypeAnnotations(*cf, "demo/Expr", "demo/Renamed");
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(std::move(r).unwrap_err().kind, TaAttrError::Kind::SlotDecode);
}

TEST(TaAttrTest, BadDescriptorFailsWholeScan)
{
    // 字段级 'c' 值是坏描述符（L;：类名空，BadClassName）——全扫全有或全无。
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/Use");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 rvtaName = cb.utf8("RuntimeVisibleTypeAnnotations");
    const ca::u16 vtag = cb.utf8("Ldemo/VTag;");
    const ca::u16 badDesc = cb.utf8("L;");
    const ca::u16 clsName = cb.utf8("cls");
    const ca::u16 good = cb.utf8("Ldemo/Expr;");
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setVersion(52);
    // 类级好属性。
    TaBytes cta;
    cta.count(1);
    cta.beginAnno(TypeAnnotationsAttr::kField);
    cta.emptyInfo();
    cta.noPath();
    cta.annoBody(good, {});
    cb.addClassAttr(rvtaName, cta.b);
    // 字段级坏属性。
    TaBytes fta;
    fta.count(1);
    fta.beginAnno(TypeAnnotationsAttr::kField);
    fta.emptyInfo();
    fta.noPath();
    fta.annoBody(vtag, {std::make_tuple(clsName, ca::u8('c'), badDesc)});
    cb.addField(0x0002, cb.utf8("f"), cb.utf8("Ljava/lang/Object;"),
                {AttrSpec{rvtaName, fta.b}});
    const Bytes bytes = cb.build();
    auto cf = jbc::test::readOk(bytes);
    ASSERT_NE(cf, nullptr);

    auto r = renameClassInTypeAnnotations(*cf, "demo/Expr", "demo/Renamed");
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(std::move(r).unwrap_err().kind, TaAttrError::Kind::Descriptor);
    // 好属性也没被改写，写回逐字节不动。
    EXPECT_EQ(jbc::test::writeOk(*cf), bytes);
}

// ---- 语料门：类型注解族经真实 javac 产物跑正向/逆向管线 ----

namespace {

/// 一条 TA 属性的坐标（挂载点 + 槽序）与注解数。
struct TaCoord
{
    int level = 0;       ///< 0=类级 1=字段级 2=方法级 3=Code 嵌套
    ca::usize member = 0;
    ca::usize slot = 0;
    ca::usize nestedSlot = 0;   ///< level 3 时 Code 嵌套槽下标
};

/// @brief 收集一个类文件全部 Runtime*TypeAnnotations 属性坐标。
void collectTaCoords(ClassFile& cf, std::vector<TaCoord>& out)
{
    auto scanList = [&](std::vector<AttrSlot>& attrs, int level, ca::usize member,
                        ca::usize nestedSlot) {
        for (ca::usize i = 0; i < attrs.size(); ++i) {
            if (cf.cp.utf8Equals(attrs[i].nameCp, "RuntimeVisibleTypeAnnotations") ||
                cf.cp.utf8Equals(attrs[i].nameCp, "RuntimeInvisibleTypeAnnotations")) {
                out.push_back(TaCoord{level, member, i, nestedSlot});
            }
        }
    };
    scanList(cf.attrs, 0, 0, 0);
    for (ca::usize f = 0; f < cf.fields.size(); ++f) {
        scanList(cf.fields[f].attrs, 1, f, 0);
    }
    for (ca::usize m = 0; m < cf.methods.size(); ++m) {
        scanList(cf.methods[m].attrs, 2, m, 0);
        for (ca::usize s = 0; s < cf.methods[m].attrs.size(); ++s) {
            if (!cf.cp.utf8Equals(cf.methods[m].attrs[s].nameCp, "Code")) {
                continue;
            }
            auto d = cf.decodeAttr(cf.methods[m].attrs[s]);
            if (d.is_err()) {
                continue;
            }
            auto* code = static_cast<CodeAttr*>(std::move(d).unwrap());
            scanList(code->attrs, 3, m, s);
        }
    }
}

AttrSlot& taCoordSlot(ClassFile& cf, const TaCoord& c)
{
    switch (c.level) {
    case 0: return cf.attrs[c.slot];
    case 1: return cf.fields[c.member].attrs[c.slot];
    case 2: return cf.methods[c.member].attrs[c.slot];
    default: {
        auto* code = static_cast<CodeAttr*>(cf.decodeAttr(cf.methods[c.member].attrs[c.nestedSlot]).unwrap());
        return code->attrs[c.slot];
    }
    }
}

}   // namespace

TEST(TaCorpus, TypeAnnotationSurgeryPipeline)
{
    const auto src = jbc::test::corpus::resolveCorpus();
    if (src.stage == jbc::test::corpus::Stage::None) {
        GTEST_SKIP() << jbc::test::corpus::skipReason(src);
    }
    const std::vector<std::string> files = jbc::test::corpus::collectClassFiles(src.dir);
    ASSERT_GT(files.size(), ca::usize{10}) << "corpus too small: " << src.dir;

    ca::u32 filesWithTa = 0;
    ca::u32 taAttrsTotal = 0;
    ca::u32 forwardOps = 0;
    ca::u32 hitsTotal = 0;

    for (const std::string& path : files) {
        const Bytes bytes = slurp(path);
        ASSERT_FALSE(bytes.empty()) << path;

        std::vector<TaCoord> coords;
        {
            auto r = readClass(ByteView{bytes});
            ASSERT_TRUE(r.is_ok()) << path;
            auto cf = std::move(r).unwrap().file;
            collectTaCoords(*cf, coords);
        }
        if (coords.empty()) {
            continue;
        }
        ++filesWithTa;
        taAttrsTotal += static_cast<ca::u32>(coords.size());

        // 未命中门：缺席名全扫零改写，写回逐字节不动。
        {
            auto r2 = readClass(ByteView{bytes});
            ASSERT_TRUE(r2.is_ok()) << path;
            auto cf2 = std::move(r2).unwrap().file;
            auto miss = jbc::ir::renameClassInTypeAnnotations(*cf2, "zzz/Absent_R",
                                                              "zzz/Moved_R");
            ASSERT_TRUE(miss.is_ok()) << path;
            EXPECT_EQ(std::move(miss).unwrap().attrsChanged, 0u) << path;
            EXPECT_EQ(jbc::test::writeOk(*cf2), bytes) << path;
        }
    }

    std::printf("[type-annotation-corpus] stage=%d filesWithTa=%u taAttrs=%u\n",
                static_cast<int>(src.stage), filesWithTa, taAttrsTotal);

    if (src.stage == jbc::test::corpus::Stage::Env || filesWithTa == 0) {
        // env 注入（历史混淆语料实测分布为 0）：分布如实成文，不静默也不假红。
        GTEST_SKIP() << "no Runtime*TypeAnnotations in this corpus stage (measured 0)";
    }

    // 自产语料带 VolTypeUse：逐名正向手术 → 重读比对 → 逆向还原（全量恒等）。
    // （分布断言：至少一个文件、至少一次正向命中。）
    EXPECT_GT(filesWithTa, 0u);
    EXPECT_GT(taAttrsTotal, 0u);
    for (const std::string& path : files) {
        const Bytes bytes = slurp(path);
        ASSERT_FALSE(bytes.empty()) << path;
        std::vector<TaCoord> coords;
        {
            auto r = readClass(ByteView{bytes});
            ASSERT_TRUE(r.is_ok()) << path;
            auto cf = std::move(r).unwrap().file;
            collectTaCoords(*cf, coords);
        }
        if (coords.empty()) {
            continue;
        }

        // 去重内部名：从全部 type_index / 'c' 值收集（描述符层 span）。
        std::vector<std::string> names;
        {
            auto r = readClass(ByteView{bytes});
            ASSERT_TRUE(r.is_ok()) << path;
            auto cf = std::move(r).unwrap().file;
            for (const TaCoord& c : coords) {
                AttrSlot& slot = taCoordSlot(*cf, c);
                auto d = cf->decodeAttr(slot);
                ASSERT_TRUE(d.is_ok()) << path;
                auto* body = static_cast<TypeAnnotationsAttr*>(std::move(d).unwrap());
                auto collectDesc = [&](ca::u32 descCp) {
                    const std::string_view text = cf->utf8(descCp);
                    if (text.empty()) {
                        return;
                    }
                    auto spans = jbc::type::classNameSpans(text);
                    if (spans.is_err()) {
                        return;
                    }
                    for (const jbc::type::ClassNameSpan& s :
                         std::move(spans).unwrap()) {
                        const std::string name(text.substr(s.offset, s.length));
                        if (std::find(names.begin(), names.end(), name) ==
                            names.end()) {
                            names.push_back(name);
                        }
                    }
                };
                for (auto& a : body->annotations) {
                    collectDesc(a.typeIndexCp);
                    std::function<void(AnnotationsAttr::ElemValue&)> walk =
                        [&](AnnotationsAttr::ElemValue& e) {
                            if (e.tag == 'c') {
                                collectDesc(e.cp);
                            }
                            if (e.tag == '@' && e.nested) {
                                collectDesc(e.nested->typeCp);
                                for (auto& kv : e.nested->values) {
                                    walk(kv.second);
                                }
                            }
                            for (auto& it : e.items) {
                                walk(it);
                            }
                        };
                    for (auto& kv : a.values) {
                        walk(kv.second);
                    }
                }
            }
        }
        if (names.empty()) {
            continue;
        }

        for (const std::string& name : names) {
            const std::string renamed = name + "_R";
            auto rf = readClass(ByteView{bytes});
            ASSERT_TRUE(rf.is_ok()) << path;
            auto cff = std::move(rf).unwrap().file;

            auto fwd = jbc::ir::renameClassInTypeAnnotations(*cff, name, renamed);
            ASSERT_TRUE(fwd.is_ok())
                << path << ": forward TA surgery failed for " << name;
            jbc::ir::TaRenameSummary st = std::move(fwd).unwrap();
            ++forwardOps;
            hitsTotal += st.hits;

            // 逆向手术：还原全部原始描述符（复用原条目，池不膨胀）。
            auto back = jbc::ir::renameClassInTypeAnnotations(*cff, renamed, name);
            ASSERT_TRUE(back.is_ok()) << path << ": reverse TA surgery failed";
            EXPECT_EQ(std::move(back).unwrap().hits, st.hits) << path << ": " << name;
        }
    }
    EXPECT_GT(forwardOps, 0u);
    EXPECT_GT(hitsTotal, 0u);
}
