/// @file        sig_attr.cpp
/// @brief       Signature 属性手术的实现：定位 → 读体 → 签名树改写 → 落位。
#include "jbc/ir/sig_attr.hpp"

#include <string_view>
#include <vector>

#include "jbc/core/attribute.hpp"
#include "jbc/core/constant_pool.hpp"

namespace jbc::ir {

namespace {

constexpr std::string_view kSignatureName = "Signature";

/// @brief CP u16 索引域上限（constant_pool_count 的合法最大值）。
constexpr ca::u32 kMaxCpCount = 0xFFFF;

/// @brief 槽是否为 Signature 属性（按属性名判定，零分配）。
bool isSignatureSlot(const core::ClassFile& cf, const core::AttrSlot& slot) noexcept
{
    return cf.cp.utf8Equals(slot.nameCp, kSignatureName);
}

/// @brief 解码并读出属性体：Signature 体 = u2 signature_index（JVMS 4.7.9）。
///
/// Signature 是已建模属性（@ref SignatureAttr，typed 解码按属性名分派）；
/// 体不足 2 字节属畸形，解码即 Err（未触碰写回仍走 copy 逐字节无损）。
Result<ca::u32, SigAttrError> readSignatureIndex(core::ClassFile& cf, core::AttrSlot& slot)
{
    const Result<core::AttrBody*, core::ReadError> d = cf.decodeAttr(slot);
    if (d.is_err()) {
        return Err(SigAttrError{SigAttrError::Kind::SlotDecode, {}});
    }
    auto* attr = static_cast<core::SignatureAttr*>(std::move(d).unwrap());
    return Ok(attr->signatureCp);
}

/// @brief 查重落位：命中既有池返回原索引，miss 才追加（追加前做容量预检）。
Result<ca::u32, SigAttrError> internSignatureString(core::ClassFile& cf, std::string_view s)
{
    core::ConstantPool& cp = cf.cp;
    if (const ca::u32 hit = cp.findUtf8(s); hit != 0) {
        return Ok(hit);
    }
    if (cp.size() > kMaxCpCount) {
        return Err(SigAttrError{SigAttrError::Kind::CpCapacity, {}});
    }
    return Ok(cp.addUtf8(s));
}

/// @brief 一个待落位项：槽 + 改写后的新签名串 + 命中数。
struct PendingRename
{
    core::AttrSlot* slot;
    ca::str::Utf8String newSig;
    ca::u32 hits;
};

/// @brief 对单个槽做纯预检（解码 + 解析 + 改名），不改写、不 intern。
///
/// 命中时把待落位项挂到 out；未命中返回 0（槽原样）；失败透传。
Result<ca::u32, SigAttrError> planSlot(core::ClassFile& cf, core::AttrSlot& slot,
                                       jbc::type::SignatureForm form,
                                       std::string_view old_internal,
                                       std::string_view new_internal,
                                       std::vector<PendingRename>& out)
{
    auto idx = readSignatureIndex(cf, slot);
    if (idx.is_err()) {
        return Err(std::move(idx).unwrap_err());
    }
    const std::string_view sig = cf.utf8(std::move(idx).unwrap());

    auto r = jbc::type::renameClassInSignature(sig, form, old_internal, new_internal);
    if (r.is_err()) {
        return Err(SigAttrError{SigAttrError::Kind::Signature, std::move(r).unwrap_err()});
    }
    jbc::type::SigRenameResult renamed = std::move(r).unwrap();
    if (renamed.hits == 0) {
        return Ok(ca::u32{0});
    }
    out.push_back(PendingRename{&slot, std::move(renamed.signature), renamed.hits});
    return Ok(renamed.hits);
}

/// @brief 落位一个待改写项：intern 去重 + 改写 signature_index + 置脏。
///
/// 体长 > 2 的畸形输入（尾部垃圾同族）只改 u2 索引，尾部经 trailing 原样
/// 保留——重编码写回天然不丢尾。
Result<void, SigAttrError> applyPending(core::ClassFile& cf, const PendingRename& p)
{
    auto idx = internSignatureString(cf, std::string_view{p.newSig});
    if (idx.is_err()) {
        return Err(std::move(idx).unwrap_err());
    }
    const ca::u32 newIdx = std::move(idx).unwrap();
    auto* attr = static_cast<core::SignatureAttr*>(p.slot->mutate());
    attr->signatureCp = newIdx;
    return Ok();
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
    std::vector<PendingRename> pending;
    auto planned = planSlot(cf, slot, form, old_internal, new_internal, pending);
    if (planned.is_err()) {
        return Err(std::move(planned).unwrap_err());
    }
    if (pending.empty()) {
        return Ok(SigAttrRename{});
    }

    const PendingRename& item = pending.front();
    const Result<void, SigAttrError> r = applyPending(cf, item);
    if (r.is_err()) {
        return Err(std::move(r).unwrap_err());
    }
    return Ok(SigAttrRename{item.hits, true});
}

Result<SigAttrRenameSummary, SigAttrError>
renameClassInSignatureAttrs(core::ClassFile& cf, std::string_view old_internal,
                            std::string_view new_internal)
{
    using jbc::type::SignatureForm;

    // ---- 阶段一：全量预检（纯读；解码只推进 Untouched→Decoded，写回对
    //      Decoded 仍走 copy，字节零影响）----
    std::vector<PendingRename> pending;
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

    // ---- 阶段二：容量预检（先算后写；新串经查重后仍 miss 的条目数，
    //      重复新串只会追加一次——首个落位后其余命中既有池）----
    ca::u32 additions = 0;
    for (const PendingRename& p : pending) {
        if (cf.cp.findUtf8(std::string_view{p.newSig}) == 0) {
            ++additions;
        }
    }
    if (cf.cp.size() + additions > kMaxCpCount) {
        return Err(SigAttrError{SigAttrError::Kind::CpCapacity, {}});
    }

    // ---- 阶段三：统一落位（容量已过，intern 不再失败；错误通道保留是
    //      如实传播，不静默吞）----
    for (const PendingRename& p : pending) {
        auto r = applyPending(cf, p);
        if (r.is_err()) {
            return Err(std::move(r).unwrap_err());
        }
        st.hits += p.hits;
    }
    return Ok(st);
}

namespace {

/// @brief 一个 LVTT 待落位行：宿主槽 + 行坐标 + 改写后的新签名串 + 命中数。
///
/// 行指针在预检与落位两阶段间有效（两阶段之间不改写任何行表结构，向量
/// 不重分配）。
struct LvttPendingRow
{
    core::AttrSlot* slot;
    core::LocalVariableTableAttr::Row* row;
    ca::str::Utf8String newSig;
    ca::u32 hits;
};

/// @brief 落位一个 LVTT 待改写行：intern 去重 + 改行 descCp + 槽置脏。
Result<void, SigAttrError> applyLvttPending(core::ClassFile& cf,
                                            const LvttPendingRow& p)
{
    auto idx = internSignatureString(cf, std::string_view{p.newSig});
    if (idx.is_err()) {
        return Err(std::move(idx).unwrap_err());
    }
    auto* lvt = static_cast<core::LocalVariableTableAttr*>(p.slot->mutate());
    p.row->descCp = std::move(idx).unwrap();
    return Ok();
}

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
    std::vector<LvttPendingRow> pending;
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
                    pending.push_back(LvttPendingRow{
                        &nested, &row, std::move(renamed.signature), renamed.hits});
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

    // ---- 阶段二：容量预检（先算后写；新串经查重后仍 miss 的条目数，
    //      重复新串只追加一次——首个落位后其余命中既有池）----
    ca::u32 additions = 0;
    for (const LvttPendingRow& p : pending) {
        if (cf.cp.findUtf8(std::string_view{p.newSig}) == 0) {
            ++additions;
        }
    }
    if (cf.cp.size() + additions > kMaxCpCount) {
        return Err(SigAttrError{SigAttrError::Kind::CpCapacity, {}});
    }

    // ---- 阶段三：统一落位（容量已过，intern 不再失败；错误通道保留是
    //      如实传播，不静默吞）----
    for (const LvttPendingRow& p : pending) {
        auto r = applyLvttPending(cf, p);
        if (r.is_err()) {
            return Err(std::move(r).unwrap_err());
        }
        st.hits += p.hits;
    }
    return Ok(st);
}

}   // namespace jbc::ir
