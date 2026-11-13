/// @file        member_ops.cpp
/// @brief       成员级增删/adopt：addMethod/addField/remove*/adopt 家族与
///              adoptMethod 的方法体重放。
#include "jbc/ir/units.hpp"

#include "jbc/ir/units_internal.hpp"
#include "jbc/ir/sem_index.hpp"

#include "jbc/core/attr_codec.hpp"
#include "jbc/core/insn_codec.hpp"

#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace jbc::ir {

Result<MethodId, LowerError> ClassSession::addMethod(const MethodSeed& seed)
{
    // 种子预检：非空、u16 域、会话内名字+描述符唯一（JVM 对重复成员是
    // 加载期错误，挂入时挡掉而不是写出一个加载不了的类）。
    if (seed.name.empty() || seed.desc.empty() || seed.name.size() > 0xFFFFu
        || seed.desc.size() > 0xFFFFu || seed.access > 0xFFFFu
        || unit_.findMethod(seed.name, seed.desc) != kNoMethod) {
        return Err(LowerError::InvalidSeed);
    }
    // 容量预检（先算后写，失败不留半截成员）：name/desc 的 Utf8 miss 各记
    // 1；withCode 时 "Code" 名串 miss 再记 1。
    SemIndex& index = sem();
    ca::u32 additions = 0;
    if (index.utf8.find(seed.name) == index.utf8.end()) {
        ++additions;
    }
    if (seed.desc != seed.name && index.utf8.find(seed.desc) == index.utf8.end()) {
        ++additions;
    }
    if (seed.withCode && index.utf8.find("Code") == index.utf8.end()) {
        ++additions;
    }
    if (backing_->cp.size() + additions > 0xFFFFu
        || backing_->methods.size() >= 0xFFFFu
        || unit_.methods.size() >= 0xFFFFu) {
        return Err(LowerError::CpAppendFailed);
    }

    core::MemberInfo m;
    m.access = static_cast<ca::u16>(seed.access);
    m.nameCp = internUtf8Sem(seed.name);
    m.descCp = internUtf8Sem(seed.desc);
    if (seed.withCode) {
        auto code       = std::make_unique<core::CodeAttr>();
        code->maxStack  = seed.maxStack;
        code->maxLocals = seed.maxLocals;
        const ca::u32 codeName = internUtf8Sem("Code");
        m.attrs.push_back(core::AttrSlot::makeNew(codeName, std::move(code)));
    }
    backing_->methods.push_back(std::move(m));

    MethodUnit mu;
    mu.id           = unit_.nextMethodId();
    mu.name         = unit_.symbols().intern(seed.name);
    mu.desc         = unit_.symbols().intern(seed.desc);
    mu.access       = seed.access;
    mu.backingIndex = backing_->methods.size() - 1;
    // 记账：挂入即新成员；身份视图取符号池（入参视图可能是暂态）。
    const std::string_view nameView = unit_.symbols().str(mu.name);
    const std::string_view descView = unit_.symbols().str(mu.desc);
    const MethodId out = mu.id;
    unit_.methods.push_back(std::move(mu));
    {
        ChangeRecord added;
        added.kind   = ChangeKind::MemberAdded;
        added.member = MemberKind::Method;
        added.name   = nameView;
        added.desc   = descView;
        noteChange(added);
    }
    return Ok(out);
}

