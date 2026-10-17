/// @file        summary.hpp
/// @brief       E3 骨架扫描：SummaryScanner 解出 ClassSummary（文本形态）。
///
/// 契约承 design.md §9 的 `SummaryScanner::scan(ByteView)` 草图 + §21 的
/// 「骨架」定义：this/super/接口/version/access + 成员表（名字/描述符/
/// access，方法另带 hasCode 位）——一个 class 字节段的骨架扫描产出，把
/// KeyPool 去重推给上层（mjbase 消费侧同路径把文本 intern 成驻留 id；
/// libjbc 本体保持拷入的 owned 文本，结构自足）。
///
/// 解码通道：@ref jbc::core::readClass 全链门禁（preserveSourceBytes
/// 关闭——摘要态不承担 copy-through）；Validated/Recovered 照常构骨架
/// （diagnostics 不属契约、归消费方另行取），Err 才归 BadClassFile。
/// hasCode 由成员表里是否命中属性名 `Code` 判定（属性体不解析——骨架
/// 态不发生 Code 结构解读）。
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "libca/core/datatype.hpp"

#include "jbc/consumer/consumer.hpp"
#include "jbc/consumer/source.hpp"

namespace jbc::consumer {

/// @brief 字段骨架行。
struct SummaryField
{
    std::string name;
    std::string desc;
    ca::u32     access = 0;
};

/// @brief 方法骨架行。
struct SummaryMethod
{
    std::string name;
    std::string desc;
    ca::u32     access = 0;
    bool        hasCode = false;  ///< 属性表含 `Code`（体不解析）
};

/// @brief 单个 class 的骨架。文本全部 owned（String 拷入）——scan 为
///        一次性产物：内部临时模型随 scan 终结，视图系形态在这里活不过
///        本次调用。
struct ClassSummary
{
    std::string internalName;         ///< 内部名（'/' 分隔）
    std::string superClass;           ///< 空串 = superClass 0（java/lang/Object）
    ca::u32     version   = 0;        ///< (major << 16) | minor
    ca::u32     access    = 0;
    std::vector<std::string> interfaces;
    std::vector<SummaryField> fields;
    std::vector<SummaryMethod> methods;
};

/// @brief E3 骨架扫描器。无状态、可并发复用（纯函数语义）。
class SummaryScanner
{
public:
    /// @brief 扫描单个 class 字节段。Err 只出自 BadClassFile 档。
    ConsumerResult<ClassSummary> scan(jbc::core::ByteView bytes) const;

    /// @brief 扫描一个源里全部类（listClasses 序）。fail-closed：任何一个
    ///        获取/解码失败即 Err——上层需要缺失容忍时逐类调 scan 自铸。
    ConsumerResult<std::vector<ClassSummary>> scanSource(const IClassSource& source) const;
};

}   // namespace jbc::consumer
