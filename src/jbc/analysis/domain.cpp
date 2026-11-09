/// @file        domain.cpp
/// @brief       StrictDomain 的类型格实现（头内只留契约与分派概要）。
///
/// ShapeDomain 的 merge 单源在 frame.cpp（同语义转发），ResolverDomain 全
/// 一行直传留在头内；本文件承载 StrictDomain 有实体的方法与私有助手。
#include "jbc/analysis/domain.hpp"

#include <string>
#include <unordered_set>
#include <utility>

namespace jbc::analysis {

// ---- 合并（Strict verifier 类型格） ----

VType StrictDomain::merge(const VType& a, const VType& b)
{
    if (sameAs(a, b)) {
        return a;   // 同 offset Uninitialized / 同名 Ref：保持
    }
    if (a.k == VKind::Top || b.k == VKind::Top) {
        // Strict verifier 类型格：top = 不可用值，汇合支配（JVMS 4.10.1.4
        // 帧匹配口径——单边未赋值槽的合并不可指派到具体类型）。与
        // BestEffort 形状格的吸收语义（ShapeDomain）格分离。
        return VType{};
    }
    if (a.k == VKind::ReturnAddress || b.k == VKind::ReturnAddress) {
        // returnAddress 只与自身合并（JVMS §4.10.2.5 合并族；同值
        // 已被上方 sameAs 早退吸收）。异值互并 = 不同 jsr 调用点共享同一
        // 子程序入口——精确语义需逐调用点状态快照（JVMS §4.10.2.5 的
        // touched-bit-vector 机制），本域显式拒绝（fail loud，不伪造帧）。
        rejectSubroutine();
        return VType{VKind::Bottom, 0, {}};
    }
    if (a.k == VKind::Null && b.k == VKind::Ref) {
        return b;   // null 是万能引用底
    }
    if (b.k == VKind::Null && a.k == VKind::Ref) {
        return a;
    }
    if (a.k == VKind::Uninitialized || b.k == VKind::Uninitialized ||
        a.k == VKind::UninitThis || b.k == VKind::UninitThis) {
        reject();   // 异 offset Uninit / UninitThis 与其他并：verifier 冲突
        return VType{VKind::Bottom, 0, {}};
    }
    if (a.k == VKind::Ref && b.k == VKind::Ref) {
        auto r = r_.merge(a, b);
        if (r.is_err()) {
            reject();
            return VType{VKind::Bottom, 0, {}};
        }
        return std::move(r).unwrap();
    }
    reject();   // 尺寸类冲突 / Null × 原始类型
    return VType{VKind::Bottom, 0, {}};
}

// ---- 类型精确注入（transfer 的值构造面） ----

VType StrictDomain::newValue(const ir::Insn& in, ir::InsnId, VKind shape)
{
    using core::Op;
    switch (in.op) {
    case Op::NEW: return VType::uninitNew(in.srcPc);
    case Op::CHECKCAST: return nameRef(detail::classNameOf(cp_, in.cp));
    case Op::GETFIELD:
    case Op::GETSTATIC: return descValue(fieldDescOf(in), in.cp);
    case Op::INVOKEVIRTUAL:
    case Op::INVOKESPECIAL:
    case Op::INVOKESTATIC:
    case Op::INVOKEINTERFACE:
    case Op::INVOKEDYNAMIC: {
        bool ok = false;
        const std::string_view d = detail::memberDesc(cp_, in.cp, ok);
        if (!ok) {
            return VType{shape, 0, {}};
        }
        const ca::usize close = d.find(')');
        if (close == std::string_view::npos) {
            return VType{shape, 0, {}};
        }
        return descValue(detail::oneType(d, close + 1), in.cp);
    }
    case Op::ANEWARRAY: return arrayRef(detail::classNameOf(cp_, in.cp));
    case Op::NEWARRAY: return primArrayName(in.a);
    case Op::MULTIANEWARRAY: return nameRef(detail::classNameOf(cp_, in.cp));
    case Op::LDC:
    case Op::LDC_W: {
        if (!cp_.valid(in.cp)) {
            return VType{VKind::Ref, 0, {}};
        }
        switch (cp_.tagOf(in.cp)) {
        case core::CpTag::String: return nameRef("java/lang/String");
        // ldc 类常量装载的是 Class 对象（验证器类型 java/lang/Class），
        // 不是常量命名的类类型本身——后者沿 astore 流进异常 handler 的
        // 合并帧会让产物 StackMapTable 与 JVM 验证器实算帧冲突。
        case core::CpTag::Class: return nameRef("java/lang/Class");
        case core::CpTag::MethodType: return nameRef("java/lang/invoke/MethodType");
        case core::CpTag::MethodHandle: return nameRef("java/lang/invoke/MethodHandle");
        case core::CpTag::Dynamic: {
            // condy 的类型即其 NameAndType 描述符，可精确注入
            const auto& e = cp_.at(in.cp);
            bool ok = false;
            const std::string_view d = natDesc(e.b, ok);
            return ok ? descValue(detail::oneType(d, 0), in.cp)
                      : VType{VKind::Ref, 0, {}};
        }
        default: return VType{shape, 0, {}};
        }
    }
    default: return VType{shape, 0, {}};
    }
}

// ---- 二元效应与翻转 ----

VType StrictDomain::binary(core::Op op, const VType& a, const VType&, VKind shape,
                           const ir::Insn& at, ir::InsnId id)
{
    // aaload 的精确元素类型来自数组操作数描述符（"[L…;"/"[I"）。
    if (op == core::Op::AALOAD && a.k == VKind::Ref && a.name.size() >= 2 &&
        a.name.front() == '[') {
        const std::string_view comp = a.name.substr(1);
        return comp.front() == 'L' ? nameRef(detail::refNameOfType(comp))
                                   : descValue(comp, at.cp);
    }
    return newValue(at, id, shape);
}

VType StrictDomain::initializedForm(const VType& receiver, const ir::Insn& call, ir::InsnId)
{
    if (receiver.k == VKind::UninitThis) {
        return nameRef(thisClass_);
    }
    if (receiver.k == VKind::Uninitialized) {
        bool ok = false;
        return nameRef(detail::refOwnerOf(cp_, call.cp, ok));
    }
    return receiver;
}

// ---- 私有助手 ----

bool StrictDomain::sameAs(const VType& a, const VType& b) noexcept
{
    return a.k == b.k && a.cls == b.cls && a.name == b.name;
}

VType StrictDomain::descValue(std::string_view t, ca::u32) const
{
    const VKind k = detail::kindOfType(t);
    if (k == VKind::Ref) {
        return nameRef(detail::refNameOfType(t));
    }
    return VType{k, 0, {}};
}

std::string_view StrictDomain::fieldDescOf(const ir::Insn& in) const
{
    bool ok = false;
    return detail::memberDesc(cp_, in.cp, ok);
}

VType StrictDomain::arrayRef(std::string_view element)
{
    if (element.empty()) {
        return VType{VKind::Ref, 0, {}};
    }
    std::string s("[");
    if (element.front() == '[') {
        s += element;
    } else {
        s += 'L';
        s += element;
        s += ';';
    }
    return VType::ref(intern(std::move(s)));
}

VType StrictDomain::primArrayName(ca::i32 atype)
{
    static constexpr char kLetters[12] = {'\0', '?', '?', '?', 'Z', 'C',
                                          'F',  'D', 'B', 'S', 'I', 'J'};
    if (atype < 4 || atype > 11) {
        return VType{VKind::Ref, 0, {}};
    }
    std::string s(1, '[');
    s += kLetters[atype];
    return VType::ref(intern(std::move(s)));
}

std::string_view StrictDomain::intern(std::string s)
{
    return *interned_.insert(std::move(s)).first;
}

std::string_view StrictDomain::natDesc(ca::u32 natIdx, bool& ok) const
{
    ok = false;
    if (!cp_.valid(natIdx)) {
        return {};
    }
    const auto& nat = cp_.at(natIdx);
    if (nat.tag != core::CpTag::NameAndType || !cp_.isUtf8(nat.b)) {
        return {};
    }
    ok = true;
    return cp_.utf8Of(nat.b);
}

}   // namespace jbc::analysis