Result<FieldId, LowerError> ClassSession::addField(const FieldSeed& seed)
{
    if (seed.name.empty() || seed.desc.empty() || seed.name.size() > 0xFFFFu
        || seed.desc.size() > 0xFFFFu || seed.access > 0xFFFFu
        || unit_.findField(seed.name, seed.desc) != kNoField) {
        return Err(LowerError::InvalidSeed);
    }
    SemIndex& index = sem();
    ca::u32 additions = 0;
    if (index.utf8.find(seed.name) == index.utf8.end()) {
        ++additions;
    }
    if (seed.desc != seed.name && index.utf8.find(seed.desc) == index.utf8.end()) {
        ++additions;
    }
    if (backing_->cp.size() + additions > 0xFFFFu
        || backing_->fields.size() >= 0xFFFFu
        || unit_.fields.size() >= 0xFFFFu) {
        return Err(LowerError::CpAppendFailed);
    }

    core::MemberInfo m;
    m.access = static_cast<ca::u16>(seed.access);
    m.nameCp = internUtf8Sem(seed.name);
    m.descCp = internUtf8Sem(seed.desc);
    backing_->fields.push_back(std::move(m));

    FieldUnit fu;
    fu.id           = unit_.nextFieldId();
    fu.name         = unit_.symbols().intern(seed.name);
    fu.desc         = unit_.symbols().intern(seed.desc);
    fu.access       = seed.access;
    fu.backingIndex = backing_->fields.size() - 1;
    // 记账：与 addMethod 同构（视图取符号池）。
    const std::string_view nameView = unit_.symbols().str(fu.name);
    const std::string_view descView = unit_.symbols().str(fu.desc);
    const FieldId out = fu.id;
    unit_.fields.push_back(std::move(fu));
    {
        ChangeRecord added;
        added.kind   = ChangeKind::MemberAdded;
        added.member = MemberKind::Field;
        added.name   = nameView;
        added.desc   = descView;
        noteChange(added);
    }
    return Ok(out);
}
Result<void, LowerError> ClassSession::removeMethod(MethodId method)
{
    MethodUnit* mu = unit_.method(method);
    if (mu == nullptr || mu->backingIndex >= backing_->methods.size()) {
        return Err(LowerError::InvalidHandle);
    }
    const ca::usize gone = mu->backingIndex;
    // 记账：摘除即失成员；身份视图先取（符号池存储不受骨架摘除
    // 影响，视图在会话存活期内有效）。
    ChangeRecord removed;
    removed.kind   = ChangeKind::MemberRemoved;
    removed.member = MemberKind::Method;
    removed.name   = unit_.symbols().str(mu->name);
    removed.desc   = unit_.symbols().str(mu->desc);
    // 物理面：摘除 method_info，后续成员的 backingIndex 统一平移，身份
    // 不动，物理位置本来就不是身份。
    backing_->methods.erase(backing_->methods.begin()
                            + static_cast<std::ptrdiff_t>(gone));
    for (MethodUnit& m : unit_.methods) {
        if (m.backingIndex > gone) {
            m.backingIndex -= 1;
        }
    }
    // 身份面：槽位回收 + generation 自增（旧句柄自此结构性失配），再摘骨架。
    unit_.retireMethodId(method);
    const ca::usize logical = unit_.indexOf(method);
    unit_.methods.erase(unit_.methods.begin() + static_cast<std::ptrdiff_t>(logical));
    noteChange(removed);
    return Ok();
}

Result<void, LowerError> ClassSession::removeField(FieldId field)
{
    FieldUnit* fu = unit_.field(field);
    if (fu == nullptr || fu->backingIndex >= backing_->fields.size()) {
        return Err(LowerError::InvalidHandle);
    }
    const ca::usize gone = fu->backingIndex;
    // 记账：与 removeMethod 同构（视图先取，成功才落账）。
    ChangeRecord removed;
    removed.kind   = ChangeKind::MemberRemoved;
    removed.member = MemberKind::Field;
    removed.name   = unit_.symbols().str(fu->name);
    removed.desc   = unit_.symbols().str(fu->desc);
    backing_->fields.erase(backing_->fields.begin()
                           + static_cast<std::ptrdiff_t>(gone));
    for (FieldUnit& f : unit_.fields) {
        if (f.backingIndex > gone) {
            f.backingIndex -= 1;
        }
    }
    unit_.retireFieldId(field);
    const ca::usize logical = unit_.indexOf(field);
    unit_.fields.erase(unit_.fields.begin() + static_cast<std::ptrdiff_t>(logical));
    noteChange(removed);
    return Ok();
}

