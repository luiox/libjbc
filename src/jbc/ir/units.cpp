/// @file        units.cpp
/// @brief       ClassSession 构建与方法级 promote/lower 实现。
#include "jbc/ir/units.hpp"

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
    case LowerError::CpAppendFailed: return "cp append failed";
    case LowerError::NotPromoted: return "not promoted";
    case LowerError::RelocationNotModeled: return "relocation not modeled";
    case LowerError::RelocationBadTarget: return "relocation bad target";
    case LowerError::InvalidHandle: return "invalid handle";
    case LowerError::InvalidSeed: return "invalid seed";
    case LowerError::NotModeled: return "not modeled";
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
    for (ca::usize i = 0; i < cf.fields.size(); ++i) {
        const core::MemberInfo& f = cf.fields[i];
        FieldUnit fu;
        fu.id           = u.makeFieldId(static_cast<ca::u32>(i));
        fu.name         = u.symbols().intern(cf.nameOf(f));
        fu.desc         = u.symbols().intern(cf.descOf(f));
        fu.access       = f.access;
        fu.backingIndex = i;
        u.fields.push_back(std::move(fu));
    }
    u.methods.reserve(cf.methods.size());
    for (ca::usize i = 0; i < cf.methods.size(); ++i) {
        const core::MemberInfo& m = cf.methods[i];
        MethodUnit mu;
        mu.id      = u.makeMethodId(static_cast<ca::u32>(i));
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

Result<std::shared_ptr<ClassSession>, PromoteError> makeSession(const NewClassSeed& seed)
{
    // 种子预检：任何越界/缺名都在建模型之前拒绝——不产出半截会话。
    if (seed.thisName.empty() || seed.thisName.size() > 0xFFFFu
        || seed.superName.size() > 0xFFFFu || seed.access > 0xFFFFu
        || seed.interfaces.size() > 0xFFFFu) {
        return Err(PromoteError::InvalidSeed);
    }
    for (std::string_view n : seed.interfaces) {
        if (n.empty() || n.size() > 0xFFFFu) {
            return Err(PromoteError::InvalidSeed);
        }
    }

    // 空类起步：CP 从零引导（池此刻为空，无去重顾虑，直接追加）。
    // this/super/接口的 Utf8 + Class 双条目落位后，backing 从第一刻起
    // 就是自洽模型（validClassReferences 通过，无源字节可写出）。
    auto backing = std::make_unique<core::ClassFile>();
    backing->major    = static_cast<ca::u16>(seed.version >> 16);
    backing->minor    = static_cast<ca::u16>(seed.version & 0xFFFFu);
    backing->access   = static_cast<ca::u16>(seed.access);
    const ca::u32 thisNameCp = backing->cp.addUtf8(seed.thisName);
    backing->thisClass = static_cast<ca::u16>(backing->cp.addClass(thisNameCp));
    if (!seed.superName.empty()) {
        const ca::u32 superNameCp = backing->cp.addUtf8(seed.superName);
        backing->superClass = static_cast<ca::u16>(backing->cp.addClass(superNameCp));
    }
    backing->interfaces.reserve(seed.interfaces.size());
    for (std::string_view n : seed.interfaces) {
        const ca::u32 nameCp = backing->cp.addUtf8(n);
        backing->interfaces.push_back(
            static_cast<ca::u16>(backing->cp.addClass(nameCp)));
    }

    // 既有构造路径接管：unit 的 version/access/this/super/接口/成员簿记
    // 全部从 backing 镜像（成员面为空），两套状态天然一致。
    auto made = makeSession(std::move(backing));
    if (made.is_ok()) {
        // E2 记账（§35.1）：从零建出 = 整类皆新，消费者按整类重建。
        ChangeRecord created;
        created.kind = ChangeKind::ClassCreated;
        made.unwrap()->noteChange(created);
    }
    return made;
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
    if (mu == nullptr) {
        // 死句柄（已删除/外来会话）与「方法存在但没有体」是两类失败：
        // 失效语义显式可判别，不混入 NoCode。
        return Err(PromoteError::InvalidHandle);
    }
    if (mu->backingIndex >= cf.methods.size()) {
        return Err(PromoteError::InvalidHandle);
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

/// TypeAnnotations 重定向（JVMS 4.7.20/4.7.21 的 Code 内挂载形态）：只动
/// 携带字节码偏移的目标——localvar/resourcevar 行（start/length 走与 LVT
/// 相同的 fail-closed 终点规则）与 offset/cast/type-arg 族（单偏移精确
/// 命中 pcmap）。catch 目标的 catch_index 指向异常表条目下标，lower 重建
/// 异常表保持条目顺序与条数，下标稳定，不动；type_param/supertype/
/// bound/empty/formals/throws 等类/方法级目标不携带 pc，同样不动。
/// 类型路径与元素值不含 pc。未知 target_type 在解码层已拒绝（Err
/// UnsupportedFeature），到不了这里。
///
/// Annotation 携带 unique_ptr 载荷不可拷贝，因此拆成「纯计算 plan +
/// 就地 apply」两步：plan 全量成功才允许 apply（all-or-nothing），
/// apply 只写标量字段，不会再失败。
struct TannAnnotationPlan
{
    bool hasOffset = false;   ///< offset/cast/type-arg 族：true 时 offset 需重写
    ca::u32 newOffset = 0;

    struct Row
    {
        ca::u32 start  = 0;
        ca::u32 length = 0;
    };
    std::vector<Row> rows;   ///< localvar 行：与 localVars 下标对齐
};

struct TannRelocation
{
    std::vector<TannAnnotationPlan> annos;   ///< 与 annotations 下标对齐
};

Result<TannRelocation, LowerError>
planTann(const core::TypeAnnotationsAttr& ta,
         const std::unordered_map<ca::u32, ca::u32>& pcmap,
         ca::u32 oldCodeLen,
         ca::u32 newCodeLen)
{
    using TA = core::TypeAnnotationsAttr;
    TannRelocation out;
    out.annos.reserve(ta.annotations.size());
    for (const TA::Annotation& a : ta.annotations) {
        TannAnnotationPlan pa;
        if (a.targetType == TA::kLocalVar || a.targetType == TA::kLocalVarResource) {
            pa.rows.resize(a.localVars.size());
            for (ca::usize i = 0; i < a.localVars.size(); ++i) {
                const TA::LocalVarEntry& row = a.localVars[i];
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
                pa.rows[i].start  = it->second;
                pa.rows[i].length = newEnd > it->second ? newEnd - it->second : 0;
            }
        } else if (a.targetType >= TA::kOffsetBase && a.targetType <= TA::kTypeArgMax) {
            // 0x43..0x46 offset、0x47 cast、0x48..0x4B type-arg：载荷首段
            // 都是旧 code 中的指令偏移（cast/type-arg 的后段是序号，不动）。
            const auto it = pcmap.find(a.offset);
            if (it == pcmap.end()) {
                return Err(LowerError::RelocationBadTarget);
            }
            pa.hasOffset = true;
            pa.newOffset = it->second;
        }
        out.annos.push_back(std::move(pa));
    }
    return Ok(std::move(out));
}

/// 按 plan 就地重写（只写标量字段；plan 已全量验证，本步不会再失败）。
void applyTann(core::TypeAnnotationsAttr& ta, const TannRelocation& plan)
{
    using TA = core::TypeAnnotationsAttr;
    for (ca::usize i = 0; i < ta.annotations.size() && i < plan.annos.size(); ++i) {
        TA::Annotation& a          = ta.annotations[i];
        const TannAnnotationPlan& pa = plan.annos[i];
        for (ca::usize j = 0; j < a.localVars.size() && j < pa.rows.size(); ++j) {
            a.localVars[j].startPc = pa.rows[j].start;
            a.localVars[j].length  = pa.rows[j].length;
        }
        if (pa.hasOffset) {
            a.offset = pa.newOffset;
        }
    }
}

/// CharacterRangeTable 重定向（javac -Xjcov 私有属性，Code 嵌套）：每行的
/// start_pc 与 end_pc 都是旧 code 中的指令起始 pc（end_pc 是 javac 写出的
/// entry.endPc-1，含末指令），各自精确命中 pcmap，miss 即 RelocationBadTarget
/// ——不存在「指向 code 长」的合法 miss（含末指令语义决定了它总是指令 pc）。
/// 字符区间与 flags 不携带 pc，原样保留。
struct CrtRelocation
{
    std::vector<ca::u32> starts;
    std::vector<ca::u32> ends;
};

Result<CrtRelocation, LowerError>
relocateCrt(const core::CharacterRangeTableAttr& crt,
            const std::unordered_map<ca::u32, ca::u32>& pcmap)
{
    CrtRelocation out;
    out.starts.reserve(crt.rows.size());
    out.ends.reserve(crt.rows.size());
    for (const auto& row : crt.rows) {
        const auto sit = pcmap.find(row.startPc);
        if (sit == pcmap.end()) {
            return Err(LowerError::RelocationBadTarget);
        }
        const auto eit = pcmap.find(row.endPc);
        if (eit == pcmap.end()) {
            return Err(LowerError::RelocationBadTarget);
        }
        out.starts.push_back(sit->second);
        out.ends.push_back(eit->second);
    }
    return Ok(std::move(out));
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
    // E2 记账（§35.1）：body 粒度以 lower 落盘点为准——code/异常表/maxes 任一
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
        // E2 记账（§35.1）：元数据粒度只在实改时记；改写前身份从 backing CP
        // 原样解析（append-only 契约保证旧条目仍在），仅身份实变（改名/换
        // 描述符）时携带——未改名（仅 access 变）保持空视图（§35.2 契约）。
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
    // E2 记账（§35.1）：类级粒度只在实改时记——CP 只追加且 intern 去重，同一
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
        // E2 记账：与 commitMethodMetadata 同构（实改才记；仅身份实变携带
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
    // E2 记账（§35.1）：挂入即新成员；身份视图取符号池（入参视图可能是暂态）。
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
    // E2 记账：与 addMethod 同构（视图取符号池）。
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
    // E2 记账（§35.1）：摘除即失成员；身份视图先取（符号池存储不受骨架摘除
    // 影响，视图在会话存活期内有效）。
    ChangeRecord removed;
    removed.kind   = ChangeKind::MemberRemoved;
    removed.member = MemberKind::Method;
    removed.name   = unit_.symbols().str(mu->name);
    removed.desc   = unit_.symbols().str(mu->desc);
    // 物理面：摘除 method_info，后续成员的 backingIndex 统一平移——身份
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
    // E2 记账：与 removeMethod 同构（视图先取，成功才落账）。
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
/// 收尾全量补校验，见 §34.3）。失败即 Err，调用方保证全有或全无（新成员
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
    // 目标校验会拒收——重放进入受控延后绑定构建态放行，收尾统一补验。
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
    // CodeEditor::loadConst 同一裁决——宽度陷阱在这里挡掉，不留给调用方）。
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
        // 全 miss 才是池结构坏——显式失败，不按普通指令静默搬运（那会把
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
                        // 语义 intern 面未覆盖，显式挂账（design.md §34）。
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

    // 收尾：退出受控延后绑定态，对全量存活指令补控制目标校验——统一改绑
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
