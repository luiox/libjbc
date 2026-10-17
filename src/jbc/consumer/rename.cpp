#include "jbc/consumer/rename.hpp"

#include <map>
#include <memory>
#include <utility>

#include "jbc/core/attribute.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/type/descriptor.hpp"

namespace jbc::consumer {

using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;

using jbc::core::AttrKind;
using jbc::core::AttrSlot;
using jbc::core::BootstrapMethodsAttr;
using jbc::core::ByteView;
using jbc::core::ClassFile;
using jbc::core::CodeAttr;
using jbc::core::ConstantPool;
using jbc::core::CpTag;
using jbc::core::LocalVariableTableAttr;
using jbc::core::MemberInfo;

// ---------------------------------------------------------------------------
// RenamePlan：计划模型（消费方组装，本模块按只读面应用）
// ---------------------------------------------------------------------------

void RenamePlan::renameClass(const std::string_view oldName, const std::string_view newName)
{
    for (ClassRenameRule& r : classes_) {
        if (r.oldName == oldName) {
            r.newName = std::string(newName);
            return;
        }
    }
    ClassRenameRule r;
    r.oldName = std::string(oldName);
    r.newName = std::string(newName);
    classes_.push_back(std::move(r));
}

void RenamePlan::renameMethod(const std::string_view owner, const std::string_view name,
                              const std::string_view desc, const std::string_view newName,
                              const std::string_view newDesc)
{
    for (MemberRenameRule& r : methods_) {
        // upsert 键 = (owner, name, desc) 三元精确相等；desc 空规则是独立的
        // 键（通配查命语义），与具体 desc 规则并存。
        if (r.owner == owner && r.name == name && r.desc == desc) {
            r.newName = std::string(newName);
            r.newDesc = std::string(newDesc);
            return;
        }
    }
    MemberRenameRule r;
    r.owner   = std::string(owner);
    r.name    = std::string(name);
    r.desc    = std::string(desc);
    r.newName = std::string(newName);
    r.newDesc = std::string(newDesc);
    methods_.push_back(std::move(r));
}

void RenamePlan::renameField(const std::string_view owner, const std::string_view name,
                             const std::string_view desc, const std::string_view newName,
                             const std::string_view newDesc)
{
    for (MemberRenameRule& r : fields_) {
        if (r.owner == owner && r.name == name && r.desc == desc) {
            r.newName = std::string(newName);
            r.newDesc = std::string(newDesc);
            return;
        }
    }
    MemberRenameRule r;
    r.owner   = std::string(owner);
    r.name    = std::string(name);
    r.desc    = std::string(desc);
    r.newName = std::string(newName);
    r.newDesc = std::string(newDesc);
    fields_.push_back(std::move(r));
}

const ClassRenameRule* RenamePlan::classRule(const std::string_view oldName) const
{
    for (const ClassRenameRule& r : classes_) {
        if (r.oldName == oldName) {
            return &r;
        }
    }
    return nullptr;
}

const MemberRenameRule* RenamePlan::memberRule(const std::vector<MemberRenameRule>& rules,
                                               const std::string_view owner,
                                               const std::string_view name,
                                               const std::string_view desc) const
{
    for (const MemberRenameRule& r : rules) {
        if (r.owner != owner || r.name != name) {
            continue;
        }
        // 规则 desc 为空 = 该 owner/name 全部 desc 命中（通配）；先到先得。
        if (r.desc.empty() || r.desc == desc) {
            return &r;
        }
    }
    return nullptr;
}

const MemberRenameRule* RenamePlan::methodRule(const std::string_view owner,
                                               const std::string_view name,
                                               const std::string_view desc) const
{
    return memberRule(methods_, owner, name, desc);
}

const MemberRenameRule* RenamePlan::fieldRule(const std::string_view owner,
                                              const std::string_view name,
                                              const std::string_view desc) const
{
    return memberRule(fields_, owner, name, desc);
}

std::string RenamePlan::mapDescriptor(const std::string_view desc) const
{
    // 类名族的 span 手术：全名命中才动；语法非法/未命中一律原样回
    // （role-guard 否决——与 rename_probe 的 fixDescriptorCp 分档一致）。
    if (classes_.empty()) {
        return std::string(desc);
    }
    auto spans = jbc::type::classNameSpans(desc);
    if (spans.is_err()) {
        return std::string(desc);
    }
    const std::vector<jbc::type::ClassNameSpan> list = std::move(spans).unwrap();
    std::vector<jbc::type::ClassNameSpan> hits;
    for (const jbc::type::ClassNameSpan& h : list) {
        if (classRule(std::string_view(desc).substr(h.offset, h.length)) != nullptr) {
            hits.push_back(h);
        }
    }
    if (hits.empty()) {
        return std::string(desc);
    }
    // 逆序替换：同串多 span 时，靠后的替换不影响靠前 span 的偏移。
    std::string cur{desc};
    for (auto it = hits.rbegin(); it != hits.rend(); ++it) {
        const std::string_view text{desc};
        const ClassRenameRule* r = classRule(text.substr(it->offset, it->length));
        auto rep = jbc::type::replaceClassName(cur, *it, r->newName);
        if (rep.is_err()) {
            return std::string(desc);   // 替换失败 → role-guard 兜底原样
        }
        cur = std::string(std::string_view(std::move(rep).unwrap()));
    }
    return cur;
}

// ---------------------------------------------------------------------------
// applyClassRename：逐类分族应用
// ---------------------------------------------------------------------------

namespace {

using jbc::ir::ClassSession;

/// @brief 错误道具（族分立，直容器搬运）。
RenameError errLower(const jbc::ir::LowerError lower)
{
    RenameError e;
    e.kind  = RenameError::Kind::Lower;
    e.lower = lower;
    return e;
}

RenameError errSig(const jbc::ir::SigAttrError& sig)
{
    RenameError e;
    e.kind = RenameError::Kind::Signature;
    e.sig  = sig;
    return e;
}

RenameError errTa(const jbc::ir::TaAttrError& ta)
{
    RenameError e;
    e.kind = RenameError::Kind::TypeAnnotation;
    e.ta   = ta;
    return e;
}

RenameError errKind(const RenameError::Kind kind)
{
    RenameError e;
    e.kind = kind;
    return e;
}

/// @brief 类名族全串映射：span 命中 → 逆序结构化替换（同 rename_probe 的
///        fixDescriptorCp 分档；与本类名规则集配套——命中查 rule 集而非
///        单对 old/new）。
std::string mapText(const RenamePlan& plan, const std::string_view text)
{
    if (plan.classes().empty()) {
        return std::string(text);
    }
    auto spans = jbc::type::classNameSpans(text);
    if (spans.is_err()) {
        return std::string(text);
    }
    const std::vector<jbc::type::ClassNameSpan> list = std::move(spans).unwrap();
    std::vector<jbc::type::ClassNameSpan> hits;
    for (const jbc::type::ClassNameSpan& h : list) {
        if (plan.classRule(text.substr(h.offset, h.length)) != nullptr) {
            hits.push_back(h);
        }
    }
    if (hits.empty()) {
        return std::string(text);
    }
    std::string cur{text};
    for (auto it = hits.rbegin(); it != hits.rend(); ++it) {
        const ClassRenameRule* r = plan.classRule(text.substr(it->offset, it->length));
        auto rep = jbc::type::replaceClassName(cur, *it, r->newName);
        if (rep.is_err()) {
            return std::string(text);
        }
        cur = std::string(std::string_view(std::move(rep).unwrap()));
    }
    return cur;
}

/// @brief 一条成员规则对当前位置是否是「真的会动」的改写（幂等自赋不算
///        改动——不触发 fail-loud 闸）。
bool ruleActuallyChanges(const MemberRenameRule& rule, const std::string_view name,
                         const std::string_view desc)
{
    const bool nameChanges = !rule.newName.empty() && rule.newName != name;
    const bool descChanges = !rule.newDesc.empty() && rule.newDesc != desc;
    return nameChanges || descChanges;
}

/// @brief MethodHandle 常量（CP MethodHandle）目标是否命中会动的成员规则。
///        kind 1..4 = 字段取放（Fieldref），5..9 = 方法调用。
bool mhTargetTouches(const ClassFile& cf, const RenamePlan& plan, const ca::u32 mhIdx)
{
    const ConstantPool& cp = cf.cp;
    if (!cp.valid(mhIdx) || cp.tagOf(mhIdx) != CpTag::MethodHandle) {
        return false;
    }
    const ConstantPool::Entry& mh = cp.at(mhIdx);
    if (!cp.valid(mh.b)) {
        return false;
    }
    const CpTag rt = cp.tagOf(mh.b);
    if (rt != CpTag::Methodref && rt != CpTag::InterfaceMethodref && rt != CpTag::Fieldref) {
        return false;
    }
    const ConstantPool::Entry& ref = cp.at(mh.b);
    if (!cp.valid(ref.a) || !cp.valid(ref.b)) {
        return false;
    }
    if (cp.tagOf(ref.a) != CpTag::Class || cp.tagOf(ref.b) != CpTag::NameAndType) {
        return false;
    }
    const ConstantPool::Entry& nat = cp.at(ref.b);
    if (!cp.isUtf8(nat.a) || !cp.isUtf8(nat.b)) {
        return false;
    }
    const std::string_view owner = cf.className(ref.a);
    const std::string_view name  = cp.utf8Of(nat.a);
    const std::string_view desc  = cp.utf8Of(nat.b);
    const MemberRenameRule* rule = (mh.a <= 4)
        ? plan.fieldRule(owner, name, desc)
        : plan.methodRule(owner, name, desc);
    return rule != nullptr && ruleActuallyChanges(*rule, name, desc);
}

/// @brief 载体触达预检（成员规则的 fail-loud 闸，先检后写）：
///        ① Class 级 BootstrapMethods（bootstrap_method_ref + args 里的
///        MethodHandle 载体）；② Code 内 ldc MethodHandle 常量。
///
/// 半支持纪律：成员族规则的 operand 改写直铺成员表与指令位，不会把
/// BSM/MethodHandle 载体上的目标改坏的正确理由只有一个——它们根本没被
/// 改写。命中即 Err(BsmTouched)，改坏调不到的地方在趟前显式暴露。
Result<bool, RenameError>
carriersTouchPlan(ClassSession& s, ClassFile& cf, const RenamePlan& plan)
{
    if (plan.methods().empty() && plan.fields().empty()) {
        return Ok(false);
    }

    // ① BootstrapMethods 载体。
    if (AttrSlot* bsm = cf.findClassAttr("BootstrapMethods"); bsm != nullptr) {
        auto d = cf.decodeAttr(*bsm);
        if (d.is_ok() && d.unwrap()->kind() == AttrKind::BootstrapMethods) {
            const auto& bm = *static_cast<const BootstrapMethodsAttr*>(bsm->body.get());
            for (const BootstrapMethodsAttr::Entry& entry : bm.entries) {
                if (mhTargetTouches(cf, plan, entry.methodRef)) {
                    return Ok(true);
                }
                for (const ca::u32 arg : entry.args) {
                    if (mhTargetTouches(cf, plan, arg)) {
                        return Ok(true);
                    }
                }
            }
        }
        // BSM 槽解码失败 = 改不动这类结构 → 静默放行（不产生假阳性；
        // 畸形结构属 Recovered 档既有事实，不改写即无新风险）。
    }

    // ② Code 内 ldc MethodHandle 常量。
    std::vector<jbc::ir::CodeVisit> visits = jbc::ir::collectCodeUnits(s);
    for (const jbc::ir::CodeVisit& visit : visits) {
        if (visit.kind != jbc::ir::CodeVisit::Kind::Ok) {
            continue;
        }
        const jbc::ir::CodeUnit& cu = *visit.unit;
        for (const jbc::ir::InsnId id : cu.dom.order()) {
            const jbc::ir::Insn* in = cu.dom.peek(id);
            if (in == nullptr) {
                continue;
            }
            ir::ClassSession::ConstSem c;
            if (s.constOf(*in, c) && c.kind == ir::ClassSession::ConstSem::Kind::MethodHandle) {
                if (mhTargetTouches(cf, plan, in->cp)) {
                    return Ok(true);
                }
            }
        }
    }
    return Ok(false);
}

/// @brief 成员表行改写：名字/描述符按规则 + 类名族映射；commit 形成 E2
///        ChangeSet 记录（prior 随行）。
Result<std::pair<ca::u32, ca::u32>, RenameError>
renameMemberRows(ClassSession& s, const RenamePlan& plan, const std::string& thisName)
{
    jbc::ir::ClassUnit& unit = s.unit();
    ca::u32 methodRows = 0;
    for (jbc::ir::MethodUnit& row : unit.methods) {
        const std::string_view oldNameV = unit.symbols().str(row.name);
        const std::string_view oldDescV = unit.symbols().str(row.desc);
        const MemberRenameRule* rule    = plan.methodRule(thisName, oldNameV, oldDescV);
        const std::string newName = (rule != nullptr && !rule->newName.empty())
            ? rule->newName
            : std::string(oldNameV);
        const std::string newDesc = (rule != nullptr && !rule->newDesc.empty())
            ? rule->newDesc
            : plan.mapDescriptor(oldDescV);
        if (newName == oldNameV && newDesc == oldDescV) {
            continue;
        }
        row.name = unit.symbols().intern(newName);
        row.desc = unit.symbols().intern(newDesc);
        if (const auto r = s.commitMethodMetadata(row.id); r.is_err()) {
            return Err(errLower(r.unwrap_err()));
        }
        ++methodRows;
    }

    ca::u32 fieldRows = 0;
    for (jbc::ir::FieldUnit& row : unit.fields) {
        const std::string_view oldNameV = unit.symbols().str(row.name);
        const std::string_view oldDescV = unit.symbols().str(row.desc);
        const MemberRenameRule* rule    = plan.fieldRule(thisName, oldNameV, oldDescV);
        const std::string newName = (rule != nullptr && !rule->newName.empty())
            ? rule->newName
            : std::string(oldNameV);
        const std::string newDesc = (rule != nullptr && !rule->newDesc.empty())
            ? rule->newDesc
            : plan.mapDescriptor(oldDescV);
        if (newName == oldNameV && newDesc == oldDescV) {
            continue;
        }
        row.name = unit.symbols().intern(newName);
        row.desc = unit.symbols().intern(newDesc);
        if (const auto r = s.commitFieldMetadata(row.id); r.is_err()) {
            return Err(errLower(r.unwrap_err()));
        }
        ++fieldRows;
    }
    return Ok(std::make_pair(methodRows, fieldRows));
}

/// @brief 类头（this/super/接口）按类名族规则改写 + commit。
Result<ca::u32, RenameError> renameClassHead(ClassSession& s, const RenamePlan& plan)
{
    jbc::ir::ClassUnit& unit = s.unit();
    ca::u32 changed = 0;
    auto touchHead = [&](jbc::ir::SymbolId& sym) {
        const std::string_view cur = unit.symbols().str(sym);
        const ClassRenameRule* r   = plan.classRule(cur);
        if (r == nullptr || r->newName == cur) {
            return;
        }
        sym = unit.symbols().intern(r->newName);
        ++changed;
    };
    touchHead(unit.thisClass);
    touchHead(unit.superClass);
    for (jbc::ir::SymbolId& iface : unit.interfaces) {
        touchHead(iface);
    }
    if (changed == 0) {
        return Ok(ca::u32{0});
    }
    if (const auto r = s.commitClassMetadata(); r.is_err()) {
        return Err(errLower(r.unwrap_err()));
    }
    return Ok(changed);
}

/// @brief 代码 operand 改写：显式类型指令走 setClass；invoke/field 成员引用
///        按原 kind 走 setMember（接口形态 kind 内部自选 ref 簇）。仅 lower
///        有改动的指令位。三类不再外推的「不动面」在此显式拒绝：
///        indy（BSM 族闸守门）、ldc Class（Class 条目重指向自动跟随）、
///        ldc String（角色纪律：字面量不是引用）。
Result<ca::u32, RenameError> rewriteOperands(ClassSession& s, const RenamePlan& plan)
{
    using jbc::ir::CodeUnit;
    using jbc::ir::CodeVisit;
    using jbc::ir::Insn;
    using jbc::ir::InsnId;
    using MemberSem = jbc::ir::ClassSession::MemberSem;

    ca::u32 retargeted = 0;
    std::vector<CodeVisit> visits = jbc::ir::collectCodeUnits(s);
    for (CodeVisit& visit : visits) {
        if (visit.kind != CodeVisit::Kind::Ok) {
            continue;   // NoCode/Failed：无 operand 位（Failed 属显式既有事实域）
        }
        CodeUnit&      cu      = *visit.unit;
        bool           touched = false;
        for (const InsnId id : cu.dom.order()) {
            auto g = cu.dom.get(id);
            if (g.is_err()) {
                continue;
            }
            Insn& in = *(g.unwrap());

            const std::string_view clsName = s.classOf(in);
            if (!clsName.empty()) {
                const ClassRenameRule* r = plan.classRule(clsName);
                if (r != nullptr && !r->newName.empty() && r->newName != clsName) {
                    if (s.setClass(in, r->newName)) {
                        touched = true;
                        ++retargeted;
                    }
                }
                continue;
            }

            MemberSem m;
            if (s.memberOf(in, m)) {
                if (m.kind == MemberSem::Kind::InvokeDynamic) {
                    continue;   // indy 改写属 BSM 族，v1 由闸守门不改写
                }
                const ClassRenameRule* ownerRule = plan.classRule(m.owner);
                const MemberRenameRule* rule     = (m.kind == MemberSem::Kind::Field)
                    ? plan.fieldRule(m.owner, m.name, m.desc)
                    : plan.methodRule(m.owner, m.name, m.desc);
                const std::string newOwner
                    = (ownerRule != nullptr && !ownerRule->newName.empty())
                        ? ownerRule->newName
                        : std::string(m.owner);
                const std::string newName
                    = (rule != nullptr && !rule->newName.empty())
                        ? rule->newName
                        : std::string(m.name);
                const std::string newDesc = (rule != nullptr && !rule->newDesc.empty())
                    ? rule->newDesc
                    : plan.mapDescriptor(m.desc);
                if (newOwner == m.owner && newName == m.name && newDesc == m.desc) {
                    continue;
                }
                // setMember 的 kind 分支全覆盖（Field/Method/InterfaceMethod），
                // false 仅剩 append 失败不可能域；保守不作第二失败面。
                if (s.setMember(in, m.kind, newOwner, newName, newDesc)) {
                    touched = true;
                    ++retargeted;
                }
            }
        }
        if (touched) {
            if (const auto lr = jbc::ir::lowerMethod(s, visit.methodIdx); lr.is_err()) {
                return Err(errLower(lr.unwrap_err()));
            }
        }
    }
    return Ok(retargeted);
}

}   // namespace

Result<RenameSummary, RenameError> applyClassRename(ClassSession& s, const RenamePlan& plan)
{
    jbc::ir::ClassUnit& unit = s.unit();
    if (plan.empty()) {
        return Ok(RenameSummary{});   // no-op 计划：零改动直接返回
    }

    // 语法预检：类名族的新名必须合法内部名（成员名无此约束，任意 UTF8）。
    for (const ClassRenameRule& r : plan.classes()) {
        if (!jbc::type::isValidInternalName(r.oldName)
            || !jbc::type::isValidInternalName(r.newName)) {
            return Err(errKind(RenameError::Kind::BadRule));
        }
    }

    RenameSummary out;
    ClassFile& cf = s.backing();

    // ---- 载体触达预检（fail-loud 闸，先检后写）----
    {
        const auto touched = carriersTouchPlan(s, cf, plan);
        if (touched.is_err()) {
            return Err(touched.unwrap_err());
        }
        if (touched.unwrap()) {
            return Err(errKind(RenameError::Kind::BsmTouched));
        }
    }

    // ---- 成员族 · 成员表行（E2 ChangeSet 记录由 commit 自动随行）----
    {
        const std::string thisName{unit.symbols().str(unit.thisClass)};
        auto rows = renameMemberRows(s, plan, thisName);
        if (rows.is_err()) {
            return Err(rows.unwrap_err());
        }
        out.methodRows = rows.unwrap().first;
        out.fieldRows  = rows.unwrap().second;
    }

    // ---- 成员族 · 代码 operand ----
    {
        auto refs = rewriteOperands(s, plan);
        if (refs.is_err()) {
            return Err(refs.unwrap_err());
        }
        out.refsRetargeted = refs.unwrap();
    }

    // ---- 类头族 ----
    {
        auto head = renameClassHead(s, plan);
        if (head.is_err()) {
            return Err(head.unwrap_err());
        }
        out.headMembersChanged = head.unwrap();
    }

    // ---- 类名族 · CP Class 条目重指向（plain + 数组形态，经 session 原语）----
    {
        ClassFile& cfMut = s.backing();
        // 数组形态：条目名字槽全文（"[[Ldemo/Expr;"）先收齐，按唯一 (old,
        // new) 文本对重指向——renameClassConstant 按「名字槽 Utf8 原文」
        // 精确匹配，数组形态只是一份不同原文的 Class 条目。
        std::map<std::string, std::string> arrayPairs;
        {
            const ConstantPool& cp = cfMut.cp;
            for (ca::u32 i = 1; i < cp.size(); ++i) {
                const ConstantPool::Entry& e = cp.at(i);
                if (e.tag != CpTag::Class || !cp.isUtf8(e.a)) {
                    continue;
                }
                const std::string_view n = cp.utf8Of(e.a);
                if (n.empty() || n.front() != '[') {
                    continue;
                }
                auto t = jbc::type::parseFieldType(n);
                if (t.is_err()) {
                    continue;
                }
                const jbc::type::TypeDesc d = std::move(t).unwrap();
                if (d.kind != jbc::type::TypeDesc::Kind::Class || d.array_dims == 0) {
                    continue;
                }
                const ClassRenameRule* r = plan.classRule(d.class_name);
                if (r == nullptr || r->newName == d.class_name) {
                    continue;
                }
                std::string oldArr(static_cast<ca::usize>(d.array_dims), '[');
                oldArr += "L";
                oldArr += d.class_name;
                oldArr += ';';
                std::string newArr(static_cast<ca::usize>(d.array_dims), '[');
                newArr += "L";
                newArr += r->newName;
                newArr += ';';
                arrayPairs.emplace(std::move(oldArr), std::move(newArr));
            }
        }
        for (const ClassRenameRule& r : plan.classes()) {
            out.classConstants += s.renameClassConstant(r.oldName, r.newName);
        }
        for (const auto& [oldArr, newArr] : arrayPairs) {
            out.classConstants += s.renameClassConstant(oldArr, newArr);
        }
    }

    // ---- 类名族 · 描述符列（NameAndType desc / MethodType desc）----
    // 条目 payload 直改（rename_probe 同款）；utf8 追加经会话 internUtf8。
    ca::u32 descCols = 0;
    {
        ConstantPool& cp      = cf.cp;
        const ca::u32 poolSize = cp.size();   // 追加只落在 Utf8 尾段，上界覆盖
        for (ca::u32 i = 1; i < poolSize; ++i) {
            const ConstantPool::Entry& e = cp.at(i);
            const bool isNat = e.tag == CpTag::NameAndType;
            const bool isMt  = e.tag == CpTag::MethodType;
            if (!isNat && !isMt) {
                continue;
            }
            const ca::u32 slotIdx = isNat ? e.b : e.a;
            if (!cp.isUtf8(slotIdx)) {
                continue;
            }
            const std::string_view text = cp.utf8Of(slotIdx);
            const std::string mapped    = mapText(plan, text);
            if (mapped == text) {
                continue;
            }
            if (isNat) {
                cp.mutateAt(i).b = s.internUtf8(mapped);
            } else {
                cp.mutateAt(i).a = s.internUtf8(mapped);
            }
            ++descCols;
        }
    }

    // ---- 类名族 · LVT 行描述符（LVT 归描述符层；LVTT 归 sig_attr 族，见下）----
    {
        for (MemberInfo& m : cf.methods) {
            auto dc = cf.decodeCode(m);
            if (dc.is_err() || dc.unwrap() == nullptr) {
                continue;
            }
            CodeAttr* code = dc.unwrap();
            for (AttrSlot& slot : code->attrs) {
                if (!cf.cp.utf8Equals(slot.nameCp, "LocalVariableTable")) {
                    continue;
                }
                auto d = cf.decodeAttr(slot);
                if (d.is_err() || d.unwrap()->kind() != AttrKind::LocalVariableTable) {
                    continue;
                }
                auto* lvt = static_cast<LocalVariableTableAttr*>(slot.mutate());
                for (LocalVariableTableAttr::Row& row : lvt->rows) {
                    if (!cf.cp.isUtf8(row.descCp)) {
                        continue;
                    }
                    const std::string_view text = cf.cp.utf8Of(row.descCp);
                    const std::string mapped    = mapText(plan, text);
                    if (mapped == text) {
                        continue;
                    }
                    row.descCp = s.internUtf8(mapped);
                    ++descCols;
                }
            }
        }
    }
    out.descriptorCols = descCols;

    // ---- 类名族 · 泛型签名家族三件（sig_attr / LVTT / TA：既有手术面）----
    {
        for (const ClassRenameRule& r : plan.classes()) {
            auto sigR = jbc::ir::renameClassInSignatureAttrs(cf, r.oldName, r.newName);
            if (sigR.is_err()) {
                return Err(errSig(sigR.unwrap_err()));
            }
            const jbc::ir::SigAttrRenameSummary sigS = std::move(sigR).unwrap();
            out.sigAttrs += sigS.attrsChanged;

            auto lvttR = jbc::ir::renameClassInLocalVariableTypeTables(cf, r.oldName, r.newName);
            if (lvttR.is_err()) {
                return Err(errSig(lvttR.unwrap_err()));
            }
            const jbc::ir::LvttRenameSummary lvttS = std::move(lvttR).unwrap();
            out.lvttTables += lvttS.tablesChanged;

            auto taR = jbc::ir::renameClassInTypeAnnotations(cf, r.oldName, r.newName);
            if (taR.is_err()) {
                return Err(errTa(taR.unwrap_err()));
            }
            const jbc::ir::TaRenameSummary taS = std::move(taR).unwrap();
            out.taAttrs += taS.attrsChanged;
        }
    }

    // ---- 缓存复位与汇总 ----
    if (descCols > 0) {
        // NAT/MT 列与 LVT 行的条目/槽位直改在 session 原语之外；各
        // renameClassConstant 已为自身复位过，这里覆盖纯列改写的路径。
        s.invalidateSemantics();
    }

    out.changed = out.classConstants > 0 || out.descriptorCols > 0 || out.methodRows > 0
        || out.fieldRows > 0 || out.refsRetargeted > 0 || out.sigAttrs > 0
        || out.lvttTables > 0 || out.taAttrs > 0 || out.headMembersChanged > 0;
    return Ok(out);
}

// ---------------------------------------------------------------------------
// renameSource：跨类 fail-closed 驱动
// ---------------------------------------------------------------------------

ConsumerResult<MemoryClassSource> renameSource(const IClassSource& source, IRenameResolver& resolver)
{
    MemoryClassSource stage;
    std::vector<std::string_view> names;
    source.listClasses(names);
    for (const std::string_view name : names) {
        const ConsumerResult<ByteView> got = source.bytes(name);
        if (got.is_err()) {
            return Err(got.unwrap_err());
        }
        const ByteView bytesView = got.unwrap();

        const ConsumerResult<const RenamePlan*> rp = resolver.planFor(name);
        if (rp.is_err()) {
            return Err(rp.unwrap_err());
        }
        const RenamePlan* plan = rp.unwrap();
        if (plan == nullptr || plan->empty()) {
            // 缺计划/空计划：原字节透传（字节保真）。
            stage.add(name, std::vector<ca::u8>(bytesView.data(), bytesView.data() + bytesView.size()));
            continue;
        }

        // 会话从字节起；改名失败一律丢弃该会话——半截状态永不写回。
        auto sr = jbc::ir::makeSessionFromBytes(bytesView);
        if (sr.is_err()) {
            return Err(ConsumerError::BadClassFile);
        }
        const std::shared_ptr<jbc::ir::ClassSession> session = std::move(sr).unwrap();

        const auto applied = applyClassRename(*session, *plan);
        if (applied.is_err()) {
            return Err(ConsumerError::RenameFailed);
        }
        if (!applied.unwrap().changed) {
            // 零改动：原字节透传（语义与输入等价，免一次重编码）。
            stage.add(name, std::vector<ca::u8>(bytesView.data(), bytesView.data() + bytesView.size()));
            continue;
        }
        const auto wb = jbc::core::writeClass(session->backing());
        if (wb.is_err()) {
            return Err(ConsumerError::WriteFailed);
        }
        stage.add(name, std::move(wb).unwrap());
    }
    return Ok(std::move(stage));
}

}   // namespace jbc::consumer
