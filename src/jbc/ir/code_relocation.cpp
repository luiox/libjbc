/// @file        code_relocation.cpp
/// @brief       Code 嵌套属性重定向原语：lower 导出面按 pcmap 先算后写。
///
/// 覆盖 LNT / LVT+LVTT / SMT / TypeAnnotations / CharacterRangeTable 五族
/// 携带字节码偏移的 Code 嵌套属性；契约是 fail-closed（见各函数注释）与
/// 两相 plan/apply（先全量算，全成功才写）。
#include "jbc/ir/units.hpp"

#include "jbc/ir/units_internal.hpp"

#include <unordered_map>
#include <utility>
#include <vector>

namespace jbc::ir {

/// 句柄 → 新 pc（导出后）。死句柄（不在 order 里）返回 Err：静默回落会把
/// 异常表边界写成 pc 0（方法入口），产出坏 classfile——死句柄只能来自绕过
/// CodeEditor::erase 的越权改动，lower 在此显式失败（不猜、不静默）。
/// 「方法末尾」不在这里兜底：end 的空句柄哨兵由调用点显式展开成 code 长度。
Result<ca::u32, LowerError> pcOfId(const CodeDom& dom,
                                   const std::vector<core::RawInsn>& raw, InsnId id)
{
    for (ca::usize i = 0; i < dom.order().size(); ++i) {
        if (dom.order()[i] == id) {
            return Ok(raw[i].pc);
        }
    }
    return Err(LowerError::RelocationBadTarget);
}

/// 标签 → 新 pc。End 只对开区间端点合法；start/handler 解析到 End 即失败。
Result<ca::u32, LowerError> pcOfLabel(const CodeDom& dom,
                                      const std::vector<core::RawInsn>& raw,
                                      LabelId label,
                                      bool allowEnd,
                                      ca::u32 codeLen)
{
    const auto target = dom.resolveLabel(label);
    if (target.is_err()) {
        return Err(LowerError::RelocationBadTarget);
    }
    const LabelTarget resolved = target.unwrap();
    if (resolved.isEnd()) {
        if (allowEnd) {
            return Ok(codeLen);
        }
        return Err(LowerError::RelocationBadTarget);
    }
    return pcOfId(dom, raw, resolved.insn);
}

/// LNT 重定向：startPc 必须精确命中 pcmap。先计算新值，避免失败时半改属性。
Result<std::vector<ca::u32>, LowerError>
relocateLnt(const core::LineNumberTableAttr& lnt,
            const std::unordered_map<ca::u32, ca::u32>& pcmap)
{
    std::vector<ca::u32> starts;
    starts.reserve(lnt.rows.size());
    for (const auto& row : lnt.rows) {
        const auto it = pcmap.find(row.startPc);
        if (it == pcmap.end()) {
            return Err(LowerError::RelocationBadTarget);
        }
        starts.push_back(it->second);
    }
    return Ok(std::move(starts));
}

/// LVT/LVTT 重定向（fail-closed）：startPc 必须精确命中 pcmap；区间终点
/// oldEnd = startPc + length 必须命中 pcmap（落在旧指令起点），或严格等于
/// oldCodeLen（只有这一种 miss 允许，映射到 newCodeLen）；其余 miss 与
/// u32 溢出都返回 RelocationBadTarget——静默钳到新长度会把「终点落在指令
/// 操作数中间」的畸形输入写成合法形态。
Result<LocalVariableRelocation, LowerError>
relocateLvt(const core::LocalVariableTableAttr& lvt,
            const std::unordered_map<ca::u32, ca::u32>& pcmap,
            ca::u32 oldCodeLen,
            ca::u32 newCodeLen)
{
    LocalVariableRelocation result;
    result.starts.reserve(lvt.rows.size());
    result.lengths.reserve(lvt.rows.size());
    for (const auto& row : lvt.rows) {
        const auto it = pcmap.find(row.startPc);
        if (it == pcmap.end()) {
            return Err(LowerError::RelocationBadTarget);
        }
        const ca::u64 end = static_cast<ca::u64>(row.startPc) + row.length;
        if (end > 0xFFFFFFFFull) {
            return Err(LowerError::RelocationBadTarget);
        }
        const ca::u32 oldEnd = static_cast<ca::u32>(end);
        const auto eit       = pcmap.find(oldEnd);
        ca::u32 newEnd;
        if (eit != pcmap.end()) {
            newEnd = eit->second;
        } else if (oldEnd == oldCodeLen) {
            newEnd = newCodeLen;
        } else {
            return Err(LowerError::RelocationBadTarget);
        }
        result.starts.push_back(it->second);
        result.lengths.push_back(newEnd > it->second ? newEnd - it->second : 0);
    }
    return Ok(std::move(result));
}

/// SMT 重定向：链式 offset_delta → 绝对 pc → pcmap → 重算 delta。
/// 紧凑形态的 delta 越界时升格为 extended 形态（251/247）。
/// 链式算术与形态标签取 core/attribute.hpp 的共享定义（与生成器同一版）。
Result<std::vector<core::StackMapTableAttr::Frame>, LowerError>
relocateSmt(const core::StackMapTableAttr& smt,
            const std::unordered_map<ca::u32, ca::u32>& pcmap)
{
    using SmtFrame = core::StackMapTableAttr::Frame;
    using SmtVType = core::StackMapTableAttr::VType;
    std::vector<core::StackMapTableAttr::Frame> frames = smt.frames;
    // 1. 旧绝对 pc 链。
    std::vector<ca::u32> abs(frames.size(), 0);
    for (ca::usize i = 0; i < frames.size(); ++i) {
        abs[i] = i == 0 ? frames[i].offset : core::smtAbsPc(abs[i - 1], frames[i].offset);
    }
    // 2. 映射到新 pc。
    std::vector<ca::u32> fresh(frames.size(), 0);
    for (ca::usize i = 0; i < frames.size(); ++i) {
        const auto it = pcmap.find(abs[i]);
        if (it == pcmap.end()) {
            return Err(LowerError::RelocationBadTarget);
        }
        fresh[i] = it->second;
    }
    // 3. verification_type_info 的 Uninitialized(8) 载荷也是旧 code 中的
    //    指令偏移，必须和 frame 目标使用同一张 pcmap；Object(7) 的载荷则
    //    是常量池索引，不能误重定位。
    const auto relocateVType = [&](core::StackMapTableAttr::VType& v)
        -> Result<void, LowerError> {
        if (v.tag != SmtVType::kUninitialized) {
            return Ok();
        }
        const auto it = pcmap.find(v.cp);
        if (it == pcmap.end()) {
            return Err(LowerError::RelocationBadTarget);
        }
        v.cp = it->second;
        return Ok();
    };
    for (auto& f : frames) {
        for (auto& v : f.locals) {
            auto r = relocateVType(v);
            if (r.is_err()) {
                return Err(r.unwrap_err());
            }
        }
        for (auto& v : f.stack) {
            auto r = relocateVType(v);
            if (r.is_err()) {
                return Err(r.unwrap_err());
            }
        }
    }

    // 4. 重算 delta；紧凑形态的 delta 越界时升格为 extended 形态，仍在
    //    紧凑范围内时必须同步更新 type（编码器从 type 取 delta）。
    for (ca::usize i = 0; i < frames.size(); ++i) {
        if (i != 0 && fresh[i] <= fresh[i - 1]) {
            return Err(LowerError::RelocationBadTarget);
        }
        const ca::u32 nd = i == 0 ? fresh[0] : core::smtDelta(fresh[i - 1], fresh[i]);
        auto& f          = frames[i];
        if (nd > 0xFFFF) {
            return Err(LowerError::RelocationBadTarget);
        }
        if (f.type <= SmtFrame::kSameMax) {
            f.type = nd <= SmtFrame::kSameMax ? static_cast<ca::u8>(nd) : SmtFrame::kSameExt;
        } else if (f.type >= SmtFrame::kSame1Base && f.type <= SmtFrame::kSame1Max) {
            f.type = nd <= SmtFrame::kSameMax
                         ? static_cast<ca::u8>(SmtFrame::kSame1Base + nd)
                         : SmtFrame::kSame1Ext;
        }
        f.offset = nd;
    }
    return Ok(std::move(frames));
}

/// TypeAnnotations 重定向（JVMS 4.7.20/4.7.21 的 Code 内挂载形态）：只动
/// 携带字节码偏移的目标——localvar/resourcevar 行（start/length 走与 LVT
/// 相同的 fail-closed 终点规则）与 offset/cast/type-arg 族（单偏移精确
/// 命中 pcmap）。catch 目标的 catch_index 指向异常表条目下标，lower 重建
/// 异常表保持条目顺序与条数，下标稳定，不动；type_param/supertype/
/// bound/empty/formals/throws 等类/方法级目标不携带 pc，同样不动。
/// 类型路径与元素值不含 pc。未知 target_type 在解码层已拒绝（Err
/// UnsupportedFeature），到不了这里。
///
/// Annotation 携带 unique_ptr 载荷不可拷贝，因此拆成「纯计算 plan +
/// 就地 apply」两步：plan 全量成功才允许 apply（all-or-nothing），
/// apply 只写标量字段，不会再失败。
Result<TannRelocation, LowerError>
planTann(const core::TypeAnnotationsAttr& ta,
         const std::unordered_map<ca::u32, ca::u32>& pcmap,
         ca::u32 oldCodeLen,
         ca::u32 newCodeLen)
{
    using TA = core::TypeAnnotationsAttr;
    TannRelocation out;
    out.annos.reserve(ta.annotations.size());
    for (const TA::Annotation& a : ta.annotations) {
        TannAnnotationPlan pa;
        if (a.targetType == TA::kLocalVar || a.targetType == TA::kLocalVarResource) {
            pa.rows.resize(a.localVars.size());
            for (ca::usize i = 0; i < a.localVars.size(); ++i) {
                const TA::LocalVarEntry& row = a.localVars[i];
                const auto it = pcmap.find(row.startPc);
                if (it == pcmap.end()) {
                    return Err(LowerError::RelocationBadTarget);
                }
                const ca::u64 end = static_cast<ca::u64>(row.startPc) + row.length;
                if (end > 0xFFFFFFFFull) {
                    return Err(LowerError::RelocationBadTarget);
                }
                const ca::u32 oldEnd = static_cast<ca::u32>(end);
                const auto eit       = pcmap.find(oldEnd);
                ca::u32 newEnd;
                if (eit != pcmap.end()) {
                    newEnd = eit->second;
                } else if (oldEnd == oldCodeLen) {
                    newEnd = newCodeLen;
                } else {
                    return Err(LowerError::RelocationBadTarget);
                }
                pa.rows[i].start  = it->second;
                pa.rows[i].length = newEnd > it->second ? newEnd - it->second : 0;
            }
        } else if (a.targetType >= TA::kOffsetBase && a.targetType <= TA::kTypeArgMax) {
            // 0x43..0x46 offset、0x47 cast、0x48..0x4B type-arg：载荷首段
            // 都是旧 code 中的指令偏移（cast/type-arg 的后段是序号，不动）。
            const auto it = pcmap.find(a.offset);
            if (it == pcmap.end()) {
                return Err(LowerError::RelocationBadTarget);
            }
            pa.hasOffset = true;
            pa.newOffset = it->second;
        }
        out.annos.push_back(std::move(pa));
    }
    return Ok(std::move(out));
}

/// 按 plan 就地重写（只写标量字段；plan 已全量验证，本步不会再失败）。
void applyTann(core::TypeAnnotationsAttr& ta, const TannRelocation& plan)
{
    using TA = core::TypeAnnotationsAttr;
    for (ca::usize i = 0; i < ta.annotations.size() && i < plan.annos.size(); ++i) {
        TA::Annotation& a          = ta.annotations[i];
        const TannAnnotationPlan& pa = plan.annos[i];
        for (ca::usize j = 0; j < a.localVars.size() && j < pa.rows.size(); ++j) {
            a.localVars[j].startPc = pa.rows[j].start;
            a.localVars[j].length  = pa.rows[j].length;
        }
        if (pa.hasOffset) {
            a.offset = pa.newOffset;
        }
    }
}

/// CharacterRangeTable 重定向（javac -Xjcov 私有属性，Code 嵌套）：每行的
/// start_pc 与 end_pc 都是旧 code 中的指令起始 pc（end_pc 是 javac 写出的
/// entry.endPc-1，含末指令），各自精确命中 pcmap，miss 即 RelocationBadTarget
/// ——不存在「指向 code 长」的合法 miss（含末指令语义决定了它总是指令 pc）。
/// 字符区间与 flags 不携带 pc，原样保留。
Result<CrtRelocation, LowerError>
relocateCrt(const core::CharacterRangeTableAttr& crt,
            const std::unordered_map<ca::u32, ca::u32>& pcmap)
{
    CrtRelocation out;
    out.starts.reserve(crt.rows.size());
    out.ends.reserve(crt.rows.size());
    for (const auto& row : crt.rows) {
        const auto sit = pcmap.find(row.startPc);
        if (sit == pcmap.end()) {
            return Err(LowerError::RelocationBadTarget);
        }
        const auto eit = pcmap.find(row.endPc);
        if (eit == pcmap.end()) {
            return Err(LowerError::RelocationBadTarget);
        }
        out.starts.push_back(sit->second);
        out.ends.push_back(eit->second);
    }
    return Ok(std::move(out));
}


}   // namespace jbc::ir
