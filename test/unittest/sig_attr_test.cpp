/// @file        sig_attr_test.cpp
/// @brief       Signature 属性手术的表驱动单测：无映射字节不动（硬门禁）、
///              命中重写、跨形态（扁平链整段替换 / 嵌套链不命中）、未命中
///              零改写，以及 typed 模型 / trailing 保持 / 全有或全无 / 失败
///              显式等契约。
///
/// 类名手术的两形态（'.' 段链与 '$' 内嵌）语义归 jbc::type::signature
/// （signature_test 全产生式覆盖）；本文件钉的是**属性层接线**：定位、
/// 形态按挂载点、CP 去重落位、置脏写回与失败面。
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "jbc/jbc.hpp"
#include "test_class_bytes.hpp"

using namespace jbc::core;
using namespace jbc::ir;
using namespace jbc::type;
using jbc::test::AttrSpec;
using jbc::test::Bytes;
using jbc::test::ClassBuilder;
using jbc::test::codeContent;

namespace {

/// Signature 属性内容：u2 的 Utf8 索引。
Bytes sigContent(ca::u16 utf8Idx)
{
    return {static_cast<ca::u8>(utf8Idx >> 8), static_cast<ca::u8>(utf8Idx)};
}

/// @brief 一个带三级挂载点 Signature 属性的最小类。
///
/// 类签名引用 demo/Box（含类型参数）、方法签名引用两处、字段签名嵌套在
/// Map 实参里；再带一个无关属性确认手术不碰邻居。
struct SigClass
{
    Bytes bytes;
    ca::u16 classSigIdx = 0;
    ca::u16 methodSigIdx = 0;
    ca::u16 fieldSigIdx = 0;

    static constexpr const char* kClassSig =
        "<T:Ljava/lang/Object;>Ljava/lang/Object;Ldemo/Box<TT;>;";
    static constexpr const char* kMethodSig =
        "(TT;)Ljava/util/List<Ldemo/Box;>;";
    static constexpr const char* kFieldSig =
        "Ljava/util/Map<Ldemo/Box;Ljava/lang/String;>;";
};

SigClass makeSigClass()
{
    ClassBuilder cb;
    const ca::u16 thisIdx  = cb.utf8("p/Use");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 sigName  = cb.utf8("Signature");
    const ca::u16 weirdName = cb.utf8("WeirdCustomAttr");
    const ca::u16 classSig = cb.utf8(SigClass::kClassSig);
    const ca::u16 methodSig = cb.utf8(SigClass::kMethodSig);
    const ca::u16 fieldSig = cb.utf8(SigClass::kFieldSig);
    const ca::u16 fName    = cb.utf8("box");
    const ca::u16 fDesc    = cb.utf8("Ljava/lang/Object;");
    const ca::u16 mName    = cb.utf8("get");
    const ca::u16 mDesc    = cb.utf8("(Ljava/lang/Object;)Ljava/lang/Object;");

    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setVersion(52);
    cb.addField(0x0002, fName, fDesc, {AttrSpec{sigName, sigContent(fieldSig)}});
    cb.addMethod(0x0001, mName, mDesc,
                 {AttrSpec{sigName, sigContent(methodSig)},
                  AttrSpec{weirdName, Bytes{0x01, 0x02}}});
    cb.addClassAttr(sigName, sigContent(classSig));
    cb.addClassAttr(weirdName, Bytes{0xAA, 0xBB});
    return SigClass{cb.build(), classSig, methodSig, fieldSig};
}

/// @brief 取指定属性槽（按挂载点 + 序号）。
AttrSlot& slotAt(ClassFile& cf, int level, ca::usize i)
{
    switch (level) {
    case 0: return cf.attrs[i];
    case 1: return cf.methods[0].attrs[i];
    default: return cf.fields[0].attrs[i];
    }
}

/// @brief 读出某槽当前指向的签名串（typed SignatureAttr 的 u2 索引一跳）。
std::string_view sigStringOf(ClassFile& cf, AttrSlot& slot)
{
    auto d = cf.decodeAttr(slot);
    EXPECT_TRUE(d.is_ok());
    if (d.is_err()) {
        return {};
    }
    auto* attr = static_cast<SignatureAttr*>(std::move(d).unwrap());
    return cf.utf8(attr->signatureCp);
}

}   // namespace

// ---- 硬门禁：无映射（不触发手术）时字节逐字节不动 ----

TEST(SigAttrTest, DecodeOnlyWriteIsByteIdentical)
{
    const SigClass sc = makeSigClass();
    auto cf = jbc::test::readOk(sc.bytes);
    ASSERT_NE(cf, nullptr);
    ASSERT_TRUE(decodeAllAttrs(*cf).is_ok());
    // 全量解码（含 Signature typed 解码）后写回：未置脏即 copy-through。
    EXPECT_EQ(jbc::test::writeOk(*cf), sc.bytes);
}

