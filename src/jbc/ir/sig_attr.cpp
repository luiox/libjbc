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
/// Signature 现状不在已建模属性之列——Raw 兜底是全库既定契约（下游 mjbase
/// 按 Raw 字节消费）。手术面因此在 Raw 体上原位读写 u2，属性模型零改动；
/// 体不足 2 字节属畸形，显式 Err（未触碰写回仍走 copy 逐字节无损，§29）。
Result<ca::u32, SigAttrError> readSignatureIndex(core::ClassFile& cf, core::AttrSlot& slot)
{
    const Result<core::AttrBody*, core::ReadError> d = cf.decodeAttr(slot);
    if (d.is_err()) {
        return Err(SigAttrError{SigAttrError::Kind::SlotDecode, {}});
    }
    auto* raw = static_cast<core::RawAttr*>(std::move(d).unwrap());
    const std::vector<ca::u8>& b = raw->bytes();
    if (b.size() < 2) {
        return Err(SigAttrError{SigAttrError::Kind::SlotDecode, {}});
    }
    return Ok((ca::u32(b[0]) << 8) | ca::u32(b[1]));
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

/// @brief 落位一个待改写项：intern 去重 + 原位改写 u2 体 + 置脏。
///
/// 体长 > 2 的畸形输入（尾部垃圾同族）只改前两字节，其余原样保留——Raw
/// 体写回逐字节copy，天然不丢尾。
Result<void, SigAttrError> applyPending(core::ClassFile& cf, const PendingRename& p)
{
    auto idx = internSignatureString(cf, std::string_view{p.newSig});
    if (idx.is_err()) {
        return Err(std::move(idx).unwrap_err());
    }
    const ca::u32 newIdx = std::move(idx).unwrap();
    auto* raw  = static_cast<core::RawAttr*>(p.slot->mutate());
    raw->bytes()[0] = static_cast<ca::u8>(newIdx >> 8);
    raw->bytes()[1] = static_cast<ca::u8>(newIdx);
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

}   // namespace jbc::ir
