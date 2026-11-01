/// @file        promote.cpp
/// @brief       方法体 promote：Code 属性 → CodeDom 工作区（含异常表入域）。
#include "jbc/ir/units.hpp"

#include "jbc/ir/units_internal.hpp"

#include <unordered_map>
#include <utility>

namespace jbc::ir {

/// 在方法属性表里定位 Code 槽；返回 {找到, 槽下标}。
std::pair<bool, ca::usize> findCodeSlot(const core::ClassFile& cf, const core::MemberInfo& m)
{
    for (ca::usize i = 0; i < m.attrs.size(); ++i) {
        if (cf.utf8(m.attrs[i].nameCp) == "Code") {
            return {true, i};
        }
    }
    return {false, 0};
}
/// srcPc → 句柄（promote 用）。srcPc 为 kNoPc 的合成指令不进表。
std::unordered_map<ca::u32, InsnId> buildPcIndex(const CodeDom& dom)
{
    std::unordered_map<ca::u32, InsnId> out;
    out.reserve(dom.size() * 2);
    for (const InsnId& id : dom.order()) {
        const Insn* in = dom.peek(id);
        if (in != nullptr && in->srcPc != kNoPc) {
            out.emplace(in->srcPc, id);
        }
    }
    return out;
}
Result<CodeUnitRef, PromoteError> promoteMethod(ClassSession& s, ca::usize methodIdx)
{
    const ClassUnit& u = s.unit();
    if (methodIdx >= u.methods.size()) {
        return Err(PromoteError::NoCode);
    }
    return promoteMethod(s, u.methods[methodIdx].id);
}

Result<CodeUnitRef, PromoteError> promoteMethod(ClassSession& s, MethodId method)
{
    core::ClassFile& cf = s.backing();
    ClassUnit& u        = s.unit();
    MethodUnit* mu = u.method(method);
    if (mu == nullptr) {
        // 死句柄（已删除/外来会话）与「方法存在但没有体」是两类失败：
        // 失效语义显式可判别，不混入 NoCode。
        return Err(PromoteError::InvalidHandle);
    }
    if (mu->backingIndex >= cf.methods.size()) {
        return Err(PromoteError::InvalidHandle);
    }
    core::MemberInfo& m = cf.methods[mu->backingIndex];

    const auto [found, slotIdx] = findCodeSlot(cf, m);
    if (!found) {
        return Err(PromoteError::NoCode);
    }

    auto d = cf.decodeAttr(m.attrs[slotIdx]);
    if (d.is_err()) {
        return Err(PromoteError::AttrNotDecoded);
    }
    core::AttrBody* body = d.unwrap();
    if (body->kind() != core::AttrKind::Code) {
        return Err(PromoteError::NoCode);
    }
    core::CodeAttr& ca = static_cast<core::CodeAttr&>(*body);

    auto domR = CodeDom::importFrom(core::ByteView{ca.code});
    if (domR.is_err()) {
        return Err(PromoteError::BadInstructions);
    }

    auto unit     = std::make_unique<CodeUnit>();
    unit->dom     = std::move(domR).unwrap();
    unit->maxStack  = ca.maxStack;
    unit->maxLocals = ca.maxLocals;

    const auto pcIndex = buildPcIndex(unit->dom);
    unit->exc.reserve(ca.excTable.size());
    for (const core::CodeAttr::ExcEntry& e : ca.excTable) {
        CodeUnit::Exc x;
        const auto sIt = pcIndex.find(e.startPc);
        const auto hIt = pcIndex.find(e.handlerPc);
        if (sIt == pcIndex.end() || hIt == pcIndex.end()) {
            return Err(PromoteError::BadInstructions);
        }
        auto startLabel = unit->dom.createLabel(sIt->second);
        auto handlerLabel = unit->dom.createLabel(hIt->second);
        if (startLabel.is_err() || handlerLabel.is_err()) {
            return Err(PromoteError::BadInstructions);
        }
        x.start   = startLabel.unwrap();
        x.handler = handlerLabel.unwrap();
        const auto eIt = pcIndex.find(e.endPc);
        if (e.endPc != ca.code.size() && eIt == pcIndex.end()) {
            return Err(PromoteError::BadInstructions);
        }
        auto endLabel = eIt != pcIndex.end() ? unit->dom.createLabel(eIt->second)
                                             : unit->dom.createLabel();
        if (endLabel.is_err()) {
            return Err(PromoteError::BadInstructions);
        }
        x.end      = endLabel.unwrap();
        x.catchCp  = e.catchType;
        unit->exc.push_back(x);
    }

    unit->method = method;
    mu->code = std::move(unit);
    // 保活：返回句柄持会话 shared_ptr——CodeUnit 是会话内部指针，句柄
    // 存活则会话（连同 backing）存活。
    return Ok(CodeUnitRef(s.shared_from_this(), mu->code.get()));
}

}   // namespace jbc::ir