TEST(SigAttrTest, TypedBodyExposesSignatureIndex)
{
    const SigClass sc = makeSigClass();
    auto cf = jbc::test::readOk(sc.bytes);
    ASSERT_NE(cf, nullptr);

    // Signature 是 typed 属性（AttrKind::Signature）：体 = u2 signature_index，
    // 手术面在该模型上读写索引；解码只推进 Untouched→Decoded，写回仍 copy。
    for (int level = 0; level < 3; ++level) {
        AttrSlot& slot = slotAt(*cf, level, 0);
        auto d = cf->decodeAttr(slot);
        ASSERT_TRUE(d.is_ok());
        AttrBody* body = std::move(d).unwrap();
        ASSERT_EQ(body->kind(), AttrKind::Signature) << "level " << level;
        auto* attr = static_cast<SignatureAttr*>(body);
        EXPECT_TRUE(attr->trailing.empty()) << "level " << level;
        const ca::u16 want = level == 0 ? sc.classSigIdx
                            : level == 1 ? sc.methodSigIdx : sc.fieldSigIdx;
        EXPECT_EQ(attr->signatureCp, want) << "level " << level;
        EXPECT_EQ(slot.state, AttrSlot::State::Decoded);
        EXPECT_FALSE(slot.needsReencode());
    }
    EXPECT_EQ(jbc::test::writeOk(*cf), sc.bytes);
}

// ---- 命中重写 ----

TEST(SigAttrTest, SweepRewritesHitsAndReparses)
{
    const SigClass sc = makeSigClass();
    auto cf = jbc::test::readOk(sc.bytes);
    ASSERT_NE(cf, nullptr);

    auto r = renameClassInSignatureAttrs(*cf, "demo/Box", "demo/Renamed");
    ASSERT_TRUE(r.is_ok());
    const SigAttrRenameSummary st = std::move(r).unwrap();
    EXPECT_EQ(st.attrsTotal, 3u);
    EXPECT_EQ(st.attrsChanged, 3u);
    EXPECT_EQ(st.hits, 3u);   // 类 1 + 方法 1 + 字段 1

    // 三级槽全部置脏，邻居属性不受影响。
    for (int level = 0; level < 3; ++level) {
        EXPECT_EQ(slotAt(*cf, level, 0).state, AttrSlot::State::Modified) << "level " << level;
    }

    const Bytes out = jbc::test::writeOk(*cf);
    auto cf2 = jbc::test::readOk(out);
    ASSERT_NE(cf2, nullptr);
    EXPECT_EQ(sigStringOf(*cf2, slotAt(*cf2, 0, 0)),
              "<T:Ljava/lang/Object;>Ljava/lang/Object;Ldemo/Renamed<TT;>;");
    EXPECT_EQ(sigStringOf(*cf2, slotAt(*cf2, 1, 0)),
              "(TT;)Ljava/util/List<Ldemo/Renamed;>;");
    EXPECT_EQ(sigStringOf(*cf2, slotAt(*cf2, 2, 0)),
              "Ljava/util/Map<Ldemo/Renamed;Ljava/lang/String;>;");
}

TEST(SigAttrTest, SingleSlotPrimitiveRewritesAndKeepsNeighbors)
{
    const SigClass sc = makeSigClass();
    auto cf = jbc::test::readOk(sc.bytes);
    ASSERT_NE(cf, nullptr);

    // 方法级槽（methods[0].attrs[0]），形态显式给出。
    auto r = renameClassInSignatureAttr(*cf, cf->methods[0].attrs[0],
                                        SignatureForm::Method, "demo/Box", "demo/Renamed");
    ASSERT_TRUE(r.is_ok());
    const SigAttrRename res = std::move(r).unwrap();
    EXPECT_EQ(res.hits, 1u);
    EXPECT_TRUE(res.changed);
    EXPECT_EQ(cf->methods[0].attrs[0].state, AttrSlot::State::Modified);
    // 类级/字段级未被触碰（保持 Untouched）。
    EXPECT_EQ(cf->attrs[0].state, AttrSlot::State::Untouched);
    EXPECT_EQ(cf->fields[0].attrs[0].state, AttrSlot::State::Untouched);

    // 非 Signature 槽：无操作不视为失败（setMember 同款探测纪律）。
    AttrSlot& weird = cf->methods[0].attrs[1];
    auto w = renameClassInSignatureAttr(*cf, weird, SignatureForm::Method,
                                        "demo/Box", "demo/Renamed");
    ASSERT_TRUE(w.is_ok());
    EXPECT_FALSE(std::move(w).unwrap().changed);
    EXPECT_EQ(weird.state, AttrSlot::State::Untouched);
}

