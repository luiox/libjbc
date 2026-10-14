/// @file        units.cpp
/// @brief       ClassSession 构建与方法级 promote/lower 实现。
#include "jbc/ir/units.hpp"

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
    }
    return "?";
}

const char* to_string(LowerError e) noexcept
{
    switch (e) {
    case LowerError::Ok: return "ok";
    case LowerError::BadInstructions: return "bad instructions";
    case LowerError::BranchOverflow: return "branch overflow";
    case LowerError::CpAppendFailed: return "cp append failed";
    case LowerError::NotPromoted: return "not promoted";
    case LowerError::RelocationNotModeled: return "relocation not modeled";
    case LowerError::RelocationBadTarget: return "relocation bad target";
    }
    return "?";
}

ClassSession::ClassSession(std::unique_ptr<core::ClassFile> backing)
    : backing_(std::move(backing))
{
    core::ClassFile& cf = *backing_;
    ClassUnit& u        = unit_;

    u.version = (static_cast<ca::u32>(cf.major) << 16) | cf.minor;
    u.access  = cf.access;
    // this/super/接口表是 Class 索引，成员名是 Utf8 索引——两种坐标系分开解析。
    auto internResolved = [&](std::string_view v) { return u.symbols().intern(v); };
    u.thisClass  = internResolved(cf.thisName());
    u.superClass = internResolved(cf.superName());
    u.interfaces.reserve(cf.interfaces.size());
    for (ca::u16 i : cf.interfaces) {
        u.interfaces.push_back(internResolved(cf.className(i)));
    }
    u.fields.reserve(cf.fields.size());
    for (const core::MemberInfo& f : cf.fields) {
        u.fields.push_back(FieldUnit{u.symbols().intern(cf.nameOf(f)),
                                     u.symbols().intern(cf.descOf(f)), f.access});
    }
    u.methods.reserve(cf.methods.size());
    for (ca::usize i = 0; i < cf.methods.size(); ++i) {
        const core::MemberInfo& m = cf.methods[i];
        MethodUnit mu;
        mu.id      = u.makeMethodId(i);
        mu.name    = u.symbols().intern(cf.nameOf(m));
        mu.desc    = u.symbols().intern(cf.descOf(m));
        mu.access  = m.access;
        mu.backingIndex = i;
        u.methods.push_back(std::move(mu));
    }
}

Result<std::shared_ptr<ClassSession>, PromoteError>
makeSession(std::unique_ptr<core::ClassFile> backing)
{
    if (backing == nullptr) {
        return Err(PromoteError::NotModeled);
    }
    // 构造函数私有（仅本函数是友元）：shared_ptr 接管寿命，同时使能
    // shared_from_this——保活句柄（CodeUnitRef / *SemRef）依赖它。
    return Ok(std::shared_ptr<ClassSession>(new ClassSession(std::move(backing))));
}

Result<std::shared_ptr<ClassSession>, SessionError>
makeSessionFromBytes(core::ByteView bytes, const core::ReadOptions& opt)
{
    auto r = core::readClass(bytes, opt);
    if (r.is_err()) {
        SessionError e;
        e.stage = SessionError::Stage::Read;
        e.read  = r.unwrap_err();
        return Err(e);
    }
    auto ses = makeSession(std::move(r).unwrap().file);
    if (ses.is_err()) {
        // makeSession 只在 backing 为空时失败；readClass 的成功产物非空，
        // 此分支按契约不可达，但错误通道必须保持完备（不猜、不静默）。
        SessionError e;
        e.stage   = SessionError::Stage::Session;
        e.session = ses.unwrap_err();
        return Err(e);
    }
    return Ok(std::move(ses).unwrap());
}

