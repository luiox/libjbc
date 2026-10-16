/// @file        annotation_attrs_test.cpp
/// @brief       注解族四件 typed 建模单测：ParameterAnnotations（JVMS
///              4.7.18/4.7.19）/ AnnotationDefault（4.7.22）/
///              MethodParameters（4.7.24）/ ConstantValue（4.7.2）。
///
/// 覆盖面：分派接线钉门、typed 解码（公开直读不触碰槽状态）、半截体显式
/// 失败、trailing 保持、域宽闸门（u1 参数维 + u2 索引面）、重编码字节恒等。
/// 注解体（type_index + element_value 全形态）与 Runtime{Visible,Invisible}-
/// Annotations 共用同一模型的决策由「复用 readAnnotation/writeAnnotation」
/// 落实，此处对嵌套/'['/'e'/'c' 形态逐一断言防分派旁路。
#include <gtest/gtest.h>

#include <memory>
#include <string_view>
#include <vector>

#include "jbc/jbc.hpp"
#include "test_class_bytes.hpp"

using namespace jbc::core;
using jbc::test::Bytes;
using jbc::test::ClassBuilder;

namespace {

/// 带一个方法级属性的方法（唯一方法的属性表）。
Bytes methodWithAttr(const std::string& attrName, const Bytes& content)
{
    ClassBuilder cb;
    const ca::u16 thisIdx  = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 mIdx     = cb.utf8("m");
    const ca::u16 vIdx     = cb.utf8("(II)I");
    const ca::u16 aIdx     = cb.utf8(attrName);
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setVersion(52);
    cb.addMethod(0x0001, mIdx, vIdx, {jbc::test::AttrSpec{aIdx, content}});
    return cb.build();
}

/// 带一个字段级属性的字段（ConstantValue 挂载点）。
Bytes fieldWithAttr(const std::string& attrName, const Bytes& content)
{
    ClassBuilder cb;
    const ca::u16 thisIdx  = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 fIdx     = cb.utf8("v");
    const ca::u16 iIdx     = cb.utf8("I");
    const ca::u16 aIdx     = cb.utf8(attrName);
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setVersion(52);
    cb.addField(0x0018, fIdx, iIdx, {jbc::test::AttrSpec{aIdx, content}});
    return cb.build();
}

/// 方法属性槽定位（全方法扫描，唯一命中）。
AttrSlot* findMethodSlot(ClassFile& cf, std::string_view name)
{
    for (MemberInfo& m : cf.methods) {
        for (AttrSlot& slot : m.attrs) {
            if (cf.cp.utf8Equals(slot.nameCp, name)) {
                return &slot;
            }
        }
    }
    return nullptr;
}

/// 字段属性槽定位。
AttrSlot* findFieldSlot(ClassFile& cf, std::string_view name)
{
    for (MemberInfo& f : cf.fields) {
        for (AttrSlot& slot : f.attrs) {
            if (cf.cp.utf8Equals(slot.nameCp, name)) {
                return &slot;
            }
        }
    }
    return nullptr;
}

/// 单注解体：u2 type + u2 值数 + { u2 名, element_value }*。
Bytes annoBytes(ca::u16 typeCp, const std::vector<std::pair<ca::u16, Bytes>>& values)
{
    Bytes b;
    jbc::test::detail::putU16(b, typeCp);
    jbc::test::detail::putU16(b, static_cast<ca::u16>(values.size()));
    for (const auto& kv : values) {
        jbc::test::detail::putU16(b, kv.first);
        b.insert(b.end(), kv.second.begin(), kv.second.end());
    }
    return b;
}

/// 单 const 元素值：tag + u2。
Bytes evConst(char tag, ca::u16 cp)
{
    Bytes b;
    b.push_back(static_cast<ca::u8>(tag));
    jbc::test::detail::putU16(b, cp);
    return b;
}

/// 枚举元素值：'e' + u2 type_name + u2 const_name。
Bytes evEnum(ca::u16 typeNameCp, ca::u16 constNameCp)
{
    Bytes b;
    b.push_back(static_cast<ca::u8>('e'));
    jbc::test::detail::putU16(b, typeNameCp);
    jbc::test::detail::putU16(b, constNameCp);
    return b;
}

/// 数组元素值：'[' + u2 项数 + 项*。
Bytes evArray(const std::vector<Bytes>& items)
{
    Bytes b;
    b.push_back(static_cast<ca::u8>('['));
    jbc::test::detail::putU16(b, static_cast<ca::u16>(items.size()));
    for (const Bytes& it : items) {
        b.insert(b.end(), it.begin(), it.end());
    }
    return b;
}

/// 嵌套注解元素值：'@' + 注解体。
Bytes evNested(const Bytes& anno)
{
    Bytes b;
    b.push_back(static_cast<ca::u8>('@'));
    b.insert(b.end(), anno.begin(), anno.end());
    return b;
}

/// ParameterAnnotations 体：u1 参数数 + 逐参数（u2 注解数 + 注解体*）。
Bytes paramAnnosBody(const std::vector<std::vector<Bytes>>& groups)
{
    Bytes b;
    b.push_back(static_cast<ca::u8>(groups.size()));
    for (const std::vector<Bytes>& g : groups) {
        jbc::test::detail::putU16(b, static_cast<ca::u16>(g.size()));
        for (const Bytes& a : g) {
            b.insert(b.end(), a.begin(), a.end());
        }
    }
    return b;
}

/// MethodParameters 体：u1 行数 + { u2 name, u2 flags }*。
Bytes methodParamsBody(const std::vector<std::pair<ca::u16, ca::u16>>& rows)
{
    Bytes b;
    b.push_back(static_cast<ca::u8>(rows.size()));
    for (const auto& r : rows) {
        jbc::test::detail::putU16(b, r.first);
        jbc::test::detail::putU16(b, r.second);
    }
    return b;
}

}   // namespace

