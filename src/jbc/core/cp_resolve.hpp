/// @file        cp_resolve.hpp
/// @brief       CP 读侧语义解析原语：成员引用一跳、MethodHandle 三段、类名。
///
/// 池级纯自由函数（无状态、零分配、header-only，形态同 bytes.hpp）：输入
/// ConstantPool + 索引，输出零拷贝 string_view 视图，视图存活期 = 池（下一次
/// append 可能扩容搬迁，跨追加只持索引，见 ConstantPool::at 的警告）。
///
/// 为什么放 core 而不是收进 ClassSession：BSM 本体与实参都是裸 CP 索引（拿不
/// 到 Insn），指令坐标的语义层（memberOf/constOf）覆盖不到 MethodHandle 的
/// 目标——constOf 对它只报 kind 不报指向；压缩与扫描两侧需要同一套一跳原语。
/// 失败一律返回 false / 空 view，不产生诊断：调用方自决失败面（降级、拒绝
/// 或记档），与 utf8Of 的「非 Utf8 返回空」同款探测语义。
///
/// 严格口径：MethodHandle 的 reference_kind ∉ 1..9 视为结构不符，解析失败，
/// 不做「非字段 kind 一律按方法」的宽松兜底——那会把损坏池悄悄解析成假语义。
/// kind 与引用条目 tag 的匹配裁决与 validateReferences 同款：1..4 → Fieldref，
/// 5..8 → Methodref/InterfaceMethodref，9 → InterfaceMethodref（kind 6/7 的
/// InterfaceMethodref 是 JVMS v52+ 的合法交叉形态，javac 确实产出）。
#pragma once

#include <string_view>

#include "libca/core/datatype.hpp"

#include "jbc/core/constant_pool.hpp"

namespace jbc::core {

/// @brief 成员引用三段（Fieldref/Methodref/InterfaceMethodref 一跳）。
struct MemberRefSem
{
    std::string_view owner;       ///< owner 内部名（Class → Utf8 一跳）
    std::string_view name;        ///< 成员名（NameAndType.a）
    std::string_view desc;        ///< 描述符（NameAndType.b）
    CpTag tag = CpTag::Fieldref;  ///< 原 tag：调用方 re-intern 需要原形
};

/// @brief MethodHandle 条目语义（JVMS 4.4.8）。
struct HandleSem
{
    std::string_view owner;    ///< 引用条目的 owner 内部名
    std::string_view name;     ///< 引用条目的成员名
    std::string_view desc;     ///< 引用条目的描述符
    ca::u32 kind = 0;          ///< reference_kind（1..9）
    bool isInterface = false;  ///< 引用条目为 InterfaceMethodref
};

/// @brief 解析成员引用条目：cpIdx 指向 Fieldref/Methodref/InterfaceMethodref
///        三族之一，owner/name/desc 三跳链全通才算成功。
///
/// 任一跳 tag 不符（含悬垂索引、Class.a 非 Utf8、NameAndType 的 name/desc
/// 任一非 Utf8）返回 false 且不触碰 out；成功时 @ref MemberRefSem::tag 记
/// 原 tag。
inline bool resolveMemberRef(const ConstantPool& cp, ca::u32 cpIdx,
                             MemberRefSem& out) noexcept
{
    if (!cp.valid(cpIdx)) {
        return false;
    }
    const CpTag tag = cp.tagOf(cpIdx);
    if (tag != CpTag::Fieldref && tag != CpTag::Methodref
        && tag != CpTag::InterfaceMethodref) {
        return false;
    }
    const ConstantPool::Entry& e = cp.at(cpIdx);
    // owner：Class 一跳再落 Utf8；name/desc：NameAndType 一跳的两份 Utf8。
    if (!cp.valid(e.a) || cp.tagOf(e.a) != CpTag::Class) {
        return false;
    }
    const ca::u32 ownerNameIdx = cp.at(e.a).a;
    if (!cp.isUtf8(ownerNameIdx)) {
        return false;
    }
    if (!cp.valid(e.b) || cp.tagOf(e.b) != CpTag::NameAndType) {
        return false;
    }
    const ConstantPool::Entry& nat = cp.at(e.b);
    if (!cp.isUtf8(nat.a) || !cp.isUtf8(nat.b)) {
        return false;
    }
    out.owner = cp.utf8Of(ownerNameIdx);
    out.name  = cp.utf8Of(nat.a);
    out.desc  = cp.utf8Of(nat.b);
    out.tag   = tag;
    return true;
}

/// @brief 解析 MethodHandle 条目（JVMS 4.4.8）：kind 域检 → kind-tag 匹配 →
///        owner/name/desc 链校验，任一环失败返回 false 且不触碰 out。
///
/// 成功时 @ref HandleSem::isInterface 按引用条目实际 tag 置位（kind 5..8 的
/// InterfaceMethodref 交叉形态由此可见），@ref HandleSem::kind 为域检后的
/// reference_kind。
inline bool resolveHandleCp(const ConstantPool& cp, ca::u32 cpIdx,
                            HandleSem& out) noexcept
{
    if (!cp.valid(cpIdx) || cp.tagOf(cpIdx) != CpTag::MethodHandle) {
        return false;
    }
    const ConstantPool::Entry& e = cp.at(cpIdx);
    const ca::u32 kind = e.a;
    if (kind < 1 || kind > 9) {
        return false;
    }
    if (!cp.valid(e.b)) {
        return false;
    }
    const CpTag refTag = cp.tagOf(e.b);
    if (kind <= 4) {
        if (refTag != CpTag::Fieldref) {
            return false;
        }
    }
    else if (kind == 9) {
        if (refTag != CpTag::InterfaceMethodref) {
            return false;
        }
    }
    else if (refTag != CpTag::Methodref && refTag != CpTag::InterfaceMethodref) {
        return false;
    }
    // owner/name/desc 链校验复用成员引用一跳：ref tag 已在上面收窄进三族，
    // resolveMemberRef 的族检在这里恒过，失败只剩链路自身。
    MemberRefSem m;
    if (!resolveMemberRef(cp, e.b, m)) {
        return false;
    }
    out.owner       = m.owner;
    out.name        = m.name;
    out.desc        = m.desc;
    out.kind        = kind;
    out.isInterface = refTag == CpTag::InterfaceMethodref;
    return true;
}

/// @brief 解析 Class 条目为内部名 Utf8 视图；非 Class 条目、悬垂索引或
///        name 槽非 Utf8 返回空 view（与 utf8Of 的探测语义同款）。
inline std::string_view resolveClassName(const ConstantPool& cp, ca::u32 cpIdx) noexcept
{
    if (!cp.valid(cpIdx) || cp.tagOf(cpIdx) != CpTag::Class
        || !cp.isUtf8(cp.at(cpIdx).a)) {
        return {};
    }
    return cp.utf8Of(cp.at(cpIdx).a);
}

}   // namespace jbc::core
