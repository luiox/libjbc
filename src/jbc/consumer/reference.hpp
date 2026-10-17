/// @file        reference.hpp
/// @brief       E1 引用导出面：ReferenceScanner 从字节直译引用事实族。
///
/// design.md §9 的 `ReferenceScanner::scan(ByteView)` 本体态。扫描范围
/// （v1 封闭面，与 §21/§35.4 一致——引用索引的分部重建以「成员段 +
/// 类级事实」为粒度消费）：
///
///   - 成员段事实：invoke 族（owner/name/desc + opcode + 站位）、字段
///     访问族、显式类型指令（new/anewarray/checkcast/instanceof/
///     multianewarray + ldc Class 常量）、ldc String 常量、invokedynamic
///     调用点（name/desc + BSM 下标）、Exceptions 属性 throws 表；
///   - 类级事实：this/super/接口、字段族（名字/描述符成对）。
///
/// v1 刻意不收编的扩张口（留给后续批次，避免一步跨进 remapper/索引域）：
/// 注解提及族、结构元数据属性（InnerClasses/Nest/('('EnclosingMethod 等）、
/// StackMapTable Object 段、LVT/LVTT 行、BootstrapMethods 内部可达面。
/// 所有文本 owned（scan 的一次性产物，内部临时模型随调用终结）。
///
/// 站位坐标系：`ordinal` = Code 体原始指令序（0..n-1，decode 顺序）；
/// `pc` = Code 体字节偏移（原码回出示位）。这是**原始 code 域**，不是
/// CodeDom::order() 的 IR 下标（后者含 label/line 占位节点）——消费方
/// 如需 IR 坐标系，走会话 promote 后自行映射。
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "libca/core/datatype.hpp"

#include "jbc/consumer/consumer.hpp"
#include "jbc/consumer/summary.hpp"
#include "jbc/consumer/source.hpp"
#include "jbc/core/opcode.hpp"

namespace jbc::consumer {

/// @brief 站位事实（Code 体局部坐标系，见文件头）。
struct RefSite
{
    core::Op opcode = core::Op::NOP;
    ca::u32  ordinal = 0;  ///< Code 体指令序（decode 顺序，0 起算）
    ca::u32  pc = 0;       ///< Code 体字节偏移（原码）
};

/// @brief invoke 族的成员引用事实。
struct MethodRefFact
{
    RefSite site;
    std::string owner;  ///< 内部名；非接口调用走 Methodref 座
    std::string name;
    std::string desc;
};

/// @brief 字段访问族事实。
struct FieldRefFact
{
    RefSite site;
    std::string owner;
    std::string name;
    std::string desc;
};

/// @brief 显式类型事实（new/anewarray/checkcast/instanceof/multianewarray
///        + ldc Class 常量——opcode 区分形态；类字面量也算）。
struct TypeRefFact
{
    RefSite site;
    std::string internalName;
};

/// @brief ldc String 常量事实。
struct StringRefFact
{
    RefSite site;
    std::string text;
};

/// @brief invokedynamic 调用点事实（owner 空——indy 语义在 BSM 处落实）。
struct DynamicRefFact
{
    RefSite site;
    ca::u32 bsm = 0;         ///< bootstrap_method_ref 下标
    std::string name;        ///< NameAndType 名
    std::string desc;        ///< NameAndType 描述符
};

/// @brief 单方法段：站位事实 + 该方法自身的描述符。
struct MethodSection
{
    std::string name;
    std::string desc;
    std::vector<MethodRefFact> invokes;
    std::vector<FieldRefFact> fieldAccesses;
    std::vector<TypeRefFact> typeOps;
    std::vector<StringRefFact> strings;
    std::vector<DynamicRefFact> indies;
    std::vector<std::string> exceptionTypes;  ///< Exceptions 属性 throws 表（保序）
};

/// @brief E1 引用事实族（class 级）。
struct ReferenceSummary
{
    std::string thisClass;
    std::string superClass;  ///< 空 = Object
    std::vector<std::string> interfaces;
    std::vector<SummaryField> fields;   ///< 字段族（name/desc 成对，含 access）
    std::vector<MethodSection> methods; // 保序（backing 成员表序）
};

/// @brief E1 引用扫描器。无状态、可并发复用。
class ReferenceScanner
{
public:
    /// @brief 扫描单个 class 字节段。Err 只出自 BadClassFile 档。
    ConsumerResult<ReferenceSummary> scan(jbc::core::ByteView bytes) const;

    /// @brief 扫描一个源里全部类。fail-closed 首错即 Err。
    ConsumerResult<std::vector<ReferenceSummary>> scanSource(const IClassSource& source) const;
};

}   // namespace jbc::consumer