Result<FieldId, LowerError> ClassSession::adoptField(const ClassSession& src,
                                                     FieldId field)
{
    if (&src == this) {
        return Err(LowerError::InvalidHandle);
    }
    const FieldUnit* sfu = src.unit().field(field);
    if (sfu == nullptr || sfu->backingIndex >= src.backing().fields.size()) {
        return Err(LowerError::InvalidHandle);
    }
    FieldSeed seed;
    seed.name   = src.unit().symbols().str(sfu->name);
    seed.desc   = src.unit().symbols().str(sfu->desc);
    seed.access = sfu->access;
    return addField(seed);
}
namespace {

/// adoptMethod 的方法体重放：指令逐条搬运，CP 操作数按源会话语义视图读出、
/// 按本会话语义 intern 写入。分支/switch 目标经标签映射重建（前向目标的
/// 占位标签在重放期未锚定，走 CodeDom 受控延后绑定构建态，统一改绑后
/// 收尾全量补校验，见 ）。失败即 Err，调用方保证全有或全无（新成员
/// 刚挂入，失败由调用方 remove 收尸或整体放弃会话）。
Result<void, LowerError> replayMethodBody(ClassSession& dstS, ClassSession& srcS,
                                          const CodeUnit& from, CodeUnit& to)
{
    CodeDom& dstDom = to.dom;

    // 携带 CP 操作数的族掩码：三读门只认 Invoke/FieldAccess/CpConst 三族。
    // insnFamilyBit 对 Branch 族同样置位（收集过滤的复用位），分支/switch
    // 无 CP 操作数，不得入三读门。
    const ca::u32 cpFamilyMask
        = (1u << static_cast<ca::u32>(InsnFamily::Invoke))
        | (1u << static_cast<ca::u32>(InsnFamily::FieldAccess))
        | (1u << static_cast<ca::u32>(InsnFamily::CpConst));

    // 标签映射：src slot → {src 句柄, dst 占位句柄}。前向分支的目标可能
    // 尚未重放，占位标签先创建（缺省锚在方法末尾），全部指令重放完成后
    // 统一改绑到映射后的锚点。占位在 append 期必然未锚定，append 的控制
    // 目标校验会拒收，重放进入受控延后绑定构建态放行，收尾统一补验。
    dstDom.beginDeferredTargets();
    std::unordered_map<ca::u32, std::pair<LabelId, LabelId>> labelMap;
    const auto labelFor = [&](LabelId srcLabel) -> Result<LabelId, LowerError> {
        const auto it = labelMap.find(srcLabel.slot);
        if (it != labelMap.end()) {
            return Ok(it->second.second);
        }
        auto created = dstDom.createLabel();
        if (created.is_err()) {
            return Err(LowerError::BadInstructions);
        }
        labelMap.emplace(srcLabel.slot, std::make_pair(srcLabel, created.unwrap()));
        return Ok(created.unwrap());
    };

    std::unordered_map<ca::u32, InsnId> insnMap;   // src slot → dst 句柄
    // ldc 装载形态选择：索引 ≤ 0xFF 用 LDC，超界自动升格 LDC_W（与
    // CodeEditor::loadConst 同一裁决，宽度陷阱在这里挡掉，不留给调用方）。
    const auto loadConstFor = [](Insn& o, ca::u32 cpIdx) {
        o.op = cpIdx <= 0xFF ? core::Op::LDC : core::Op::LDC_W;
        o.cp = cpIdx;
    };
    for (const InsnId& srcId : from.dom.order()) {
        const Insn* in = from.dom.peek(srcId);
        if (in == nullptr) {
            return Err(LowerError::BadInstructions);
        }
        Insn out;
        out.op   = in->op;
        out.wide = in->wide;
        out.a    = in->a;
        out.b    = in->b;
        out.keys = in->keys;   // LOOKUPSWITCH 的 key 表（switch 形状校验依赖）

        // CP 操作数三读：memberOf（invoke/字段访问族）→ classOf（类族）→
        // constOf（ldc 族）。门按携带 CP 操作数的族判定；携带族内指令三读
        // 全 miss 才是池结构坏，显式失败，不按普通指令静默搬运（那会把
        // 坏池引用洗成 0 号槽）。
        if ((insnFamilyBit(in->op) & cpFamilyMask) != 0) {
            ClassSession::MemberSem ms;
            ClassSession::ConstSem cs;
            if (srcS.memberOf(*in, ms)) {
                if (ms.kind == ClassSession::MemberSem::Kind::InvokeDynamic) {
                    return Err(LowerError::NotModeled);
                }
                if (!dstS.setMember(out, ms.kind, ms.owner, ms.name, ms.desc)) {
                    return Err(LowerError::BadInstructions);
                }
            } else {
                bool handled = false;
                const std::string_view cn = srcS.classOf(*in);
                if (!cn.empty()) {
                    handled = dstS.setClass(out, cn);
                } else if (srcS.constOf(*in, cs)) {
                    switch (cs.kind) {
                    case ClassSession::ConstSem::Kind::Int:
                        loadConstFor(out, dstS.internInteger(static_cast<ca::i32>(cs.num)));
                        handled = true;
                        break;
                    case ClassSession::ConstSem::Kind::String:
                        loadConstFor(out, dstS.internString(cs.str));
                        handled = true;
                        break;
                    case ClassSession::ConstSem::Kind::Class:
                        loadConstFor(out, dstS.internClass(cs.str));
                        handled = true;
                        break;
                    default:
                        // Long/Float/Double/MethodType/MethodHandle/Dynamic：
                        // 语义 intern 面未覆盖，显式拒绝。
                        return Err(LowerError::NotModeled);
                    }
                }
                if (!handled) {
                    return Err(LowerError::BadInstructions);
                }
            }
        }

        // 分支/switch 目标标签（占位，绑定延后）。
        if (in->target != kNoLabel) {
            auto t = labelFor(in->target);
            if (t.is_err()) {
                return Err(t.unwrap_err());
            }
            out.target = t.unwrap();
        }
        out.targets.reserve(in->targets.size());
        for (const LabelId& l : in->targets) {
            auto t = labelFor(l);
            if (t.is_err()) {
                return Err(t.unwrap_err());
            }
            out.targets.push_back(t.unwrap());
        }

        auto appended = dstDom.append(std::move(out));
        if (appended.is_err()) {
            return Err(LowerError::BadInstructions);
        }
        insnMap.emplace(srcId.slot, appended.unwrap());
    }

    // 异常表边界的标签先纳入映射（分支未引用它们时也要改绑）。
    for (const CodeUnit::Exc& e : from.exc) {
        for (const LabelId l : {e.start, e.end, e.handler}) {
            auto t = labelFor(l);
            if (t.is_err()) {
                return Err(t.unwrap_err());
            }
        }
    }

    // 统一改绑：占位标签 → 映射后的锚点（End 保持 End）。
    for (const auto& entry : labelMap) {
        const auto target = from.dom.resolveLabel(entry.second.first);
        if (target.is_err()) {
            return Err(LowerError::BadInstructions);
        }
        const LabelTarget resolved = target.unwrap();
        InsnId anchor = kNoInsn;
        if (!resolved.isEnd()) {
            const auto it = insnMap.find(resolved.insn.slot);
            if (it == insnMap.end()) {
                return Err(LowerError::BadInstructions);
            }
            anchor = it->second;
        }
        if (dstDom.bindLabel(entry.second.second, anchor).is_err()) {
            return Err(LowerError::BadInstructions);
        }
    }

    // 收尾：退出受控延后绑定态，对全量存活指令补控制目标校验，统一改绑
    // 后仍悬垂的占位在此显式失败（fail-closed，不留给 lower）。
    if (dstDom.endDeferredTargets().is_err()) {
        return Err(LowerError::BadInstructions);
    }

    // 异常表：标签经映射重建；catch 类索引按源语义读出、本会话 intern。
    to.exc.reserve(from.exc.size());
    for (const CodeUnit::Exc& e : from.exc) {
        CodeUnit::Exc x;
        x.start   = labelMap.find(e.start.slot)->second.second;
        x.end     = labelMap.find(e.end.slot)->second.second;
        x.handler = labelMap.find(e.handler.slot)->second.second;
        if (e.catchCp != 0) {
            const std::string_view cn = srcS.backing().className(e.catchCp);
            if (cn.empty()) {
                return Err(LowerError::BadInstructions);
            }
            x.catchCp = dstS.internClass(cn);
        }
        to.exc.push_back(x);
    }
    return Ok();
}
}   // namespace

