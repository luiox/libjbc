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
