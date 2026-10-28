/// @file        code_editor.hpp
/// @brief       变换协议：编辑便利、删除策略与基本块视图。
///
/// design.md §7.3 的最小可用集：结构化变更全部经过句柄与策略表达；
/// 「摘链但忘记 delete」「label 池旁路」在这套 API 上没有入口。基本块
/// 是按需计算的只读视图（入口 = 顺序头/跳转目标/异常处理器起点），
/// 不是持久结构——v0.3 尚未引入 BlockId 域，块操作走指令句柄。
#pragma once

#include <vector>

#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/units.hpp"

namespace jbc::ir {

class CodeEditor
{
public:
    explicit CodeEditor(CodeUnit& u) noexcept : u_(u) {}

    CodeUnit& unit() noexcept { return u_; }
    const CodeUnit& unit() const noexcept { return u_; }

    // ---- 构建/插入（合成指令；srcPc 由 CodeDom 强制为 kNoPc） ----

    Result<InsnId, EditError> append(Insn in) { return u_.dom.append(std::move(in)); }
    Result<InsnId, EditError> insertBefore(InsnId at, Insn in)
    {
        return u_.dom.insertBefore(at, std::move(in));
    }
    Result<InsnId, EditError> insertAfter(InsnId at, Insn in)
    {
        return u_.dom.insertAfter(at, std::move(in));
    }
    Result<InsnId, EditError> insertAtLabel(LabelId label, Insn in)
    {
        return u_.dom.insertAtLabel(label, std::move(in));
    }
    Result<void, EditError> moveBefore(InsnId id, InsnId at)
    {
        return u_.dom.moveBefore(id, at);
    }
    Result<void, EditError> moveAfter(InsnId id, InsnId at)
    {
        return u_.dom.moveAfter(id, at);
    }
    /// @brief 方法入口插桩的最短路径（first() 为 kNoInsn 时失败）。
    Result<InsnId, EditError> insertAtStart(Insn in)
    {
        const InsnId at = u_.dom.first();
        if (at == kNoInsn) {
            return u_.dom.append(std::move(in));
        }
        return u_.dom.insertBefore(at, std::move(in));
    }

    /// @brief 区间搬移策略：段首边界标签的两种去向（issue #10）。
    ///
    /// 段内（非段首）标签不进策略——它们锚定段内指令，随段迁移本就是
    /// 顺序重排的默认语义；策略只裁决段首标签歧义（它既可读作段的入口，
    /// 也可读作原段位的标记）。
    enum class MoveRangePolicy {
        kHeadFollows,  ///< 段首标签随段迁移（默认）：锚定关系不动，段内标签
                       ///< 与段外对段内的引用随句柄自然保持
        kHeadStays,    ///< 段首标签留在原位承接原 fallthrough：锚点重定向到
                       ///< 原段位后继（段居尾则改绑方法末边界）；被异常表
                       ///< 引用的段首标签不可机械改绑，StillUsed 拒绝
    };

    /// @brief 把 `[first, last)` 连续指令段整体搬到 at 之前，段内相对顺序
    ///        不变；`at == kNoInsn` = 搬到方法末尾，`last == kNoInsn` =
    ///        段延伸到方法末尾（尾哨兵，half-open 语义下尾段的可命名形）。
    ///
    /// 原子性：句柄存活、段几何（空段/倒置段/锚点在段内 → BadRange）与
    /// 策略冲突（kHeadStays 遇异常表引用或段居尾时控制字段仍引用的段首
    /// 标签 → StillUsed）全部在变更前校验，任一步失败不留半搬状态。
    /// 无操作（锚点即段首或紧随段尾）整体不触发改绑。段内标签随段迁移、
    /// 段外分支指向段内标签的引用保持有效，均由句柄锚定自然成立，无需
    /// 改写任何字段；checkHandles 的「控制目标须锚定存活指令」不变量在
    /// 两条策略下都保持（见 MoveRangePolicy::kHeadStays 的拒绝面）。
    Result<void, EditError> moveRange(InsnId first, InsnId last, InsnId at,
                                      MoveRangePolicy p = MoveRangePolicy::kHeadFollows);