TEST(SigAttrTest, DollarChainAndDotChainBothRewritten)
{
    // '$' 内嵌（javac 常规产物）与 '.' 段链两种字节形态，同一内部名全名匹配。
    struct Case
    {
        const char* sig;
        const char* renamed;
    };
    const std::vector<Case> cases = {
        {"Ldemo/Outer$Inner;", "Ldemo/Renamed$Inner;"},
        {"Ldemo/Outer<TT;>.Inner;", "Ldemo/Renamed<TT;>.Inner;"},
    };
    for (const Case& c : cases) {
        ClassBuilder cb;
        const ca::u16 thisIdx = cb.utf8("p/T");
        const ca::u16 superIdx = cb.utf8("java/lang/Object");
        const ca::u16 sigName = cb.utf8("Signature");
        const ca::u16 sig = cb.utf8(c.sig);
        cb.setThis(cb.classRef(thisIdx));
        cb.setSuper(cb.classRef(superIdx));
        cb.setVersion(52);
        cb.addClassAttr(sigName, sigContent(sig));
        const Bytes bytes = cb.build();

        auto cf = jbc::test::readOk(bytes);
        ASSERT_NE(cf, nullptr) << c.sig;
        auto r = renameClassInSignatureAttrs(*cf, "demo/Outer$Inner", "demo/Renamed$Inner");
        ASSERT_TRUE(r.is_ok()) << c.sig;
        EXPECT_EQ(std::move(r).unwrap().hits, 1u) << c.sig;
        EXPECT_EQ(sigStringOf(*cf, cf->attrs[0]), c.renamed) << c.sig;
    }
}

// ---- 跨形态：扁平段链替换 / 真嵌套链不命中跳过 ----

TEST(SigAttrTest, CrossShapeFlatChainRewritesAndNestedSkips)
{
    const SigClass sc = makeSigClass();
    auto cf = jbc::test::readOk(sc.bytes);
    ASSERT_NE(cf, nullptr);
    const Bytes before = jbc::test::writeOk(*cf);

    // 扁平段链（demo/Box 单段名）改为嵌套名：new 按 '$' 合成写满本段
    // （javac 通行非规范 $ 形态，跨混淆器映射还原的落地通道）。
    auto r = renameClassInSignatureAttrs(*cf, "demo/Box", "demo/Moved$In");
    ASSERT_TRUE(r.is_ok());
    const SigAttrRenameSummary st = std::move(r).unwrap();
    EXPECT_EQ(st.attrsTotal, 3u);
    EXPECT_EQ(st.attrsChanged, 3u);
    EXPECT_EQ(st.hits, 3u);
    EXPECT_EQ(sigStringOf(*cf, cf->attrs[0]),
              "<T:Ljava/lang/Object;>Ljava/lang/Object;Ldemo/Moved$In<TT;>;");
    EXPECT_EQ(sigStringOf(*cf, cf->methods[0].attrs[0]),
              "(TT;)Ljava/util/List<Ldemo/Moved$In;>;");

    // 反向还原：合成的 '$' 段名按两部件匹配，单段还原为顶级名；restore
    // 经 findUtf8 复用原条目（文案逐字节回），孤儿池条目按 append-only
    // 契约留存（字节总数不再与改前一致，压缩逃生口另议）。
    auto back = renameClassInSignatureAttrs(*cf, "demo/Moved$In", "demo/Box");
    ASSERT_TRUE(back.is_ok());
    const SigAttrRenameSummary backSt = std::move(back).unwrap();
    EXPECT_EQ(backSt.hits, 3u);
    EXPECT_EQ(sigStringOf(*cf, cf->attrs[0]), SigClass::kClassSig);
    EXPECT_EQ(sigStringOf(*cf, cf->methods[0].attrs[0]), SigClass::kMethodSig);
    EXPECT_EQ(sigStringOf(*cf, cf->fields[0].attrs[0]), SigClass::kFieldSig);
}

TEST(SigAttrTest, NestedChainCrossShapeSkipsAsNoReach)
{
    const SigClass sc = makeSigClass();
    auto cf = jbc::test::readOk(sc.bytes);
    ASSERT_NE(cf, nullptr);
    const Bytes before = jbc::test::writeOk(*cf);

    // old 两段（Outer$Inner）、new 一段：真嵌套段链不再产生 Err——部件
    // 越界并入「不命中」族，原字节与 CP 零改写（跳过语义）。
    auto r = renameClassInSignatureAttrs(*cf, "demo/Box$Inner", "demo/Moved");
    ASSERT_TRUE(r.is_ok());
    const SigAttrRenameSummary st = std::move(r).unwrap();
    EXPECT_EQ(st.attrsTotal, 3u);
    EXPECT_EQ(st.attrsChanged, 0u);
    EXPECT_EQ(st.hits, 0u);
    for (int level = 0; level < 3; ++level) {
        EXPECT_NE(slotAt(*cf, level, 0).state, AttrSlot::State::Modified) << "level " << level;
    }
    EXPECT_EQ(jbc::test::writeOk(*cf), before);
}

