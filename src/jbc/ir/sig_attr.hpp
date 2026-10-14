/// @file        sig_attr.hpp
/// @brief       Signature 属性的泛型保持手术：类名改写的属性层接线。
///
/// 类名手术（rename）经 libjbc 编辑路径发生时，class/method/field 三级挂载
/// 点的 Signature 属性若不随之重写，产出即悬垂泛型——JVM 按 Signature 解析
/// 泛型会指回旧名。本模块把 design.md §32 的签名树手术面（纯函数）接到属性
/// 层：调用方显式注入一对旧名/新名并触发手术，才 promote（解码属性体 + 解析
/// 签名树）→ 改写 → lower（intern 新串 + 重编码）；不触发则槽保持 Untouched，
/// 字节逐字节 copy-through。
///
/// 属性模型零改动：Signature 属性体是 `u2 signature_index`（JVMS 4.7.9），
/// 现状不在已建模属性之列——Raw 兜底是全库既定契约（下游消费者按 Raw 字节
/// 消费）。手术面在 Raw 体上原位读写 u2 并经 @ref core::AttrSlot::mutate
/// 置脏，写回走既有 Raw 重编码路径；typed Signature 属性模型留待 B 轨与
/// 下游消费者协同迁移的后续批。
///
/// 契约（design.md §32.4）：
///   1. **显式调用出活**：库不发明会话级 rename 编排（跨类映射计划属
///      legacy-stack ClassIndex 域）；本层只提供单类内、按旧名/新名成对的手术入口。
///   2. **形态由挂载点决定**：类级 → Class、方法级 → Method、字段级 →
///      Field（§32.3 形态歧义不猜；单槽原语显式收 form 参数）。
///   3. **手术语义复用签名层契约**：'.' 段链与 '$' 内嵌双形态全名匹配、
///      '$' 结构不一致 RenameShapeMismatch 显式拒绝不猜、未命中零改写。
///   4. **失败显式 Err，不静默跳过**；全扫入口全有或全无（先全量预检后
///      落位，失败不留半截改写）。
///   5. **CP 只追加**：新签名串经 findUtf8 去重落位（命中复用不膨胀）；
///      旧串成为孤儿条目（append-only 契约允许，压缩逃生口另议）。
#pragma once

#include "libca/core/result.hpp"

#include "jbc/core/class_file.hpp"
#include "jbc/type/signature.hpp"

namespace jbc::ir {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;

/// @brief Signature 属性手术的失败事实。
///
/// 三个失败族如实分立，不做跨族映射：签名层失败（解析失败/形态不一致/
/// 坏名）原样携带 @ref jbc::type::SigError（含字节偏移）；槽解码失败
/// （属性体半截等，降级事实见 @ref ClassFile::diagnostics）与 CP 容量预检
/// 失败各自成族——三者发生时都未改写任何槽；CpCapacity 另未追加任何条目。
struct SigAttrError
{
    /// @brief 失败族。
    enum class Kind : ca::u8 {
        Signature,   ///< 签名层失败（sig 字段有效）
        SlotDecode,  ///< Signature 槽解码失败
        CpCapacity,  ///< CP 容量预检拒绝
    };

    Kind kind = Kind::Signature;
    jbc::type::SigError sig;  ///< kind == Signature 时有效
};

/// @brief 单个 Signature 属性槽一次类名改写的结果。
struct SigAttrRename
{
    ca::u32 hits = 0;     ///< 命中并改写的类类型出现次数
    bool changed = false; ///< 是否发生改写（hits > 0 蕴含 true）
};

/// @brief 对一个 Signature 属性槽做一次类名改写（显式调用出活）。
///
/// - 槽不是 Signature 属性（按属性名判定）→ 无操作返回 hits=0（与
///   setMember 对 op 族不符返回 false 同款探测纪律，不视为失败）。
/// - 未命中（签名里没有该内部名）→ 槽保持原状态（不置脏），字节零改写。
/// - 命中 → 新签名串经 findUtf8/addUtf8 去重落位（CP 只追加），改写
///   sigCp 并经 @ref AttrSlot::mutate 置脏，写回走重编码。
/// - 失败（槽解码失败 / 签名解析失败 / RenameShapeMismatch / CP 容量
///   不足）→ 显式 @ref SigAttrError，槽保持原样（不置脏）。
Result<SigAttrRename, SigAttrError>
renameClassInSignatureAttr(core::ClassFile& cf, core::AttrSlot& slot,
                           jbc::type::SignatureForm form,
                           std::string_view old_internal, std::string_view new_internal);

/// @brief 类文件全量手术的汇总。
struct SigAttrRenameSummary
{
    ca::u32 attrsTotal = 0;   ///< 扫到的 Signature 属性数（三级挂载点合计）
    ca::u32 attrsChanged = 0; ///< 其中发生改写的属性数
    ca::u32 hits = 0;         ///< 命中类类型出现总次数
};

/// @brief 三级挂载点（类/方法/字段）全部 Signature 属性的类名改写。
///
/// 形态按挂载点自动判定（§32.4 契约 2）。**全有或全无**：先对全部属性做
/// 解码 + 解析 + 改名预检（纯读，任何一步失败即 Err 且未改写任何槽、未
/// 追加任何 CP 条目），再统一做容量预检，最后才 intern 落位置脏——调用方
/// 拿到 Err 即可整类放弃，不会遇到半改写状态。
Result<SigAttrRenameSummary, SigAttrError>
renameClassInSignatureAttrs(core::ClassFile& cf, std::string_view old_internal,
                            std::string_view new_internal);

}   // namespace jbc::ir
