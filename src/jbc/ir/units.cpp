/// @file        units.cpp
/// @brief       ir 层错误文案、Code 在位查询与全类方法体收集。
///
/// 会话生命周期见 session_lifecycle.cpp；promote/lower/成员增删/重定向
/// 各见同名实现文件。
#include "jbc/ir/units.hpp"

#include "jbc/ir/units_internal.hpp"

#include <cstddef>
#include <unordered_map>
#include <utility>

#include "jbc/ir/sem_index.hpp"

#include "jbc/core/attr_codec.hpp"
#include "jbc/core/insn_codec.hpp"

namespace jbc::ir {
const char* to_string(PromoteError e) noexcept
{
    switch (e) {
    case PromoteError::Ok: return "ok";
    case PromoteError::NoCode: return "no code";
    case PromoteError::AttrNotDecoded: return "attr not decoded";
    case PromoteError::BadInstructions: return "bad instructions";
    case PromoteError::NotModeled: return "not modeled";
    case PromoteError::InvalidHandle: return "invalid handle";
    case PromoteError::InvalidSeed: return "invalid seed";
    }
    return "?";
}

const char* to_string(LowerError e) noexcept
{
    switch (e) {
    case LowerError::Ok: return "ok";
    case LowerError::BadInstructions: return "bad instructions";
    case LowerError::BranchOverflow: return "branch overflow";
    case LowerError::CodeTooLarge: return "code too large";
    case LowerError::CpAppendFailed: return "cp append failed";
    case LowerError::NotPromoted: return "not promoted";
    case LowerError::RelocationNotModeled: return "relocation not modeled";
    case LowerError::RelocationBadTarget: return "relocation bad target";
    case LowerError::InvalidHandle: return "invalid handle";
    case LowerError::InvalidSeed: return "invalid seed";
    case LowerError::NotModeled: return "not modeled";
    case LowerError::AlreadyMounted: return "already mounted";
    case LowerError::BadMount: return "bad mount";
    case LowerError::DomainOverflow: return "domain overflow";
    case LowerError::MalformedAttr: return "malformed attr";
    }
    return "?";
}

bool hasCode(const ClassSession& s, ca::usize methodIdx) noexcept
{
    const core::ClassFile& cf = s.backing();
    const ClassUnit& u        = s.unit();
    if (methodIdx >= u.methods.size()) {
        return false;
    }
    // 逻辑下标经 MethodUnit 的 backingIndex 转物理位置：成员级增删/reorder
    // 之后逻辑序与物理序不再恒等，直接同下标查 backing 会读错方法。
    const ca::usize backing = u.methods[methodIdx].backingIndex;
    if (backing >= cf.methods.size()) {
        return false;
    }
    return findCodeSlot(cf, cf.methods[backing]).first;
}



std::vector<CodeVisit> collectCodeUnits(ClassSession& s)
{
    return collectCodeUnits(s, CodeCollectFilter{});
}

ca::u32 insnFamilyBit(core::Op op) noexcept
{
    using core::Op;
    switch (op) {
    case Op::INVOKEVIRTUAL: case Op::INVOKESPECIAL: case Op::INVOKESTATIC:
    case Op::INVOKEINTERFACE: case Op::INVOKEDYNAMIC:
        return 1u << static_cast<ca::u32>(InsnFamily::Invoke);
    case Op::GETSTATIC: case Op::PUTSTATIC: case Op::GETFIELD: case Op::PUTFIELD:
        return 1u << static_cast<ca::u32>(InsnFamily::FieldAccess);
    case Op::LDC: case Op::LDC_W: case Op::LDC2_W:
    case Op::NEW: case Op::NEWARRAY: case Op::ANEWARRAY: case Op::MULTIANEWARRAY:
    case Op::CHECKCAST: case Op::INSTANCEOF:
        return 1u << static_cast<ca::u32>(InsnFamily::CpConst);
    case Op::GOTO: case Op::GOTO_W: case Op::JSR: case Op::JSR_W: case Op::RET:
    case Op::IFEQ: case Op::IFNE: case Op::IFLT: case Op::IFGE: case Op::IFGT:
    case Op::IFLE: case Op::IF_ICMPEQ: case Op::IF_ICMPNE: case Op::IF_ICMPLT:
    case Op::IF_ICMPGE: case Op::IF_ICMPGT: case Op::IF_ICMPLE: case Op::IF_ACMPEQ:
    case Op::IF_ACMPNE: case Op::IFNULL: case Op::IFNONNULL:
    case Op::TABLESWITCH: case Op::LOOKUPSWITCH:
        return 1u << static_cast<ca::u32>(InsnFamily::Branch);
    default:
        return 0;
    }
}

namespace {

/// 方法体是否含至少一条命中指令族掩码的指令（掩码 0 = 不过滤）。
bool matchesInsnFamilies(const CodeUnit& u, const CodeCollectFilter& filter)
{
    if (filter.insnFamilies == 0) {
        return true;
    }
    for (const InsnId& id : u.dom.order()) {
        const Insn* in = u.dom.peek(id);
        if (in != nullptr && (insnFamilyBit(in->op) & filter.insnFamilies) != 0) {
            return true;
        }
    }
    return false;
}

/// Code 属性嵌套属性是否含至少一个命中 kind 掩码（掩码 0 = 不过滤；
/// 嵌套解码失败按未命中，见 CodeCollectFilter 契约）。
bool matchesAttrKinds(ClassSession& s, ca::usize methodIdx, const CodeCollectFilter& filter)
{
    if (filter.attrKinds == 0) {
        return true;
    }
    core::ClassFile& cf = s.backing();
    const ClassUnit& u  = s.unit();
    if (methodIdx >= u.methods.size()) {
        return false;
    }
    // 逻辑下标经 backingIndex 转物理位置（与 hasCode 同款纪律）。
    const ca::usize backing = u.methods[methodIdx].backingIndex;
    if (backing >= cf.methods.size()) {
        return false;
    }
    const auto [found, slotIdx] = findCodeSlot(cf, cf.methods[backing]);
    if (!found) {
        return false;
    }
    core::AttrSlot& codeSlot = cf.methods[backing].attrs[slotIdx];
    auto d = cf.decodeAttr(codeSlot);
    if (d.is_err() || codeSlot.body == nullptr
        || codeSlot.body->kind() != core::AttrKind::Code) {
        return false;
    }
    auto& code = static_cast<core::CodeAttr&>(*codeSlot.body);
    for (core::AttrSlot& nested : code.attrs) {
        auto nd = cf.decodeAttr(nested);
        if (nd.is_err() || nested.peek() == nullptr) {
            continue;
        }
        if ((filter.attrKinds & (1u << static_cast<ca::u32>(nested.peek()->kind()))) != 0) {
            return true;
        }
    }
    return false;
}

}   // namespace

std::vector<CodeVisit> collectCodeUnits(ClassSession& s, const CodeCollectFilter& filter)
{
    std::vector<CodeVisit> out;
    ClassUnit& u = s.unit();
    out.reserve(u.methods.size());
    for (ca::usize i = 0; i < u.methods.size(); ++i) {
        CodeVisit v;
        v.methodIdx = i;
        if (!hasCode(s, i)) {
            if (filter.includeNoCode) {
                v.kind = CodeVisit::Kind::NoCode;
                out.push_back(std::move(v));
            }
            continue;
        }
        if (u.methods[i].code != nullptr) {
            v.kind = CodeVisit::Kind::Ok;
            v.unit = CodeUnitRef(s.shared_from_this(), u.methods[i].code.get());
        }
        else {
            auto p = promoteMethod(s, i);
            if (p.is_ok()) {
                v.kind = CodeVisit::Kind::Ok;
                v.unit = p.unwrap();
            }
            else {
                v.kind = CodeVisit::Kind::Failed;
                v.error = p.unwrap_err();
            }
        }
        if (v.kind == CodeVisit::Kind::Failed) {
            out.push_back(std::move(v));   // 纪律：真失败恒保留
            continue;
        }
        // Ok：按族过滤（两维交集；维度掩码 0 = 该维度不过滤）。
        if (!matchesInsnFamilies(*v.unit.get(), filter)
            || !matchesAttrKinds(s, i, filter)) {
            continue;
        }
        out.push_back(std::move(v));
    }
    return out;
}
}   // namespace jbc::ir