// ---- ParameterAnnotations：分派 + 形态 + 半截 + trailing + 闸门 ----

TEST(ParameterAnnotationsAttrTest, TypedDispatchProducesTypedBody)
{
    // 参数维两组：组 0 一条注解（'s' 值），组 1 空表（无注解参数合法）。
    const Bytes body = paramAnnosBody({{annoBytes(0x10, {{0x20, evConst('s', 0x30)}})}, {}});
    const Bytes cls  = methodWithAttr("RuntimeVisibleParameterAnnotations", body);

    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    auto d = cf->decodeAttr(cf->methods[0].attrs[0]);
    ASSERT_TRUE(d.is_ok());
    ASSERT_EQ(std::move(d).unwrap()->kind(), AttrKind::ParameterAnnotations);
    const auto* pa = static_cast<const ParameterAnnotationsAttr*>(
        cf->methods[0].attrs[0].peek());
    ASSERT_NE(pa, nullptr);
    ASSERT_EQ(pa->parameters.size(), 2u);
    ASSERT_EQ(pa->parameters[0].size(), 1u);
    EXPECT_EQ(pa->parameters[0][0].typeCp, 0x10u);
    ASSERT_EQ(pa->parameters[0][0].values.size(), 1u);
    EXPECT_EQ(pa->parameters[0][0].values[0].second.tag, 's');
    EXPECT_EQ(pa->parameters[0][0].values[0].second.cp, 0x30u);
    EXPECT_EQ(pa->parameters[1].size(), 0u);
    // 未触碰（Decoded 态写回走 copy）：字节一致。
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);

    // Invisible 名共用同一模型（可见性由槽名承载，体不感知）。
    const Bytes clsI = methodWithAttr("RuntimeInvisibleParameterAnnotations", body);
    auto cfI = jbc::test::readOk(clsI);
    ASSERT_NE(cfI, nullptr);
    auto dI = cfI->decodeAttr(cfI->methods[0].attrs[0]);
    ASSERT_TRUE(dI.is_ok());
    ASSERT_EQ(std::move(dI).unwrap()->kind(), AttrKind::ParameterAnnotations);
    EXPECT_EQ(jbc::test::writeOk(*cfI), clsI);
}

