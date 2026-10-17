/// @file        rename.hpp
/// @brief       E-track rename 编排：计划模型 + 逐类应用 + 注入 resolver
///              + 跨类 fail-closed 驱动。
///
/// §9 的分层定契在本模块落齐：**计划 formation 属消费方域**（上层从
/// ClassIndex/属性表产出规则组，经 @ref IRenameResolver 注入）；libjbc
/// 只负责计划可见的逐类应用与跨类驱动——编排协议即「注入 resolver
/// + fail-closed 驱动 + 原字节透传不动的类」。
///
/// 应用模型（v1 分族封闭面；蓝本为 rename_probe 实跑 pass 的库化）：
///   - **类名族**：CP Class 条目重指向（plain 内部名 via
///     ClassSession::renameClassConstant + 数组形态经 type 层形态重建）+
///     描述符列手术（NameAndType/MethodType/成员头/LocalVariableTable，
///     span 全名命中才动）+ 泛型签名族三件（sig_attr/LVTT/TA 的既有
///     结构化手术面）。String 条目不碰（角色防线：字符串常量里的
///     «L名;» 是字面量事实）。
///   - **成员族**：成员表行改写（SymbolId 置换 + commit 造成 E2
///     ChangeSet 记录自动随行）+ 代码 operand 改写（setMember 按 kind
///     途径 + setClass 类型指令）。invokedynamic/BSM 触点**不改写**，而
///     是 fail-loud：预检扫 BootstrapMethods 的 MethodHandle 载体，规则
///     命中即 Err(BsmTouched)——半支持状态的静默改坏被显式拒之于趟前。
///   - **类头族**：this/super/接口经 ClassUnit 高层机位 + commit
///     ClassMetadata。
///
/// 失败纪律：逐类应用内部全有或全无以各手术面自身的保证为准（退不出已
/// 写本的类由 Error 交出）；驱动层 fail-closed 首错即 Err，不产 stage。
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"

#include "jbc/consumer/consumer.hpp"
#include "jbc/consumer/source.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/ir/sig_attr.hpp"
#include "jbc/ir/type_annotation_attr.hpp"
#include "jbc/ir/units.hpp"

namespace jbc::consumer {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;

/// @brief 类名改名规则（一次输入一次输出）。
struct ClassRenameRule
{
    std::string oldName;
    std::string newName;
};

/// @brief 成员改名规则（方法/字段同形）。
struct MemberRenameRule
{
    std::string owner;   ///< 声明类内部名（'/' 分隔）
    std::string name;
    std::string desc;    ///< 空 = 该 owner/name 全部 desc 命中（通配）
    std::string newName; ///< 空 = 保留
    std::string newDesc; ///< 空 = 保留（成员行侧）；类名族的描述符映射独立生效
};

/// @brief 改名计划（消费方组装；本模块按只读面应用）。
///
/// 同一 oldKey 的重复收账为 upsert（后写生效）；查命按先到先得（线性
/// 扫，规则组规模即可控小——计划 formation 属消费方域，超大规模映射应
/// 在消费侧归并）。
class RenamePlan
{
public:
    /// @brief 收编一条类名规则（同旧名 upsert）。
    void renameClass(std::string_view oldName, std::string_view newName);

    /// @brief 收编一条方法规则（同旧键 upsert）。
    void renameMethod(std::string_view owner, std::string_view name, std::string_view desc,
                      std::string_view newName, std::string_view newDesc = {});

    /// @brief 收编一条字段规则（同旧键 upsert）。
    void renameField(std::string_view owner, std::string_view name, std::string_view desc,
                     std::string_view newName, std::string_view newDesc = {});

    /// @brief 类名规则查命（miss nullptr）。
    const ClassRenameRule* classRule(std::string_view oldName) const;

    /// @brief 方法规则查命：owner/name 精确 + 规则 desc 空 = 通配。
    const MemberRenameRule* methodRule(std::string_view owner, std::string_view name,
                                       std::string_view desc) const;

    /// @brief 字段规则查命（同方法语义）。
    const MemberRenameRule* fieldRule(std::string_view owner, std::string_view name,
                                      std::string_view desc) const;

    /// @brief 类名族的描述符映射：span 全名命中才动（type 层手术面）。
    ///        未命中/描述符形态非法 → 原样回（role-guard 否决）。
    std::string mapDescriptor(std::string_view desc) const;

    bool empty() const noexcept
    {
        return classes_.empty() && methods_.empty() && fields_.empty();
    }

