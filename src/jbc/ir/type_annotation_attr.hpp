/// @file        type_annotation_attr.hpp
/// @brief       Runtime{Visible,Invisible}TypeAnnotations 的类名手术接线。
///
/// 类型注解体里承载类名的位置——注解类型描述符（type_index）与 'c'
/// element_value 的类描述符（如 `@Anno(Expr.class)`）——是字段描述符形态，
/// 类名改写经 jbc::type::descriptor 的 span 手术面发生。
/// target_info 里的 supertype/throws 下标指向 CP Class 条目（interfaces 表
/// 与 Exceptions 表），随 CP Class 条目重指向自动跟随，不属本入口。
///
/// 契约：
///   1. **显式调用出活**：调用方注入一对旧名/新名；未触发则槽保持
///      Untouched，字节逐字节 copy-through（硬门禁）。
///   2. **描述符层语义**：span 全名匹配 + 结构化替换，描述符语法非法
///      （除基类型/void——它们不可能含类名，按无类名跳过）显式 Err 不猜。
///   3. **失败显式 Err，不静默跳过**；全扫全有或全无（先全量预检后容量
///      预检再落位，失败不留半截改写、不追加任何 CP 条目）。
///   4. **CP 只追加 + 查重不膨胀**：新描述符 findUtf8 命中复用、miss 才
///      追加；逆向 rename 复用回原条目。
#pragma once

#include "libca/core/result.hpp"

#include "jbc/core/class_file.hpp"
#include "jbc/type/descriptor.hpp"

namespace jbc::ir {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;

/// @brief 类型注解手术的失败事实。
struct TaAttrError
{
    /// @brief 失败族。
    enum class Kind : ca::u8 {
        Descriptor,  ///< 描述符层失败（desc 字段有效）
        SlotDecode,  ///< 属性槽解码失败（体半截/非法 target_type 等）
        CpCapacity,  ///< CP 容量预检拒绝
    };

    Kind kind = Kind::Descriptor;
    jbc::type::DescError desc = jbc::type::DescError::Empty;  ///< kind == Descriptor 时有效
};

/// @brief 类型注解手术的汇总。
struct TaRenameSummary
{
    ca::u32 attrsTotal = 0;    ///< 扫到的 Runtime*TypeAnnotations 属性数（含 Code 嵌套）
    ca::u32 attrsChanged = 0;  ///< 其中发生改写的属性数
    ca::u32 hits = 0;          ///< 命中类名出现次数（type_index 与 'c' 元素值合计）
};

/// @brief 全挂载点（类/字段/方法属性表 + Code 嵌套）Runtime*TypeAnnotations
///        的类名改写。
///
/// **全有或全无**（与 @ref renameClassInSignatureAttrs 同款三阶段）：先做
/// 解码 + 描述符解析 + 改名全量预检（纯读；任何一步失败即 Err 且未改写
/// 任何槽、未追加任何 CP 条目），再统一容量预检，最后才 intern 落位置脏。
Result<TaRenameSummary, TaAttrError>
renameClassInTypeAnnotations(core::ClassFile& cf, std::string_view old_internal,
                             std::string_view new_internal);

}   // namespace jbc::ir
