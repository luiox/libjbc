/// @file        attr_build.cpp
/// @brief       类构建挂载面：注解族/简单
///              注解的语义合成与 typed 属性挂载 + BSM 语义 intern 家族
///              （ClassSession 成员实现文件）。
//
// 挂载规则封闭表（单一知识源，勿在调用点散写）：
//   - "Code" 一律拒绝——方法体的唯一管径是 promote → CodeEditor → lower，
//     从属性表侧面手挂 Code 会绕过 IR 一致性检查。
//   - LNT / LVT / LVTT 允许多条（JVMS 未单例化，javac 也不合并重复表）。
//   - 其余（含未知名）至多一条；重复挂载 Err(AlreadyMounted)，替换语义走
//     既有 decodeAttr + mutate() 通路。
//
// 错误族分流（LowerError， 域宽闸门的合成侧同源纪律）：
//   DomainOverflow = 声明结构行数写不下（u1/u16）；
//   InvalidSeed    = 规格本身不合法（空描述符/描述符形态不符/深度越界/
//                    与成员描述符 arity 失配）；
//   AlreadyMounted = 单例属性重复挂载；
//   BadMount       = 挂载点不接受（挂 Code）；
//   CpAppendFailed = CP 容量预检不过。
//
// 全链「先算后写」：每个 attach 入口的执行序固定为
//   1) 句柄解析；(2) 形状/域宽预检；(3) 挂载预检（单例/名字容量，不触碰
//   模型）；(4) 规格容量预检（CP 追加数含属性名）；(5) 合成（CP 追加）；
//   (6) 落位（attr table push）。任何一步失败都发生在其后续步骤之前，
//   CP 与属性表不留半截。
#include <limits>
#include <optional>
#include <utility>

#include "jbc/core/attribute.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/constant_pool.hpp"
#include "jbc/ir/error.hpp"
#include "jbc/ir/sem_index.hpp"
#include "jbc/ir/units.hpp"
#include "jbc/type/descriptor.hpp"