// ---- 未命中零改写 ----

TEST(SigAttrTest, MissLeavesBytesAndPoolUntouched)
{
    const SigClass sc = makeSigClass();
    auto cf = jbc::test::readOk(sc.bytes);
    ASSERT_NE(cf, nullptr);
    const Bytes before = jbc::test::writeOk(*cf);
    const ca::u32 cpBefore = cf->cp.size();

    auto r = renameClassInSignatureAttrs(*cf, "demo/Absent", "demo/Renamed");
    ASSERT_TRUE(r.is_ok());
    const SigAttrRenameSummary st = std::move(r).unwrap();
    EXPECT_EQ(st.attrsTotal, 3u);
    EXPECT_EQ(st.attrsChanged, 0u);
    EXPECT_EQ(st.hits, 0u);
    EXPECT_EQ(cf->cp.size(), cpBefore) << "miss must not intern";
    for (int level = 0; level < 3; ++level) {
        EXPECT_NE(slotAt(*cf, level, 0).state, AttrSlot::State::Modified);
    }
    EXPECT_EQ(jbc::test::writeOk(*cf), before);
}

TEST(SigAttrTest, SameTargetStringReusesExistingPoolEntry)
{
    // 两个槽指向同一签名串：只追加一个新 Utf8（findUtf8 去重，不膨胀）。
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 sigName = cb.utf8("Signature");
    const ca::u16 sig = cb.utf8("Ldemo/Box;");
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setVersion(52);
    cb.addClassAttr(sigName, sigContent(sig));
    cb.addField(0x0002, cb.utf8("a"), cb.utf8("Ljava/lang/Object;"),
                {AttrSpec{sigName, sigContent(sig)}});
    const Bytes bytes = cb.build();

    auto cf = jbc::test::readOk(bytes);
    ASSERT_NE(cf, nullptr);
    const ca::u32 cpBefore = cf->cp.size();
    auto r = renameClassInSignatureAttrs(*cf, "demo/Box", "demo/Renamed");
    ASSERT_TRUE(r.is_ok());
    const SigAttrRenameSummary st = std::move(r).unwrap();
    EXPECT_EQ(st.attrsChanged, 2u);
    EXPECT_EQ(cf->cp.size(), cpBefore + 1) << "identical new string interns once";

    // 逆向 rename 复用回原条目：不再追加。
    const ca::u32 cpMid = cf->cp.size();
    auto back = renameClassInSignatureAttrs(*cf, "demo/Renamed", "demo/Box");
    ASSERT_TRUE(back.is_ok());
    EXPECT_EQ(cf->cp.size(), cpMid) << "reverse rename reuses original entry";
}

// ---- 非 2 字节体：手术只改 u2 索引，尾部经 trailing 原样保留----

TEST(SigAttrTest, ExtraBodyBytesSurviveSurgery)
{
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 sigName = cb.utf8("Signature");
    const ca::u16 sig = cb.utf8("Ldemo/Box;");
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setVersion(52);
    // 非法 3 字节体：u2 索引 + 1 尾巴（畸形输入的可表达事实， 同族）。
    cb.addClassAttr(sigName, {sigContent(sig)[0], sigContent(sig)[1], 0xFE});
    const Bytes bytes = cb.build();

    auto cf = jbc::test::readOk(bytes);
    ASSERT_NE(cf, nullptr);
    auto r = renameClassInSignatureAttrs(*cf, "demo/Box", "demo/Renamed");
    ASSERT_TRUE(r.is_ok());

    const Bytes out = jbc::test::writeOk(*cf);
    auto cf2 = jbc::test::readOk(out);
    ASSERT_NE(cf2, nullptr);
    auto d = cf2->decodeAttr(cf2->attrs[0]);
    ASSERT_TRUE(d.is_ok());
    auto* attr = static_cast<SignatureAttr*>(std::move(d).unwrap());
    ASSERT_EQ(attr->trailing.size(), ca::usize{1});
    EXPECT_EQ(attr->trailing[0], 0xFE) << "tail byte must survive";
    EXPECT_EQ(cf2->utf8(attr->signatureCp), "Ldemo/Renamed;");
}

// ---- 失败显式：坏签名 / 形态不符 / 半截属性体 ----

