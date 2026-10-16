/// @file        operand.cpp
/// @brief       指令 CP 操作数的语义层：读解析 + 去重 intern。
///
/// 没有这一层，消费者必须理解 Methodref→NameAndType→Utf8 的两跳布局才能
/// 读一条 getfield，必须手工拼四个 addXxx 才能插一条 invokestatic——
/// CP 知识泄漏到每个消费者（ergonomics.md F0）。
#include <cstdint>
#include <unordered_map>

#include "jbc/ir/sem_index.hpp"

#include "jbc/ir/units.hpp"
#include "jbc/type/descriptor.hpp"

namespace jbc::ir {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;

namespace {

/// Utf8 条目内容的 string_view（key 用；存储稳定：源字节副本或 CP arena）。
std::string_view cpUtf8View(const core::ConstantPool& cp, ca::u32 idx)
{
    const core::ConstantPool::Entry& e = cp.at(idx);
    return std::string_view(reinterpret_cast<const char*>(e.utf8), e.utf8Len);
}

}   // namespace

// ---- 读路径 ----

bool ClassSession::memberOf(const Insn& in, MemberSem& out) const noexcept
{
    using core::Op;
    // invokevirtual/special/static 允许 Methodref 或 InterfaceMethodref
    // （JVMS 5.4.3.3：方法解析两种都收，混淆器确实会产交叉形态）；
    // invokeinterface 同理。kind 描述语义族而非 tag。
    auto isMethodTag = [](core::CpTag t) {
        return t == core::CpTag::Methodref || t == core::CpTag::InterfaceMethodref;
    };
    bool wantField = false;
    bool wantMethod = false;
    bool wantIndy  = false;
    switch (in.op) {
    case Op::GETSTATIC:
    case Op::PUTSTATIC:
    case Op::GETFIELD:
    case Op::PUTFIELD:
        wantField = true;
        out.kind  = MemberSem::Kind::Field;
        break;
    case Op::INVOKEVIRTUAL:
    case Op::INVOKESPECIAL:
    case Op::INVOKESTATIC:
        wantMethod = true;
        out.kind   = MemberSem::Kind::Method;
        break;
    case Op::INVOKEINTERFACE:
        wantMethod = true;
        out.kind   = MemberSem::Kind::InterfaceMethod;
        break;
    case Op::INVOKEDYNAMIC:
        wantIndy = true;
        out.kind = MemberSem::Kind::InvokeDynamic;
        break;
    default:
        return false;
    }
    const core::ConstantPool& cp = backing_->cp;
    if (!cp.valid(in.cp)) {
        return false;
    }
    const core::CpTag tag = cp.tagOf(in.cp);
    const bool tagOk = wantField   ? tag == core::CpTag::Fieldref
                       : wantMethod ? isMethodTag(tag)
                                    : tag == core::CpTag::InvokeDynamic;
    if (!tagOk) {
        return false;
    }
    const auto& e = cp.at(in.cp);
    if (!cp.valid(e.b)) {
        return false;
    }
    const auto& nat = cp.at(e.b);
    if (nat.tag != core::CpTag::NameAndType || !cp.isUtf8(nat.a) || !cp.isUtf8(nat.b)) {
        return false;
    }
    // indy 的 b 直接是 NaT（a 是 BSM）；成员引用的 owner 在 Class 一跳之后。
    if (wantIndy) {
        out.owner = {};
    } else {
        if (!cp.valid(e.a) || cp.tagOf(e.a) != core::CpTag::Class ||
            !cp.isUtf8(cp.at(e.a).a)) {
            return false;
        }
        out.owner = cp.utf8Of(cp.at(e.a).a);
    }
    out.name = cp.utf8Of(nat.a);
    out.desc = cp.utf8Of(nat.b);
    return true;
}

std::string_view ClassSession::classOf(const Insn& in) const noexcept
{
    using core::Op;
    // multianewarray 与 setClass 同族：写侧把它当类操作改写，读侧类名解析
    // 必须对称收口，否则 CP 语义面上留下「指令形态可枚举、语义不可查」的洞。
    const bool isClassOp = in.op == Op::NEW || in.op == Op::ANEWARRAY ||
                           in.op == Op::CHECKCAST || in.op == Op::INSTANCEOF ||
                           in.op == Op::MULTIANEWARRAY;
    const core::ConstantPool& cp = backing_->cp;
    if (!isClassOp || !cp.valid(in.cp) || cp.tagOf(in.cp) != core::CpTag::Class ||
        !cp.isUtf8(cp.at(in.cp).a)) {
        return {};
    }
    return cp.utf8Of(cp.at(in.cp).a);
}

bool ClassSession::constOf(const Insn& in, ConstSem& out) const noexcept
{
    using core::Op;
    if (in.op != Op::LDC && in.op != Op::LDC_W && in.op != Op::LDC2_W) {
        return false;
    }
    const core::ConstantPool& cp = backing_->cp;
    if (!cp.valid(in.cp)) {
        return false;
    }
    const auto& e = cp.at(in.cp);
    switch (e.tag) {
    case core::CpTag::Integer: out.kind = ConstSem::Kind::Int; out.num = e.ival; return true;
    case core::CpTag::Long: out.kind = ConstSem::Kind::Long; out.num = e.lval; return true;
    case core::CpTag::Float:
        out.kind  = ConstSem::Kind::Float;
        out.bits  = static_cast<ca::u32>(e.ival);
        return true;
    case core::CpTag::Double:
        out.kind  = ConstSem::Kind::Double;
        out.bits  = static_cast<ca::u64>(e.lval);
        return true;
    case core::CpTag::String:
        out.kind  = ConstSem::Kind::String;
        out.str   = cp.isUtf8(e.a) ? cp.utf8Of(e.a) : std::string_view{};
        return true;
    case core::CpTag::Class:
        out.kind  = ConstSem::Kind::Class;
        out.str   = cp.isUtf8(e.a) ? cp.utf8Of(e.a) : std::string_view{};
        return true;
    case core::CpTag::MethodType:
        out.kind  = ConstSem::Kind::MethodType;
        out.str   = cp.isUtf8(e.a) ? cp.utf8Of(e.a) : std::string_view{};
        return true;
    case core::CpTag::MethodHandle: out.kind = ConstSem::Kind::MethodHandle; return true;
    case core::CpTag::Dynamic: out.kind = ConstSem::Kind::Dynamic; return true;
    default: return false;
    }
}


// ---- 语义改写 ----

bool ClassSession::setMember(Insn& in, MemberSem::Kind kind, std::string_view owner,
                             std::string_view name, std::string_view desc)
{
    using core::Op;
    // op 族与目标 kind 双向匹配：get/put 只配 Field，invoke 族只配方法族。
    switch (in.op) {
    case Op::GETSTATIC:
    case Op::PUTSTATIC:
    case Op::GETFIELD:
    case Op::PUTFIELD:
        if (kind != MemberSem::Kind::Field) {
            return false;
        }
        in.cp = internFieldRef(owner, name, desc);
        return true;
    case Op::INVOKEVIRTUAL:
    case Op::INVOKESPECIAL:
    case Op::INVOKESTATIC:
    case Op::INVOKEINTERFACE:
        {
            if (kind != MemberSem::Kind::Method && kind != MemberSem::Kind::InterfaceMethod) {
                return false;
            }
            // invokeinterface 的 count 字段必须随描述符重算（design.md §27.4）：
            // count = 参数槽位数 + 1（this 槽，JVMS 6.5 invokeinterface 语义）。
            // 只改 CP 引用不改 count，会在指向参数更多的方法后产出 JVM 可拒绝
            // 的 class。描述符解析失败时不改写（fail-closed），指令保持原样。
            ca::i32 interfaceCount = in.a;
            if (in.op == Op::INVOKEINTERFACE) {
                const Result<jbc::type::MethodDesc, jbc::type::DescError> parsed =
                    jbc::type::parseMethodDescriptor(desc);
                if (parsed.is_err()) {
                    return false;
                }
                interfaceCount =
                    static_cast<ca::i32>(jbc::type::paramSlotCount(parsed.unwrap()) + 1);
            }
            in.cp = kind == MemberSem::Kind::InterfaceMethod
                        ? internInterfaceMethodRef(owner, name, desc)
                        : internMethodRef(owner, name, desc);
            in.a  = interfaceCount;
            return true;
        }
    default:
        return false;   // indy 的改写需要 BSM 语义，v0.3.6 不做
    }
}

bool ClassSession::setClass(Insn& in, std::string_view internalName)
{
    using core::Op;
    const core::Shape sh = core::shapeOf(in.op);
    const bool classShaped =
        in.op == Op::NEW || in.op == Op::ANEWARRAY || in.op == Op::CHECKCAST ||
        in.op == Op::INSTANCEOF || sh == core::Shape::CP2Dims;
    if (!classShaped) {
        return false;
    }
    in.cp = internClass(internalName);
    return true;
}

bool ClassSession::setString(Insn& in, std::string_view s)
{
    using core::Op;
    if (in.op != Op::LDC && in.op != Op::LDC_W && in.op != Op::LDC2_W) {
        return false;
    }
    const ca::u32 idx = internString(s);
    in.cp = idx;
    in.op = idx <= 0xFF ? Op::LDC : Op::LDC_W;   // u1 域宽自动升格
    return true;
}

// ---- 保活孪生 ----

MemberSemRef ClassSession::memberSemRef(const Insn& in) const noexcept
{
    MemberSem m;
    if (memberOf(in, m)) {
        return MemberSemRef(shared_from_this(), m);
    }
    return MemberSemRef{};
}

ConstSemRef ClassSession::constSemRef(const Insn& in) const noexcept
{
    ConstSem c;
    if (constOf(in, c)) {
        return ConstSemRef(shared_from_this(), c);
    }
    return ConstSemRef{};
}

ClassSemRef ClassSession::classSemRef(const Insn& in) const noexcept
{
    return ClassSemRef(shared_from_this(), classOf(in));
}

// ---- 写路径 ----

ClassSession::~ClassSession() = default;

SemIndex& ClassSession::sem()
{
    if (sem_ == nullptr) {
        sem_ = std::make_unique<SemIndex>(backing_->cp);
    }
    return *sem_;
}

ca::u32 ClassSession::internUtf8Sem(std::string_view s)
{
    SemIndex& ix  = sem();
    const auto it = ix.utf8.find(s);
    if (it != ix.utf8.end()) {
        return it->second;
    }
    const ca::u32 idx = backing_->cp.addUtf8(s);
    // addUtf8 的 arena 存储稳定，视图可长期作 key。
    ix.utf8.emplace(cpUtf8View(backing_->cp, idx), idx);
    return idx;
}

ca::u32 ClassSession::internClass(std::string_view internalName)
{
    const ca::u32 nameIdx = internUtf8Sem(internalName);
    SemIndex& ix          = sem();
    const auto it         = ix.byName.find(nameIdx);
    if (it != ix.byName.end()) {
        return it->second;
    }
    const ca::u32 idx = backing_->cp.addClass(nameIdx);
    ix.byName.emplace(nameIdx, idx);
    return idx;
}

ca::u32 ClassSession::internString(std::string_view s)
{
    const ca::u32 utf8Idx = internUtf8Sem(s);
    SemIndex& ix          = sem();
    const auto it         = ix.strConst.find(utf8Idx);
    if (it != ix.strConst.end()) {
        return it->second;
    }
    const ca::u32 idx = backing_->cp.addString(utf8Idx);
    ix.strConst.emplace(utf8Idx, idx);
    return idx;
}

ca::u32 ClassSession::internInteger(ca::i32 v)
{
    SemIndex& ix  = sem();
    const auto it = ix.intConst.find(v);
    if (it != ix.intConst.end()) {
        return it->second;
    }
    const ca::u32 idx = backing_->cp.addInteger(v);
    ix.intConst.emplace(v, idx);
    return idx;
}

// ---- 合成基座补充（attr_build 类构建挂载面；ReadError 到 Index 的语义与
// ---- 上方 interns 同一条 intern 纪律：命中既有池不膨胀，miss 才追加）----

ca::u32 ClassSession::internUtf8(std::string_view s)
{
    return internUtf8Sem(s);
}

ca::u32 ClassSession::internLong(ca::i64 v)
{
    SemIndex& ix  = sem();
    const auto it = ix.longConst.find(v);
    if (it != ix.longConst.end()) {
        return it->second;
    }
    const ca::u32 idx = backing_->cp.addLong(v);
    ix.longConst.emplace(v, idx);
    return idx;
}

ca::u32 ClassSession::internFloatBits(ca::u32 bits)
{
    SemIndex& ix  = sem();
    const auto it = ix.floatConst.find(bits);
    if (it != ix.floatConst.end()) {
        return it->second;
    }
    const ca::u32 idx = backing_->cp.addFloatBits(bits);
    ix.floatConst.emplace(bits, idx);
    return idx;
}

ca::u32 ClassSession::internDoubleBits(ca::u64 bits)
{
    SemIndex& ix  = sem();
    const auto it = ix.doubleConst.find(bits);
    if (it != ix.doubleConst.end()) {
        return it->second;
    }
    const ca::u32 idx = backing_->cp.addDoubleBits(bits);
    ix.doubleConst.emplace(bits, idx);
    return idx;
}

ca::u32 ClassSession::internRef(core::CpTag tag, std::string_view owner,
                                std::string_view name, std::string_view desc)
{
    const ca::u32 classIdx = internClass(owner);
    const ca::u32 nameIdx  = internUtf8Sem(name);
    const ca::u32 descIdx  = internUtf8Sem(desc);
    SemIndex& ix           = sem();

    ca::u32 natIdx;
    const auto nit = ix.nat.find(packNatKey(nameIdx, descIdx));
    if (nit != ix.nat.end()) {
        natIdx = nit->second;
    } else {
        natIdx = backing_->cp.addNameAndType(nameIdx, descIdx);
        ix.nat.emplace(packNatKey(nameIdx, descIdx), natIdx);
    }

    const ca::u64 key = packRefKey(tag, classIdx, natIdx);
    const auto rit    = ix.ref.find(key);
    if (rit != ix.ref.end()) {
        return rit->second;
    }
    const ca::u32 idx = backing_->cp.addRef(tag, classIdx, natIdx);
    ix.ref.emplace(key, idx);
    return idx;
}

ca::u32 ClassSession::internFieldRef(std::string_view owner, std::string_view name,
                                     std::string_view desc)
{
    return internRef(core::CpTag::Fieldref, owner, name, desc);
}

ca::u32 ClassSession::internMethodRef(std::string_view owner, std::string_view name,
                                      std::string_view desc)
{
    return internRef(core::CpTag::Methodref, owner, name, desc);
}

ca::u32 ClassSession::internInterfaceMethodRef(std::string_view owner,
                                               std::string_view name,
                                               std::string_view desc)
{
    return internRef(core::CpTag::InterfaceMethodref, owner, name, desc);
}

}   // namespace jbc::ir
