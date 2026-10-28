/// @file        code_editor.cpp
/// @brief       变换协议实现：删除策略与基本块视图。
#include "jbc/ir/code_editor.hpp"

#include <unordered_set>

namespace jbc::ir {

Result<void, EditError> CodeEditor::setTarget(InsnId branch, LabelId to)
{
    auto in = u_.dom.get(branch);
    if (in.is_err()) {
        return Err(in.unwrap_err());
    }
    const core::Shape sh = core::shapeOf(in.unwrap()->op);
    if (sh != core::Shape::Branch2 && sh != core::Shape::Branch4 &&
        sh != core::Shape::TableSwitch && sh != core::Shape::LookupSwitch) {
        return Err(EditError::BadOperand);
    }
    const auto target = u_.dom.resolveLabel(to);
    if (target.is_err() || target.unwrap().isEnd()) {
        return Err(EditError::InvalidHandle);
    }
    in.unwrap()->target = to;
    return Ok();
}

Result<void, EditError> CodeEditor::erase(InsnId id, ErasePolicy p)
{
    // 异常表边界是 Boundary 角色标签。CodeDom::erase 会拒绝删除仍被
    // branch/switch 字段引用或被存活 Boundary 标签锚定的指令；调用方必须
    // 先显式移动/销毁对应边界，再编辑 u_.exc，不能由 fallthrough 策略
    // 猜测保护区间语义。
    if (p == ErasePolicy::kRejectIfUsed) {
        return u_.dom.erase(id);
    }
    for (const CodeUnit::Exc& e : u_.exc) {
        for (LabelId label : {e.start, e.end, e.handler}) {
            const auto target = u_.dom.resolveLabel(label);
            if (target.is_ok() && !target.unwrap().isEnd() &&
                target.unwrap().insn == id) {
                return Err(EditError::StillUsed);
            }
        }
    }
    // kRetargetToFallthrough：找到顺序后继，把 incoming uses 换过去再删。
    const auto& order = u_.dom.order();
    ca::usize pos = order.size();
    for (ca::usize i = 0; i < order.size(); ++i) {
        if (order[i] == id) {
            pos = i;
            break;
        }
    }
    if (pos == order.size() || pos + 1 >= order.size()) {
        // 末条指令没有顺序后继可承接引用。
        return Err(EditError::StillUsed);
    }
    const InsnId next = order[pos + 1];
    auto r = u_.dom.replaceAllUses(id, next);
    if (r.is_err()) {
        return Err(r.unwrap_err());
    }
    return u_.dom.erase(id);
}

Result<void, EditError> CodeEditor::eraseBatch(const std::vector<InsnId>& ids,
                                               ErasePolicy p)
{
    // 异常表边界是 Boundary 角色标签：与单条 erase 同规，不可由 fallthrough
    // 策略猜测保护区间语义，先显式拒绝（在任何变更之前）。
    if (p == ErasePolicy::kRejectIfUsed) {
        return u_.dom.eraseBatch(ids);
    }
    std::unordered_set<ca::u32> doomedSlots;
    for (const InsnId& id : ids) {
        if (u_.dom.peek(id) == nullptr) {
            return Err(EditError::InvalidHandle);
        }
        doomedSlots.insert(id.slot);
    }
    for (const CodeUnit::Exc& e : u_.exc) {
        for (LabelId label : {e.start, e.end, e.handler}) {
            const auto target = u_.dom.resolveLabel(label);
            if (target.is_ok() && !target.unwrap().isEnd() &&
                doomedSlots.count(target.unwrap().insn.slot) != 0) {
                return Err(EditError::StillUsed);
            }
        }
    }
    // kRetargetToFallthrough 的批量推广：批内成员的承接者 = 其后第一条
    // 批外存活指令。倒序单趟扫描得出全部安排；末尾无承接者的成员失败，
    // 且失败发生在任何变更之前（安排还未交给 eraseBatch）。
    const auto& order = u_.dom.order();
    std::vector<std::pair<InsnId, InsnId>> retarget;
    retarget.reserve(ids.size());
    InsnId nextSurvivor = kNoInsn;
    for (ca::usize i = order.size(); i-- > 0;) {
        if (doomedSlots.count(order[i].slot) != 0) {
            if (nextSurvivor == kNoInsn) {
                return Err(EditError::StillUsed);
            }
            retarget.push_back({order[i], nextSurvivor});
        }
        else {
            nextSurvivor = order[i];
        }
    }
    return u_.dom.eraseBatch(ids, retarget);
}

Result<void, EditError> CodeEditor::moveRange(InsnId first, InsnId last, InsnId at,
                                              MoveRangePolicy p)
{
    // ---- 校验阶段（全只读，任何失败发生在变更之前）----
    if (u_.dom.peek(first) == nullptr) {
        return Err(EditError::InvalidHandle);
    }
    const bool toEnd     = at == kNoInsn;
    const bool lastIsEnd = last == kNoInsn;   // 尾哨兵：段延伸到方法末尾
    if (!toEnd && u_.dom.peek(at) == nullptr) {
        return Err(EditError::InvalidHandle);
    }
    if (!lastIsEnd && u_.dom.peek(last) == nullptr) {
        return Err(EditError::InvalidHandle);
    }
    const auto& order = u_.dom.order();
    ca::usize firstPos = order.size();
    ca::usize lastPos  = order.size();
    ca::usize atPos    = order.size();   // kNoInsn 锚点 = 末尾哨兵位
    for (ca::usize i = 0; i < order.size(); ++i) {
        if (order[i] == first) {
            firstPos = i;
        }
        if (!lastIsEnd && order[i] == last) {
            lastPos = i;
        }
        if (order[i] == at) {
            atPos = i;
        }
    }
    // 段几何与 CodeDom::moveRange 同规：空段/倒置段/锚点在段内拒绝。
    if (firstPos >= lastPos || (!toEnd && atPos > firstPos && atPos < lastPos)) {
        return Err(EditError::BadRange);
    }
    // 段已在锚点位置：整体无操作，kHeadStays 也不触发改绑——没有位移就
    // 没有「留原地」的语义可执行，保持锚定现状才是无操作的本义。
    const bool noop = toEnd ? lastPos == order.size()
                            : (atPos == firstPos || atPos == lastPos);
    if (noop || p == MoveRangePolicy::kHeadFollows) {
        return u_.dom.moveRange(first, last, at);
    }

    // ---- kHeadStays：段首标签全部留在原位 ----
    // 承接者 = 原段位后继（搬走后顶替原段位的指令，即原 exclusive 端）；
    // 段居尾（last 为尾哨兵）则承接者是方法末边界。段内其余标签不在改绑
    // 之列（随段迁移是默认语义）。
    const InsnId successor = lastPos < order.size() ? order[lastPos] : kNoInsn;
    const std::vector<LabelId> heads = u_.dom.labelsAt(first);
    // 异常表边界引用的段首标签不可机械改绑：改边界 = 改保护区间语义，
    // 与 erase 的 fallthrough 策略同规——调用方先显式 bindLabel/destroyLabel。
    for (const CodeUnit::Exc& e : u_.exc) {
        for (LabelId label : {e.start, e.end, e.handler}) {
            for (const LabelId head : heads) {
                if (head == label) {
                    return Err(EditError::StillUsed);
                }
            }
        }
    }
    // 段居尾时承接者是 end 边界：仍被控制字段引用的段首标签不可改绑过去
    // （分支目标须锚定存活指令是 checkHandles/exportRaw 的 standing 不变量，
    // end 锚定的控制目标连导出都无法表达）。
    if (successor == kNoInsn) {
        for (const InsnId& uid : order) {
            const Insn* in = u_.dom.peek(uid);
            if (in == nullptr) {
                continue;
            }
            const core::Shape sh = core::shapeOf(in->op);
            const bool isBranch = sh == core::Shape::Branch2 || sh == core::Shape::Branch4;
            const bool isSwitch = sh == core::Shape::TableSwitch ||
                                  sh == core::Shape::LookupSwitch;
            if (!isBranch && !isSwitch) {
                continue;
            }
            for (const LabelId head : heads) {
                if (in->target == head) {
                    return Err(EditError::StillUsed);
                }
                if (isSwitch) {
                    for (const LabelId& t : in->targets) {
                        if (t == head) {
                            return Err(EditError::StillUsed);
                        }
                    }
                }
            }
        }
    }
    // ---- 变更阶段：改绑 + 单趟重排，失败条件已被校验全部排除 ----
    for (const LabelId head : heads) {
        // heads 来自存活 DOM，successor 活或为末哨兵，bindLabel 不会失败；
        // 错误通道保留完整性，防御性返回。
        auto r = u_.dom.bindLabel(head, successor);
        if (r.is_err()) {
            return Err(r.unwrap_err());
        }
    }
    return u_.dom.moveRange(first, last, at);
}

std::vector<CodeEditor::Block> CodeEditor::blocks() const
{
    const auto& order = u_.dom.order();
    std::vector<Block> out;
    if (order.empty()) {
        return out;
    }

    std::unordered_set<ca::u32> leaderSlots;
    leaderSlots.insert(order[0].slot);
    const auto markLeader = [&](InsnId id) {
        if (const Insn* in = u_.dom.peek(id)) {
            (void)in;
            leaderSlots.insert(id.slot);
        }
    };
    const auto markLabel = [&](LabelId label) {
        const auto target = u_.dom.resolveLabel(label);
        if (target.is_ok() && !target.unwrap().isEnd()) {
            markLeader(target.unwrap().insn);
        }
    };
    const auto markAfter = [&](ca::usize i) {
        if (i + 1 < order.size()) {
            leaderSlots.insert(order[i + 1].slot);
        }
    };

    for (ca::usize i = 0; i < order.size(); ++i) {
        const Insn* in = u_.dom.peek(order[i]);
        if (in == nullptr) {
            continue;
        }
        const core::Shape sh = core::shapeOf(in->op);
        if (sh == core::Shape::Branch2 || sh == core::Shape::Branch4) {
            markLabel(in->target);
            markAfter(i);
        } else if (sh == core::Shape::TableSwitch || sh == core::Shape::LookupSwitch) {
            markLabel(in->target);
            for (const LabelId& t : in->targets) {
                markLabel(t);
            }
            markAfter(i);
        } else {
            const core::Op op = in->op;
            const bool endsFlow = op == core::Op::RETURN || op == core::Op::IRETURN ||
                                  op == core::Op::LRETURN || op == core::Op::FRETURN ||
                                  op == core::Op::DRETURN || op == core::Op::ARETURN ||
                                  op == core::Op::ATHROW || op == core::Op::RET;
            if (endsFlow) {
                markAfter(i);
            }
        }
    }
    for (const CodeUnit::Exc& e : u_.exc) {
        markLabel(e.start);
        markLabel(e.handler);
        // end 是开区间边界：若不是方法末尾，它本身也是新块的入口。
        markLabel(e.end);
    }

    // 顺序扫描切分。
    Block cur;
    for (ca::usize i = 0; i < order.size(); ++i) {
        if (leaderSlots.count(order[i].slot) != 0 && !cur.insns.empty()) {
            out.push_back(std::move(cur));
            cur = Block{};
        }
        if (cur.insns.empty()) {
            cur.entry = order[i];
        }
        cur.insns.push_back(order[i]);
    }
    if (!cur.insns.empty()) {
        out.push_back(std::move(cur));
    }
    return out;
}

}   // namespace jbc::ir