TEST(SigAttrTest, MalformedSignatureStringFailsClosed)
{
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 sigName = cb.utf8("Signature");
    const ca::u16 good = cb.utf8("Ldemo/Box;");
    const ca::u16 bad = cb.utf8("not a signature");
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setVersion(52);
    cb.addClassAttr(sigName, sigContent(good));
    cb.addField(0x0002, cb.utf8("a"), cb.utf8("Ljava/lang/Object;"),
                {AttrSpec{sigName, sigContent(bad)}});
    const Bytes bytes = cb.build();

    auto cf = jbc::test::readOk(bytes);
    ASSERT_NE(cf, nullptr);
    const Bytes before = jbc::test::writeOk(*cf);

    auto r = renameClassInSignatureAttrs(*cf, "demo/Box", "demo/Renamed");
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(std::move(r).unwrap_err().kind, SigAttrError::Kind::Signature);
    // 全有或全无：好槽也没被改写。
    EXPECT_NE(cf->attrs[0].state, AttrSlot::State::Modified);
    EXPECT_EQ(jbc::test::writeOk(*cf), before);
}

TEST(SigAttrTest, MountPointDecidesForm)
{
    // 方法形态的签名串挂在类级：按挂载点形态解析即失败（形态歧义不猜）。
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 sigName = cb.utf8("Signature");
    const ca::u16 sig = cb.utf8("(TT;)Ldemo/Box;");
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setVersion(52);
    cb.addClassAttr(sigName, sigContent(sig));
    const Bytes bytes = cb.build();

    auto cf = jbc::test::readOk(bytes);
    ASSERT_NE(cf, nullptr);
    auto r = renameClassInSignatureAttrs(*cf, "demo/Box", "demo/Renamed");
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(std::move(r).unwrap_err().kind, SigAttrError::Kind::Signature);

    // 单槽原语允许调用方显式给 Field 形态吗？不行——字段形态不接受方法串；
    // 显式给 Method 形态则命中（挂载点知识在调用方手里的合法用法）。
    auto m = renameClassInSignatureAttr(*cf, cf->attrs[0], SignatureForm::Method,
                                        "demo/Box", "demo/Renamed");
    ASSERT_TRUE(m.is_ok());
    EXPECT_TRUE(std::move(m).unwrap().changed);
}

TEST(SigAttrTest, TruncatedSlotFailsDecodeKind)
{
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 sigName = cb.utf8("Signature");
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setVersion(52);
    cb.addClassAttr(sigName, {0x00});   // u2 索引截半
    const Bytes bytes = cb.build();

    auto cf = jbc::test::readOk(bytes);
    ASSERT_NE(cf, nullptr);
    auto r = renameClassInSignatureAttrs(*cf, "demo/Box", "demo/Renamed");
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(std::move(r).unwrap_err().kind, SigAttrError::Kind::SlotDecode);
    EXPECT_EQ(jbc::test::writeOk(*cf), bytes) << "untouched write must roundtrip";
}

// ---- LVTT descriptor 列（签名宿主第二处）：结构化手术接线 ----

namespace {

/// LVTT/LVT 行内容：5 × u2（JVMS 4.7.13 行布局）。
Bytes lvtRowContent(ca::u16 startPc, ca::u16 length, ca::u16 nameCp,
                    ca::u16 descCp, ca::u16 slot)
{
    Bytes b;
    const auto put16 = [&b](ca::u16 v) {
        b.push_back(static_cast<ca::u8>(v >> 8));
        b.push_back(static_cast<ca::u8>(v));
    };
    put16(startPc);
    put16(length);
    put16(nameCp);
    put16(descCp);
    put16(slot);
    return b;
}

/// 行表内容：u2 count + 行（LVTT/LVT 同布局）。
Bytes lvtTableContent(const std::vector<Bytes>& rows)
{
    Bytes b;
    b.push_back(static_cast<ca::u8>(rows.size() >> 8));
    b.push_back(static_cast<ca::u8>(rows.size()));
    for (const Bytes& r : rows) {
        b.insert(b.end(), r.begin(), r.end());
    }
    return b;
}

/// @brief 带一个方法（Code 嵌套 LVTT + LVT）的最小类。
///
/// LVTT 三行：'$' 内嵌命中行 / '.' 段链命中行（同一内部名的两等价字节
/// 形态）/ 未命中普通引用行；LVT 一行擦除描述符（不属 LVTT 手术入口，
/// 作邻居对照）。命中对：demo/Box$Inner → demo/Box$Renamed（同 '$' 段
/// 结构，匹配跨 '$' 形态改动不涉足）。
struct LvttClass
{
    Bytes bytes;
    ca::u16 hitInnerIdx = 0;     ///< Ldemo/Box$Inner;
    ca::u16 hitChainIdx = 0;     ///< Ldemo/Box<TT;>.Inner;
    ca::u16 missIdx = 0;         ///< Ljava/lang/String;
    ca::u16 lvtRowDescIdx = 0;   ///< Ldemo/Expr;（LVT 擦除行）

