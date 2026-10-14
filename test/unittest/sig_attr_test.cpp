/// @file        sig_attr_test.cpp
/// @brief       Signature 属性手术的表驱动单测：无映射字节不动（硬门禁）、
///              命中重写、RenameShapeMismatch 拒绝、未命中零改写，以及
///              typed 模型 / trailing 保持 / 全有或全无 / 失败显式等契约。
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

/// @brief 读出某槽当前指向的签名串（Raw 体 u2 索引一跳）。
std::string_view sigStringOf(ClassFile& cf, AttrSlot& slot)
{
    auto d = cf.decodeAttr(slot);
    EXPECT_TRUE(d.is_ok());
    if (d.is_err()) {
        return {};
    }
    auto* raw = static_cast<RawAttr*>(std::move(d).unwrap());
    const std::vector<ca::u8>& b = raw->bytes();
    EXPECT_EQ(b.size(), ca::usize{2});
    if (b.size() < 2) {
        return {};
    }
    return cf.utf8((ca::u32(b[0]) << 8) | ca::u32(b[1]));
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

TEST(SigAttrTest, RawBodyExposesSignatureIndex)
{
    const SigClass sc = makeSigClass();
    auto cf = jbc::test::readOk(sc.bytes);
    ASSERT_NE(cf, nullptr);

    // Signature 现状不在已建模属性之列：Raw 兜底（下游按 Raw 字节消费的
    // 既定契约），体 = u2 signature_index；手术面即在该体上原位读写。
    for (int level = 0; level < 3; ++level) {
        AttrSlot& slot = slotAt(*cf, level, 0);
        auto d = cf->decodeAttr(slot);
        ASSERT_TRUE(d.is_ok());
        AttrBody* body = std::move(d).unwrap();
        ASSERT_EQ(body->kind(), AttrKind::Raw) << "level " << level;
        auto* raw = static_cast<RawAttr*>(body);
        ASSERT_EQ(raw->bytes().size(), ca::usize{2});
        const ca::u16 want = level == 0 ? sc.classSigIdx
                            : level == 1 ? sc.methodSigIdx : sc.fieldSigIdx;
        EXPECT_EQ((ca::u32(raw->bytes()[0]) << 8) | raw->bytes()[1], want) << "level " << level;
        // 未置脏：解码只推进 Untouched→Decoded，写回仍 copy。
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

// ---- RenameShapeMismatch：'$' 结构不一致显式拒绝，不猜 ----

TEST(SigAttrTest, RenameShapeMismatchRejectedAndUntouched)
{
    const SigClass sc = makeSigClass();
    auto cf = jbc::test::readOk(sc.bytes);
    ASSERT_NE(cf, nullptr);
    const Bytes before = jbc::test::writeOk(*cf);

    // old 两段（Outer$Inner）、new 一段：结构重排被拒。
    auto r = renameClassInSignatureAttrs(*cf, "demo/Box$Inner", "demo/Moved");
    ASSERT_TRUE(r.is_err());
    const SigAttrError err = std::move(r).unwrap_err();
    EXPECT_EQ(err.kind, SigAttrError::Kind::Signature);
    EXPECT_EQ(err.sig.code, SigErrorCode::RenameShapeMismatch);

    // 全有或全无：一个槽都没被改写。
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

// ---- 非 2 字节体：手术只改前两字节，其余原样保留（Raw 体逐字节写回）----

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
    // 非法 3 字节体：u2 索引 + 1 尾巴（畸形输入的可表达事实，§27.1 同族）。
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
    auto* raw = static_cast<RawAttr*>(std::move(d).unwrap());
    ASSERT_EQ(raw->bytes().size(), ca::usize{3});
    EXPECT_EQ(raw->bytes()[2], 0xFE) << "tail byte must survive";
    const ca::u32 idx = (ca::u32(raw->bytes()[0]) << 8) | raw->bytes()[1];
    EXPECT_EQ(cf2->utf8(idx), "Ldemo/Renamed;");
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