TEST(ParameterAnnotationsAttrTest, DirectDecodeLeavesSlotUntouched)
{
    const Bytes body = paramAnnosBody({{annoBytes(0x10, {})}});
    const Bytes cls  = methodWithAttr("RuntimeVisibleParameterAnnotations", body);
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findMethodSlot(*cf, "RuntimeVisibleParameterAnnotations");
    ASSERT_NE(slot, nullptr);
    auto d = decodeParameterAnnotationsBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto bodyOut = std::move(d).unwrap();
    // 直读入口不触碰槽状态（lazy 语义保持）。
    EXPECT_FALSE(slot->decoded());
    ASSERT_EQ(bodyOut->kind(), AttrKind::ParameterAnnotations);
    ASSERT_EQ(static_cast<const ParameterAnnotationsAttr&>(*bodyOut).parameters.size(), 1u);
    EXPECT_TRUE(static_cast<const ParameterAnnotationsAttr&>(*bodyOut).trailing.empty());
}

TEST(ParameterAnnotationsAttrTest, NestedAndArrayValueShapesRoundtrip)
{
    // 同参数双注解：注解 0 带 '@' 嵌套 + 'e' 枚举，注解 1 带 '[' 数组
    // （'c' 类字面量元素）——全形态经共用注解体通道往返。
    const Bytes inner = annoBytes(0x11, {{0x21, evConst('c', 0x31)}});
    const Bytes body  = paramAnnosBody({{annoBytes(0x10, {{0x20, evNested(inner)},
                                                          {0x22, evEnum(0x12, 0x32)}}),
                                         annoBytes(0x13, {{0x23, evArray({evConst('I', 0x41),
                                                                          evConst('I', 0x42)})}})}});
    const Bytes cls = methodWithAttr("RuntimeInvisibleParameterAnnotations", body);
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findMethodSlot(*cf, "RuntimeInvisibleParameterAnnotations");
    ASSERT_NE(slot, nullptr);
    auto d = decodeParameterAnnotationsBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto bodyOut = std::move(d).unwrap();
    const auto& pa = static_cast<const ParameterAnnotationsAttr&>(*bodyOut);
    ASSERT_EQ(pa.parameters.size(), 1u);
    ASSERT_EQ(pa.parameters[0].size(), 2u);
    ASSERT_EQ(pa.parameters[0][0].values.size(), 2u);
    ASSERT_NE(pa.parameters[0][0].values[0].second.nested, nullptr);
    EXPECT_EQ(pa.parameters[0][0].values[0].second.nested->typeCp, 0x11u);
    EXPECT_EQ(pa.parameters[0][0].values[1].second.tag, 'e');
    EXPECT_EQ(pa.parameters[0][0].values[1].second.constNameCp, 0x32u);
    ASSERT_EQ(pa.parameters[0][1].values[0].second.tag, '[');
    ASSERT_EQ(pa.parameters[0][1].values[0].second.items.size(), 2u);

    // 置脏重编码：结构输出字节一致（共用写面 writeAnnotation 无损）。
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(bodyOut));
    EXPECT_EQ(jbc::test::writeOk(*cf), cls) << "re-encode diverged";
}

TEST(ParameterAnnotationsAttrTest, TruncatedAnnotationIsExplicitFailure)
{
    // 组 0 一条注解，注解体的值数声明被截断（type 后即 EOF）：半截显式 Err；
    // 失败不影响 lazy copy。
    const Bytes body = {0x01,                   // 1 个参数
                        0x00, 0x01,             // 组 0：1 条注解
                        0x00, 0x10};            // typeCp，值数声明截断
    const Bytes cls = methodWithAttr("RuntimeVisibleParameterAnnotations", body);
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    auto d = cf->decodeAttr(cf->methods[0].attrs[0]);
    ASSERT_TRUE(d.is_err());
    EXPECT_EQ(d.unwrap_err(), ReadError::Truncated);
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

TEST(ParameterAnnotationsAttrTest, TrailingSurvivesReencode)
{
    Bytes body = paramAnnosBody({{}, {}});
    body.push_back(0xDE);
    body.push_back(0xAD);
    const Bytes cls = methodWithAttr("RuntimeVisibleParameterAnnotations", body);
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findMethodSlot(*cf, "RuntimeVisibleParameterAnnotations");
    ASSERT_NE(slot, nullptr);
    auto d = decodeParameterAnnotationsBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto bodyOut = std::move(d).unwrap();
    ASSERT_EQ(static_cast<const ParameterAnnotationsAttr&>(*bodyOut).trailing.size(), 2u);
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(bodyOut));
    EXPECT_EQ(jbc::test::writeOk(*cf), cls) << "re-encode dropped trailing";
}