namespace {

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

}   // namespace

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
    if (mu == nullptr || mu->backingIndex >= cf.methods.size()) {
        return Err(PromoteError::NoCode);
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

namespace {

/// 句柄 → 新 pc（导出后）。死句柄（不在 order 里）返回 Err：静默回落会把
/// 异常表边界写成 pc 0（方法入口），产出坏 classfile——死句柄只能来自绕过
/// CodeEditor::erase 的越权改动，lower 在此显式失败（不猜、不静默）。
/// 「方法末尾」不在这里兜底：end 的空句柄哨兵由调用点显式展开成 code 长度。
Result<ca::u32, LowerError> pcOfId(const CodeDom& dom,
                                   const std::vector<core::RawInsn>& raw, InsnId id)
{
    for (ca::usize i = 0; i < dom.order().size(); ++i) {
        if (dom.order()[i] == id) {
            return Ok(raw[i].pc);
        }
    }
    return Err(LowerError::RelocationBadTarget);
}

/// 标签 → 新 pc。End 只对开区间端点合法；start/handler 解析到 End 即失败。
Result<ca::u32, LowerError> pcOfLabel(const CodeDom& dom,
                                      const std::vector<core::RawInsn>& raw,
                                      LabelId label,
                                      bool allowEnd,
                                      ca::u32 codeLen)
{
    const auto target = dom.resolveLabel(label);
    if (target.is_err()) {
        return Err(LowerError::RelocationBadTarget);
    }
    const LabelTarget resolved = target.unwrap();
    if (resolved.isEnd()) {
        if (allowEnd) {
            return Ok(codeLen);
        }
        return Err(LowerError::RelocationBadTarget);
    }
    return pcOfId(dom, raw, resolved.insn);
}

/// LNT 重定向：startPc 必须精确命中 pcmap。先计算新值，避免失败时半改属性。
Result<std::vector<ca::u32>, LowerError>
relocateLnt(const core::LineNumberTableAttr& lnt,
            const std::unordered_map<ca::u32, ca::u32>& pcmap)
{
    std::vector<ca::u32> starts;
    starts.reserve(lnt.rows.size());
    for (const auto& row : lnt.rows) {
        const auto it = pcmap.find(row.startPc);
        if (it == pcmap.end()) {
            return Err(LowerError::RelocationBadTarget);
        }
        starts.push_back(it->second);
    }
    return Ok(std::move(starts));
}

/// LVT/LVTT 重定向（fail-closed）：startPc 必须精确命中 pcmap；区间终点
/// oldEnd = startPc + length 必须命中 pcmap（落在旧指令起点），或严格等于
/// oldCodeLen（只有这一种 miss 允许，映射到 newCodeLen）；其余 miss 与
/// u32 溢出都返回 RelocationBadTarget——静默钳到新长度会把「终点落在指令
/// 操作数中间」的畸形输入写成合法形态。
struct LocalVariableRelocation
{
    std::vector<ca::u32> starts;
    std::vector<ca::u32> lengths;
};

Result<LocalVariableRelocation, LowerError>
relocateLvt(const core::LocalVariableTableAttr& lvt,
            const std::unordered_map<ca::u32, ca::u32>& pcmap,
            ca::u32 oldCodeLen,
            ca::u32 newCodeLen)
{
    LocalVariableRelocation result;
    result.starts.reserve(lvt.rows.size());
    result.lengths.reserve(lvt.rows.size());
    for (const auto& row : lvt.rows) {
        const auto it = pcmap.find(row.startPc);
        if (it == pcmap.end()) {
            return Err(LowerError::RelocationBadTarget);
        }
        const ca::u64 end = static_cast<ca::u64>(row.startPc) + row.length;
        if (end > 0xFFFFFFFFull) {
            return Err(LowerError::RelocationBadTarget);
        }
        const ca::u32 oldEnd = static_cast<ca::u32>(end);
        const auto eit       = pcmap.find(oldEnd);
        ca::u32 newEnd;
        if (eit != pcmap.end()) {
            newEnd = eit->second;
        } else if (oldEnd == oldCodeLen) {
            newEnd = newCodeLen;
        } else {
            return Err(LowerError::RelocationBadTarget);
        }
        result.starts.push_back(it->second);
        result.lengths.push_back(newEnd > it->second ? newEnd - it->second : 0);
    }
    return Ok(std::move(result));
}

/// SMT 重定向：链式 offset_delta → 绝对 pc → pcmap → 重算 delta。
/// 紧凑形态的 delta 越界时升格为 extended 形态（251/247）。
/// 链式算术与形态标签取 core/attribute.hpp 的共享定义（与生成器同一版）。
Result<std::vector<core::StackMapTableAttr::Frame>, LowerError>
relocateSmt(const core::StackMapTableAttr& smt,
            const std::unordered_map<ca::u32, ca::u32>& pcmap)
{
    using SmtFrame = core::StackMapTableAttr::Frame;
    using SmtVType = core::StackMapTableAttr::VType;
    std::vector<core::StackMapTableAttr::Frame> frames = smt.frames;
    // 1) 旧绝对 pc 链。
    std::vector<ca::u32> abs(frames.size(), 0);
    for (ca::usize i = 0; i < frames.size(); ++i) {
        abs[i] = i == 0 ? frames[i].offset : core::smtAbsPc(abs[i - 1], frames[i].offset);
    }
    // 2) 映射到新 pc。
    std::vector<ca::u32> fresh(frames.size(), 0);
    for (ca::usize i = 0; i < frames.size(); ++i) {
        const auto it = pcmap.find(abs[i]);
        if (it == pcmap.end()) {
            return Err(LowerError::RelocationBadTarget);
        }
        fresh[i] = it->second;
    }
    // 3) verification_type_info 的 Uninitialized(8) 载荷也是旧 code 中的
    //    指令偏移，必须和 frame 目标使用同一张 pcmap；Object(7) 的载荷则
    //    是常量池索引，不能误重定位。
    const auto relocateVType = [&](core::StackMapTableAttr::VType& v)
        -> Result<void, LowerError> {
        if (v.tag != SmtVType::kUninitialized) {
            return Ok();
        }
        const auto it = pcmap.find(v.cp);
        if (it == pcmap.end()) {
            return Err(LowerError::RelocationBadTarget);
        }
        v.cp = it->second;
        return Ok();
    };
    for (auto& f : frames) {
        for (auto& v : f.locals) {
            auto r = relocateVType(v);
            if (r.is_err()) {
                return Err(r.unwrap_err());
            }
        }
        for (auto& v : f.stack) {
            auto r = relocateVType(v);
            if (r.is_err()) {
                return Err(r.unwrap_err());
            }
        }
    }

    // 4) 重算 delta；紧凑形态的 delta 越界时升格为 extended 形态，仍在
    //    紧凑范围内时必须同步更新 type（编码器从 type 取 delta）。
    for (ca::usize i = 0; i < frames.size(); ++i) {
        if (i != 0 && fresh[i] <= fresh[i - 1]) {
            return Err(LowerError::RelocationBadTarget);
        }
        const ca::u32 nd = i == 0 ? fresh[0] : core::smtDelta(fresh[i - 1], fresh[i]);
        auto& f          = frames[i];
        if (nd > 0xFFFF) {
            return Err(LowerError::RelocationBadTarget);
        }
        if (f.type <= SmtFrame::kSameMax) {
            f.type = nd <= SmtFrame::kSameMax ? static_cast<ca::u8>(nd) : SmtFrame::kSameExt;
        } else if (f.type >= SmtFrame::kSame1Base && f.type <= SmtFrame::kSame1Max) {
            f.type = nd <= SmtFrame::kSameMax
                         ? static_cast<ca::u8>(SmtFrame::kSame1Base + nd)
                         : SmtFrame::kSame1Ext;
        }
        f.offset = nd;
    }
    return Ok(std::move(frames));
}

}   // namespace

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
    if (mu == nullptr || mu->code == nullptr || mu->backingIndex >= cf.methods.size()) {
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
        std::vector<core::StackMapTableAttr::Frame> frames;
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
    return Ok();
}

Result<void, LowerError> ClassSession::commitMethodMetadata(MethodId method)
{
    MethodUnit* mu = unit_.method(method);
    if (mu == nullptr || mu->backingIndex >= backing_->methods.size()) {
        return Err(LowerError::NotPromoted);
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
        // 直到这里才可能为空，说明模型被越权改坏——拒绝提交而非写入坏引用。
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
    return Ok();
}

bool hasCode(const ClassSession& s, ca::usize methodIdx) noexcept
{
    const core::ClassFile& cf = s.backing();
    if (methodIdx >= s.unit().methods.size() || methodIdx >= cf.methods.size()) {
        return false;
    }
    return findCodeSlot(cf, cf.methods[methodIdx]).first;
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
    if (methodIdx >= cf.methods.size()) {
        return false;
    }
    const auto [found, slotIdx] = findCodeSlot(cf, cf.methods[methodIdx]);
    if (!found) {
        return false;
    }
    core::AttrSlot& codeSlot = cf.methods[methodIdx].attrs[slotIdx];
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
            out.push_back(std::move(v));   // F1 纪律：真失败恒保留
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
