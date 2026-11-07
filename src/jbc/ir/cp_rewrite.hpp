/// @defgroup ir_cp_rewrite CP 改写落位机
///
/// 属性手术共用的 CP 落位（ir 内部头）。

/// @file        cp_rewrite.hpp
/// @brief       属性手术共用的 CP 改写落位机（ir 内部头，同 units_internal.hpp 角色）。
///
/// Signature / LVTT / TypeAnnotations 三族类名改写手术共用同一个三段式契约：
/// 阶段一全量纯读预检（调用方各自 plan，产出 PendingCpRewrite 列表）→
/// 阶段二容量预检（先算后写）→ 阶段三统一落位（intern 去重 + 写索引 + 置脏）。
/// 计划/应用机制与 CP 上限在此单源，不再每处手术各持一份。
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "jbc/core/attribute.hpp"
#include "jbc/core/constant_pool.hpp"

#include "libca/core/result.hpp"

namespace jbc::ir {

/// @brief CP u16 索引域上限（constant_pool_count 的合法最大值）。
constexpr ca::u32 kMaxCpCount = 0xFFFF;

/// @brief 一个待落位项：CP 索引槽位（已解码体内的 u32 字段）+ 改写后的新串
///        + 命中数。
///
/// field 指向解码体内的 CP 索引字段：decodeAttr/peek 与 mutate 返回同一
/// 对象（AttrSlot 缓存体指针），故 plan 期取的地址在落位期有效——两阶段
/// 之间不改写任何体结构、向量不重分配。
struct PendingCpRewrite
{
    core::AttrSlot* slot = nullptr;
    ca::u32*        field = nullptr;
    std::string     newText;
    ca::u32         hits = 0;
};

/// @brief 查重落位：命中既有池返回原索引，miss 才追加（追加前做容量预检）。
///        capacityError 由调用方给本族错误形状（SigAttrError / TaAttrError）。
template <typename E>
Result<ca::u32, E> internUtf8Dedup(core::ClassFile& cf, std::string_view s, E capacityError)
{
    core::ConstantPool& cp = cf.cp;
    if (const ca::u32 hit = cp.findUtf8(s); hit != 0) {
        return Ok(hit);
    }
    if (cp.size() > kMaxCpCount) {
        return Err(std::move(capacityError));
    }
    return Ok(cp.addUtf8(s));
}

/// @brief 阶段二容量预检（先算后写）：新串经查重后仍 miss 的条目数；重复
///        新串只追加一次——首个落位后其余命中既有池。capacityError 由调用
///        方给本族错误形状（SigAttrError / TaAttrError）。
template <typename E>
Result<void, E> precheckPendingCapacity(core::ClassFile&                     cf,
                                        const std::vector<PendingCpRewrite>& pending,
                                        E                                    capacityError)
{
    if (pending.empty()) {
        return Ok();
    }
    ca::u32 additions = 0;
    for (const PendingCpRewrite& p : pending) {
        if (cf.cp.findUtf8(p.newText) == 0) {
            ++additions;
        }
    }
    if (cf.cp.size() + additions > kMaxCpCount) {
        return Err(std::move(capacityError));
    }
    return Ok();
}

/// @brief 阶段三统一落位：intern 去重 + 写回 CP 索引 + 槽置脏。容量已过
///        时 intern 不再失败；错误通道保留是如实传播，不静默吞。
template <typename E>
Result<void, E> applyPendingRewrites(core::ClassFile&                     cf,
                                     const std::vector<PendingCpRewrite>& pending,
                                     E                                    capacityError)
{
    for (const PendingCpRewrite& p : pending) {
        auto idx = internUtf8Dedup(cf, p.newText, capacityError);
        if (idx.is_err()) {
            return Err(std::move(idx).unwrap_err());
        }
        *p.field = std::move(idx).unwrap();
        (void)p.slot->mutate();
    }
    return Ok();
}

}   // namespace jbc::ir