    /// @brief 重写一条分支/switch 默认目标（不动 case 表）。
    ///
    /// 只改写该指令的控制流字段，不改写其他字段或异常表边界；被替换的
    /// 旧控制流标签留在标签槽中，但不再保护其锚点（见 CodeDom::erase）。
    /// 调用方把普通 Boundary 标签同时用作控制流目标和外部范围时，需要
    /// 显式拆分标签——库不能从共享标签推断两种语义。
    Result<void, EditError> setTarget(InsnId branch, LabelId to);

    /// @brief 创建/移动/销毁方法边界标签。
    ///
    /// 公开创建的标签是 Boundary 角色（元数据边界语义，保护锚点）；分支/
    /// switch 操作数与异常表当前都已使用 LabelId，导入时的控制流标签由
    /// CodeDom 以 ControlFlow 角色内部创建。
    Result<LabelId, EditError> createLabel(InsnId anchor = kNoInsn)
    {
        return u_.dom.createLabel(anchor);
    }
    Result<void, EditError> bindLabel(LabelId label, InsnId anchor = kNoInsn)
    {
        return u_.dom.bindLabel(label, anchor);
    }
    Result<void, EditError> destroyLabel(LabelId label)
    {
        return u_.dom.destroyLabel(label);
    }

    enum class ErasePolicy {
        kRejectIfUsed,        ///< 有 incoming use 即 StillUsed（默认，显式安全；
                              ///< 异常表 start/end/handler 引用也算 use）
        kRetargetToFallthrough,  ///< 先把跳转 uses 指到顺序后继再删；无后继即
                                 ///< 失败。异常表边界引用不可机械 retarget
                                 ///< （改边界 = 改保护区间语义），一律 StillUsed
    };
    Result<void, EditError> erase(InsnId id, ErasePolicy p = ErasePolicy::kRejectIfUsed);

    /// @brief 批量删除（D3）：一次校验规划、单趟重建，替代循环单删。
    ///
    /// 策略语义与单条 erase 对齐的批量推广：
    ///   - kRejectIfUsed：任一存活引用即 StillUsed，什么都不删（全有或全无）。
    ///   - kRetargetToFallthrough：先把存活指令面向批内成员的字段改锚到该
    ///     成员之后的第一个批外存活指令（顺序后继在批量语境的推广），再整批
    ///     删除。异常表边界引用与「之后无存活指令」的成员仍按 StillUsed
    ///     拒绝；任何失败都不落变更。
    /// 批内互指不算引用（随批消亡）；重复句柄按并集处理。
    Result<void, EditError> eraseBatch(const std::vector<InsnId>& ids,
                                       ErasePolicy p = ErasePolicy::kRejectIfUsed);

    // ---- 基本块视图（只读，按需计算） ----

    struct Block
    {
        InsnId entry;                 ///< 块第一条指令
        std::vector<InsnId> insns;    ///< 顺序成员（含 entry）
    };

    /// @brief 按领导指令切块：顺序头、跳转/switch 目标、异常处理器入口，
    ///        以及任何 return/athrow/分支/switch 的顺序后继。
    std::vector<Block> blocks() const;

    // ---- 合成指令工厂（常用形状；其余字段默认） ----

    static Insn simple(core::Op op) { Insn i; i.op = op; return i; }
    static Insn local(core::Op op, ca::u32 idx)
    {
        Insn i;
        i.op = op;
        i.a  = static_cast<ca::i32>(idx);
        return i;
    }
    static Insn branch(core::Op op, LabelId target)
    {
        Insn i;
        i.op     = op;
        i.target = target;
        return i;
    }
    static Insn cpInsn(core::Op op, ca::u32 cp)
    {
        Insn i;
        i.op = op;
        i.cp = cp;
        return i;
    }
    /// @brief ldc 族常量装载：索引 ≤ 0xFF 用 LDC，否则自动升格 LDC_W。
    ///
    /// LDC 的操作数域只有 u1，池增长后索引超界是变换期的常见坑——
    /// 编码器对超界 LDC 返回 Unencodable，这里直接替调用方选对形态。
    static Insn loadConst(ca::u32 cpIdx)
    {
        Insn i;
        i.op = cpIdx <= 0xFF ? core::Op::LDC : core::Op::LDC_W;
        i.cp = cpIdx;
        return i;
    }
    static Insn iinc(ca::u32 idx, ca::i32 c)
    {
        Insn i;
        i.op = core::Op::IINC;
        i.a  = static_cast<ca::i32>(idx);
        i.b  = c;
        return i;
    }

private:
    CodeUnit& u_;
};

}   // namespace jbc::ir
