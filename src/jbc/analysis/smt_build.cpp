/// @file        smt_build.cpp
/// @brief       StackMapTable 生成器：目标集合 → 全帧链 → 紧凑形态编码。
#include "jbc/analysis/smt_build.hpp"

#include <algorithm>
#include <set>
#include <unordered_map>

#include "jbc/core/opcode.hpp"

namespace jbc::analysis {

namespace {

using core::StackMapTableAttr;
using Entries   = std::vector<StackMapTableAttr::VType>;
using SmtVType  = StackMapTableAttr::VType;
using SmtFrame  = StackMapTableAttr::Frame;

/// @brief 单槽 → verification_type 条目。Ref 的精确类名经 session 的
///        internClass 落位（既有同类条目复用，miss 才追加）；无精确名的
///        引用说明输入帧不是播种完整的 Strict 结果——防御拒绝，不编造
///        Object(0) 之类的坏条目。
ca::core::Result<SmtVType, SmtBuildError> toVerif(ir::ClassSession& s, const VType& v)
{
    using R = ca::core::Result<SmtVType, SmtBuildError>;
    switch (v.k) {
    case VKind::Top: return R(ca::core::Ok(SmtVType{SmtVType::kTop, 0}));
    case VKind::Int: return R(ca::core::Ok(SmtVType{SmtVType::kInteger, 0}));
    case VKind::Float: return R(ca::core::Ok(SmtVType{SmtVType::kFloat, 0}));
    case VKind::Double: return R(ca::core::Ok(SmtVType{SmtVType::kDouble, 0}));
    case VKind::Long: return R(ca::core::Ok(SmtVType{SmtVType::kLong, 0}));
    case VKind::Null: return R(ca::core::Ok(SmtVType{SmtVType::kNull, 0}));
    case VKind::UninitThis: return R(ca::core::Ok(SmtVType{SmtVType::kUninitializedThis, 0}));
    case VKind::Uninitialized:
        return R(ca::core::Ok(SmtVType{SmtVType::kUninitialized, v.uninitOffset()}));
    case VKind::ReturnAddress:
        // C2 裁决：full_frame 的 verification_type_info 无 returnAddress 形态
        //（JVMS  封闭 tag 集）——SMT 生成对子程序方法显式拒绝，不静默
        // 改码为 Top/Int 伪造帧（v≤49 无 SMT 走旧校验是规范许可行为）。
        return R(ca::core::Err(SmtBuildError::Subroutine));
    case VKind::Ref:
        if (v.name.empty()) {
            return R(ca::core::Err(SmtBuildError::BottomInFrame));
        }
        return R(ca::core::Ok(SmtVType{SmtVType::kObject, s.internClass(v.name)}));
    default:
        return R(ca::core::Err(SmtBuildError::BottomInFrame));   // Bottom 残留
    }
}

/// @brief Uninitialized 载荷必须落在方法内某个 new 指令 pc 上（
///        帧域携带的是 srcPc 口径，这里守住「指向 new」这一条不变量）。
ca::core::Result<void, SmtBuildError>
checkUninitPayloads(const Entries& es, const std::set<ca::u32>& newPcs)
{
    for (const SmtVType& v : es) {
        if (v.tag == SmtVType::kUninitialized && newPcs.count(v.cp) == 0) {
            return ca::core::Err(SmtBuildError::TargetNotInsn);
        }
    }
    return ca::core::Ok();
}

/// @brief srcPc → order 下标（目标帧对齐用；pc 唯一，建一次反查表）。
std::unordered_map<ca::u32, ca::usize> pcIndex(const ir::CodeUnit& u)
{
    std::unordered_map<ca::u32, ca::usize> m;
    m.reserve(u.dom.order().size() * 2);
    for (ca::usize i = 0; i < u.dom.order().size(); ++i) {
        const ir::Insn* in = u.dom.peek(u.dom.order()[i]);
        if (in != nullptr) {
            m.emplace(in->srcPc, i);
        }
    }
    return m;
}

}   // namespace

namespace smt_detail {

ca::core::Result<std::vector<core::StackMapTableAttr::VType>, SmtBuildError>
smtEntriesOf(ir::ClassSession& s, const std::vector<VType>& slots)
{
    using R = ca::core::Result<std::vector<core::StackMapTableAttr::VType>, SmtBuildError>;
    Entries out;
    out.reserve(slots.size());
    for (ca::usize i = 0; i < slots.size(); ++i) {
        const VType& v = slots[i];
        if (v.k == VKind::Long || v.k == VKind::Double) {
            // cat2 展开规则：值槽 + 垫底槽合并为单条目；垫底槽缺失或被
            // 占用 = 帧不自洽（槽位不变量破坏），拒绝而非猜测。
            if (i + 1 >= slots.size() || slots[i + 1].k != VKind::Top) {
                return R(ca::core::Err(SmtBuildError::BottomInFrame));
            }
            ++i;
        }
        auto r = toVerif(s, v);
        if (r.is_err()) {
            return R(r.unwrap_err());
        }
        out.push_back(std::move(r).unwrap());
    }
    return R(ca::core::Ok(std::move(out)));
}

core::StackMapTableAttr::Frame
chooseSmtFrame(ca::u32 delta,
               const std::vector<core::StackMapTableAttr::VType>& prevLocals,
               const std::vector<core::StackMapTableAttr::VType>& locals,
               const std::vector<core::StackMapTableAttr::VType>& stack)
{
    SmtFrame f;
    f.offset = delta;

    if (locals == prevLocals) {
        if (stack.empty()) {
            f.type = delta <= SmtFrame::kSameMax ? static_cast<ca::u8>(delta)
                                                 : SmtFrame::kSameExt;
            return f;
        }
        if (stack.size() == 1) {
            f.type = delta <= SmtFrame::kSameMax
                         ? static_cast<ca::u8>(SmtFrame::kSame1Base + delta)
                         : SmtFrame::kSame1Ext;
            f.stack = stack;
            return f;
        }
    } else if (stack.empty()) {
        // chop：新 locals 是旧的去掉末尾 k（1..3）条。
        if (locals.size() < prevLocals.size() &&
            std::equal(locals.begin(), locals.end(), prevLocals.begin())) {
            const ca::usize k = prevLocals.size() - locals.size();
            if (k <= 3) {
                f.type = static_cast<ca::u8>(251 - k);
                return f;
            }
        }
        // append：新 locals 是旧的末尾追加 k（1..3）条。
        if (locals.size() > prevLocals.size() &&
            std::equal(prevLocals.begin(), prevLocals.end(), locals.begin())) {
            const ca::usize k = locals.size() - prevLocals.size();
            if (k <= 3) {
                f.type = static_cast<ca::u8>(251 + k);
                f.locals.assign(locals.end() - static_cast<std::ptrdiff_t>(k), locals.end());
                return f;
            }
        }
    }

    f.type     = SmtFrame::kFull;
    f.locals   = locals;
    f.stack    = stack;
    return f;
}

}   // namespace smt_detail

const char* to_string(SmtBuildError e) noexcept
{
    switch (e) {
    case SmtBuildError::ClassTooOld: return "classfile version < 50 (SMT undefined)";
    case SmtBuildError::FramesNotOk: return "frames are not an ok strict compute result";
    case SmtBuildError::BottomInFrame: return "conflict/unnamed slot in frame";
    case SmtBuildError::TargetNotInsn: return "uninitialized payload not a new pc";
    case SmtBuildError::TooBig: return "frame count or delta exceeds u2";
    case SmtBuildError::Subroutine: return "frame slot holds returnAddress (subroutine not encodable)";
    }
    return "?";
}

Result<SmtBuildResult, SmtBuildError>
buildStackMapTable(ir::ClassSession& s, const ir::CodeUnit& u,
                   const std::vector<FrameAt>& frames)
{
    using R = Result<SmtBuildResult, SmtBuildError>;
    using ca::core::Err;
    using ca::core::Ok;

    // 版本门：SMT 属性自 classfile 50 才有定义；显式 Err，不静默。
    if (s.backing().major < 50) {
        return R(Err(SmtBuildError::ClassTooOld));
    }
    const auto& order = u.dom.order();
    if (order.empty()) {
        return R(Ok(SmtBuildResult{}));   // 无指令：无目标、无属性
    }
    if (frames.size() != order.size()) {
        return R(Err(SmtBuildError::FramesNotOk));
    }

    // 目标集合：分支/switch 全部目标 + 异常 handler 入口。不可达
    // 目标随后按帧的 reachable 过滤——verifier 跳过不可达代码，不要求帧。
    std::set<ca::u32> targetPcs;
    const auto addTarget = [&](ir::LabelId label) {
        const auto t = u.dom.resolveLabel(label);
        if (t.is_err() || t.unwrap().isEnd()) {
            return;
        }
        const ir::Insn* in = u.dom.peek(t.unwrap().insn);
        if (in != nullptr) {
            targetPcs.insert(in->srcPc);
        }
    };
    for (const ir::InsnId id : order) {
        const ir::Insn* in = u.dom.peek(id);
        if (in == nullptr) {
            continue;
        }
        const core::Shape sh = core::shapeOf(in->op);
        if (sh == core::Shape::Branch2 || sh == core::Shape::Branch4 ||
            sh == core::Shape::TableSwitch || sh == core::Shape::LookupSwitch) {
            addTarget(in->target);
            for (const ir::LabelId t : in->targets) {
                addTarget(t);
            }
        }
    }
    for (const ir::CodeUnit::Exc& e : u.exc) {
        addTarget(e.handler);
    }
    if (targetPcs.empty()) {
        return R(Ok(SmtBuildResult{}));   // 无控制流：不产属性（空表也不写）
    }

    // Uninitialized 载荷校验基準：方法内全部 new 指令 pc。
    std::set<ca::u32> newPcs;
    for (const ir::InsnId id : order) {
        const ir::Insn* in = u.dom.peek(id);
        if (in != nullptr && in->op == core::Op::NEW) {
            newPcs.insert(in->srcPc);
        }
    }

    // 链的起点 = 隐式初始帧：首指令到达帧的 locals（与引擎 MethodSig 播种
    // 同源——构造器 this 槽是 UninitThis，参数携带描述符精确类名）。
    const std::unordered_map<ca::u32, ca::usize> idxOf = pcIndex(u);
    auto initR = smt_detail::smtEntriesOf(s, frames[0].locals);
    if (initR.is_err()) {
        return R(Err(initR.unwrap_err()));
    }
    Entries prev = std::move(initR).unwrap();
    auto cu = checkUninitPayloads(prev, newPcs);
    if (cu.is_err()) {
        return R(Err(cu.unwrap_err()));
    }

    auto attr  = std::make_unique<StackMapTableAttr>();
    ca::u32 prevAbsPc = 0;
    ca::usize emitted  = 0;
    for (const ca::u32 pc : targetPcs) {   // set 迭代即 pc 升序
        const auto it = idxOf.find(pc);
        if (it == idxOf.end()) {
            return R(Err(SmtBuildError::FramesNotOk));   // 目标与帧不对齐
        }
        const FrameAt& fr = frames[it->second];
        if (!fr.reachable) {
            continue;   // 不可达目标不产帧
        }
        auto locR = smt_detail::smtEntriesOf(s, fr.locals);
        if (locR.is_err()) {
            return R(Err(locR.unwrap_err()));
        }
        auto stkR = smt_detail::smtEntriesOf(s, fr.stack);
        if (stkR.is_err()) {
            return R(Err(stkR.unwrap_err()));
        }
        Entries locals = std::move(locR).unwrap();
        Entries stack  = std::move(stkR).unwrap();
        cu = checkUninitPayloads(locals, newPcs);
        if (cu.is_err()) {
            return R(Err(cu.unwrap_err()));
        }
        cu = checkUninitPayloads(stack, newPcs);
        if (cu.is_err()) {
            return R(Err(cu.unwrap_err()));
        }

        // offset_delta：首帧 prev = -1（delta 即 pc），此后按链式语义。
        const ca::u32 delta = emitted == 0 ? pc : core::smtDelta(prevAbsPc, pc);
        if (delta > 0xFFFFu) {
            return R(Err(SmtBuildError::TooBig));
        }
        attr->frames.push_back(
            smt_detail::chooseSmtFrame(delta, prev, locals, stack));
        ++emitted;
        prev      = std::move(locals);
        prevAbsPc = pc;
    }

    if (emitted == 0) {
        return R(Ok(SmtBuildResult{}));   // 全部目标不可达（死代码）：无属性
    }
    if (emitted > 0xFFFFu) {
        return R(Err(SmtBuildError::TooBig));
    }

    SmtBuildResult out;
    out.needed   = true;
    out.frameCount = static_cast<ca::u32>(attr->frames.size());
    out.attr     = std::move(attr);
    return R(Ok(std::move(out)));
}

}   // namespace jbc::analysis