Result<MethodId, LowerError> ClassSession::adoptMethod(ClassSession& src,
                                                       MethodId method)
{
    if (&src == this) {
        return Err(LowerError::InvalidHandle);
    }
    const MethodUnit* smu = src.unit().method(method);
    if (smu == nullptr || smu->backingIndex >= src.backing().methods.size()) {
        return Err(LowerError::InvalidHandle);
    }

    // 源体提升：NoCode（abstract/native）合法走无体搬移；其余失败如实转发。
    CodeUnitRef body;
    const auto promoted = promoteMethod(src, method);
    if (promoted.is_ok()) {
        body = promoted.unwrap();
    } else {
        const PromoteError e = promoted.unwrap_err();
        if (e != PromoteError::NoCode) {
            return Err(e == PromoteError::InvalidHandle ? LowerError::InvalidHandle
                                                        : LowerError::BadInstructions);
        }
    }

    MethodSeed seed;
    seed.name   = src.unit().symbols().str(smu->name);
    seed.desc   = src.unit().symbols().str(smu->desc);
    seed.access = smu->access;
    if (body.get() != nullptr) {
        seed.withCode  = true;
        seed.maxStack  = body->maxStack;
        seed.maxLocals = body->maxLocals;
    }
    auto added = addMethod(seed);
    if (added.is_err()) {
        return Err(added.unwrap_err());
    }
    const MethodId dst = added.unwrap();
    if (body.get() == nullptr) {
        return Ok(dst);   // 无体成员：元数据搬移即完成
    }

    auto dstBody = promoteMethod(*this, dst);
    if (dstBody.is_err()) {
        // 空体 promote 只会因句柄失效失败，这里是模型不变量的防御面。
        return Err(LowerError::BadInstructions);
    }
    const auto replayed = replayMethodBody(*this, src, *body.get(), *dstBody.unwrap().get());
    if (replayed.is_err()) {
        return Err(replayed.unwrap_err());
    }
    const auto lowered = lowerMethod(*this, dst);
    if (lowered.is_err()) {
        return Err(lowered.unwrap_err());
    }
    return Ok(dst);
}

}   // namespace jbc::ir