TEST(ParameterAnnotationsAttrTest, EncoderRejectsOutOfRangeGroupCount)
{
    // 参数数是 u1 域：超 255 显式 Err，不得静默截断。
    const Bytes cls = methodWithAttr("RuntimeVisibleParameterAnnotations",
                                     paramAnnosBody({{}}));
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findMethodSlot(*cf, "RuntimeVisibleParameterAnnotations");
    ASSERT_NE(slot, nullptr);
    auto d = decodeParameterAnnotationsBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto bodyOut = std::move(d).unwrap();
    // 注解体持 unique_ptr 不可拷贝：扩容走逐组构造而非 assign。
    auto& paBody = static_cast<ParameterAnnotationsAttr&>(*bodyOut);
    paBody.parameters.clear();
    for (int i = 0; i < 0x100; ++i) {
        paBody.parameters.emplace_back();
    }
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(bodyOut));
    const auto r = writeClass(*cf);
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), WriteError::Unencodable);
}

// ---- AnnotationDefault：分派 + 形态 + 半截 + trailing + 闸门 ----

TEST(AnnotationDefaultAttrTest, TypedDispatchProducesTypedBody)
{
    // 默认值 = 数组（'e' 枚举 + 'c' 类字面量两项）。
    const Bytes body = evArray({evEnum(0x11, 0x12), evConst('c', 0x13)});
    const Bytes cls  = methodWithAttr("AnnotationDefault", body);

    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    auto d = cf->decodeAttr(cf->methods[0].attrs[0]);
    ASSERT_TRUE(d.is_ok());
    ASSERT_EQ(std::move(d).unwrap()->kind(), AttrKind::AnnotationDefault);
    const auto* ad = static_cast<const AnnotationDefaultAttr*>(cf->methods[0].attrs[0].peek());
    ASSERT_NE(ad, nullptr);
    EXPECT_EQ(ad->defaultValue.tag, '[');
    ASSERT_EQ(ad->defaultValue.items.size(), 2u);
    EXPECT_EQ(ad->defaultValue.items[0].tag, 'e');
    EXPECT_EQ(ad->defaultValue.items[0].typeNameCp, 0x11u);
    EXPECT_EQ(ad->defaultValue.items[0].constNameCp, 0x12u);
    EXPECT_EQ(ad->defaultValue.items[1].tag, 'c');
    EXPECT_EQ(ad->defaultValue.items[1].cp, 0x13u);
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

TEST(AnnotationDefaultAttrTest, NestedAnnotationDefaultRoundtrips)
{
    // 默认值 = 嵌套注解（javac 的 nested() default @Nested(id = 0) 形态）。
    const Bytes body = evNested(annoBytes(0x10, {{0x20, evConst('I', 0x30)}}));
    const Bytes cls  = methodWithAttr("AnnotationDefault", body);
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findMethodSlot(*cf, "AnnotationDefault");
    ASSERT_NE(slot, nullptr);
    auto d = decodeAnnotationDefaultBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto bodyOut = std::move(d).unwrap();
    const auto& ad = static_cast<const AnnotationDefaultAttr&>(*bodyOut);
    EXPECT_EQ(ad.defaultValue.tag, '@');
    ASSERT_NE(ad.defaultValue.nested, nullptr);
    EXPECT_EQ(ad.defaultValue.nested->typeCp, 0x10u);
    // 直读不触碰槽；置脏重编码字节一致。
    EXPECT_FALSE(slot->decoded());
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(bodyOut));
    EXPECT_EQ(jbc::test::writeOk(*cf), cls) << "re-encode diverged";
}

TEST(AnnotationDefaultAttrTest, TruncatedValueIsExplicitFailure)
{
    // tag 'B' 后缺 u2 常量索引：半截显式 Err。
    const Bytes cls = methodWithAttr("AnnotationDefault", {0x01});
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    auto d = cf->decodeAttr(cf->methods[0].attrs[0]);
    ASSERT_TRUE(d.is_err());
    EXPECT_EQ(d.unwrap_err(), ReadError::Truncated);
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

TEST(AnnotationDefaultAttrTest, TrailingSurvivesReencode)
{
    Bytes body = evConst('I', 0x30);
    body.push_back(0xDE);
    const Bytes cls = methodWithAttr("AnnotationDefault", body);
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findMethodSlot(*cf, "AnnotationDefault");
    ASSERT_NE(slot, nullptr);
    auto d = decodeAnnotationDefaultBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto bodyOut = std::move(d).unwrap();
    ASSERT_EQ(static_cast<const AnnotationDefaultAttr&>(*bodyOut).trailing.size(), 1u);
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(bodyOut));
    EXPECT_EQ(jbc::test::writeOk(*cf), cls) << "re-encode dropped trailing";
}

