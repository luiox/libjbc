/// @file        lower.cpp
/// @brief       方法体 lower：CodeDom 导出落盘 + Code 嵌套属性重定向调度 +
///              成员/类级元数据提交。
#include "jbc/ir/units.hpp"

#include "jbc/ir/units_internal.hpp"
#include "jbc/ir/sem_index.hpp"

#include "jbc/core/attr_codec.hpp"
#include "jbc/core/insn_codec.hpp"

#include <unordered_map>
#include <utility>
#include <vector>

namespace jbc::ir {

Result<void, LowerError> lowerMethod(ClassSession& s, ca::usize methodIdx)
{
    const ClassUnit& u = s.unit();
    if (methodIdx >= u.methods.size()) {
        return Err(LowerError::NotPromoted);
    }
    return lowerMethod(s, u.methods[methodIdx].id);
}

Result<void, LowerError> lowerMethod(ClassSession& s, MethodId method)
{
    core::ClassFile& cf = s.backing();
    ClassUnit& u        = s.unit();
    MethodUnit* mu = u.method(method);
    if (mu == nullptr) {
        return Err(LowerError::InvalidHandle);
    }
    if (mu->code == nullptr || mu->backingIndex >= cf.methods.size()) {
        return Err(LowerError::NotPromoted);
    }
    CodeUnit& cu = *mu->code;
    core::MemberInfo& m = cf.methods[mu->backingIndex];
    const auto [foundCode, slotIdx] = findCodeSlot(cf, m);
    if (!foundCode) {
        return Err(LowerError::NotPromoted);
    }
    core::AttrSlot& slot = m.attrs[slotIdx];

    auto d = cf.decodeAttr(slot);
    if (d.is_err()) {
        return Err(LowerError::BadInstructions);
    }
    core::CodeAttr& ca = static_cast<core::CodeAttr&>(*d.unwrap());

    auto rawR = cu.dom.exportRaw();
    if (rawR.is_err()) {
        return Err(rawR.unwrap_err() == EditError::BranchOverflow
                       ? LowerError::BranchOverflow
                       : LowerError::BadInstructions);
    }
    std::vector<core::RawInsn> raw = std::move(rawR).unwrap();
    auto bytesR = core::encodeInsns(raw);
    if (bytesR.is_err()) {
        return Err(LowerError::BadInstructions);
    }
    std::vector<ca::u8> bytes = std::move(bytesR).unwrap();

    // JVMS 4.7.3：code_length 必须 > 0 且 ≤ 65535。在 lower 闸住而不是拖到
    // writeClass 的 Unencodable，那里既无方法名也无实际大小，排查困难。
    // 早退路径（与 backing 一致时不进此门）天然安全：backing 字节来自
    // reader，已受同一上限约束。
    if (bytes.empty()) {
        return Err(LowerError::BadInstructions);
    }
    if (bytes.size() > 0xFFFFu) {
        return Err(LowerError::CodeTooLarge);
    }

    // pcmap：旧 pc（import 时的 srcPc）→ 新 pc。
    std::unordered_map<ca::u32, ca::u32> pcmap;
    pcmap.reserve(cu.dom.size() * 2);
    for (ca::usize i = 0; i < cu.dom.order().size(); ++i) {
        const Insn* in = cu.dom.peek(cu.dom.order()[i]);
        if (in != nullptr && in->srcPc != kNoPc) {
            pcmap.emplace(in->srcPc, raw[i].pc);
        }
    }

    // 新异常表。end 的空句柄（方法末尾哨兵）是唯一合法的非指令边界；
    // 其余句柄死亡即 Err（pcOfId），不静默回落。
    std::vector<core::CodeAttr::ExcEntry> newExc;
    newExc.reserve(cu.exc.size());
    for (const CodeUnit::Exc& e : cu.exc) {
        const auto sPc = pcOfLabel(cu.dom, raw, e.start, false,
                                   static_cast<ca::u32>(bytes.size()));
        const auto hPc = pcOfLabel(cu.dom, raw, e.handler, false,
                                   static_cast<ca::u32>(bytes.size()));
        if (sPc.is_err() || hPc.is_err()) {
            return Err(sPc.is_err() ? sPc.unwrap_err() : hPc.unwrap_err());
        }
        const auto ePc = pcOfLabel(cu.dom, raw, e.end, true,
                                   static_cast<ca::u32>(bytes.size()));
        if (ePc.is_err()) {
            return Err(ePc.unwrap_err());
        }
        core::CodeAttr::ExcEntry x;
        x.startPc   = sPc.unwrap();
        x.handlerPc = hPc.unwrap();
        x.endPc     = ePc.unwrap();
        x.catchType = e.catchCp;
        newExc.push_back(x);
    }

    const bool codeChanged = bytes != ca.code;
    const bool excChanged  = [&] {
        if (newExc.size() != ca.excTable.size()) {
            return true;
        }
        for (ca::usize i = 0; i < newExc.size(); ++i) {
            const auto& a = newExc[i];
            const auto& b = ca.excTable[i];
            if (a.startPc != b.startPc || a.endPc != b.endPc ||
                a.handlerPc != b.handlerPc || a.catchType != b.catchType) {
                return true;
            }
        }
        return false;
    }();
    const bool maxesChanged =
        cu.maxStack != ca.maxStack || cu.maxLocals != ca.maxLocals;

    if (!codeChanged && !excChanged && !maxesChanged) {
        auto metadata = s.commitMethodMetadata(method);
        if (metadata.is_err()) {
            return Err(metadata.unwrap_err());
        }
        cu.dom.acceptLoweredLayout(raw);
        return Ok();   // 没有 classfile 变化：属性保持 copy-through。
    }

    struct NestedRelocation
    {
        core::AttrSlot* slot = nullptr;
        core::AttrKind kind = core::AttrKind::Raw;
        std::vector<ca::u32> starts;
        std::vector<ca::u32> lengths;
        std::vector<ca::u32> ends;   ///< CharacterRangeTable 的 end_pc 列
        std::vector<core::StackMapTableAttr::Frame> frames;
        TannRelocation tanns;
    };
    std::vector<NestedRelocation> relocations;

    // ---- relocation contract：先预检并暂存所有受影响属性 ----
    if (codeChanged) {
        relocations.reserve(ca.attrs.size());
        for (core::AttrSlot& nested : ca.attrs) {
            auto dn = cf.decodeAttr(nested);
            if (dn.is_err()) {
                return Err(LowerError::BadInstructions);
            }
            core::AttrBody* nb = dn.unwrap();
            switch (nb->kind()) {
            case core::AttrKind::LineNumberTable: {
                auto r = relocateLnt(static_cast<const core::LineNumberTableAttr&>(*nb), pcmap);
                if (r.is_err()) {
                    return Err(r.unwrap_err());
                }
                NestedRelocation plan;
                plan.slot = &nested;
                plan.kind = core::AttrKind::LineNumberTable;
                plan.starts = std::move(r).unwrap();
                relocations.push_back(std::move(plan));
                break;
            }
            case core::AttrKind::LocalVariableTable: {
                auto r = relocateLvt(static_cast<const core::LocalVariableTableAttr&>(*nb),
                                     pcmap,
                                     static_cast<ca::u32>(ca.code.size()),
                                     static_cast<ca::u32>(bytes.size()));
                if (r.is_err()) {
                    return Err(r.unwrap_err());
                }
                LocalVariableRelocation values = std::move(r).unwrap();
                NestedRelocation plan;
                plan.slot = &nested;
                plan.kind = core::AttrKind::LocalVariableTable;
                plan.starts = std::move(values.starts);
                plan.lengths = std::move(values.lengths);
                relocations.push_back(std::move(plan));
                break;
            }
            case core::AttrKind::StackMapTable: {
                auto r = relocateSmt(static_cast<const core::StackMapTableAttr&>(*nb), pcmap);
                if (r.is_err()) {
                    return Err(r.unwrap_err());
                }
                NestedRelocation plan;
                plan.slot = &nested;
                plan.kind = core::AttrKind::StackMapTable;
                plan.frames = std::move(r).unwrap();
                relocations.push_back(std::move(plan));
                break;
            }
            case core::AttrKind::TypeAnnotations: {
                auto r = planTann(static_cast<const core::TypeAnnotationsAttr&>(*nb),
                                  pcmap,
                                  static_cast<ca::u32>(ca.code.size()),
                                  static_cast<ca::u32>(bytes.size()));
                if (r.is_err()) {
                    return Err(r.unwrap_err());
                }
                NestedRelocation plan;
                plan.slot = &nested;
                plan.kind = core::AttrKind::TypeAnnotations;
                plan.tanns = std::move(r).unwrap();
                relocations.push_back(std::move(plan));
                break;
            }
            case core::AttrKind::CharacterRangeTable: {
                auto r = relocateCrt(static_cast<const core::CharacterRangeTableAttr&>(*nb),
                                     pcmap);
                if (r.is_err()) {
                    return Err(r.unwrap_err());
                }
                CrtRelocation values = std::move(r).unwrap();
                NestedRelocation plan;
                plan.slot   = &nested;
                plan.kind   = core::AttrKind::CharacterRangeTable;
                plan.starts = std::move(values.starts);
                plan.ends   = std::move(values.ends);
                relocations.push_back(std::move(plan));
                break;
            }
            default:
                // 未建模嵌套属性 + 布局已变：明确失败，不静默丢、不猜。
                return Err(LowerError::RelocationNotModeled);
            }
        }
    }

    auto metadata = s.commitMethodMetadata(method);
    if (metadata.is_err()) {
        return Err(metadata.unwrap_err());
    }

    for (NestedRelocation& plan : relocations) {
        core::AttrBody* body = plan.slot->mutate();
        switch (plan.kind) {
        case core::AttrKind::LineNumberTable: {
            auto& attr = static_cast<core::LineNumberTableAttr&>(*body);
            for (ca::usize i = 0; i < attr.rows.size(); ++i) {
                attr.rows[i].startPc = plan.starts[i];
            }
            break;
        }
        case core::AttrKind::LocalVariableTable: {
            auto& attr = static_cast<core::LocalVariableTableAttr&>(*body);
            for (ca::usize i = 0; i < attr.rows.size(); ++i) {
                attr.rows[i].startPc = plan.starts[i];
                attr.rows[i].length = plan.lengths[i];
            }
            break;
        }
        case core::AttrKind::StackMapTable:
            static_cast<core::StackMapTableAttr&>(*body).frames = std::move(plan.frames);
            break;
        case core::AttrKind::TypeAnnotations:
            applyTann(static_cast<core::TypeAnnotationsAttr&>(*body), plan.tanns);
            break;
        case core::AttrKind::CharacterRangeTable: {
            auto& attr = static_cast<core::CharacterRangeTableAttr&>(*body);
            for (ca::usize i = 0; i < attr.rows.size(); ++i) {
                attr.rows[i].startPc = plan.starts[i];
                attr.rows[i].endPc   = plan.ends[i];
            }
            break;
        }
        default:
            break;
        }
    }

    core::CodeAttr* out = static_cast<core::CodeAttr*>(slot.mutate());
    if (out == nullptr) {
        return Err(LowerError::BadInstructions);
    }
    out->code      = std::move(bytes);
    out->excTable  = std::move(newExc);
    out->maxStack  = cu.maxStack;
    out->maxLocals = cu.maxLocals;
    cu.dom.acceptLoweredLayout(raw);
    // 记账：body 粒度以 lower 落盘点为准，code/异常表/maxes 任一
    // 实改才记（copy-through 早退路径不进此处）。身份视图取符号池（只增不移）。
    {
        ChangeRecord body;
        body.kind   = ChangeKind::MethodBody;
        body.member = MemberKind::Method;
        body.name   = u.symbols().str(mu->name);
        body.desc   = u.symbols().str(mu->desc);
        s.noteChange(body);
    }
    return Ok();
}

Result<void, LowerError> ClassSession::commitMethodMetadata(MethodId method)
{
    MethodUnit* mu = unit_.method(method);
    if (mu == nullptr) {
        return Err(LowerError::InvalidHandle);
    }
    if (mu->backingIndex >= backing_->methods.size()) {
        return Err(LowerError::InvalidHandle);
    }
    const std::string_view name = unit_.symbols().str(mu->name);
    const std::string_view desc = unit_.symbols().str(mu->desc);
    if (name.size() > 0xFFFF || desc.size() > 0xFFFF || mu->access > 0xFFFF) {
        return Err(LowerError::CpAppendFailed);
    }
    SemIndex& index = sem();
    const bool needName = index.utf8.find(name) == index.utf8.end();
    const bool needDesc = desc != name && index.utf8.find(desc) == index.utf8.end();
    const ca::u32 additions = static_cast<ca::u32>(needName) +
                              static_cast<ca::u32>(needDesc);
    if (backing_->cp.size() + additions > 0xFFFFu) {
        return Err(LowerError::CpAppendFailed);
    }
    const ca::u32 nameCp = internUtf8Sem(name);
    const ca::u32 descCp = internUtf8Sem(desc);
    if (nameCp > 0xFFFF || descCp > 0xFFFF) {
        return Err(LowerError::CpAppendFailed);
    }
    core::MemberInfo& m = backing_->methods[mu->backingIndex];
    if (m.nameCp != nameCp || m.descCp != descCp || m.access != mu->access) {
        // 记账：元数据粒度只在实改时记；改写前身份从 backing CP
        // 原样解析（append-only 契约保证旧条目仍在），仅身份实变（改名/换
        // 描述符）时携带，未改名（仅 access 变）保持空视图（契约）。
        ChangeRecord meta;
        meta.kind   = ChangeKind::MemberMetadata;
        meta.member = MemberKind::Method;
        meta.name   = name;
        meta.desc   = desc;
        const std::string_view priorName = backing_->cp.utf8Of(m.nameCp);
        const std::string_view priorDesc = backing_->cp.utf8Of(m.descCp);
        if (priorName != name || priorDesc != desc) {
            meta.priorName = priorName;
            meta.priorDesc = priorDesc;
        }
        noteChange(meta);
        m.nameCp = static_cast<ca::u16>(nameCp);
        m.descCp = static_cast<ca::u16>(descCp);
        m.access = static_cast<ca::u16>(mu->access);
    }
    return Ok();
}

Result<void, LowerError> ClassSession::commitClassMetadata()
{
    ClassUnit& u         = unit_;
    core::ClassFile& cf  = *backing_;
    if (u.access > 0xFFFF) {
        return Err(LowerError::CpAppendFailed);
    }
    const std::string_view thisName = u.symbols().str(u.thisClass);
    if (thisName.empty()) {
        // 空类名无法构成合法 classfile（reader 保证 thisClass 可解析出非空名）；
        // 直到这里才可能为空，说明模型被越权改坏，拒绝提交而非写入坏引用。
        return Err(LowerError::CpAppendFailed);
    }
    const std::string_view superName = u.symbols().str(u.superClass);

    // 精确容量预检（先算后写，失败不留半截追加）：每个名字 utf8 miss 记 1
    // （此时 Class 条目也必然 miss，再记 1）；utf8 hit 而 Class miss 记 1。
    SemIndex& index = sem();
    ca::u32 additions = 0;
    const auto planOne = [&index, &additions](std::string_view name) {
        if (name.empty()) {
            return;
        }
        const auto uit = index.utf8.find(name);
        if (uit == index.utf8.end()) {
            additions += 2;
            return;
        }
        if (index.byName.find(uit->second) == index.byName.end()) {
            additions += 1;
        }
    };
    planOne(thisName);
    planOne(superName);
    for (const SymbolId& iface : u.interfaces) {
        planOne(u.symbols().str(iface));
    }
    if (cf.cp.size() + additions > 0xFFFFu) {
        return Err(LowerError::CpAppendFailed);
    }

    // 提交：intern 去重落位，再写回头字段。super 空名 = 无父类（Object），写 0。
    // 记账：类级粒度只在实改时记，CP 只追加且 intern 去重，同一
    // 名字的索引不变，故头字段索引/旗标的前后相等 ⇔ 元数据未变。
    const ca::u16 oldAccess = cf.access;
    const ca::u16 oldMajor  = cf.major;
    const ca::u16 oldMinor  = cf.minor;
    const ca::u16 oldThis   = cf.thisClass;
    const ca::u16 oldSuper  = cf.superClass;
    const std::vector<ca::u16> oldIfaces = cf.interfaces;
    const ca::u32 thisIdx = internClass(thisName);
    if (thisIdx > 0xFFFF) {
        return Err(LowerError::CpAppendFailed);
    }
    cf.access    = static_cast<ca::u16>(u.access);
    cf.thisClass = static_cast<ca::u16>(thisIdx);
    cf.superClass =
        superName.empty() ? ca::u16{0} : static_cast<ca::u16>(internClass(superName));
    cf.major = static_cast<ca::u16>(u.version >> 16);
    cf.minor = static_cast<ca::u16>(u.version & 0xFFFFu);
    cf.interfaces.clear();
    cf.interfaces.reserve(u.interfaces.size());
    for (const SymbolId& iface : u.interfaces) {
        const ca::u16 idx = static_cast<ca::u16>(internClass(u.symbols().str(iface)));
        cf.interfaces.push_back(idx);
    }
    if (oldAccess != cf.access || oldMajor != cf.major || oldMinor != cf.minor
        || oldThis != cf.thisClass || oldSuper != cf.superClass
        || oldIfaces != cf.interfaces) {
        ChangeRecord meta;
        meta.kind = ChangeKind::ClassMetadata;
        noteChange(meta);
    }
    return Ok();
}

Result<void, LowerError> ClassSession::commitFieldMetadata(FieldId field)
{
    FieldUnit* fu = unit_.field(field);
    if (fu == nullptr || fu->backingIndex >= backing_->fields.size()) {
        return Err(LowerError::InvalidHandle);
    }
    const std::string_view name = unit_.symbols().str(fu->name);
    const std::string_view desc = unit_.symbols().str(fu->desc);
    if (name.size() > 0xFFFF || desc.size() > 0xFFFF || fu->access > 0xFFFF) {
        return Err(LowerError::CpAppendFailed);
    }
    SemIndex& index = sem();
    const bool needName = index.utf8.find(name) == index.utf8.end();
    const bool needDesc = desc != name && index.utf8.find(desc) == index.utf8.end();
    const ca::u32 additions = static_cast<ca::u32>(needName) +
                              static_cast<ca::u32>(needDesc);
    if (backing_->cp.size() + additions > 0xFFFFu) {
        return Err(LowerError::CpAppendFailed);
    }
    const ca::u32 nameCp = internUtf8Sem(name);
    const ca::u32 descCp = internUtf8Sem(desc);
    core::MemberInfo& m = backing_->fields[fu->backingIndex];
    if (m.nameCp != nameCp || m.descCp != descCp || m.access != fu->access) {
        // 记账：与 commitMethodMetadata 同构（实改才记；仅身份实变携带
        // 改写前身份）。
        ChangeRecord meta;
        meta.kind   = ChangeKind::MemberMetadata;
        meta.member = MemberKind::Field;
        meta.name   = name;
        meta.desc   = desc;
        const std::string_view priorName = backing_->cp.utf8Of(m.nameCp);
        const std::string_view priorDesc = backing_->cp.utf8Of(m.descCp);
        if (priorName != name || priorDesc != desc) {
            meta.priorName = priorName;
            meta.priorDesc = priorDesc;
        }
        noteChange(meta);
        m.nameCp = static_cast<ca::u16>(nameCp);
        m.descCp = static_cast<ca::u16>(descCp);
        m.access = static_cast<ca::u16>(fu->access);
    }
    return Ok();
}

}   // namespace jbc::ir
