/// @file        type_annotation_attr.cpp
/// @brief       类型注解手术的实现：定位 → 描述符 span 改写 → 落位。
#include "jbc/ir/type_annotation_attr.hpp"

#include <string>
#include <string_view>
#include <vector>

#include "jbc/core/attribute.hpp"
#include "jbc/core/constant_pool.hpp"

namespace jbc::ir {

namespace {

constexpr std::string_view kRvtaName = "RuntimeVisibleTypeAnnotations";
constexpr std::string_view kRitaName = "RuntimeInvisibleTypeAnnotations";

/// @brief CP u16 索引域上限（constant_pool_count 的合法最大值）。
constexpr ca::u32 kMaxCpCount = 0xFFFF;

bool isTaSlot(const core::ClassFile& cf, const core::AttrSlot& slot) noexcept
{
    return cf.cp.utf8Equals(slot.nameCp, kRvtaName) ||
           cf.cp.utf8Equals(slot.nameCp, kRitaName);
}

/// @brief 查重落位：命中既有池返回原索引，miss 才追加（追加前做容量预检）。
Result<ca::u32, TaAttrError> internDescriptor(core::ClassFile& cf, std::string_view s)
{
    core::ConstantPool& cp = cf.cp;
    if (const ca::u32 hit = cp.findUtf8(s); hit != 0) {
        return Ok(hit);
    }
    if (cp.size() > kMaxCpCount) {
        return Err(TaAttrError{TaAttrError::Kind::CpCapacity,
                               jbc::type::DescError::Empty});
    }
    return Ok(cp.addUtf8(s));
}

/// @brief 一个待落位项：CP 索引槽位（已解码体内的 u32）+ 改写后的新描述符
///        + 命中数。槽位指针在预检与落位两阶段间有效（两阶段之间不改写
///        任何体结构，向量不重分配）。
struct PendingCpRewrite
{
    core::AttrSlot* slot;
    ca::u32* field;
    std::string newDesc;
    ca::u32 hits;
};

/// @brief 对一个承载字段描述符的 CP 槽位做纯预检（解析 + span 命中 + 改写
///        串计算），不改写、不 intern。
///
/// 基类型/void 描述符（B C D F I J S Z V）不可能含类名，直接跳过——
/// `@Anno(void.class)` 的 'c' 值是合法输入，不是畸形。
Result<ca::u32, TaAttrError> planDescriptorPos(core::ClassFile& cf, core::AttrSlot& slot,
                                               ca::u32& field, std::string_view old_internal,
                                               std::string_view new_internal,
                                               std::vector<PendingCpRewrite>& out)
{
    static constexpr std::string_view kBaseTypes = "BCDFIJSZV";
    const std::string_view text = cf.utf8(field);
    if (text.empty()) {
        return Err(TaAttrError{TaAttrError::Kind::Descriptor,
                               jbc::type::DescError::Empty});
    }
    if (kBaseTypes.find(text.front()) != std::string_view::npos) {
        return Ok(ca::u32{0});
    }
    auto spans = jbc::type::classNameSpans(text);
    if (spans.is_err()) {
        return Err(TaAttrError{TaAttrError::Kind::Descriptor, spans.unwrap_err()});
    }
    std::vector<jbc::type::ClassNameSpan> hits;
    for (const jbc::type::ClassNameSpan& s : std::move(spans).unwrap()) {
        if (text.substr(s.offset, s.length) == old_internal) {
            hits.push_back(s);
        }
    }
    if (hits.empty()) {
        return Ok(ca::u32{0});
    }
    // 倒序逐 span 结构化替换（后面的偏移先改，前面 span 的偏移保持有效）。
    std::string cur{text};
    for (auto it = hits.rbegin(); it != hits.rend(); ++it) {
        auto r = jbc::type::replaceClassName(cur, *it, new_internal);
        if (r.is_err()) {
            return Err(TaAttrError{TaAttrError::Kind::Descriptor, r.unwrap_err()});
        }
        cur = std::string(std::string_view{std::move(r).unwrap()});
    }
    out.push_back(PendingCpRewrite{&slot, &field, std::move(cur),
                                   static_cast<ca::u32>(hits.size())});
    return Ok(static_cast<ca::u32>(hits.size()));
}

/// @brief 递归预检一条 type_annotation：type_index + 全部 element_value。
///        （前置声明：'@' 嵌套使 planElemValue 与之互递归。）
Result<ca::u32, TaAttrError> planAnnotation(core::ClassFile& cf, core::AttrSlot& slot,
                                            core::TypeAnnotationsAttr::Annotation& a,
                                            std::string_view old_internal,
                                            std::string_view new_internal,
                                            std::vector<PendingCpRewrite>& out);

/// @brief 递归预检一个 element_value 里的 'c' 类描述符（含嵌套 '@'/'['）。
///        （前置声明：'@' 嵌套使它与 planSimpleAnnotation 互递归。）
Result<ca::u32, TaAttrError> planElemValue(core::ClassFile& cf, core::AttrSlot& slot,
                                           core::AnnotationsAttr::ElemValue& e,
                                           std::string_view old_internal,
                                           std::string_view new_internal,
                                           std::vector<PendingCpRewrite>& out);

/// @brief 递归预检一条简单注解（'@' 嵌套形态）：type 描述符 + 全部值。
Result<ca::u32, TaAttrError> planSimpleAnnotation(
    core::ClassFile& cf, core::AttrSlot& slot, core::AnnotationsAttr::Annotation& a,
    std::string_view old_internal, std::string_view new_internal,
    std::vector<PendingCpRewrite>& out)
{
    ca::u32 hits = 0;
    auto t = planDescriptorPos(cf, slot, a.typeCp, old_internal, new_internal, out);
    if (t.is_err()) {
        return Err(std::move(t).unwrap_err());
    }
    hits += std::move(t).unwrap();
    for (auto& kv : a.values) {
        auto n = planElemValue(cf, slot, kv.second, old_internal, new_internal, out);
        if (n.is_err()) {
            return Err(std::move(n).unwrap_err());
        }
        hits += std::move(n).unwrap();
    }
    return Ok(hits);
}

/// @brief 递归预检一个 element_value 里的 'c' 类描述符（含嵌套 '@'/'['）。
Result<ca::u32, TaAttrError> planElemValue(core::ClassFile& cf, core::AttrSlot& slot,
                                           core::AnnotationsAttr::ElemValue& e,
                                           std::string_view old_internal,
                                           std::string_view new_internal,
                                           std::vector<PendingCpRewrite>& out)
{
    ca::u32 hits = 0;
    if (e.tag == 'c') {
        auto n = planDescriptorPos(cf, slot, e.cp, old_internal, new_internal, out);
        if (n.is_err()) {
            return Err(std::move(n).unwrap_err());
        }
        hits += std::move(n).unwrap();
    }
    if (e.tag == '@' && e.nested != nullptr) {
        auto n = planSimpleAnnotation(cf, slot, *e.nested, old_internal, new_internal,
                                      out);
        if (n.is_err()) {
            return Err(std::move(n).unwrap_err());
        }
        hits += std::move(n).unwrap();
    }
    for (auto& item : e.items) {
        auto n = planElemValue(cf, slot, item, old_internal, new_internal, out);
        if (n.is_err()) {
            return Err(std::move(n).unwrap_err());
        }
        hits += std::move(n).unwrap();
    }
    return Ok(hits);
}

/// @brief 递归预检一条 type_annotation：type_index + 全部 element_value。
Result<ca::u32, TaAttrError> planAnnotation(core::ClassFile& cf, core::AttrSlot& slot,
                                            core::TypeAnnotationsAttr::Annotation& a,
                                            std::string_view old_internal,
                                            std::string_view new_internal,
                                            std::vector<PendingCpRewrite>& out)
{
    ca::u32 hits = 0;
    auto t = planDescriptorPos(cf, slot, a.typeIndexCp, old_internal, new_internal, out);
    if (t.is_err()) {
        return Err(std::move(t).unwrap_err());
    }
    hits += std::move(t).unwrap();
    for (auto& kv : a.values) {
        auto n = planElemValue(cf, slot, kv.second, old_internal, new_internal, out);
        if (n.is_err()) {
            return Err(std::move(n).unwrap_err());
        }
        hits += std::move(n).unwrap();
    }
    return Ok(hits);
}

/// @brief 预检一个 TypeAnnotations 属性体（类型已判定的调用方用）。
Result<ca::u32, TaAttrError> planTaBody(core::ClassFile& cf, core::AttrSlot& slot,
                                        core::TypeAnnotationsAttr& body,
                                        std::string_view old_internal,
                                        std::string_view new_internal,
                                        std::vector<PendingCpRewrite>& out)
{
    ca::u32 hits = 0;
    for (auto& a : body.annotations) {
        auto n = planAnnotation(cf, slot, a, old_internal, new_internal, out);
        if (n.is_err()) {
            return Err(std::move(n).unwrap_err());
        }
        hits += std::move(n).unwrap();
    }
    return Ok(hits);
}

/// @brief 落位一个待改写项：intern 去重 + 改 CP 索引 + 槽置脏。
Result<void, TaAttrError> applyPending(core::ClassFile& cf, const PendingCpRewrite& p)
{
    auto idx = internDescriptor(cf, p.newDesc);
    if (idx.is_err()) {
        return Err(std::move(idx).unwrap_err());
    }
    *p.field = std::move(idx).unwrap();
    (void)p.slot->mutate();
    return Ok();
}

/// @brief 属性表扫描（一层的全部 Runtime*TypeAnnotations 槽）。
Result<ca::u32, TaAttrError> planAttrList(core::ClassFile& cf,
                                          std::vector<core::AttrSlot>& slots,
                                          ca::u32& attrsTotal,
                                          std::string_view old_internal,
                                          std::string_view new_internal,
                                          std::vector<PendingCpRewrite>& out)
{
    ca::u32 hits = 0;
    for (core::AttrSlot& slot : slots) {
        if (!isTaSlot(cf, slot)) {
            continue;
        }
        ++attrsTotal;
        auto d = cf.decodeAttr(slot);
        if (d.is_err()) {
            return Err(TaAttrError{TaAttrError::Kind::SlotDecode,
                                   jbc::type::DescError::Empty});
        }
        auto* body = static_cast<core::TypeAnnotationsAttr*>(std::move(d).unwrap());
        auto n = planTaBody(cf, slot, *body, old_internal, new_internal, out);
        if (n.is_err()) {
            return Err(std::move(n).unwrap_err());
        }
        hits += std::move(n).unwrap();
    }
    return Ok(hits);
}

}   // namespace

Result<TaRenameSummary, TaAttrError>
renameClassInTypeAnnotations(core::ClassFile& cf, std::string_view old_internal,
                             std::string_view new_internal)
{
    // ---- 阶段一：全量预检（纯读；解码只推进 Untouched→Decoded，写回对
    //      Decoded 仍走 copy，字节零影响）----
    std::vector<PendingCpRewrite> pending;
    TaRenameSummary st;

    auto rClass = planAttrList(cf, cf.attrs, st.attrsTotal, old_internal, new_internal,
                               pending);
    if (rClass.is_err()) {
        return Err(std::move(rClass).unwrap_err());
    }
    for (core::MemberInfo& f : cf.fields) {
        auto r = planAttrList(cf, f.attrs, st.attrsTotal, old_internal, new_internal,
                              pending);
        if (r.is_err()) {
            return Err(std::move(r).unwrap_err());
        }
    }
    for (core::MemberInfo& m : cf.methods) {
        auto r = planAttrList(cf, m.attrs, st.attrsTotal, old_internal, new_internal,
                              pending);
        if (r.is_err()) {
            return Err(std::move(r).unwrap_err());
        }
        // Code 嵌套（CAST/INSTANCEOF/NEW/构造实参等指令级 type annotation）。
        for (core::AttrSlot& slot : m.attrs) {
            if (!cf.cp.utf8Equals(slot.nameCp, "Code")) {
                continue;
            }
            auto c = cf.decodeAttr(slot);
            if (c.is_err()) {
                return Err(TaAttrError{TaAttrError::Kind::SlotDecode,
                                       jbc::type::DescError::Empty});
            }
            auto* code = static_cast<core::CodeAttr*>(std::move(c).unwrap());
            auto n = planAttrList(cf, code->attrs, st.attrsTotal, old_internal,
                                  new_internal, pending);
            if (n.is_err()) {
                return Err(std::move(n).unwrap_err());
            }
        }
    }
    if (pending.empty()) {
        return Ok(st);   // 未命中：零改写、零追加
    }

    // attrsChanged：pending 追加序即槽游程序，数首遇槽。
    {
        core::AttrSlot* last = nullptr;
        for (const PendingCpRewrite& p : pending) {
            if (last != p.slot) {
                ++st.attrsChanged;
                last = p.slot;
            }
        }
    }

    // ---- 阶段二：容量预检（先算后写；新串经查重后仍 miss 的条目数）----
    ca::u32 additions = 0;
    for (const PendingCpRewrite& p : pending) {
        if (cf.cp.findUtf8(p.newDesc) == 0) {
            ++additions;
        }
    }
    if (cf.cp.size() + additions > kMaxCpCount) {
        return Err(TaAttrError{TaAttrError::Kind::CpCapacity,
                               jbc::type::DescError::Empty});
    }

    // ---- 阶段三：统一落位（容量已过，intern 不再失败；错误通道保留是
    //      如实传播，不静默吞）----
    for (const PendingCpRewrite& p : pending) {
        auto r = applyPending(cf, p);
        if (r.is_err()) {
            return Err(std::move(r).unwrap_err());
        }
        st.hits += p.hits;
    }
    return Ok(st);
}

}   // namespace jbc::ir