    /// @brief 规则组只读访问（BSM 载体闸与上层统计用，不改组）。
    const std::vector<ClassRenameRule>&  classes() const noexcept { return classes_; }
    const std::vector<MemberRenameRule>& methods() const noexcept { return methods_; }
    const std::vector<MemberRenameRule>& fields()  const noexcept { return fields_; }

private:
    /// @brief 成员族查命共享（kind = 方法表/字段表择一）。
    const MemberRenameRule* memberRule(const std::vector<MemberRenameRule>& rules,
                                       std::string_view owner, std::string_view name,
                                       std::string_view desc) const;

    std::vector<ClassRenameRule>  classes_;
    std::vector<MemberRenameRule> methods_;
    std::vector<MemberRenameRule> fields_;
};

/// @brief 单类改名应用失败（fail-closed 家族如实分立）。
struct RenameError
{
    /// @brief 失败族。
    enum class Kind : ca::u8 {
        Lower,             ///< 会话侧 LowerError（lower 字段有效）
        Signature,         ///< sig_attr 族手术失败（sig 字段有效）
        TypeAnnotation,    ///< 类型注解族手术失败（ta 字段有效）
        Decode,            ///< 属性槽/Code 通路解码失败
        BsmTouched,        ///< 规则命中 BSM 触达面（v1 不改写，fail-loud）
        BadRule,           ///< 计划本身不合法（坏内部名等）
    };

    Kind kind = Kind::Decode;
    jbc::ir::LowerError lower = jbc::ir::LowerError::Ok;       ///< kind == Lower 时有效
    jbc::ir::SigAttrError sig{};                               ///< kind == Signature 时有效
    jbc::ir::TaAttrError ta{};                                 ///< kind == TypeAnnotation 时有效
};

/// @brief 单类改名应用的命中汇总（全部为 0/计位 = 无变化）。
struct RenameSummary
{
    bool   changed            = false;  ///< 任一家族命中迹象（即非 no-op）
    ca::u32 classConstants    = 0;      ///< CP Class 条目重指向数（plain + 数组形态）
    ca::u32 descriptorCols    = 0;      ///< 描述符列改写（NAT/MethodType/成员头/LVT 行）
    ca::u32 methodRows        = 0;      ///< 成员表方法行改写数
    ca::u32 fieldRows         = 0;      ///< 成员表字段行改写数
    ca::u32 refsRetargeted    = 0;      ///< 代码 operand 改写数
    ca::u32 sigAttrs          = 0;      ///< Signature 属性改写数
    ca::u32 lvttTables        = 0;      ///< LVTT 表改写数
    ca::u32 taAttrs           = 0;      ///< 类型注解属性改写数
    ca::u32 headMembersChanged = 0;     ///< 类头触点（this/super/接口按项计）
};

/// @brief 单类改名应用（会话 + backing 双层分族落地）。
///
/// `plan.empty()` 时零改动直接 Ok（no-op 语义）。返回的 summary 变状态
/// 仅就本类计；ChangeSet 明细请看 @ref ClassSession::changeSet。
Result<RenameSummary, RenameError> applyClassRename(jbc::ir::ClassSession& s, const RenamePlan& plan);

/// @brief 逐类改名计划供给（跨类编排经此注入）。
///
/// 计划 formation 属消费方域：resolver 侧持有索引/属性表产出的
/// RenamePlan 组，并按类下发。返回 Ok(nullptr) = 该类不改（驱动原字
/// 节透传）；Err = 中止整轮（fail-closed，驱动不会产出 stage）。
class IRenameResolver
{
public:
    virtual ~IRenameResolver() = default;

    /// @brief 下发该类的计划。计划生命周期需覆盖整轮驱动。
    virtual ConsumerResult<const RenamePlan*> planFor(std::string_view className) = 0;
};

/// @brief 跨类改名驱动：源全枚举，逐类「计划 → 会话 → 应用 → 写回」，
///        阶段产物进入返回的内存源。
///
/// - plan 为空/缺计划的类**原字节透传**（stage 与输入等价字节，不动）；
/// - fail-closed：任何一站失败（获取/解码/应用/写回/resolver）即 Err，
///   驱动不产出部分 stage；
/// - 改名后的类文件按 this_name 命新名的落位语义不变（stage 键 = 枚举
///   名，即源里的原内部名——JVM 加载语义由消费方落盘规则承接）。
ConsumerResult<MemoryClassSource> renameSource(const IClassSource& source, IRenameResolver& resolver);

}   // namespace jbc::consumer