    static constexpr const char* kHitInner = "Ldemo/Box$Inner;";
    static constexpr const char* kHitChain = "Ldemo/Box<TT;>.Inner;";
    static constexpr const char* kMiss = "Ljava/lang/String;";
    static constexpr const char* kLvtRowDesc = "Ldemo/Expr;";
};

LvttClass makeLvttClass()
{
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/Use");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 codeName = cb.utf8("Code");
    const ca::u16 lvttName = cb.utf8("LocalVariableTypeTable");
    const ca::u16 lvtName = cb.utf8("LocalVariableTable");
    const ca::u16 varName = cb.utf8("i");
    const ca::u16 hitInner = cb.utf8(LvttClass::kHitInner);
    const ca::u16 hitChain = cb.utf8(LvttClass::kHitChain);
    const ca::u16 miss = cb.utf8(LvttClass::kMiss);
    const ca::u16 lvtRowDesc = cb.utf8(LvttClass::kLvtRowDesc);
    const ca::u16 runName = cb.utf8("run");
    const ca::u16 runDesc = cb.utf8("()V");

    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setVersion(52);

    std::vector<AttrSpec> nested;
    nested.push_back(AttrSpec{lvttName,
                              lvtTableContent({lvtRowContent(0, 1, varName, hitInner, 0),
                                               lvtRowContent(0, 1, varName, hitChain, 1),
                                               lvtRowContent(0, 1, varName, miss, 2)})});
    nested.push_back(AttrSpec{lvtName,
                              lvtTableContent({lvtRowContent(0, 1, varName, lvtRowDesc, 0)})});
    cb.addMethod(0x0001, runName, runDesc,
                 {AttrSpec{codeName, codeContent(0, 2, Bytes{0xb1}, {}, nested)}});
    return LvttClass{cb.build(), hitInner, hitChain, miss, lvtRowDesc};
}

/// @brief 解出方法 0 的 Code 内第 nestIdx 个嵌套属性（LocalVariableTableAttr）。
LocalVariableTableAttr& nestedLvtAt(ClassFile& cf, ca::usize nestIdx)
{
    AttrSlot& codeSlot = cf.methods[0].attrs[0];
    auto d = cf.decodeAttr(codeSlot);
    EXPECT_TRUE(d.is_ok());
    auto* code = static_cast<CodeAttr*>(std::move(d).unwrap());
    auto ld = cf.decodeAttr(code->attrs[nestIdx]);
    EXPECT_TRUE(ld.is_ok());
    return *static_cast<LocalVariableTableAttr*>(std::move(ld).unwrap());
}

}   // namespace

TEST(SigAttrTest, LvttHardGateNoHitIsByteIdentical)
{
    const LvttClass lc = makeLvttClass();
    auto cf = jbc::test::readOk(lc.bytes);
    ASSERT_NE(cf, nullptr);

    // 缺席名全扫：零命中零改写，写回逐字节不动（解码发生也不置脏）。
    auto miss = renameClassInLocalVariableTypeTables(*cf, "zzz/Absent_R", "zzz/Moved_R");
    ASSERT_TRUE(miss.is_ok());
    const LvttRenameSummary ms = std::move(miss).unwrap();
    EXPECT_EQ(ms.tablesTotal, 1u);
    EXPECT_EQ(ms.tablesChanged, 0u);
    EXPECT_EQ(jbc::test::writeOk(*cf), lc.bytes);
}

TEST(SigAttrTest, LvttRowsRewriteDualForm)
{
    const LvttClass lc = makeLvttClass();
    auto cf = jbc::test::readOk(lc.bytes);
    ASSERT_NE(cf, nullptr);

    // 同一内部名（demo/Box$Inner）的两种签名字节形态各命中一次：
    // '$' 内嵌行整体段名替换，'.' 段链行末段替换（其余字节原样保留）。
    auto r = renameClassInLocalVariableTypeTables(*cf, "demo/Box$Inner",
                                                  "demo/Box$Renamed");
    ASSERT_TRUE(r.is_ok());
    const LvttRenameSummary st = std::move(r).unwrap();
    EXPECT_EQ(st.tablesTotal, 1u);
    EXPECT_EQ(st.tablesChanged, 1u);
    EXPECT_EQ(st.rowsChanged, 2u);
    EXPECT_GE(st.hits, 2u);

    // 重读：两命中行结构化改写，未命中行与邻居 LVT 行原样。
    LocalVariableTableAttr& lvtt = nestedLvtAt(*cf, 0);
    EXPECT_EQ(cf->utf8(lvtt.rows[0].descCp), "Ldemo/Box$Renamed;");
    EXPECT_EQ(cf->utf8(lvtt.rows[1].descCp), "Ldemo/Box<TT;>.Renamed;");
    EXPECT_EQ(lvtt.rows[2].descCp, lc.missIdx);
    LocalVariableTableAttr& lvt = nestedLvtAt(*cf, 1);
    EXPECT_EQ(lvt.rows[0].descCp, lc.lvtRowDescIdx);
}