TEST(AnnotationDefaultAttrTest, EncoderRejectsOutOfRangeIndex)
{
    const Bytes cls = methodWithAttr("AnnotationDefault", evConst('I', 0x30));
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findMethodSlot(*cf, "AnnotationDefault");
    ASSERT_NE(slot, nullptr);
    auto d = decodeAnnotationDefaultBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto bodyOut = std::move(d).unwrap();
    static_cast<AnnotationDefaultAttr&>(*bodyOut).defaultValue.cp = 0x10000;
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(bodyOut));
    const auto r = writeClass(*cf);
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), WriteError::Unencodable);
}

// ---- MethodParameters：分派 + 无名行 + 半截 + trailing + 闸门 ----

TEST(MethodParametersAttrTest, TypedDispatchProducesTypedBody)
{
    // 行 0 具名 + ACC_FINAL；行 1 无名（name=0 合法）+ ACC_MANDATED。
    const Bytes cls = methodWithAttr(
        "MethodParameters", methodParamsBody({{0x30, 0x0010}, {0x0000, 0x8000}}));

    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    auto d = cf->decodeAttr(cf->methods[0].attrs[0]);
    ASSERT_TRUE(d.is_ok());
    ASSERT_EQ(std::move(d).unwrap()->kind(), AttrKind::MethodParameters);
    const auto* mp = static_cast<const MethodParametersAttr*>(cf->methods[0].attrs[0].peek());
    ASSERT_NE(mp, nullptr);
    ASSERT_EQ(mp->rows.size(), 2u);
    EXPECT_EQ(mp->rows[0].nameCp, 0x30u);
    EXPECT_EQ(mp->rows[0].accessFlags, 0x0010u);
    EXPECT_EQ(mp->rows[1].nameCp, 0u);
    EXPECT_EQ(mp->rows[1].accessFlags, 0x8000u);
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

TEST(MethodParametersAttrTest, DirectDecodeLeavesSlotUntouched)
{
    const Bytes cls = methodWithAttr("MethodParameters", methodParamsBody({{0x30, 0}}));
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findMethodSlot(*cf, "MethodParameters");
    ASSERT_NE(slot, nullptr);
    auto d = decodeMethodParametersBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto bodyOut = std::move(d).unwrap();
    EXPECT_FALSE(slot->decoded());
    ASSERT_EQ(bodyOut->kind(), AttrKind::MethodParameters);
    EXPECT_EQ(static_cast<const MethodParametersAttr&>(*bodyOut).rows.size(), 1u);
}

TEST(MethodParametersAttrTest, TruncatedRowIsExplicitFailure)
{
    // 声明 1 行但只带 2 字节（行是 4 字节定长）：半截显式 Err。
    const Bytes cls = methodWithAttr("MethodParameters", {0x01, 0x00, 0x30});
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    auto d = cf->decodeAttr(cf->methods[0].attrs[0]);
    ASSERT_TRUE(d.is_err());
    EXPECT_EQ(d.unwrap_err(), ReadError::Truncated);
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

TEST(MethodParametersAttrTest, TrailingSurvivesReencode)
{
    Bytes body = methodParamsBody({{0x30, 0x0010}});
    body.push_back(0xDE);
    const Bytes cls = methodWithAttr("MethodParameters", body);
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findMethodSlot(*cf, "MethodParameters");
    ASSERT_NE(slot, nullptr);
    auto d = decodeMethodParametersBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto bodyOut = std::move(d).unwrap();
    ASSERT_EQ(static_cast<const MethodParametersAttr&>(*bodyOut).trailing.size(), 1u);
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(bodyOut));
    EXPECT_EQ(jbc::test::writeOk(*cf), cls) << "re-encode dropped trailing";
}

TEST(MethodParametersAttrTest, EncoderRejectsOutOfRangeFields)
{
    // 行数是 u1 域：超 255 显式 Err。
    {
        const Bytes cls = methodWithAttr("MethodParameters", methodParamsBody({{0x30, 0}}));
        auto cf = jbc::test::readOk(cls);
        ASSERT_NE(cf, nullptr);
        AttrSlot* slot = findMethodSlot(*cf, "MethodParameters");
        ASSERT_NE(slot, nullptr);
        auto d = decodeMethodParametersBody(*cf, *slot);
        ASSERT_TRUE(d.is_ok());
        auto bodyOut = std::move(d).unwrap();
        static_cast<MethodParametersAttr&>(*bodyOut).rows.assign(0x100, {0x30, 0});
        *slot = AttrSlot::makeNew(slot->nameCp, std::move(bodyOut));
        const auto r = writeClass(*cf);
        ASSERT_TRUE(r.is_err());
        EXPECT_EQ(r.unwrap_err(), WriteError::Unencodable);
    }
    // nameCp 超 u16 域。
    {
        const Bytes cls = methodWithAttr("MethodParameters", methodParamsBody({{0x30, 0}}));
        auto cf = jbc::test::readOk(cls);
        ASSERT_NE(cf, nullptr);
        AttrSlot* slot = findMethodSlot(*cf, "MethodParameters");
        ASSERT_NE(slot, nullptr);
        auto d = decodeMethodParametersBody(*cf, *slot);
        ASSERT_TRUE(d.is_ok());
        auto bodyOut = std::move(d).unwrap();
        static_cast<MethodParametersAttr&>(*bodyOut).rows[0].nameCp = 0x10000;
        *slot = AttrSlot::makeNew(slot->nameCp, std::move(bodyOut));
        const auto r = writeClass(*cf);
        ASSERT_TRUE(r.is_err());
        EXPECT_EQ(r.unwrap_err(), WriteError::Unencodable);
    }
}

// ---- ConstantValue：分派 + 半截 + trailing + 闸门 ----

TEST(ConstantValueAttrTest, TypedDispatchProducesTypedBody)
{
    const Bytes cls = fieldWithAttr("ConstantValue", {0x00, 0x14});

    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    auto d = cf->decodeAttr(cf->fields[0].attrs[0]);
    ASSERT_TRUE(d.is_ok());
    ASSERT_EQ(std::move(d).unwrap()->kind(), AttrKind::ConstantValue);
    const auto* cv = static_cast<const ConstantValueAttr*>(cf->fields[0].attrs[0].peek());
    ASSERT_NE(cv, nullptr);
    EXPECT_EQ(cv->constantValueCp, 0x14u);
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

TEST(ConstantValueAttrTest, TruncatedBodyIsExplicitFailure)
{
    const Bytes cls = fieldWithAttr("ConstantValue", {0x00});
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    auto d = cf->decodeAttr(cf->fields[0].attrs[0]);
    ASSERT_TRUE(d.is_err());
    EXPECT_EQ(d.unwrap_err(), ReadError::Truncated);
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

TEST(ConstantValueAttrTest, TrailingSurvivesReencode)
{
    Bytes body = {0x00, 0x14, 0xDE, 0xAD};
    const Bytes cls = fieldWithAttr("ConstantValue", body);
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findFieldSlot(*cf, "ConstantValue");
    ASSERT_NE(slot, nullptr);
    auto d = decodeConstantValueBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto bodyOut = std::move(d).unwrap();
    ASSERT_EQ(static_cast<const ConstantValueAttr&>(*bodyOut).trailing.size(), 2u);
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(bodyOut));
    EXPECT_EQ(jbc::test::writeOk(*cf), cls) << "re-encode dropped trailing";
}

TEST(ConstantValueAttrTest, EncoderRejectsOutOfRangeIndex)
{
    const Bytes cls = fieldWithAttr("ConstantValue", {0x00, 0x14});
    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    AttrSlot* slot = findFieldSlot(*cf, "ConstantValue");
    ASSERT_NE(slot, nullptr);
    auto d = decodeConstantValueBody(*cf, *slot);
    ASSERT_TRUE(d.is_ok());
    auto bodyOut = std::move(d).unwrap();
    static_cast<ConstantValueAttr&>(*bodyOut).constantValueCp = 0x10000;
    *slot = AttrSlot::makeNew(slot->nameCp, std::move(bodyOut));
    const auto r = writeClass(*cf);
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), WriteError::Unencodable);
}
