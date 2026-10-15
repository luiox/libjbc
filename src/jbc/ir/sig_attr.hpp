/// @file        sig_attr.hpp
/// @brief       泛型签名的属性层手术：类名改写的属性层接线。
///
/// 类名手术（rename）经 libjbc 编辑路径发生时，泛型签名在类文件中的两处
/// 宿主若不随之重写，产出即悬垂泛型——JVM 按签名解析泛型会指回旧名：
/// Signature 属性（class/method/field 三级挂载点）与 LocalVariableTypeTable
/// 行的 descriptor 列（字段形态签名）。本模块把 design.md §32 的签名树
/// 手术面（纯函数）接到属性层：调用方显式注入一对旧名/新名并触发手术，
/// 才 promote（解码属性体 + 解析签名树）→ 改写 → lower（intern 新串 +
/// 重编码）；不触发则槽保持 Untouched，字节逐字节 copy-through。
///
/// 属性模型：Signature 是 typed 属性（@ref core::SignatureAttr，属性体
/// `u2 signature_index`，JVMS 4.7.9；签名串在 CP Utf8）。手术面经 typed 解码
/// 读索引、经 @ref core::AttrSlot::mutate 改索引置脏，写回走既有重编码路径。
///
/// 契约（design.md §32.4）：
///   1. **显式调用出活**：库不发明会话级 rename 编排（跨类映射计划属
///      上层消费方 ClassIndex 域）；本层只提供单类内、按旧名/新名成对的手术入口。
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

/// @brief LVTT 行手术的汇总。
struct LvttRenameSummary
{
    ca::u32 tablesTotal = 0;   ///< 扫到的 LocalVariableTypeTable 属性数
    ca::u32 tablesChanged = 0; ///< 其中发生行改写的表数
    ca::u32 rowsChanged = 0;   ///< 改写的行数
    ca::u32 hits = 0;          ///< 命中类类型出现总次数
};

/// @brief 全方法 Code 内全部 LocalVariableTypeTable 行（descriptor 列）的
///        类名改写。
///
/// LVTT 的 descriptor 列承载**字段形态签名**（JVMS 4.7.9.1
/// FieldTypeSignature）——泛型签名在类文件中的第二处宿主（§32.4 挂账的
/// LVT 族半边）。逐行走 @ref renameClassInSignature（form 固定 Field），
/// §32 双形态全名匹配 / RenameShapeMismatch 拒绝 / 未命中零改写逐字适用。
///
/// **全有或全无**（与 @ref renameClassInSignatureAttrs 同款三阶段）：先做
/// 解码 + 解析 + 改名全量预检（纯读；Code/LVTT 槽解码失败或任一行签名
/// 解析失败即 Err，未改写任何槽、未追加任何 CP 条目），再统一容量预检，
/// 最后才 intern 落位置脏。未命中的表不置脏，写回逐字节 copy-through。
/// 新串经 findUtf8 查重落位（命中复用不膨胀，逆向 rename 复用回原条目）。
///
/// LVT 行（LocalVariableTable，擦除后描述符）不属本入口——描述符手术归
/// 描述符层（jbc::type descriptor 的 span 面），签名/描述符两族不混。
Result<LvttRenameSummary, SigAttrError>
renameClassInLocalVariableTypeTables(core::ClassFile& cf,
                                     std::string_view old_internal,
                                     std::string_view new_internal);

}   // namespace jbc::ir