TEST(SigAttrTest, LvttGenericTypeArgRewrite)
{
    // 类型实参嵌套：命中点在实参位置的类类型上。
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/Use");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 codeName = cb.utf8("Code");
    const ca::u16 lvttName = cb.utf8("LocalVariableTypeTable");
    const ca::u16 varName = cb.utf8("i");
    const ca::u16 row = cb.utf8("Ldemo/Box<Ldemo/Expr;>;");
    const ca::u16 runName = cb.utf8("run");
    const ca::u16 runDesc = cb.utf8("()V");
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setVersion(52);
    const Bytes lvttRows = lvtTableContent({lvtRowContent(0, 1, varName, row, 0)});
    cb.addMethod(0x0001, runName, runDesc,
                 {AttrSpec{codeName,
                           codeContent(0, 1, Bytes{0xb1}, {},
                                       {AttrSpec{lvttName, lvttRows}})}});
    auto cf = jbc::test::readOk(cb.build());
    ASSERT_NE(cf, nullptr);

    auto r = renameClassInLocalVariableTypeTables(*cf, "demo/Expr", "demo/Renamed");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(std::move(r).unwrap().rowsChanged, 1u);
    LocalVariableTableAttr& lvtt = nestedLvtAt(*cf, 0);
    EXPECT_EQ(cf->utf8(lvtt.rows[0].descCp), "Ldemo/Box<Ldemo/Renamed;>;");
}

TEST(SigAttrTest, LvttReverseRenameRestoresBytes)
{
    const LvttClass lc = makeLvttClass();
    auto cf = jbc::test::readOk(lc.bytes);
    ASSERT_NE(cf, nullptr);

    auto fwd = renameClassInLocalVariableTypeTables(*cf, "demo/Box$Inner",
                                                    "demo/Box$Renamed");
    ASSERT_TRUE(fwd.is_ok());
    const ca::usize cpAfterForward = cf->cp.size();

    // 逆向 rename：复用回原 CP 条目（查重命中，不再追加），行串逐字节还原。
    auto back =
        renameClassInLocalVariableTypeTables(*cf, "demo/Box$Renamed", "demo/Box$Inner");
    ASSERT_TRUE(back.is_ok());
    EXPECT_EQ(std::move(back).unwrap().rowsChanged, 2u);
    EXPECT_EQ(cf->cp.size(), cpAfterForward) << "reverse rename must not grow the pool";
    LocalVariableTableAttr& lvtt = nestedLvtAt(*cf, 0);
    EXPECT_EQ(cf->utf8(lvtt.rows[0].descCp), LvttClass::kHitInner);
    EXPECT_EQ(cf->utf8(lvtt.rows[1].descCp), LvttClass::kHitChain);
    EXPECT_EQ(lvtt.rows[0].descCp, lc.hitInnerIdx) << "original entry reused";
    EXPECT_EQ(lvtt.rows[1].descCp, lc.hitChainIdx) << "original entry reused";
}

TEST(SigAttrTest, LvttNestedChainCrossShapeSkips)
{
    // 真嵌套 '.' 段链跨形态：部件越界并入「不命中」族（原显式拒绝语义
    // 并入零改写跳过，不再 Err）——行串原样，全扫零写。
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/Use");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 codeName = cb.utf8("Code");
    const ca::u16 lvttName = cb.utf8("LocalVariableTypeTable");
    const ca::u16 varName = cb.utf8("i");
    const ca::u16 chain = cb.utf8("Ldemo/Outer<TT;>.Inner;");
    const ca::u16 runName = cb.utf8("run");
    const ca::u16 runDesc = cb.utf8("()V");
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setVersion(52);
    const Bytes lvttRows = lvtTableContent({lvtRowContent(0, 1, varName, chain, 0)});
    cb.addMethod(0x0001, runName, runDesc,
                 {AttrSpec{codeName,
                           codeContent(0, 1, Bytes{0xb1}, {},
                                       {AttrSpec{lvttName, lvttRows}})}});
    const Bytes bytes = cb.build();
    auto cf = jbc::test::readOk(bytes);
    ASSERT_NE(cf, nullptr);

    auto r = renameClassInLocalVariableTypeTables(*cf, "demo/Outer$Inner",
                                                  "demo/Renamed");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(std::move(r).unwrap().rowsChanged, 0u);
    EXPECT_EQ(jbc::test::writeOk(*cf), bytes) << "skip must not write";
}

