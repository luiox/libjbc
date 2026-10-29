/// @file        smt_read.cpp
/// @brief       StackMapTable 读侧：属性定位 → 形态链展开 → 槽语义帧交付。
#include "jbc/analysis/smt_read.hpp"

#include <set>
#include <unordered_map>

#include "jbc/analysis/domain.hpp"
#include "jbc/core/opcode.hpp"

namespace jbc::analysis {

namespace {

using core::StackMapTableAttr;
using SmtVType = StackMapTableAttr::VType;

/// 方法 Code 属性定位（lazy 解码置 Decoded 态缓存）；无 Code 返回 nullptr。
core::CodeAttr* findCodeAttr(core::ClassFile& cf, core::MemberInfo& m)
{
    for (core::AttrSlot& slot : m.attrs) {
        auto b = cf.decodeAttr(slot);
        if (b.is_ok() && b.unwrap()->kind() == core::AttrKind::Code) {
            return static_cast<core::CodeAttr*>(b.unwrap());
        }
    }
    return nullptr;
}

/// Code 内嵌套 SMT 属性定位；缺席返回 nullptr（合法缺席，调用方交空视图）。
const StackMapTableAttr* findSmtAttr(core::ClassFile& cf, core::CodeAttr& code)
{
    for (core::AttrSlot& slot : code.attrs) {
        auto b = cf.decodeAttr(slot);
        if (b.is_ok() && slot.peek() != nullptr &&
            slot.peek()->kind() == core::AttrKind::StackMapTable) {
            return static_cast<const StackMapTableAttr*>(slot.peek());
        }
    }
    return nullptr;
}

/// verification_type 条目 → 语义 VType。Object 经 CP 解析精确类名（借用
/// 视图，寿命 = 常量池）；tag-8 载荷按写侧  同规校验落点。
ca::core::Result<VType, SmtReadError>
toSemantic(const SmtVType& e, const core::ConstantPool& cp,
           const std::set<ca::u32>& newPcs)
{
    using R = ca::core::Result<VType, SmtReadError>;
    switch (e.tag) {
    case SmtVType::kTop: return R(ca::core::Ok(VType{VKind::Top, 0, {}}));
    case SmtVType::kInteger: return R(ca::core::Ok(VType{VKind::Int, 0, {}}));
    case SmtVType::kFloat: return R(ca::core::Ok(VType{VKind::Float, 0, {}}));
    case SmtVType::kDouble: return R(ca::core::Ok(VType{VKind::Double, 0, {}}));
    case SmtVType::kLong: return R(ca::core::Ok(VType{VKind::Long, 0, {}}));
    case SmtVType::kNull: return R(ca::core::Ok(VType{VKind::Null, 0, {}}));
    case SmtVType::kUninitializedThis:
        return R(ca::core::Ok(VType{VKind::UninitThis, 0, {}}));
    case SmtVType::kObject:
        if (!cp.valid(e.cp) || cp.tagOf(e.cp) != core::CpTag::Class ||
            !cp.isUtf8(cp.at(e.cp).a)) {
            return R(ca::core::Err(SmtReadError::BadCpEntry));
        }
        return R(ca::core::Ok(VType::ref(cp.utf8Of(cp.at(e.cp).a))));
    case SmtVType::kUninitialized:
        if (newPcs.count(e.cp) == 0) {
            return R(ca::core::Err(SmtReadError::UninitNotOnNew));
        }
        return R(ca::core::Ok(VType::uninitNew(e.cp)));
    default:
        return R(ca::core::Err(SmtReadError::BadFramePayload));   // 保留 tag
    }
}

/// 条目序列 → 槽语义（cat2 = 值槽 + Top 垫底槽； 交付口径，与
/// FrameEngine 的 deliverFrames 同构）。
void expandSlots(const std::vector<VType>& entries, std::vector<VType>& slots)
{
    slots.clear();
    slots.reserve(entries.size() * 2);
    for (const VType& v : entries) {
        slots.push_back(v);
        if (v.k == VKind::Long || v.k == VKind::Double) {
            slots.push_back(VType{VKind::Top, 0, {}});
        }
    }
}

}   // namespace

const char* to_string(SmtReadError e) noexcept
{
    switch (e) {
    case SmtReadError::NoCode: return "method has no code attribute";
    case SmtReadError::BadCpEntry: return "object entry cp not a readable class";
    case SmtReadError::FrameNotAligned: return "frame pc not on an instruction";
    case SmtReadError::LocalsUnderflow: return "chop exceeds chained locals";
    case SmtReadError::LocalsOverflow: return "locals exceed declared maxLocals";
    case SmtReadError::StackOverflow: return "stack exceeds declared maxStack";
    case SmtReadError::UninitNotOnNew: return "uninitialized payload not a new pc";
    case SmtReadError::BadFramePayload: return "frame tag contradicts payload";
    }
    return "?";
}

Result<std::vector<SmtFrameView>, SmtReadError>
readFramesFromSmt(ir::ClassSession& s, const ir::CodeUnit& u)
{
    using R = Result<std::vector<SmtFrameView>, SmtReadError>;
    using ca::core::Err;
    using ca::core::Ok;

    core::ClassFile& cf = s.backing();
    const ir::ClassUnit& unit = s.unit();
    const ir::MethodUnit* mu = unit.method(u.method);
    if (mu == nullptr || mu->backingIndex >= cf.methods.size()) {
        return R(Err(SmtReadError::NoCode));
    }
    core::CodeAttr* code = findCodeAttr(cf, cf.methods[mu->backingIndex]);
    if (code == nullptr) {
        return R(Err(SmtReadError::NoCode));
    }
    const StackMapTableAttr* smt = findSmtAttr(cf, *code);
    if (smt == nullptr) {
        return R(Ok(std::vector<SmtFrameView>{}));   // 属性缺席（合法）：空视图
    }

    // 链头 = 隐式初始帧：this 形态 + 描述符参数，条目口径——链
    // 算术（chop/append）在条目空间进行，不按 maxLocals 预垫 Top（预垫会把
    // append 载荷推到垫顶之后，错位全部槽号）。
    MethodSig sig;
    sig.isStatic  = (mu->access & 0x0008) != 0;
    sig.desc      = unit.symbols().str(mu->desc);
    sig.name      = unit.symbols().str(mu->name);
    sig.thisClass = unit.symbols().str(unit.thisClass);
    std::vector<VType> entries = initialLocalsOf(sig);

    // tag-8 载荷校验基准 + pc → order 下标反查表（各建一次）。
    std::set<ca::u32> newPcs;
    std::unordered_map<ca::u32, ca::usize> idxOfPc;
    const auto& order = u.dom.order();
    for (ca::usize i = 0; i < order.size(); ++i) {
        const ir::Insn* in = u.dom.peek(order[i]);
        if (in == nullptr) {
            continue;
        }
        if (in->op == core::Op::NEW) {
            newPcs.insert(in->srcPc);
        }
        idxOfPc.emplace(in->srcPc, i);
    }

    std::vector<SmtFrameView> views;
    views.reserve(smt->frames.size());
    std::vector<VType> stackEntries;
    ca::u32 prevAbsPc = 0;
    for (ca::usize i = 0; i < smt->frames.size(); ++i) {
        const StackMapTableAttr::Frame& f = smt->frames[i];
        const ca::u8 t = f.type;

        // 形态标签 → 链算术（条目空间）。载荷矛盾显式拒绝：手拼属性不经
        // codec 校验也能到达这里，读侧不猜。
        using FR = StackMapTableAttr::Frame;
        stackEntries.clear();
        if (t <= FR::kSameMax || t == FR::kSameExt) {
            if (!f.stack.empty() || !f.locals.empty()) {
                return R(Err(SmtReadError::BadFramePayload));
            }
        } else if ((t >= FR::kSame1Base && t <= FR::kSame1Max) || t == FR::kSame1Ext) {
            if (f.stack.size() != 1 || !f.locals.empty()) {
                return R(Err(SmtReadError::BadFramePayload));
            }
            auto r = toSemantic(f.stack[0], cf.cp, newPcs);
            if (r.is_err()) {
                return R(Err(r.unwrap_err()));
            }
            stackEntries.push_back(std::move(r).unwrap());
        } else if (t >= FR::kChopMax && t <= FR::kChopMin) {
            if (!f.locals.empty() || !f.stack.empty()) {
                return R(Err(SmtReadError::BadFramePayload));
            }
            const ca::usize k = static_cast<ca::usize>(251 - t);
            if (entries.size() < k) {
                return R(Err(SmtReadError::LocalsUnderflow));
            }
            entries.resize(entries.size() - k);
        } else if (t >= FR::kAppendMin && t <= FR::kAppendMax) {
            if (!f.stack.empty()
                || f.locals.size() != static_cast<ca::usize>(t - FR::kAppendMin + 1)) {
                return R(Err(SmtReadError::BadFramePayload));
            }
            for (const SmtVType& e : f.locals) {
                auto r = toSemantic(e, cf.cp, newPcs);
                if (r.is_err()) {
                    return R(Err(r.unwrap_err()));
                }
                entries.push_back(std::move(r).unwrap());
            }
        } else if (t == FR::kFull) {
            entries.clear();
            for (const SmtVType& e : f.locals) {
                auto r = toSemantic(e, cf.cp, newPcs);
                if (r.is_err()) {
                    return R(Err(r.unwrap_err()));
                }
                entries.push_back(std::move(r).unwrap());
            }
            for (const SmtVType& e : f.stack) {
                auto r = toSemantic(e, cf.cp, newPcs);
                if (r.is_err()) {
                    return R(Err(r.unwrap_err()));
                }
                stackEntries.push_back(std::move(r).unwrap());
            }
        } else {
            return R(Err(SmtReadError::BadFramePayload));   // 128..246 保留标签
        }

        // 绝对 pc：首帧 delta 即 pc，其后链式；真实 SMT 的目标严格递增，
        // 回绕/回落说明链已坏（防 u32 回绕别名到小 pc 的假命中）。
        const ca::u32 pc = i == 0 ? f.offset : core::smtAbsPc(prevAbsPc, f.offset);
        if (i != 0 && pc <= prevAbsPc) {
            return R(Err(SmtReadError::FrameNotAligned));
        }
        const auto hit = idxOfPc.find(pc);
        if (hit == idxOfPc.end()) {
            return R(Err(SmtReadError::FrameNotAligned));
        }

        // 槽语义交付 + maxes 边界防御（超出声明上限的帧 verifier 必拒）。
        SmtFrameView view;
        view.pc               = pc;
        view.at               = order[hit->second];
        view.frame.reachable  = true;
        expandSlots(entries, view.frame.locals);
        expandSlots(stackEntries, view.frame.stack);
        if (view.frame.locals.size() > static_cast<ca::usize>(u.maxLocals)) {
            return R(Err(SmtReadError::LocalsOverflow));
        }
        if (view.frame.stack.size() > static_cast<ca::usize>(u.maxStack)) {
            return R(Err(SmtReadError::StackOverflow));
        }

        prevAbsPc = pc;
        views.push_back(std::move(view));
    }
    return R(Ok(std::move(views)));
}

}   // namespace jbc::analysis