namespace jbc::ir {

namespace {

constexpr int kMaxSynthDepth = 128;   ///< 合成递归深度闸（规格树的荒谬形态）

/// @brief 方法描述符参数个数（长/双各算一个参数——MethodParameters/
///        ParameterAnnotations 的行数语义）。解析失败（描述符损坏）按
///        InvalidSeed 收口：seed 支柱在合成期已校验，此处防御式兜底。
Result<ca::u32, LowerError> methodDescArity(const ClassSession& s, SymbolId desc)
{
    const Result<jbc::type::MethodDesc, jbc::type::DescError> rd
        = jbc::type::parseMethodDescriptor(s.unit().symbols().str(desc));
    if (rd.is_err()) {
        return Err(LowerError::InvalidSeed);
    }
    return Ok(static_cast<ca::u32>(rd.unwrap().params.size()));
}

/// @brief 挂载规则：该属性名是否允许多条。
bool attrAllowsMultiple(std::string_view name) noexcept
{
    // LNT/LVT/LVTT 是唯三的多条属性；其余全单例。LVT 与 LVTT 在 AttrKind
    // 上同体（LocalVariableTableAttr），但属性名各归各，按名判定。
    return name == "LineNumberTable" || name == "LocalVariableTable"
        || name == "LocalVariableTypeTable";
}

/// @brief 注解类型描述符判据：JVMS 4.7.16 的 type_index / 'e' type_name 是
///        「L<name>;」形态的字段类型描述符。合成面只做形态判据（非空 + L;
///        包围），泛型开剥离归 jbc::type 语义层。
bool isClassFieldDescriptor(std::string_view s) noexcept
{
    return s.size() >= 3 && s.front() == 'L' && s.back() == ';';
}

/// @brief 元素规格静态合法性（容量计数与合成两条遍历共用的单一判据源）。
bool elemWellFormed(const SemElemValue& e) noexcept
{
    switch (e.form) {
    case SemElemValue::Form::String:
    case SemElemValue::Form::Class:
        return !e.str.empty();
    case SemElemValue::Form::Enum:
        return isClassFieldDescriptor(e.enumType) && !e.str.empty();
    case SemElemValue::Form::Nested:
        return e.nested != nullptr && isClassFieldDescriptor(e.nested->typeDesc);
    case SemElemValue::Form::Array:
        // JVMS 数组 element_value 允许 0 项，但合成空数组无承载语义——
        // 规格面拒绝；消费方确需空数组走低层面直接构造。
        return !e.array.empty();
    case SemElemValue::Form::Int:
    case SemElemValue::Form::Bool:
    case SemElemValue::Form::Byte:
    case SemElemValue::Form::Char:
    case SemElemValue::Form::Short:
    case SemElemValue::Form::Long:
    case SemElemValue::Form::Float:
    case SemElemValue::Form::Double:
        return true;
    }
    return false;
}

bool annoWellFormed(const SemAnnotation& a) noexcept
{
    return isClassFieldDescriptor(a.typeDesc);
}

/// @brief 标量元素（Nested/Array 之外）的 CP 追加计数：miss 才计——与 intern
///        家族同一去重判据，直接查 SemIndex 表。调用方保证 form 不是
///        Nested/Array（递归在 attach 入口统一走规格树遍历）。
ca::u32 countScalarElem(const SemElemValue& e, SemIndex& ix)
{
    auto utf8Miss = [&](std::string_view s) {
        return ix.utf8.find(s) == ix.utf8.end() ? ca::u32{1} : ca::u32{0};
    };
    switch (e.form) {
    case SemElemValue::Form::Int:
    case SemElemValue::Form::Bool:
    case SemElemValue::Form::Byte:
    case SemElemValue::Form::Char:
    case SemElemValue::Form::Short:
        return ix.intConst.find(static_cast<ca::i32>(e.num)) == ix.intConst.end() ? 1 : 0;
    case SemElemValue::Form::Long:
        return ix.longConst.find(e.num) == ix.longConst.end() ? 1 : 0;
    case SemElemValue::Form::Float:
        return ix.floatConst.find(e.fbits) == ix.floatConst.end() ? 1 : 0;
    case SemElemValue::Form::Double:
        return ix.doubleConst.find(e.dbits) == ix.doubleConst.end() ? 1 : 0;
    case SemElemValue::Form::String:
        // 's' 与 'c' 同账：只落 Utf8（注解字符串不隔 String 条目）。
        return utf8Miss(e.str);
    case SemElemValue::Form::Class:
        // 'c' 的 const_value_index 直接指向 Utf8（javac 同形：基本类型 =
        // 类型描述符如 "I"，类类型 = 类型描述符如 "Ljava/lang/String;"），
        // 没有 Class 条目这个二级账。
        return utf8Miss(e.str);
    case SemElemValue::Form::Enum:
        return utf8Miss(e.enumType) + utf8Miss(e.str);
    default:
        return 0;
    }
}

/// @brief reference_kind → 目标成员条目 tag（JVMS 4.4.8 封闭表）：1..4 字段
///        形（0..3 get/put field/static → Fieldref）、5..8 方法形（virtual/
///        static/special/newInvokeSpecial → Methodref）、9 接口形（Interface-
///        Methodref）。域外即 null（JVM 类解析对 kind ∉ 1..9 直接拒）。
std::optional<core::CpTag> mhRefTagOf(ca::u16 kind) noexcept
{
    switch (kind) {
    case 1:
    case 2:
    case 3:
    case 4:
        return core::CpTag::Fieldref;
    case 5:
    case 6:
    case 7:
    case 8:
        return core::CpTag::Methodref;
    case 9:
        return core::CpTag::InterfaceMethodref;
    default:
        return std::nullopt;
    }
}

/// @brief utf8 视图的既有索引解析：miss ⇒ 下游所有以该索引为键的映射
///        （byName/nat/ref/mh/mt…）必然 miss（池只追加，既有条目不可能
///        引用尚不存在的下标）——计数链据此剪枝，与 intern 实现同构。
struct Utf8Known
{
    ca::u32 idx = 0;
    bool known = false;
};

Utf8Known resolveUtf8(const SemIndex& ix, std::string_view s)
{
    const auto it = ix.utf8.find(s);
    return it == ix.utf8.end() ? Utf8Known{} : Utf8Known{it->second, true};
}

/// @brief BSM 编目计数器（miss 才计、保守超计可行、漏计不可——与
///        countScalarElem 同一纪律）。与带去重 intern 的查找序列严格同构：
///        utf8→下游键；utf8 命中才查二级表，miss 直接按全链计加。
ca::u32 countHandleAdditions(const SemIndex& ix, core::CpTag refTag,
                             const MethodHandleSpec& h)
{
    const Utf8Known o = resolveUtf8(ix, h.owner);
    const Utf8Known n = resolveUtf8(ix, h.name);
    const Utf8Known d = resolveUtf8(ix, h.desc);
    // 三件 utf8 各自的 miss。
    ca::u32 nAdds = (o.known ? 0 : 1) + (n.known ? 0 : 1) + (d.known ? 0 : 1);

    // Class（owner）：utf8 已在才查 byName；否则必随建。
    ca::u32 classIdx = 0;
    bool classKnown = false;
    if (o.known) {
        const auto it = ix.byName.find(o.idx);
        if (it != ix.byName.end()) {
            classKnown = true;
            classIdx   = it->second;
        } else {
            ++nAdds;
        }
    } else {
        ++nAdds;
    }

    // NameAndType(name, desc)：两端 utf8 都已在才查；否则必随建。
    ca::u32 natIdx = 0;
    bool natKnown  = false;
    if (n.known && d.known) {
        const auto it = ix.nat.find(packNatKey(n.idx, d.idx));
        if (it != ix.nat.end()) {
            natKnown = true;
            natIdx   = it->second;
        } else {
            ++nAdds;
        }
    } else {
        ++nAdds;
    }

    // 成员引用：上游都已在才查；否则必随建。
    ca::u32 refIdx = 0;
    bool refKnown  = false;
    if (classKnown && natKnown) {
        const auto it = ix.ref.find(packRefKey(refTag, classIdx, natIdx));
        if (it != ix.ref.end()) {
            refKnown = true;
            refIdx   = it->second;
        } else {
            ++nAdds;
        }
    } else {
        ++nAdds;
    }

    // 处理条目（MethodHandle 壳）：ref 已在才查；否则必随建。
    if (refKnown) {
        if (ix.mhConst.find(packMhKey(h.referenceKind, refIdx)) == ix.mhConst.end()) {
            ++nAdds;
        }
    } else {
        ++nAdds;
    }
    return nAdds;
}

/// @brief 单个静态参数的追加计数（Dynamic 叶子绑定既有条目，无嵌套规格树）。
ca::u32 countBsmArgAdditions(const SemIndex& ix, const BsmArg& a)
{
    auto miss = [](auto& map, const auto& key) {
        return map.find(key) == map.end() ? ca::u32{1} : ca::u32{0};
    };
    auto utf8Miss = [&](std::string_view s) {
        return ix.utf8.find(s) == ix.utf8.end() ? ca::u32{1} : ca::u32{0};
    };
    switch (a.kind) {
    case BsmArg::Kind::Int:
        return miss(ix.intConst, static_cast<ca::i32>(a.num));
    case BsmArg::Kind::Long:
        return miss(ix.longConst, a.num);
    case BsmArg::Kind::Float:
        return miss(ix.floatConst, a.fbits);
    case BsmArg::Kind::Double:
        return miss(ix.doubleConst, a.dbits);
    case BsmArg::Kind::String:
        if (!utf8Miss(a.str)) {
            return miss(ix.strConst, resolveUtf8(ix, a.str).idx);
        }
        return 2;   // utf8 与 String 壳必同名随建
    case BsmArg::Kind::Class:
        if (!utf8Miss(a.str)) {
            return ix.byName.find(resolveUtf8(ix, a.str).idx) == ix.byName.end() ? 1 : 0;
        }
        return 2;   // utf8 与 Class 壳必随建
    case BsmArg::Kind::MethodType:
        if (!utf8Miss(a.str)) {
            return miss(ix.mtConst, resolveUtf8(ix, a.str).idx);
        }
        return 2;   // utf8 与 MethodType 壳必随建
    case BsmArg::Kind::MethodHandle: {
        const std::optional<core::CpTag> tag = mhRefTagOf(a.handle.referenceKind);
        if (!tag.has_value()) {
            return 0;   // 域外 kind 在合法性预检已拒（计数不可能到达此处）
        }
        return countHandleAdditions(ix, *tag, a.handle);
    }
    case BsmArg::Kind::Dynamic: {
        const Utf8Known n = resolveUtf8(ix, a.name);
        const Utf8Known d = resolveUtf8(ix, a.desc);
        ca::u32 nn = (n.known ? 0 : 1) + (d.known ? 0 : 1);
        ca::u32 natIdx = 0;
        bool natKnown  = false;
        if (n.known && d.known) {
            const auto it = ix.nat.find(packNatKey(n.idx, d.idx));
            if (it != ix.nat.end()) {
                natKnown = true;
                natIdx   = it->second;
            } else {
                ++nn;
            }
        } else {
            ++nn;
        }
        if (natKnown) {
            nn += miss(ix.ref, packRefKey(core::CpTag::Dynamic, a.bsm, natIdx));
        } else {
            ++nn;   // ref 必随建
        }
        return nn;
    }
    }
    return 0;   // 不可达（Kind 封闭枚举）
}

}   // namespace

// ---------------------------------------------------------------------------
// 挂载内核：预检（不触碰模型）+ 落位（CP intern + attr push）
// ---------------------------------------------------------------------------

Result<void, LowerError> ClassSession::mountPrecheck(std::vector<core::AttrSlot>& table,
                                                     std::string_view attrName)
{
    if (attrName.empty() || attrName.size() > 0xFFFFu) {
        return Err(LowerError::InvalidSeed);
    }
    if (attrName == "Code") {
        return Err(LowerError::BadMount);
    }
    if (table.size() >= 0xFFFFu) {
        return Err(LowerError::CpAppendFailed);   // attributes_count u16 域
    }
    // 单例规则（封闭表单一知识源），重复挂载显式失败。
    if (!attrAllowsMultiple(attrName)) {
        for (const core::AttrSlot& slot : table) {
            if (backing_->cp.utf8Equals(slot.nameCp, attrName)) {
                return Err(LowerError::AlreadyMounted);
            }
        }
    }
    // 属性名 Utf8 的追加容量单独预检（追加数 = miss 才 1）。
    SemIndex& ix = sem();
    if (backing_->cp.size()
            + (ix.utf8.find(attrName) == ix.utf8.end() ? 1 : 0)
        > 0xFFFFu) {
        return Err(LowerError::CpAppendFailed);
    }
    return Ok();
}

Result<void, LowerError> ClassSession::mountAttr(
    std::vector<core::AttrSlot>& table, std::string_view attrName,
    std::unique_ptr<core::AttrBody> body)
{
    if (body == nullptr) {
        return Err(LowerError::InvalidSeed);
    }
    const ca::u32 nameCp = internUtf8Sem(attrName);
    table.push_back(core::AttrSlot::makeNew(nameCp, std::move(body)));
    return Ok();
}

// ---------------------------------------------------------------------------
// 规格树容量计数（miss 才计数的预检遍历；与合成遍历同构但零 intern 效应）
// ---------------------------------------------------------------------------

Result<ca::u32, LowerError> ClassSession::countAnnoAdditions(const SemAnnotation& a,
                                                             int depth)
{
    if (depth > kMaxSynthDepth || !annoWellFormed(a)) {
        return Err(LowerError::InvalidSeed);
    }
    SemIndex& ix = sem();
    ca::u32 n    = (ix.utf8.find(a.typeDesc) == ix.utf8.end()) ? 1 : 0;
    for (const auto& [name, value] : a.values) {
        if (name.empty()) {
            return Err(LowerError::InvalidSeed);
        }
        n += ix.utf8.find(name) == ix.utf8.end() ? 1 : 0;
        if (value.form == SemElemValue::Form::Nested) {
            const Result<ca::u32, LowerError> rn = countAnnoAdditions(*value.nested, depth + 1);
            if (rn.is_err()) {
                return Err(rn.unwrap_err());
            }
            n += rn.unwrap();
        } else if (value.form == SemElemValue::Form::Array) {
            for (const SemElemValue& item : value.array) {
                if (!elemWellFormed(item)) {
                    return Err(LowerError::InvalidSeed);
                }
                if (item.form == SemElemValue::Form::Nested) {
                    const Result<ca::u32, LowerError> rc
                        = countAnnoAdditions(*item.nested, depth + 1);
                    if (rc.is_err()) {
                        return Err(rc.unwrap_err());
                    }
                    n += rc.unwrap();
                } else {
                    n += countScalarElem(item, ix);
                }
            }
        } else {
            if (!elemWellFormed(value)) {
                return Err(LowerError::InvalidSeed);
            }
            n += countScalarElem(value, ix);
        }
    }
    return Ok(n);
}

Result<ca::u32, LowerError> ClassSession::countElemAdditions(const SemElemValue& e,
                                                             int depth)
{
    if (depth > kMaxSynthDepth || !elemWellFormed(e)) {
        return Err(LowerError::InvalidSeed);
    }
    SemIndex& ix = sem();
    if (e.form == SemElemValue::Form::Nested) {
        return countAnnoAdditions(*e.nested, depth + 1);
    }
    if (e.form == SemElemValue::Form::Array) {
        ca::u32 n = 0;
        for (const SemElemValue& item : e.array) {
            if (!elemWellFormed(item)) {
                return Err(LowerError::InvalidSeed);
            }
            if (item.form == SemElemValue::Form::Nested) {
                const Result<ca::u32, LowerError> rc
                    = countAnnoAdditions(*item.nested, depth + 1);
                if (rc.is_err()) {
                    return Err(rc.unwrap_err());
                }
                n += rc.unwrap();
            } else {
                n += countScalarElem(item, ix);
            }
        }
        return Ok(n);
    }
    return Ok(countScalarElem(e, ix));
}

// ---------------------------------------------------------------------------
// 合成：规格树 → core typed 体（CP 语义 intern）
// ---------------------------------------------------------------------------

Result<core::AnnotationsAttr::Annotation, LowerError>
    ClassSession::synthAnnotation(const SemAnnotation& a, int depth)
{
    if (depth > kMaxSynthDepth || !annoWellFormed(a)) {
        return Err(LowerError::InvalidSeed);
    }
    core::AnnotationsAttr::Annotation out;
    out.typeCp = internUtf8Sem(a.typeDesc);
    for (const auto& [name, value] : a.values) {
        if (name.empty()) {
            return Err(LowerError::InvalidSeed);
        }
        Result<core::AnnotationsAttr::ElemValue, LowerError> rv
            = synthElem(value, depth + 1);
        if (rv.is_err()) {
            return Err(rv.unwrap_err());
        }
        out.values.emplace_back(internUtf8Sem(name), std::move(rv).unwrap());
    }
    return Ok(std::move(out));
}

Result<core::AnnotationsAttr::ElemValue, LowerError> ClassSession::synthElem(
    const SemElemValue& e, int depth)
{
    if (depth > kMaxSynthDepth || !elemWellFormed(e)) {
        return Err(LowerError::InvalidSeed);
    }
    core::AnnotationsAttr::ElemValue out;
    using Form = SemElemValue::Form;
    // 标量族的 CP 直落（Nested/Array 不走这个 switch）。
    switch (e.form) {
    case Form::Int:
        out.tag = 'I';
        break;
    case Form::Bool:
        out.tag = 'Z';
        break;
    case Form::Byte:
        out.tag = 'B';
        break;
    case Form::Char:
        out.tag = 'C';
        break;
    case Form::Short:
        out.tag = 'S';
        break;
    case Form::Long:
        out.tag = 'J';
        break;
    case Form::Float:
        out.tag = 'F';
        break;
    case Form::Double:
        out.tag = 'D';
        break;
    case Form::String:
        out.tag = 's';
        // 's' 的 const_value_index 直接指向 Utf8（JVMS 4.7.16.1 + javac
        // 同形——见对照实验 B4：javac 对 "hi" 编 s#7 而 #7=Utf8）：注解
        // 字符串不隔 CONSTANT_String 条目。
        out.cp = internUtf8Sem(e.str);
        return Ok(std::move(out));
    case Form::Class:
        out.tag = 'c';
        // 'c' 的 const_value_index 指向 Utf8（JVMS 4.7.22 + javac 同形）：
        // 基本类型/void 用对应类型描述符（"I"/"V"/...），类类型用类型描
        // 述符（"Ljava/lang/String;"）——并非 CONSTANT_Class 条目。JVM 的
        // 'c' 解析器按此类描述符语义 readback（int.class 等）。
        out.cp = internUtf8Sem(e.str);
        return Ok(std::move(out));
    case Form::Enum:
        out.tag         = 'e';
        out.typeNameCp  = internUtf8Sem(e.enumType);
        out.constNameCp = internUtf8Sem(e.str);
        return Ok(std::move(out));
    case Form::Nested: {
        out.tag = '@';
        Result<core::AnnotationsAttr::Annotation, LowerError> ra
            = synthAnnotation(*e.nested, depth + 1);
        if (ra.is_err()) {
            return Err(ra.unwrap_err());
        }
        out.nested = std::make_unique<core::AnnotationsAttr::Annotation>(
            std::move(ra).unwrap());
        return Ok(std::move(out));
    }
    case Form::Array: {
        out.tag = '[';
        for (const SemElemValue& item : e.array) {
            if (!elemWellFormed(item)) {
                return Err(LowerError::InvalidSeed);
            }
            if (item.form == Form::Nested) {
                Result<core::AnnotationsAttr::Annotation, LowerError> ra
                    = synthAnnotation(*item.nested, depth + 1);
                if (ra.is_err()) {
                    return Err(ra.unwrap_err());
                }
                core::AnnotationsAttr::ElemValue ev;
                ev.tag    = '@';
                ev.nested = std::make_unique<core::AnnotationsAttr::Annotation>(
                    std::move(ra).unwrap());
                out.items.push_back(std::move(ev));
            } else {
                Result<core::AnnotationsAttr::ElemValue, LowerError> rv
                    = synthElem(item, depth + 1);
                if (rv.is_err()) {
                    return Err(rv.unwrap_err());
                }
                out.items.push_back(std::move(rv).unwrap());
            }
        }
        return Ok(std::move(out));
    }
    }
    // 数值族（Int/Bool/Byte/Char/Short/Long/Float/Double）的常量落位。
    switch (e.form) {
    case Form::Int:
    case Form::Bool:
    case Form::Byte:
    case Form::Char:
    case Form::Short:
        out.cp = internInteger(static_cast<ca::i32>(e.num));
        break;
    case Form::Long:
        out.cp = internLong(e.num);
        break;
    case Form::Float:
        out.cp = internFloatBits(e.fbits);
        break;
    case Form::Double:
        out.cp = internDoubleBits(e.dbits);
        break;
    default:
        return Err(LowerError::InvalidSeed);
    }
    return Ok(std::move(out));
}

// ---------------------------------------------------------------------------
// 公开面：注解族合成 + 注解系列（先算后写五步序）
// ---------------------------------------------------------------------------

Result<void, LowerError> ClassSession::attachAnnotationsToTable(
    std::vector<core::AttrSlot>& table, bool visible, const std::vector<SemAnnotation>& annos,
    std::optional<ChangeRecord> rec)
{
    const std::string_view attrName = visible ? "RuntimeVisibleAnnotations"
                                              : "RuntimeInvisibleAnnotations";
    if (annos.size() > 0xFFFFu) {
        return Err(LowerError::DomainOverflow);   // num_annotations 是 u2
    }
    const Result<void, LowerError> rp = mountPrecheck(table, attrName);
    if (rp.is_err()) {
        return rp;
    }
    // 规格容量预检：全树追加数 + 属性名（属性名的 miss 数与 mountPrecheck
    // 同判据，重叠安全——两次判定同一事实）。
    {
        ca::u32 additions = 0;
        for (const SemAnnotation& a : annos) {
            const Result<ca::u32, LowerError> rc = countAnnoAdditions(a, 0);
            if (rc.is_err()) {
                return Err(rc.unwrap_err());
            }
            additions += rc.unwrap();
        }
        SemIndex& ix = sem();
        additions += ix.utf8.find(attrName) == ix.utf8.end() ? 1 : 0;
        if (backing_->cp.size() + additions > 0xFFFFu) {
            return Err(LowerError::CpAppendFailed);
        }
    }
    auto body = std::make_unique<core::AnnotationsAttr>();
    for (const SemAnnotation& a : annos) {
        Result<core::AnnotationsAttr::Annotation, LowerError> ra
            = synthAnnotation(a, 0);
        if (ra.is_err()) {
            return Err(ra.unwrap_err());
        }
        body->annos.push_back(std::move(ra).unwrap());
    }
    const Result<void, LowerError> rm = mountAttr(table, attrName, std::move(body));
    if (rm.is_err()) {
        return rm;
    }
    if (rec.has_value()) {
        noteChange(std::move(*rec));
    }
    return Ok();
}

Result<void, LowerError> ClassSession::attachAnnotations(MethodId method, bool visible,
                                                         const std::vector<SemAnnotation>& annos)
{
    const MethodUnit* mu = unit_.method(method);
    if (mu == nullptr || mu->backingIndex >= backing_->methods.size()) {
        return Err(LowerError::InvalidHandle);
    }
    ChangeRecord rec;
    rec.kind   = ChangeKind::MemberMetadata;
    rec.member = MemberKind::Method;
    rec.name   = unit_.symbols().str(mu->name);
    rec.desc   = unit_.symbols().str(mu->desc);
    return attachAnnotationsToTable(backing_->methods[mu->backingIndex].attrs, visible,
                                    annos, std::move(rec));
}

Result<void, LowerError> ClassSession::attachAnnotations(FieldId field, bool visible,
                                                         const std::vector<SemAnnotation>& annos)
{
    const FieldUnit* fu = unit_.field(field);
    if (fu == nullptr || fu->backingIndex >= backing_->fields.size()) {
        return Err(LowerError::InvalidHandle);
    }
    ChangeRecord rec;
    rec.kind   = ChangeKind::MemberMetadata;
    rec.member = MemberKind::Field;
    rec.name   = unit_.symbols().str(fu->name);
    rec.desc   = unit_.symbols().str(fu->desc);
    return attachAnnotationsToTable(backing_->fields[fu->backingIndex].attrs, visible,
                                    annos, std::move(rec));
}

Result<void, LowerError> ClassSession::attachClassAnnotations(
    bool visible, const std::vector<SemAnnotation>& annos)
{
    ChangeRecord rec;
    rec.kind   = ChangeKind::ClassMetadata;
    rec.member = MemberKind::None;
    return attachAnnotationsToTable(backing_->attrs, visible, annos, std::move(rec));
}

Result<void, LowerError> ClassSession::attachParameterAnnotations(
    MethodId method, bool visible,
    const std::vector<std::vector<SemAnnotation>>& params)
{
    const MethodUnit* mu = unit_.method(method);
    if (mu == nullptr || mu->backingIndex >= backing_->methods.size()) {
        return Err(LowerError::InvalidHandle);
    }
    if (params.size() > std::numeric_limits<ca::u8>::max()) {
        return Err(LowerError::DomainOverflow);   // num_parameters 是 u1
    }
    for (const std::vector<SemAnnotation>& group : params) {
        if (group.size() > 0xFFFFu) {
            return Err(LowerError::DomainOverflow);   // num_annotations 是 u2
        }
    }
    // 描述符 arity 硬契约：参数组数必须等于方法参数个数（HotSpot 类解析对
    // 失配抛 MalformedParametersException，合成面提前拦下——同上方域宽闸）。
    {
        const Result<ca::u32, LowerError> rArity = methodDescArity(*this, mu->desc);
        if (rArity.is_err()) {
            return Err(rArity.unwrap_err());
        }
        if (params.size() != rArity.unwrap()) {
            return Err(LowerError::InvalidSeed);
        }
    }
    const std::string_view attrName = visible ? "RuntimeVisibleParameterAnnotations"
                                              : "RuntimeInvisibleParameterAnnotations";
    core::MemberInfo& m = backing_->methods[mu->backingIndex];
    const Result<void, LowerError> rp = mountPrecheck(m.attrs, attrName);
    if (rp.is_err()) {
        return rp;
    }
    // 规格容量预检。
    {
        ca::u32 additions = 0;
        for (const std::vector<SemAnnotation>& group : params) {
            for (const SemAnnotation& a : group) {
                const Result<ca::u32, LowerError> rc = countAnnoAdditions(a, 0);
                if (rc.is_err()) {
                    return Err(rc.unwrap_err());
                }
                additions += rc.unwrap();
            }
        }
        SemIndex& ix = sem();
        additions += ix.utf8.find(attrName) == ix.utf8.end() ? 1 : 0;
        if (backing_->cp.size() + additions > 0xFFFFu) {
            return Err(LowerError::CpAppendFailed);
        }
    }
    auto body = std::make_unique<core::ParameterAnnotationsAttr>();
    for (const std::vector<SemAnnotation>& group : params) {
        std::vector<core::AnnotationsAttr::Annotation> out;
        for (const SemAnnotation& a : group) {
            Result<core::AnnotationsAttr::Annotation, LowerError> ra
                = synthAnnotation(a, 0);
            if (ra.is_err()) {
                return Err(ra.unwrap_err());
            }
            out.push_back(std::move(ra).unwrap());
        }
        body->parameters.push_back(std::move(out));
    }
    const Result<void, LowerError> rm = mountAttr(m.attrs, attrName, std::move(body));
    if (rm.is_err()) {
        return rm;
    }
    ChangeRecord rec;
    rec.kind   = ChangeKind::MemberMetadata;
    rec.member = MemberKind::Method;
    rec.name   = unit_.symbols().str(mu->name);
    rec.desc   = unit_.symbols().str(mu->desc);
    noteChange(rec);
    return Ok();
}

Result<void, LowerError> ClassSession::attachAnnotationDefault(MethodId method,
                                                               const SemElemValue& v)
{
    const MethodUnit* mu = unit_.method(method);
    if (mu == nullptr || mu->backingIndex >= backing_->methods.size()) {
        return Err(LowerError::InvalidHandle);
    }
    core::MemberInfo& m = backing_->methods[mu->backingIndex];
    const Result<void, LowerError> rp = mountPrecheck(m.attrs, "AnnotationDefault");
    if (rp.is_err()) {
        return rp;
    }
    // 规格容量预检（元素规格树；Nested 走 anno 遍历、其余走 elem 遍历）。
    // 结构合法性先判（ElemValue::Nested 的空指针防御）。
    {
        if (!elemWellFormed(v)) {
            return Err(LowerError::InvalidSeed);
        }
        const Result<ca::u32, LowerError> rc
            = v.form == SemElemValue::Form::Nested ? countAnnoAdditions(*v.nested, 0)
                                                   : countElemAdditions(v, 0);
        if (rc.is_err()) {
            return Err(rc.unwrap_err());
        }
        SemIndex& ix = sem();
        if (backing_->cp.size() + rc.unwrap()
                + (ix.utf8.find("AnnotationDefault") == ix.utf8.end() ? 1 : 0)
            > 0xFFFFu) {
            return Err(LowerError::CpAppendFailed);
        }
    }
    Result<core::AnnotationsAttr::ElemValue, LowerError> rv = synthElem(v, 0);
    if (rv.is_err()) {
        return Err(rv.unwrap_err());
    }
    auto body = std::make_unique<core::AnnotationDefaultAttr>();
    body->defaultValue = std::move(rv).unwrap();
    const Result<void, LowerError> rm = mountAttr(m.attrs, "AnnotationDefault", std::move(body));
    if (rm.is_err()) {
        return rm;
    }
    ChangeRecord rec;
    rec.kind   = ChangeKind::MemberMetadata;
    rec.member = MemberKind::Method;
    rec.name   = unit_.symbols().str(mu->name);
    rec.desc   = unit_.symbols().str(mu->desc);
    noteChange(rec);
    return Ok();
}

Result<void, LowerError> ClassSession::attachMethodParameters(
    MethodId method, const std::vector<ParamNameRow>& rows)
{
    const MethodUnit* mu = unit_.method(method);
    if (mu == nullptr || mu->backingIndex >= backing_->methods.size()) {
        return Err(LowerError::InvalidHandle);
    }
    if (rows.size() > std::numeric_limits<ca::u8>::max()) {
        return Err(LowerError::DomainOverflow);   // parameters_count 是 u1
    }
    for (const ParamNameRow& row : rows) {
        if (row.name.size() > 0xFFFFu) {
            return Err(LowerError::DomainOverflow);
        }
    }
    // 描述符 arity 硬契约：行数等于方法参数个数，实例方法额外放行「描述符
    // arity + 1」的接收者形态——JVMS 4.7.24 允许 MethodParameters 把 receiver
    // 计入参数行（javac 对非静态方法恒带 this 行）；反射入口对失配产物抛
    // MalformedParametersException，能被 JVM 证伪的垃圾按域宽闸门同一纪律
    // 提前拦下（接收者行内容不可静态证伪，只按行数放行）。
    {
        const Result<ca::u32, LowerError> rArity = methodDescArity(*this, mu->desc);
        if (rArity.is_err()) {
            return Err(rArity.unwrap_err());
        }
        const ca::u32 arity = rArity.unwrap();
        const bool    isStatic = (mu->access & 0x0008) != 0;   // ACC_STATIC
        const bool    ok       = rows.size() == arity ||
                     (!isStatic && rows.size() == static_cast<ca::usize>(arity) + 1);
        if (!ok) {
            return Err(LowerError::InvalidSeed);
        }
    }
    core::MemberInfo& m = backing_->methods[mu->backingIndex];
    const Result<void, LowerError> rp = mountPrecheck(m.attrs, "MethodParameters");
    if (rp.is_err()) {
        return rp;
    }
    // 容量预检：行名字 Utf8 miss 各计 1。
    {
        SemIndex& ix      = sem();
        ca::u32 additions = ix.utf8.find("MethodParameters") == ix.utf8.end() ? 1 : 0;
        for (const ParamNameRow& row : rows) {
            if (!row.name.empty() && ix.utf8.find(row.name) == ix.utf8.end()) {
                ++additions;
            }
        }
        if (backing_->cp.size() + additions > 0xFFFFu) {
            return Err(LowerError::CpAppendFailed);
        }
    }
    auto body = std::make_unique<core::MethodParametersAttr>();
    for (const ParamNameRow& row : rows) {
        core::MethodParametersAttr::Row r;
        r.nameCp = row.name.empty() ? 0 : internUtf8Sem(row.name);
        r.accessFlags = row.flags;
        body->rows.push_back(r);
    }
    const Result<void, LowerError> rm = mountAttr(m.attrs, "MethodParameters", std::move(body));
    if (rm.is_err()) {
        return rm;
    }
    ChangeRecord rec;
    rec.kind   = ChangeKind::MemberMetadata;
    rec.member = MemberKind::Method;
    rec.name   = unit_.symbols().str(mu->name);
    rec.desc   = unit_.symbols().str(mu->desc);
    noteChange(rec);
    return Ok();
}

Result<void, LowerError> ClassSession::attachConstantValue(FieldId field,
                                                           const ConstValueSpec& v)
{
    const FieldUnit* fu = unit_.field(field);
    if (fu == nullptr || fu->backingIndex >= backing_->fields.size()) {
        return Err(LowerError::InvalidHandle);
    }
    if (v.kind == ConstValueSpec::Kind::String && v.str.size() > 0xFFFFu) {
        return Err(LowerError::DomainOverflow);
    }
    core::MemberInfo& m = backing_->fields[fu->backingIndex];
    const Result<void, LowerError> rp = mountPrecheck(m.attrs, "ConstantValue");
    if (rp.is_err()) {
        return rp;
    }
    // 容量预检：常量条目 miss 计 1 + 属性名 miss 计 1。
    {
        SemIndex& ix = sem();
        ca::u32 additions
            = ix.utf8.find("ConstantValue") == ix.utf8.end() ? 1 : 0;
        using Kind = ConstValueSpec::Kind;
        switch (v.kind) {
        case Kind::Int:
        case Kind::Bool:
        case Kind::Byte:
        case Kind::Char:
        case Kind::Short:
            additions += ix.intConst.find(static_cast<ca::i32>(v.num)) == ix.intConst.end()
                             ? 1
                             : 0;
            break;
        case Kind::Long:
            additions += ix.longConst.find(v.num) == ix.longConst.end() ? 1 : 0;
            break;
        case Kind::Float:
            additions += ix.floatConst.find(v.fbits) == ix.floatConst.end() ? 1 : 0;
            break;
        case Kind::Double:
            additions += ix.doubleConst.find(v.dbits) == ix.doubleConst.end() ? 1 : 0;
            break;
        case Kind::String: {
            if (ix.utf8.find(v.str) == ix.utf8.end()) {
                additions += 2;
            } else if (ix.strConst.find(ix.utf8.at(v.str)) == ix.strConst.end()) {
                additions += 1;
            }
            break;
        }
        default:
            return Err(LowerError::InvalidSeed);
        }
        if (backing_->cp.size() + additions > 0xFFFFu) {
            return Err(LowerError::CpAppendFailed);
        }
    }
    auto body = std::make_unique<core::ConstantValueAttr>();
    using Kind   = ConstValueSpec::Kind;
    switch (v.kind) {
    case Kind::Int:
    case Kind::Bool:
    case Kind::Byte:
    case Kind::Char:
    case Kind::Short:
        body->constantValueCp = internInteger(static_cast<ca::i32>(v.num));
        break;
    case Kind::Long:
        body->constantValueCp = internLong(v.num);
        break;
    case Kind::Float:
        body->constantValueCp = internFloatBits(v.fbits);
        break;
    case Kind::Double:
        body->constantValueCp = internDoubleBits(v.dbits);
        break;
    case Kind::String:
        body->constantValueCp = internString(v.str);
        break;
    }
    const Result<void, LowerError> rm = mountAttr(m.attrs, "ConstantValue", std::move(body));
    if (rm.is_err()) {
        return rm;
    }
    ChangeRecord rec;
    rec.kind   = ChangeKind::MemberMetadata;
    rec.member = MemberKind::Field;
    rec.name   = unit_.symbols().str(fu->name);
    rec.desc   = unit_.symbols().str(fu->desc);
    noteChange(rec);
    return Ok();
}

// ---------------------------------------------------------------------------
// 通用挂载：调用方自构造 typed 体的直挂入口
// ---------------------------------------------------------------------------

Result<void, LowerError> ClassSession::attachAttr(MethodId method,
                                                  std::string_view attrName,
                                                  std::unique_ptr<core::AttrBody> body)
{
    const MethodUnit* mu = unit_.method(method);
    if (mu == nullptr || mu->backingIndex >= backing_->methods.size()) {
        return Err(LowerError::InvalidHandle);
    }
    core::MemberInfo& m = backing_->methods[mu->backingIndex];
    const Result<void, LowerError> rp = mountPrecheck(m.attrs, attrName);
    if (rp.is_err()) {
        return rp;
    }
    const Result<void, LowerError> rm = mountAttr(m.attrs, attrName, std::move(body));
    if (rm.is_err()) {
        return rm;
    }
    ChangeRecord rec;
    rec.kind   = ChangeKind::MemberMetadata;
    rec.member = MemberKind::Method;
    rec.name   = unit_.symbols().str(mu->name);
    rec.desc   = unit_.symbols().str(mu->desc);
    noteChange(rec);
    return Ok();
}

Result<void, LowerError> ClassSession::attachAttr(FieldId field,
                                                  std::string_view attrName,
                                                  std::unique_ptr<core::AttrBody> body)
{
    const FieldUnit* fu = unit_.field(field);
    if (fu == nullptr || fu->backingIndex >= backing_->fields.size()) {
        return Err(LowerError::InvalidHandle);
    }
    core::MemberInfo& m = backing_->fields[fu->backingIndex];
    const Result<void, LowerError> rp = mountPrecheck(m.attrs, attrName);
    if (rp.is_err()) {
        return rp;
    }
    const Result<void, LowerError> rm = mountAttr(m.attrs, attrName, std::move(body));
    if (rm.is_err()) {
        return rm;
    }
    ChangeRecord rec;
    rec.kind   = ChangeKind::MemberMetadata;
    rec.member = MemberKind::Field;
    rec.name   = unit_.symbols().str(fu->name);
    rec.desc   = unit_.symbols().str(fu->desc);
    noteChange(rec);
    return Ok();
}

Result<void, LowerError> ClassSession::attachClassAttr(std::string_view attrName,
                                                       std::unique_ptr<core::AttrBody> body)
{
    const Result<void, LowerError> rp = mountPrecheck(backing_->attrs, attrName);
    if (rp.is_err()) {
        return rp;
    }
    const Result<void, LowerError> rm
        = mountAttr(backing_->attrs, attrName, std::move(body));
    if (rm.is_err()) {
        return rm;
    }
    ChangeRecord rec;
    rec.kind   = ChangeKind::ClassMetadata;
    rec.member = MemberKind::None;
    noteChange(rec);
    return Ok();
}

// ---------------------------------------------------------------------------
// BSM 语义 intern 家族：句柄/MethodType/Module/Package/Dynamic 的
// 去重 intern（命中既有池不膨胀，miss 才追加）+ BootstrapMethods 条目面。
// ---------------------------------------------------------------------------

Result<core::BootstrapMethodsAttr*, LowerError> seekBsmAttr(ClassSession& s)
{
    core::ClassFile& cf = s.backing();
    for (core::AttrSlot& slot : cf.attrs) {
        if (!cf.cp.utf8Equals(slot.nameCp, "BootstrapMethods")) {
            continue;
        }
        if (!slot.decoded()) {
            const Result<core::AttrBody*, core::ReadError> d = cf.decodeAttr(slot);
            if (d.is_err()) {
                return Err(LowerError::MalformedAttr);
            }
        }
        if (slot.peek()->kind() != core::AttrKind::BootstrapMethods) {
            return Err(LowerError::MalformedAttr);
        }
        return Ok(static_cast<core::BootstrapMethodsAttr*>(slot.body.get()));
    }
    return Ok(static_cast<core::BootstrapMethodsAttr*>(nullptr));
}

Result<ca::u32, LowerError> ClassSession::internMethodHandle(const MethodHandleSpec& h)
{
    const std::optional<core::CpTag> tag = mhRefTagOf(h.referenceKind);
    if (!tag.has_value()) {
        return Err(LowerError::InvalidSeed);
    }
    ca::u32 refIdx = 0;
    switch (*tag) {
    case core::CpTag::Fieldref:
        refIdx = internFieldRef(h.owner, h.name, h.desc);
        break;
    case core::CpTag::Methodref:
        refIdx = internMethodRef(h.owner, h.name, h.desc);
        break;
    default:
        refIdx = internInterfaceMethodRef(h.owner, h.name, h.desc);
        break;
    }
    SemIndex& ix = sem();
    const ca::u64 key = packMhKey(h.referenceKind, refIdx);
    const auto it = ix.mhConst.find(key);
    if (it != ix.mhConst.end()) {
        return Ok(it->second);
    }
    const ca::u32 idx = backing_->cp.addMethodHandle(h.referenceKind, refIdx);
    ix.mhConst.emplace(key, idx);
    return Ok(idx);
}

ca::u32 ClassSession::internMethodType(std::string_view desc)
{
    const ca::u32 descIdx = internUtf8Sem(desc);
    SemIndex& ix = sem();
    const auto it = ix.mtConst.find(descIdx);
    if (it != ix.mtConst.end()) {
        return it->second;
    }
    const ca::u32 idx = backing_->cp.addMethodType(descIdx);
    ix.mtConst.emplace(descIdx, idx);
    return idx;
}

ca::u32 ClassSession::internModule(std::string_view name)
{
    const ca::u32 nameIdx = internUtf8Sem(name);
    SemIndex& ix = sem();
    const ca::u64 key = packNamedKey(core::CpTag::Module, nameIdx);
    const auto it = ix.namedConst.find(key);
    if (it != ix.namedConst.end()) {
        return it->second;
    }
    const ca::u32 idx = backing_->cp.addModule(nameIdx);
    ix.namedConst.emplace(key, idx);
    return idx;
}

ca::u32 ClassSession::internPackage(std::string_view name)
{
    const ca::u32 nameIdx = internUtf8Sem(name);
    SemIndex& ix = sem();
    const ca::u64 key = packNamedKey(core::CpTag::Package, nameIdx);
    const auto it = ix.namedConst.find(key);
    if (it != ix.namedConst.end()) {
        return it->second;
    }
    const ca::u32 idx = backing_->cp.addPackage(nameIdx);
    ix.namedConst.emplace(key, idx);
    return idx;
}

Result<ca::u32, LowerError> ClassSession::internDynamic(ca::u32 bsmIndex,
                                                        std::string_view name,
                                                        std::string_view desc)
{
    // BSM 依赖锚点存在性：条目下标必须绑定在既有 BootstrapMethods 属性上
    // （JVM 类解析对无锚 BSM 引用直接拒——condy/indy 共用条目空间）。
    const Result<core::BootstrapMethodsAttr*, LowerError> rb = seekBsmAttr(*this);
    if (rb.is_err()) {
        return Err(rb.unwrap_err());
    }
    const core::BootstrapMethodsAttr* body = rb.unwrap();
    if (body == nullptr || bsmIndex >= body->entries.size()) {
        return Err(LowerError::InvalidSeed);
    }
    const ca::u32 nameIdx = internUtf8Sem(name);
    const ca::u32 descIdx = internUtf8Sem(desc);
    SemIndex& ix = sem();
    ca::u32 natIdx;
    const auto nit = ix.nat.find(packNatKey(nameIdx, descIdx));
    if (nit != ix.nat.end()) {
        natIdx = nit->second;
    } else {
        natIdx = backing_->cp.addNameAndType(nameIdx, descIdx);
        ix.nat.emplace(packNatKey(nameIdx, descIdx), natIdx);
    }
    // Dynamic 并入 ref 表：键 = (tag, bsm 下标占 a 槽, nat)。
    const ca::u64 key = packRefKey(core::CpTag::Dynamic, bsmIndex, natIdx);
    const auto it = ix.ref.find(key);
    if (it != ix.ref.end()) {
        return Ok(it->second);
    }
    const ca::u32 idx = backing_->cp.addDynamic(bsmIndex, natIdx);
    ix.ref.emplace(key, idx);
    return Ok(idx);
}

/// @brief 静态参数落位（预检之后的合成步）。
Result<ca::u32, LowerError> internBsmArg(ClassSession& s, const BsmArg& a)
{
    switch (a.kind) {
    case BsmArg::Kind::Int:
        return Ok(s.internInteger(static_cast<ca::i32>(a.num)));
    case BsmArg::Kind::Long:
        return Ok(s.internLong(a.num));
    case BsmArg::Kind::Float:
        return Ok(s.internFloatBits(a.fbits));
    case BsmArg::Kind::Double:
        return Ok(s.internDoubleBits(a.dbits));
    case BsmArg::Kind::String:
        return Ok(s.internString(a.str));
    case BsmArg::Kind::Class:
        return Ok(s.internClass(a.str));
    case BsmArg::Kind::MethodType:
        return Ok(s.internMethodType(a.str));
    case BsmArg::Kind::MethodHandle:
        return s.internMethodHandle(a.handle);
    case BsmArg::Kind::Dynamic:
        return s.internDynamic(a.bsm, a.name, a.desc);
    }
    return Err(LowerError::InvalidSeed);
}

Result<ca::u32, LowerError> ClassSession::appendBootstrapMethod(
    const MethodHandleSpec& handle, const std::vector<BsmArg>& args)
{
    // (1) 句柄域闸：kind ∉ 1..9 是 JVM 类解析即拒的形态。
    if (!mhRefTagOf(handle.referenceKind).has_value()) {
        return Err(LowerError::InvalidSeed);
    }
    // (2) 域宽预检：bootstrap_arguments 是 u2。
    if (args.size() > 0xFFFFu) {
        return Err(LowerError::DomainOverflow);
    }
    // (3) 条目形状/合法性预检：既有槽先解码（MalformedAttr = 模型侧损坏）；
    //     条目数 u2 闸 + 条件参数的下标锚点检查（attr 缺位而引 Dynamic 是
    //     与 JVM 解析同拒的形态）。
    const Result<core::BootstrapMethodsAttr*, LowerError> rb = seekBsmAttr(*this);
    if (rb.is_err()) {
        return Err(rb.unwrap_err());
    }
    core::BootstrapMethodsAttr* body = rb.unwrap();
    if (body != nullptr && body->entries.size() >= 0xFFFFu) {
        return Err(LowerError::DomainOverflow);   // num_bootstrap_methods u2
    }
    auto checkArg = [&](const BsmArg& a) -> Result<void, LowerError> {
        switch (a.kind) {
        case BsmArg::Kind::String:
        case BsmArg::Kind::Class:
        case BsmArg::Kind::MethodType:
            if (a.str.empty()) {
                return Err(LowerError::InvalidSeed);
            }
            break;
        case BsmArg::Kind::Dynamic:
            if (body == nullptr || a.bsm >= body->entries.size()) {
                return Err(LowerError::InvalidSeed);
            }
            break;
        default:
            break;
        }
        return Ok();
    };
    for (const BsmArg& a : args) {
        const Result<void, LowerError> ra = checkArg(a);
        if (ra.is_err()) {
            return Err(ra.unwrap_err());
        }
    }
    // (4) 规格容量预检（CP 追加数：attr 名 + 句柄族 + 各参数；miss 才计）。
    {
        SemIndex& ix = sem();
        const core::CpTag tag = mhRefTagOf(handle.referenceKind).value();
        ca::u32 additions = countHandleAdditions(ix, tag, handle);
        for (const BsmArg& a : args) {
            additions += countBsmArgAdditions(ix, a);
        }
        additions += ix.utf8.find("BootstrapMethods") == ix.utf8.end() ? 1 : 0;
        if (backing_->cp.size() + additions > 0xFFFFu) {
            return Err(LowerError::CpAppendFailed);
        }
    }
    // (5) 合成（CP 追加；条件参数此刻必命中 (3) 的锚点闸——预检与合成之间
    //     条目数只增不减）。
    const Result<ca::u32, LowerError> rh = internMethodHandle(handle);
    if (rh.is_err()) {
        return Err(rh.unwrap_err());
    }
    const ca::u32 handleRef = rh.unwrap();
    std::vector<ca::u32> argCp(args.size());
    for (size_t i = 0; i < args.size(); ++i) {
        const Result<ca::u32, LowerError> ra = internBsmArg(*this, args[i]);
        if (ra.is_err()) {
            return Err(ra.unwrap_err());
        }
        argCp[i] = ra.unwrap();
    }
    // (6) 落位：同形条目命中复用——但仅在计数为 0（纯命中）时可能成立：
    //     计数 > 0 必有真追加，追加下标 ≥ 池既有容量，不可能与既有条目同值。
    if (body != nullptr) {
        for (ca::u32 i = 0; i < body->entries.size(); ++i) {
            const core::BootstrapMethodsAttr::Entry& e = body->entries[i];
            if (e.methodRef == handleRef && e.args == argCp) {
                return Ok(i);   // 纯命中：无实改，不记账
            }
        }
    }
    // 条目落位：attr 缺则建（类级单例；attributes_count 域已在此前闸）。
    if (body == nullptr) {
        if (backing_->attrs.size() >= 0xFFFFu) {
            return Err(LowerError::CpAppendFailed);   // attributes_count u16 域
        }
        auto born = std::make_unique<core::BootstrapMethodsAttr>();
        body = born.get();
        const Result<void, LowerError> rm = mountAttr(backing_->attrs, "BootstrapMethods",
                                                      std::move(born));
        if (rm.is_err()) {
            return Err(rm.unwrap_err());
        }
    }
    body->entries.push_back(core::BootstrapMethodsAttr::Entry{handleRef, std::move(argCp)});
    ChangeRecord rec;
    rec.kind   = ChangeKind::ClassMetadata;
    rec.member = MemberKind::None;
    noteChange(rec);
    return Ok(static_cast<ca::u32>(body->entries.size() - 1));
}

}   // namespace jbc::ir