TEST(SigAttrTest, LvttBadRowSignatureFailsWholeScan)
{
    // 一行非签名（擦除基元描述符 I）：字段形态解析失败——全扫全有或全无。
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/Use");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 codeName = cb.utf8("Code");
    const ca::u16 lvttName = cb.utf8("LocalVariableTypeTable");
    const ca::u16 varName = cb.utf8("i");
    const ca::u16 good = cb.utf8("Ldemo/Box;");
    const ca::u16 bad = cb.utf8("I");
    const ca::u16 runName = cb.utf8("run");
    const ca::u16 runDesc = cb.utf8("()V");
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setVersion(52);
    const Bytes lvttRows =
        lvtTableContent({lvtRowContent(0, 1, varName, good, 0),
                         lvtRowContent(0, 1, varName, bad, 1)});
    cb.addMethod(0x0001, runName, runDesc,
                 {AttrSpec{codeName,
                           codeContent(0, 1, Bytes{0xb1}, {},
                                       {AttrSpec{lvttName, lvttRows}})}});
    const Bytes bytes = cb.build();
    auto cf = jbc::test::readOk(bytes);
    ASSERT_NE(cf, nullptr);

    auto r = renameClassInLocalVariableTypeTables(*cf, "demo/Box", "demo/Renamed");
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(std::move(r).unwrap_err().kind, SigAttrError::Kind::Signature);
    // 好行也没被改写，写回逐字节不动。
    EXPECT_EQ(jbc::test::writeOk(*cf), bytes);
}

TEST(SigAttrTest, LvttTruncatedBodyFailsDecodeKind)
{
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/Use");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 codeName = cb.utf8("Code");
    const ca::u16 lvttName = cb.utf8("LocalVariableTypeTable");
    const ca::u16 runName = cb.utf8("run");
    const ca::u16 runDesc = cb.utf8("()V");
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setVersion(52);
    // count=1 但行只有 3 个 u2：行中途截断。
    cb.addMethod(0x0001, runName, runDesc,
                 {AttrSpec{codeName,
                           codeContent(0, 1, Bytes{0xb1}, {},
                                       {AttrSpec{lvttName, {0x00, 0x01, 0x00, 0x00,
                                                            0x00, 0x01, 0x00, 0x00}}})}});
    const Bytes bytes = cb.build();
    auto cf = jbc::test::readOk(bytes);
    ASSERT_NE(cf, nullptr);

    auto r = renameClassInLocalVariableTypeTables(*cf, "demo/Box", "demo/Renamed");
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(std::move(r).unwrap_err().kind, SigAttrError::Kind::SlotDecode);
    EXPECT_EQ(jbc::test::writeOk(*cf), bytes) << "untouched write must roundtrip";
}

TEST(SigAttrTest, LvttTrailingBytesSurviveSurgery)
{
    // 体尾垃圾（trailing 同族）：行改写后重编码原样保留尾字节。
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/Use");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 codeName = cb.utf8("Code");
    const ca::u16 lvttName = cb.utf8("LocalVariableTypeTable");
    const ca::u16 varName = cb.utf8("i");
    const ca::u16 row = cb.utf8("Ldemo/Box;");
    const ca::u16 runName = cb.utf8("run");
    const ca::u16 runDesc = cb.utf8("()V");
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setVersion(52);
    Bytes lvtt = lvtTableContent({lvtRowContent(0, 1, varName, row, 0)});
    lvtt.push_back(0xFE);   // 尾部垃圾
    cb.addMethod(0x0001, runName, runDesc,
                 {AttrSpec{codeName,
                           codeContent(0, 1, Bytes{0xb1}, {}, {AttrSpec{lvttName, lvtt}})}});
    const Bytes bytes = cb.build();
    auto cf = jbc::test::readOk(bytes);
    ASSERT_NE(cf, nullptr);

    auto r = renameClassInLocalVariableTypeTables(*cf, "demo/Box", "demo/Renamed");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(std::move(r).unwrap().rowsChanged, 1u);

    const Bytes out = jbc::test::writeOk(*cf);
    auto rr = jbc::test::readOk(out);
    ASSERT_NE(rr, nullptr);
    LocalVariableTableAttr& lvt = nestedLvtAt(*rr, 0);
    EXPECT_EQ(rr->utf8(lvt.rows[0].descCp), "Ldemo/Renamed;");
    ASSERT_EQ(lvt.trailing.size(), ca::usize{1});
    EXPECT_EQ(lvt.trailing[0], 0xFE) << "tail byte must survive";
}
