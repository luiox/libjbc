/// @file        frame.cpp
/// @brief       帧引擎：DomainEngine<VType> 的壳 + 描述符工具 + 形状合并策略。
///
/// v0.3.8 起 worklist/指令效应/合并/异常播种全部在 domain.hpp 的模板引擎；
/// 本文件只留：公开描述符工具（包装 detail 扫描）、ShapeResolver::merge
/// （公共 API，ShapeDomain 转发到此——单一实现）、FrameEngine 构造与
/// DomainResult → FrameAt 转换。
#include "jbc/analysis/frame.hpp"

#include <memory>

#include "jbc/analysis/domain.hpp"

namespace jbc::analysis {

ca::i32 methodArgSlots(std::string_view desc)
{
    if (desc.empty() || desc[0] != '(') {
        return -1;
    }
    ca::usize pos = 1;
    ca::i32 total = 0;
    while (pos < desc.size() && desc[pos] != ')') {
        const std::string_view t = detail::oneType(desc, pos);
        const ca::i32 n = detail::typeSlots(t);
        if (n < 0 || t.empty()) {
            return -1;
        }
        total += n;
        pos += t.size();
    }
    return total;
}

ca::i32 methodRetSlots(std::string_view desc)
{
    const ca::usize close = desc.find(')');
    if (close == std::string_view::npos) {
        return -1;
    }
    return detail::typeSlots(detail::oneType(desc, close + 1));
}

ca::i32 fieldSlots(std::string_view desc)
{
    return detail::typeSlots(detail::oneType(desc, 0));
}

std::vector<VType> initialLocalsOf(const MethodSig& sig)
{
    std::vector<VType> out;
    if (!sig.isStatic) {
        out.push_back(sig.name == "<init>" && !sig.thisClass.empty()
                          ? VType{VKind::UninitThis, 0, {}}
                          : VType::ref(sig.thisClass));
    }
    const std::string_view d = sig.desc;
    if (!d.empty() && d[0] == '(') {
        ca::usize pos = 1;
        while (pos < d.size() && d[pos] != ')') {
            const std::string_view t = detail::oneType(d, pos);
            if (t.empty()) {
                break;
            }
            const VKind k = detail::kindOfType(t);
            out.push_back(k == VKind::Ref ? VType::ref(detail::refNameOfType(t))
                                          : VType{k, 0, {}});
            pos += t.size();
        }
    }
    return out;
}

bool applyComputedMaxes(ir::CodeUnit& u, const FrameSummary& computed,
                        const MethodSig& sig) noexcept
{
    if (!computed.ok) {
        return false;   // 失败计算无真值，不采用
    }

    // verifier 地板：参数槽（this + 描述符参数；描述符不可解析时地板退化
    // 为 0，采用值即观测值）+ 异常 handler 入口栈。
    ca::u32 maxLocals = computed.maxLocals;
    const ca::i32 args   = methodArgSlots(sig.desc);
    const ca::u32 floor_ = (args < 0 ? 0u : static_cast<ca::u32>(args))
                         + (sig.isStatic ? 0u : 1u);
    if (floor_ > maxLocals) {
        maxLocals = floor_;
    }
    ca::u32 maxStack = computed.maxStack;
    if (!u.exc.empty() && maxStack < 1) {
        maxStack = 1;
    }

    if (maxStack > 0xFFFFu || maxLocals > 0xFFFFu) {
        return false;   // Code 属性 maxes 是 u16：域外值截断即伪造，显式不采用
    }
    u.maxStack  = static_cast<ca::u16>(maxStack);
    u.maxLocals = static_cast<ca::u16>(maxLocals);
    return true;
}

ca::core::Result<VType, TypeMergeError> ShapeResolver::merge(VType a, VType b)
{
    if (a.k == b.k) {
        if (a.k == VKind::Ref && a.cls != b.cls) {
            return ca::core::Ok(VType{VKind::Ref, 0, {}});   // 具体类不同：退化到通用引用
        }
        return ca::core::Ok(a);
    }
    if (a.k == VKind::Top) {
        return ca::core::Ok(b);
    }
    if (b.k == VKind::Top) {
        return ca::core::Ok(a);
    }
    if (a.k == VKind::Bottom || b.k == VKind::Bottom) {
        return ca::core::Ok(VType{VKind::Bottom, 0, {}});   // 冲突吸收：单调向下，保证收敛
    }
    const bool aRef = a.k == VKind::Ref || a.k == VKind::Null || a.k == VKind::UninitThis;
    const bool bRef = b.k == VKind::Ref || b.k == VKind::Null || b.k == VKind::UninitThis;
    if (aRef && bRef) {
        return ca::core::Ok(VType{VKind::Ref, 0, {}});
    }
    return ca::core::Ok(VType{VKind::Bottom, 0, {}});   // 尺寸类冲突
}

VType ShapeDomain::merge(const VType& a, const VType& b)
{
    static ShapeResolver r;   // 无状态策略；静态实例避免每次合并构造
    return std::move(r.merge(a, b)).unwrap();   // ShapeResolver 永不失败
}

const char* to_string(FrameSummary::Fail f) noexcept
{
    switch (f) {
    case FrameSummary::Fail::None: return "none";
    case FrameSummary::Fail::Underflow: return "stack underflow";
    case FrameSummary::Fail::BadLocal: return "bad local index";
    case FrameSummary::Fail::BadCpRef: return "bad cp ref";
    case FrameSummary::Fail::Wide: return "wide";
    case FrameSummary::Fail::NoFixpoint: return "no fixpoint";
    case FrameSummary::Fail::Subroutine: return "subroutine (jsr/ret) rejected";
    case FrameSummary::Fail::TypeConflict: return "type conflict";
    case FrameSummary::Fail::NoResolver: return "strict without type resolver";
    case FrameSummary::Fail::NotSupported: return "policy not supported";
    }
    return "?";
}

namespace {

/// @brief 按档位与 resolver 装配实现域；Strict 无 resolver / Preserve 占位
///        一律返回空（compute 据此 fail loud，不悄悄降级）。
std::unique_ptr<IValueDomain<VType>> buildDomain(const core::ConstantPool& cp,
                                                 FramePolicy policy,
                                                 ITypeResolver* resolver)
{
    if (policy == FramePolicy::Strict) {
        return resolver == nullptr ? nullptr
                                   : std::make_unique<StrictDomain>(cp, *resolver);
    }
    if (policy == FramePolicy::Preserve) {
        return nullptr;   // 读属性不是数据流：Preserve 的装配点在 smt_read.hpp
                          // 的独立入口，compute 对本档显式拒绝（§10.5）
    }
    if (resolver != nullptr) {
        return std::make_unique<ResolverDomain>(*resolver);
    }
    return std::make_unique<ShapeDomain>();
}

/// @brief Strict/Preserve 的显式拒绝 summary（ok=false，fail loud）。
FrameSummary policyRefusal(FramePolicy policy)
{
    FrameSummary s;
    s.fail = policy == FramePolicy::Preserve ? FrameSummary::Fail::NotSupported
                                             : FrameSummary::Fail::NoResolver;
    return s;
}

/// @brief DomainResult → FrameAt（值行搬运；cat2 底槽行读作 Top）。
///
/// locals 按槽位序原样搬运（值行在前、bottom 行在后）。引擎栈的内部布局
/// 是 [垫底行, 值行]（值行恒在 back()，Machine::pushVal 先压垫底行），
/// 交付口径与 SMT 条目空间（smt_read 展开 / smtEntriesOf 收缩）及
/// JVMS §4.7.4 同序：值槽在前、Top 垫底槽在后，故栈上 cat2 对在交付时
/// 翻转为 [值, Top]。
void deliverFrames(DomainResult<VType>& r, std::vector<FrameAt>& out)
{
    out.clear();
    if (!r.summary.ok) {
        return;
    }
    const auto row = [](const SlotV<VType>& s) {
        return s.bottom ? VType{VKind::Top, 0, {}} : s.val;
    };
    out.resize(r.frames.size());
    for (ca::usize i = 0; i < r.frames.size(); ++i) {
        out[i].reachable = r.frames[i].reachable;
        out[i].locals.reserve(r.frames[i].locals.size());
        for (const SlotV<VType>& s : r.frames[i].locals) {
            out[i].locals.push_back(row(s));
        }
        const std::vector<SlotV<VType>>& stack = r.frames[i].stack;
        out[i].stack.reserve(stack.size());
        for (ca::usize j = 0; j < stack.size(); ++j) {
            if (stack[j].bottom && j + 1 < stack.size() && !stack[j + 1].bottom) {
                out[i].stack.push_back(stack[j + 1].val);
                out[i].stack.push_back(VType{VKind::Top, 0, {}});
                ++j;
                continue;
            }
            out[i].stack.push_back(row(stack[j]));
        }
    }
}

}   // namespace

FrameEngine::FrameEngine(const core::ConstantPool& cp, ITypeResolver& resolver)
    : cp_(cp)
    , resolver_(&resolver)
    , dom_(buildDomain(cp, FramePolicy::BestEffort, &resolver))
{}

FrameEngine::FrameEngine(const core::ConstantPool& cp)
    : cp_(cp)
    , dom_(buildDomain(cp, FramePolicy::BestEffort, nullptr))
{}

FrameEngine::FrameEngine(const core::ConstantPool& cp, FramePolicy policy)
    : cp_(cp)
    , policy_(policy)
    , dom_(buildDomain(cp, policy, nullptr))
{}

FrameEngine::FrameEngine(const core::ConstantPool& cp, ITypeResolver& resolver,
                         FramePolicy policy)
    : cp_(cp)
    , policy_(policy)
    , resolver_(&resolver)
    , dom_(buildDomain(cp, policy, &resolver))
{}

FrameEngine::~FrameEngine() = default;

FrameSummary FrameEngine::compute(const ir::CodeUnit& u)
{
    if (!dom_) {
        return policyRefusal(policy_);
    }
    DomainEngine<VType> eng(cp_, *dom_);
    DomainResult<VType> r = eng.analyze(u);
    return r.summary;
}

FrameSummary FrameEngine::compute(const ir::CodeUnit& u, std::vector<FrameAt>& out)
{
    if (!dom_) {
        out.clear();
        return policyRefusal(policy_);
    }
    DomainEngine<VType> eng(cp_, *dom_);
    DomainResult<VType> r = eng.analyze(u);
    deliverFrames(r, out);
    return r.summary;
}

FrameSummary FrameEngine::compute(const ir::CodeUnit& u, const MethodSig& sig)
{
    if (!dom_) {
        return policyRefusal(policy_);
    }
    DomainEngine<VType> eng(cp_, *dom_);
    DomainResult<VType> r = eng.analyze(u, sig);
    return r.summary;
}

FrameSummary FrameEngine::compute(const ir::CodeUnit& u, const MethodSig& sig,
                                  std::vector<FrameAt>& out)
{
    if (!dom_) {
        out.clear();
        return policyRefusal(policy_);
    }
    DomainEngine<VType> eng(cp_, *dom_);
    DomainResult<VType> r = eng.analyze(u, sig);
    deliverFrames(r, out);
    return r.summary;
}

}   // namespace jbc::analysis
