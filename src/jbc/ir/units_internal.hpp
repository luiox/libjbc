/// @file        units_internal.hpp
/// @brief       ir 内部共享面：promote/lower 的互助原语与重定位计划结构。
///
/// 本头不出公开面——仅 ir 各 .cpp 经此互认（同 sem_index.hpp 的角色）；
/// 公开 API 一律以 units.hpp 为准。行为契约注释随实现（code_relocation.cpp /
/// promote.cpp），此处只列签名。
#pragma once

#include <cstddef>
#include <unordered_map>
#include <utility>
#include <vector>

#include "jbc/core/class_file.hpp"

#include "jbc/ir/units.hpp"

namespace jbc::ir {

/// 在方法属性表里定位 Code 槽；返回 {找到, 槽下标}。
std::pair<bool, ca::usize> findCodeSlot(const core::ClassFile& cf, const core::MemberInfo& m);

/// srcPc → 句柄（promote 用）。srcPc 为 kNoPc 的合成指令不进表。
std::unordered_map<ca::u32, InsnId> buildPcIndex(const CodeDom& dom);

/// 句柄 → 新 pc（导出后）。死句柄返回 Err：静默回落会把异常表边界写成
/// pc 0（方法入口），产出坏 classfile——死句柄只能来自绕过 CodeEditor::erase
/// 的越权改动，lower 在此显式失败（不猜、不静默）。
Result<ca::u32, LowerError> pcOfId(const CodeDom& dom,
                                   const std::vector<core::RawInsn>& raw, InsnId id);

/// 标签 → 新 pc。End 只对开区间端点合法；start/handler 解析到 End 即失败。
Result<ca::u32, LowerError> pcOfLabel(const CodeDom& dom,
                                      const std::vector<core::RawInsn>& raw,
                                      LabelId label,
                                      bool allowEnd,
                                      ca::u32 codeLen);

/// LVT/LVTT 重定向的先算后写计划（startPc/length 成对；fail-closed 终点
/// 规则见 relocateLvt 实现处注释）。
struct LocalVariableRelocation
{
    std::vector<ca::u32> starts;
    std::vector<ca::u32> lengths;
};

Result<LocalVariableRelocation, LowerError>
relocateLvt(const core::LocalVariableTableAttr& lvt,
            const std::unordered_map<ca::u32, ca::u32>& pcmap,
            ca::u32 oldCodeLen,
            ca::u32 newCodeLen);

/// LNT 重定向（startPc 精确命中 pcmap）。
Result<std::vector<ca::u32>, LowerError>
relocateLnt(const core::LineNumberTableAttr& lnt,
            const std::unordered_map<ca::u32, ca::u32>& pcmap);

/// SMT 重定向（链式 delta → 绝对 pc → pcmap → 重算 delta，越界升格 extended）。
Result<std::vector<core::StackMapTableAttr::Frame>, LowerError>
relocateSmt(const core::StackMapTableAttr& smt,
            const std::unordered_map<ca::u32, ca::u32>& pcmap);

/// TypeAnnotations 重定位计划：Annotation 携带 unique_ptr 载荷不可拷贝，
/// 因此拆成「纯计算 plan + 就地 apply」两步——plan 全量成功才允许 apply
/// （all-or-nothing），apply 只写标量字段，不会再失败。
struct TannAnnotationPlan
{
    bool hasOffset = false;   ///< offset/cast/type-arg 族：true 时 offset 需重写
    ca::u32 newOffset = 0;

    struct Row
    {
        ca::u32 start  = 0;
        ca::u32 length = 0;
    };
    std::vector<Row> rows;   ///< localvar 行：与 localVars 下标对齐
};

struct TannRelocation
{
    std::vector<TannAnnotationPlan> annos;   ///< 与 annotations 下标对齐
};

Result<TannRelocation, LowerError>
planTann(const core::TypeAnnotationsAttr& ta,
         const std::unordered_map<ca::u32, ca::u32>& pcmap,
         ca::u32 oldCodeLen,
         ca::u32 newCodeLen);

/// 按 plan 就地重写（只写标量字段；plan 已全量验证，本步不会再失败）。
void applyTann(core::TypeAnnotationsAttr& ta, const TannRelocation& plan);

/// CharacterRangeTable 重定向计划（start_pc/end_pc 双列，均须精确命中
/// pcmap，miss 即 RelocationBadTarget——含末指令语义决定了它总是指令 pc）。
struct CrtRelocation
{
    std::vector<ca::u32> starts;
    std::vector<ca::u32> ends;
};

Result<CrtRelocation, LowerError>
relocateCrt(const core::CharacterRangeTableAttr& crt,
            const std::unordered_map<ca::u32, ca::u32>& pcmap);

}   // namespace jbc::ir
