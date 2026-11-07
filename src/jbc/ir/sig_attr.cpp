/// @file        sig_attr.cpp
/// @brief       Signature 属性手术的实现：定位 → 读体 → 签名树改写 → 落位。
#include "jbc/ir/sig_attr.hpp"

#include <string_view>
#include <vector>

#include "jbc/core/attribute.hpp"
#include "jbc/core/constant_pool.hpp"

#include "jbc/ir/cp_rewrite.hpp"

namespace jbc::ir {

namespace {

constexpr std::string_view kSignatureName = "Signature";

/// @brief 槽是否为 Signature 属性（按属性名判定，零分配）。
bool isSignatureSlot(const core::ClassFile& cf, const core::AttrSlot& slot) noexcept
{
    return cf.cp.utf8Equals(slot.nameCp, kSignatureName);
}

/// @brief 对单个槽做纯预检（解码 + 解析 + 改名），不改写、不 intern。
///
/// 命中时把待落位项挂到 out（field 指向解码体 signature_index，指针存活
/// 契约见 PendingCpRewrite）；未命中返回 0（槽原样）；失败透传。
///
/// 体长 > 2 的畸形输入（尾部垃圾同族）落位时只改 u2 索引，尾部经 trailing
/// 原样保留——重编码写回天然不丢尾。
Result<ca::u32, SigAttrError> planSlot(core::ClassFile& cf, core::AttrSlot& slot,
                                       jbc::type::SignatureForm form,
                                       std::string_view old_internal,
                                       std::string_view new_internal,
                                       std::vector<PendingCpRewrite>& out)
{
    const Result<core::AttrBody*, core::ReadError> d = cf.decodeAttr(slot);
    if (d.is_err()) {
        return Err(SigAttrError{SigAttrError::Kind::SlotDecode, {}});
    }
    auto* attr = static_cast<core::SignatureAttr*>(std::move(d).unwrap());
    const std::string_view sig = cf.utf8(attr->signatureCp);

    auto r = jbc::type::renameClassInSignature(sig, form, old_internal, new_internal);
    if (r.is_err()) {
        return Err(SigAttrError{SigAttrError::Kind::Signature, std::move(r).unwrap_err()});
    }
    jbc::type::SigRenameResult renamed = std::move(r).unwrap();
    if (renamed.hits == 0) {
        return Ok(ca::u32{0});
    }
    out.push_back(PendingCpRewrite{&slot, &attr->signatureCp,
                                   std::string(std::string_view{renamed.signature}),
                                   renamed.hits});
    return Ok(renamed.hits);
}

}   // namespace

Result<SigAttrRename, SigAttrError>
renameClassInSignatureAttr(core::ClassFile& cf, core::AttrSlot& slot,
                           jbc::type::SignatureForm form,
                           std::string_view old_internal, std::string_view new_internal)
{
    if (!isSignatureSlot(cf, slot)) {
        return Ok(SigAttrRename{});
    }

    // 预检（纯读）：解码 + 解析 + 改名；失败/未命中槽保持原样。
    std::vector<PendingCpRewrite> pending;
    auto planned = planSlot(cf, slot, form, old_internal, new_internal, pending);
    if (planned.is_err()) {
        return Err(std::move(planned).unwrap_err());
    }
    if (pending.empty()) {
        return Ok(SigAttrRename{});
    }

    const SigAttrError capacityError{SigAttrError::Kind::CpCapacity, {}};
    const Result<void, SigAttrError> r = applyPendingRewrites(cf, pending, capacityError);
    if (r.is_err()) {
        return Err(std::move(r).unwrap_err());
    }
    return Ok(SigAttrRename{pending.front().hits, true});
}

Result<SigAttrRenameSummary, SigAttrError>
renameClassInSignatureAttrs(core::ClassFile& cf, std::string_view old_internal,
                            std::string_view new_internal)
{
    using jbc::type::SignatureForm;

    // ---- 阶段一：全量预检（纯读；解码只推进 Untouched→Decoded，写回对
    //      Decoded 仍走 copy，字节零影响）----
    std::vector<PendingCpRewrite> pending;
    SigAttrRenameSummary st;

    auto planList = [&](std::vector<core::AttrSlot>& slots,
                        SignatureForm form) -> Result<void, SigAttrError> {
        for (core::AttrSlot& slot : slots) {
            if (!isSignatureSlot(cf, slot)) {
                continue;
            }
            ++st.attrsTotal;
            auto n = planSlot(cf, slot, form, old_internal, new_internal, pending);
            if (n.is_err()) {
                return Err(std::move(n).unwrap_err());
            }
            if (std::move(n).unwrap() > 0) {
                ++st.attrsChanged;
            }
        }
        return Ok();
    };

    auto rClass = planList(cf.attrs, SignatureForm::Class);
    if (rClass.is_err()) {
        return Err(std::move(rClass).unwrap_err());
    }
    for (core::MemberInfo& m : cf.methods) {
        auto r = planList(m.attrs, SignatureForm::Method);
        if (r.is_err()) {
            return Err(std::move(r).unwrap_err());
        }
    }
    for (core::MemberInfo& f : cf.fields) {
        auto r = planList(f.attrs, SignatureForm::Field);
        if (r.is_err()) {
            return Err(std::move(r).unwrap_err());
        }
    }
    if (pending.empty()) {
        return Ok(st);   // 未命中：零改写、零追加
    }

    // ---- 阶段二/三：容量预检 + 统一落位（先算后写；机制单源 cp_rewrite.hpp）。
    const SigAttrError capacityError{SigAttrError::Kind::CpCapacity, {}};
    const Result<void, SigAttrError> r =
        precheckPendingCapacity(cf, pending, capacityError);
    if (r.is_err()) {
        return Err(std::move(r).unwrap_err());
    }
    const Result<void, SigAttrError> applied =
        applyPendingRewrites(cf, pending, capacityError);
    if (applied.is_err()) {
        return Err(std::move(applied).unwrap_err());
    }
    for (const PendingCpRewrite& p : pending) {
        st.hits += p.hits;
    }
    return Ok(st);
}

namespace {

}   // namespace

Result<LvttRenameSummary, SigAttrError>
renameClassInLocalVariableTypeTables(core::ClassFile& cf,
                                     std::string_view old_internal,
                                     std::string_view new_internal)
{
    using jbc::type::SignatureForm;

    constexpr std::string_view kCodeName = "Code";
    constexpr std::string_view kLvttName = "LocalVariableTypeTable";

    // ---- 阶段一：全量预检（纯读）。解码只推进 Untouched→Decoded，写回对
    //      Decoded 仍走 copy；任何解码/解析失败在此整体 Err，零改写零追加。
    //      行指针跨阶段存续（见 LvttPendingRow）。----
    std::vector<PendingCpRewrite> pending;
    LvttRenameSummary st;
    core::AttrSlot* lastSlot = nullptr;   ///< pending 追加序即槽游程序，表计数用

    for (core::MemberInfo& m : cf.methods) {
        for (core::AttrSlot& slot : m.attrs) {
            if (!cf.cp.utf8Equals(slot.nameCp, kCodeName)) {
                continue;
            }
            auto d = cf.decodeAttr(slot);
            if (d.is_err()) {
                return Err(SigAttrError{SigAttrError::Kind::SlotDecode, {}});
            }
            auto* code = static_cast<core::CodeAttr*>(std::move(d).unwrap());
            for (core::AttrSlot& nested : code->attrs) {
                if (!cf.cp.utf8Equals(nested.nameCp, kLvttName)) {
                    continue;
                }
                auto ld = cf.decodeAttr(nested);
                if (ld.is_err()) {
                    return Err(SigAttrError{SigAttrError::Kind::SlotDecode, {}});
                }
                auto* lvt =
                    static_cast<core::LocalVariableTableAttr*>(std::move(ld).unwrap());
                ++st.tablesTotal;
                for (core::LocalVariableTableAttr::Row& row : lvt->rows) {
                    const std::string_view sig = cf.utf8(row.descCp);
                    auto r = jbc::type::renameClassInSignature(
                        sig, SignatureForm::Field, old_internal, new_internal);
                    if (r.is_err()) {
                        return Err(SigAttrError{SigAttrError::Kind::Signature,
                                                std::move(r).unwrap_err()});
                    }
                    jbc::type::SigRenameResult renamed = std::move(r).unwrap();
                    if (renamed.hits == 0) {
                        continue;
                    }
                    pending.push_back(PendingCpRewrite{
                        &nested, &row.descCp,
                        std::string(std::string_view{renamed.signature}), renamed.hits});
                    if (lastSlot != &nested) {
                        ++st.tablesChanged;
                        lastSlot = &nested;
                    }
                }
            }
        }
    }
    if (pending.empty()) {
        return Ok(st);   // 未命中：零改写、零追加
    }
    st.rowsChanged = static_cast<ca::u32>(pending.size());

    // ---- 阶段二/三：容量预检 + 统一落位（先算后写；机制单源 cp_rewrite.hpp）。
    const SigAttrError capacityError{SigAttrError::Kind::CpCapacity, {}};
    const Result<void, SigAttrError> r =
        precheckPendingCapacity(cf, pending, capacityError);
    if (r.is_err()) {
        return Err(std::move(r).unwrap_err());
    }
    const Result<void, SigAttrError> applied =
        applyPendingRewrites(cf, pending, capacityError);
    if (applied.is_err()) {
        return Err(std::move(applied).unwrap_err());
    }
    for (const PendingCpRewrite& p : pending) {
        st.hits += p.hits;
    }
    return Ok(st);
}

}   // namespace jbc::ir
