/// @file        code_dom.cpp
/// @brief       指令 DOM 实现：slot 存储、顺序序列、句柄校验与 codec 桥。
#include "jbc/ir/code_dom.hpp"

#include <algorithm>
#include <cassert>
#include <atomic>
#include <limits>
#include <unordered_map>

#include "jbc/core/insn_codec.hpp"

namespace jbc::ir {

namespace {
std::atomic<ca::u32> g_ownerCounter{1};
}

ca::u32 nextOwnerId() noexcept
{
    return g_ownerCounter.fetch_add(1, std::memory_order_relaxed);
}

const char* to_string(EditError e) noexcept
{
    switch (e) {
    case EditError::Ok: return "ok";
    case EditError::InvalidHandle: return "invalid handle";
    case EditError::StillUsed: return "still used";
    case EditError::BadTarget: return "bad target";
    case EditError::BranchOverflow: return "branch overflow";
    case EditError::BadOperand: return "bad operand";
    }
    return "?";
}

CodeDom::CodeDom() noexcept : owner_(nextOwnerId()) {}

CodeDom::CodeDom(CodeDom&& other) noexcept
    : owner_(other.owner_),
      slots_(std::move(other.slots_)),
      order_(std::move(other.order_)),
      labels_(std::move(other.labels_))
{
    // A moved-from DOM remains a valid, empty DOM.  Give it a fresh owner so
    // its future handles cannot collide with handles now owned by *this.
    other.owner_ = nextOwnerId();
    other.slots_.clear();
    other.order_.clear();
    other.labels_.clear();
}

CodeDom& CodeDom::operator=(CodeDom&& other) noexcept
{
    if (this != &other) {
        owner_  = other.owner_;
        slots_  = std::move(other.slots_);
        order_  = std::move(other.order_);
        labels_ = std::move(other.labels_);
        other.owner_ = nextOwnerId();
        other.slots_.clear();
        other.order_.clear();
        other.labels_.clear();
    }
    return *this;
}

auto CodeDom::slotOf(InsnId id) const noexcept -> const Slot*
{
    if (id.owner != owner_ || id.slot >= slots_.size()) {
        return nullptr;
    }
    const Slot& s = slots_[id.slot];
    if (!s.live || s.gen != id.gen) {
        return nullptr;
    }
    return &s;
}

auto CodeDom::slotOf(InsnId id) noexcept -> Slot*
{
    return const_cast<Slot*>(static_cast<const CodeDom*>(this)->slotOf(id));
}

auto CodeDom::labelSlotOf(LabelId id) const noexcept -> const LabelSlot*
{
    if (id.owner != owner_ || id.slot >= labels_.size()) {
        return nullptr;
    }
    const LabelSlot& s = labels_[id.slot];
    if (!s.live || s.gen != id.gen) {
        return nullptr;
    }
    return &s;
}

auto CodeDom::labelSlotOf(LabelId id) noexcept -> LabelSlot*
{
    return const_cast<LabelSlot*>(static_cast<const CodeDom*>(this)->labelSlotOf(id));
}

Result<void, EditError> CodeDom::checkHandles(const Insn& in) const noexcept
{
    const core::Shape sh = core::shapeOf(in.op);
    const bool isBranch = sh == core::Shape::Branch2 || sh == core::Shape::Branch4;
    const bool isSwitch = sh == core::Shape::TableSwitch || sh == core::Shape::LookupSwitch;
    // 受控延后绑定态（整体重放路径）：控制目标的锚定校验延后，由
    // endDeferredTargets 对全量存活指令补验；switch 形状校验不依赖锚定，
    // 两态一致保留。正常编辑路径防呆不变。
    if ((isBranch || isSwitch) && !deferredTargets_) {
        const auto validControlTarget = [&](LabelId label) {
            const LabelSlot* slot = labelSlotOf(label);
            return slot != nullptr && slot->anchor != kNoInsn
                && slotOf(slot->anchor) != nullptr;
        };
        if (!validControlTarget(in.target)) {
            return Err(EditError::InvalidHandle);
        }
    }
    if (sh == core::Shape::TableSwitch) {
        if (in.b < in.a) {
            return Err(EditError::BadOperand);
        }
        const ca::u64 count = static_cast<ca::u64>(
            static_cast<ca::i64>(in.b) - static_cast<ca::i64>(in.a)) + 1;
        if (count != in.targets.size()) {
            return Err(EditError::BadOperand);
        }
    } else if (sh == core::Shape::LookupSwitch && in.keys.size() != in.targets.size()) {
        return Err(EditError::BadOperand);
    }
    if (isSwitch && !deferredTargets_) {
        for (const LabelId& t : in.targets) {
            const auto validControlTarget = [&](LabelId label) {
                const LabelSlot* slot = labelSlotOf(label);
                return slot != nullptr && slot->anchor != kNoInsn
                    && slotOf(slot->anchor) != nullptr;
            };
            if (!validControlTarget(t)) {
                return Err(EditError::InvalidHandle);
            }
        }
    }
    return Ok();
}

void CodeDom::beginDeferredTargets() noexcept
{
    deferredTargets_ = true;
}

Result<void, EditError> CodeDom::endDeferredTargets()
{
    // 先退态再校验：失败面不留松弛态，DOM 回到正常编辑语义。
    deferredTargets_ = false;
    for (const InsnId id : order_) {
        const Slot* s = slotOf(id);
        if (s == nullptr) {
            continue;
        }
        auto check = checkHandles(s->insn);
        if (check.is_err()) {
            return Err(check.unwrap_err());
        }
    }
    return Ok();
}

Result<InsnId, EditError> CodeDom::appendSlot(Insn&& in)
{
    auto check = checkHandles(in);
    if (check.is_err()) {
        return Err(check.unwrap_err());
    }
    in.srcPc = kNoPc;   // 合成指令没有源位置；原指令的 srcPc 只来自 importFrom
    const ca::u32 slot = static_cast<ca::u32>(slots_.size());
    slots_.push_back(Slot{std::move(in), true, 0});
    const InsnId id{owner_, slot, 0};
    order_.push_back(id);
    return Ok(id);
}

Result<InsnId, EditError> CodeDom::append(Insn in)
{
    return appendSlot(std::move(in));
}

Result<InsnId, EditError> CodeDom::insertAt(InsnId at, Insn in, bool before)
{
    if (slotOf(at) == nullptr) {
        return Err(EditError::InvalidHandle);
    }
    auto check = checkHandles(in);
    if (check.is_err()) {
        return Err(check.unwrap_err());
    }
    in.srcPc = kNoPc;
    const ca::u32 slot = static_cast<ca::u32>(slots_.size());
    slots_.push_back(Slot{std::move(in), true, 0});
    const InsnId id{owner_, slot, 0};
    for (ca::usize i = 0; i < order_.size(); ++i) {
        if (order_[i] == at) {
            order_.insert(order_.begin() + static_cast<std::ptrdiff_t>(i) +
                              (before ? 0 : 1),
                          id);
            return Ok(id);
        }
    }
    // slotOf 已校验 at 存活，order 里必然找得到——防御性兜底。
    slots_.pop_back();
    return Err(EditError::InvalidHandle);
}

Result<InsnId, EditError> CodeDom::insertBefore(InsnId at, Insn in)
{
    return insertAt(at, std::move(in), true);
}

Result<InsnId, EditError> CodeDom::insertAfter(InsnId at, Insn in)
{
    return insertAt(at, std::move(in), false);
}

Result<InsnId, EditError> CodeDom::insertAtLabel(LabelId label, Insn in)
{
    const LabelSlot* labelSlot = labelSlotOf(label);
    if (labelSlot == nullptr) {
        return Err(EditError::InvalidHandle);
    }

    const InsnId anchor = labelSlot->anchor;
    // End is a real open boundary.  Appending at it leaves the boundary at
    // the new method end; rebinding it to the inserted instruction would
    // incorrectly turn the end range into a one-instruction range.
    if (anchor == kNoInsn) {
        return append(std::move(in));
    }
    Result<InsnId, EditError> inserted = insertBefore(anchor, std::move(in));
    if (inserted.is_err()) {
        return Err(inserted.unwrap_err());
    }

    // The label denotes the same non-end boundary, so after insertion it must
    // point at the newly inserted first instruction of that boundary.
    const InsnId id = inserted.unwrap();
    auto rebound = bindLabel(label, id);
    if (rebound.is_err()) {
        // bindLabel can only fail if the label was concurrently invalidated;
        // CodeDom is single-threaded, so this is defensive and should be
        // unreachable.  Keep the operation's error channel complete.
        (void)erase(id);
        return Err(rebound.unwrap_err());
    }
    return Ok(id);
}

namespace {

Result<void, EditError> moveInOrder(std::vector<InsnId>& order,
                                    InsnId id,
                                    InsnId at,
                                    bool after)
{
    ca::usize source = order.size();
    ca::usize target = order.size();
    for (ca::usize i = 0; i < order.size(); ++i) {
        if (order[i] == id) {
            source = i;
        }
        if (order[i] == at) {
            target = i;
        }
    }
    if (source == order.size() || target == order.size()) {
        return Err(EditError::InvalidHandle);
    }
    if (source == target) {
        return Ok();
    }

    const InsnId value = order[source];
    order.erase(order.begin() + static_cast<std::ptrdiff_t>(source));
    if (source < target) {
        --target;
    }
    ca::usize insertAt = after ? target + 1 : target;
    order.insert(order.begin() + static_cast<std::ptrdiff_t>(insertAt), value);
    return Ok();
}

}   // namespace

Result<void, EditError> CodeDom::moveBefore(InsnId id, InsnId at)
{
    if (slotOf(id) == nullptr || slotOf(at) == nullptr) {
        return Err(EditError::InvalidHandle);
    }
    return moveInOrder(order_, id, at, false);
}

Result<void, EditError> CodeDom::moveAfter(InsnId id, InsnId at)
{
    if (slotOf(id) == nullptr || slotOf(at) == nullptr) {
        return Err(EditError::InvalidHandle);
    }
    return moveInOrder(order_, id, at, true);
}

std::vector<InsnId> CodeDom::usersOf(InsnId id) const
{
    std::vector<InsnId> out;
    const auto targetsId = [&](LabelId label) {
        const LabelSlot* labelSlot = labelSlotOf(label);
        return labelSlot != nullptr && labelSlot->anchor == id;
    };
    for (const InsnId& uid : order_) {
        const Slot* s = slotOf(uid);
        if (s == nullptr) {
            continue;
        }
        const core::Shape sh = core::shapeOf(s->insn.op);
        if ((sh == core::Shape::Branch2 || sh == core::Shape::Branch4 ||
             sh == core::Shape::TableSwitch || sh == core::Shape::LookupSwitch) &&
            targetsId(s->insn.target)) {
            out.push_back(uid);
            continue;
        }
        if (sh == core::Shape::TableSwitch || sh == core::Shape::LookupSwitch) {
            for (const LabelId& t : s->insn.targets) {
                if (targetsId(t)) {
                    out.push_back(uid);
                    break;
                }
            }
        }
    }
    return out;
}

Result<void, EditError> CodeDom::erase(InsnId id)
{
    Slot* s = slotOf(id);
    if (s == nullptr) {
        return Err(EditError::InvalidHandle);
    }
    if (!usersOf(id).empty()) {
        return Err(EditError::StillUsed);
    }
    // 元数据边界标签（Boundary 角色）保护其锚点：异常表、调试范围等引用
    // 必须显式移动或销毁后才能删除锚点指令。历史控制流标签不保护锚点——
    // 字段改写后遗留的 ControlFlow 标签已经没有字段引用，不应阻止删除；
    // 其句柄此后解析为 InvalidHandle，新指令也无法引用它。
    for (const LabelSlot& label : labels_) {
        if (label.live && label.role == LabelRole::Boundary && label.anchor == id) {
            return Err(EditError::StillUsed);
        }
    }
    s->live = false;
    ++s->gen;
    for (ca::usize i = 0; i < order_.size(); ++i) {
        if (order_[i] == id) {
            order_.erase(order_.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    return Ok();
}

Result<void, EditError> CodeDom::eraseBatch(
    const std::vector<InsnId>& ids,
    const std::vector<std::pair<InsnId, InsnId>>& retarget)
{
    // ---- 校验（全部只读；任何失败在变更前返回，全有或全无）----
    std::vector<char> doomed(slots_.size(), 0);
    for (const InsnId& id : ids) {
        if (slotOf(id) == nullptr) {
            return Err(EditError::InvalidHandle);
        }
        doomed[id.slot] = 1;   // 重复句柄按并集处理
    }
    std::unordered_map<ca::u32, InsnId> reroute;   // from.slot → to
    for (const auto& move : retarget) {
        if (slotOf(move.first) == nullptr || doomed[move.first.slot] == 0) {
            return Err(EditError::InvalidHandle);   // 安排的 from 必须是批内成员
        }
        if (slotOf(move.second) == nullptr || doomed[move.second.slot] != 0) {
            return Err(EditError::InvalidHandle);   // to 必须是批外存活指令
        }
        reroute[move.first.slot] = move.second;
    }

    // 一趟扫序：存活指令面向批内目标的字段。与 usersOf 同规——字段锚定
    // 目标已死（gen 漂移后 slotOf 为 nullptr）不算引用；批内成员自己的
    // 字段不参与（同批消亡，引用随批消失）。
    struct PendingRewrite
    {
        InsnId owner;
        bool isDefault;   ///< true = branch/switch default 字段；false = case 表项
        ca::u32 caseIdx;
        InsnId to;
        LabelId old;
    };
    std::vector<PendingRewrite> plan;
    const auto scanField = [&](const InsnId& uid, const LabelId& field,
                               bool isDefault, ca::u32 caseIdx) -> bool {
        const LabelSlot* ls = labelSlotOf(field);
        if (ls == nullptr) {
            return true;
        }
        const InsnId anchor = ls->anchor;
        if (anchor == kNoInsn || slotOf(anchor) == nullptr || doomed[anchor.slot] == 0) {
            return true;   // 非批内目标（含死锚）：无关
        }
        const auto it = reroute.find(anchor.slot);
        if (it == reroute.end()) {
            return false;   // 未安排的存活引用 → StillUsed
        }
        plan.push_back(PendingRewrite{uid, isDefault, caseIdx, it->second, field});
        return true;
    };
    for (const InsnId& uid : order_) {
        Slot* s = slotOf(uid);
        if (s == nullptr || doomed[uid.slot] != 0) {
            continue;
        }
        const core::Shape sh = core::shapeOf(s->insn.op);
        const bool isBranch = sh == core::Shape::Branch2 || sh == core::Shape::Branch4;
        const bool isSwitch = sh == core::Shape::TableSwitch ||
                              sh == core::Shape::LookupSwitch;
        if (!isBranch && !isSwitch) {
            continue;
        }
        if (!scanField(uid, s->insn.target, true, 0)) {
            return Err(EditError::StillUsed);
        }
        if (isSwitch) {
            for (ca::u32 j = 0; j < s->insn.targets.size(); ++j) {
                if (!scanField(uid, s->insn.targets[j], false, j)) {
                    return Err(EditError::StillUsed);
                }
            }
        }
    }
    // Boundary 标签保护：异常表、调试范围等引用必须显式移动/销毁后才能
    // 删锚点（与单条 erase 同规；历史 ControlFlow 标签不保护）。
    for (const LabelSlot& label : labels_) {
        if (!label.live || label.role != LabelRole::Boundary || label.anchor == kNoInsn) {
            continue;
        }
        if (slotOf(label.anchor) != nullptr && doomed[label.anchor.slot] != 0) {
            return Err(EditError::StillUsed);
        }
    }

    // ---- 变更（校验全部通过）----
    // 改写计划落地：每字段一枚锚定 to 的新 ControlFlow 标签；被替换的旧
    // ControlFlow 标签记入 retired（与 replaceAllUses 同规，改写后销毁）。
    std::vector<LabelId> retired;
    for (const PendingRewrite& p : plan) {
        auto fresh = createLabel(p.to, LabelRole::ControlFlow);
        if (fresh.is_err()) {
            return Err(fresh.unwrap_err());
        }
        Slot* s = slotOf(p.owner);
        const LabelSlot* old = labelSlotOf(p.old);
        if (s == nullptr || old == nullptr) {
            return Err(EditError::InvalidHandle);   // 不可达：校验已过
        }
        if (p.isDefault) {
            s->insn.target = fresh.unwrap();
        }
        else {
            s->insn.targets[p.caseIdx] = fresh.unwrap();
        }
        if (old->role == LabelRole::ControlFlow) {
            retired.push_back(p.old);
        }
    }
    for (const LabelId label : retired) {
        (void)destroyLabel(label);
    }
    // 置死 + 单趟重建 order_（逐条版是每删一条扫一遍）。
    for (const InsnId& id : ids) {
        Slot* s = slotOf(id);
        if (s == nullptr) {
            continue;   // 重复句柄：已置死
        }
        s->live = false;
        ++s->gen;
    }
    order_.erase(std::remove_if(order_.begin(), order_.end(),
                                [&](const InsnId& x) { return doomed[x.slot] != 0; }),
                 order_.end());
    return Ok();
}

Result<void, EditError> CodeDom::replaceAllUses(InsnId from, InsnId to)
{
    if (slotOf(from) == nullptr || slotOf(to) == nullptr) {
        return Err(EditError::InvalidHandle);
    }
    if (from == to) {
        return Ok();
    }

    // Control-flow operands own their target labels.  Give each rewritten
    // operand a fresh label so a range/debug boundary that happens to share
    // the old instruction remains unchanged.  Imported control labels are
    // retired after the rewrite; ordinary labels created by callers remain
    // live and therefore continue to protect their anchor.
    std::vector<LabelId> retired;
    const auto retarget = [&](LabelId& field) -> Result<void, EditError> {
        const LabelSlot* old = labelSlotOf(field);
        if (old == nullptr || old->anchor != from) {
            return Ok();
        }
        const LabelId oldId = field;
        const LabelRole oldRole = old->role;
        auto fresh = createLabel(to, LabelRole::ControlFlow);
        if (fresh.is_err()) {
            return Err(fresh.unwrap_err());
        }
        field = fresh.unwrap();
        if (oldRole == LabelRole::ControlFlow) {
            retired.push_back(oldId);
        }
        return Ok();
    };

    for (const InsnId& uid : order_) {
        Slot* slot = slotOf(uid);
        if (slot == nullptr) {
            continue;
        }
        const core::Shape sh = core::shapeOf(slot->insn.op);
        const bool isBranch = sh == core::Shape::Branch2 || sh == core::Shape::Branch4;
        const bool isSwitch = sh == core::Shape::TableSwitch ||
                              sh == core::Shape::LookupSwitch;
        if (!isBranch && !isSwitch) {
            continue;
        }
        auto r = retarget(slot->insn.target);
        if (r.is_err()) {
            return Err(r.unwrap_err());
        }
        if (isSwitch) {
            for (LabelId& target : slot->insn.targets) {
                r = retarget(target);
                if (r.is_err()) {
                    return Err(r.unwrap_err());
                }
            }
        }
    }
    for (const LabelId label : retired) {
        (void)destroyLabel(label);
    }
    return Ok();
}

Result<LabelId, EditError> CodeDom::createLabel(InsnId anchor)
{
    return createLabel(anchor, LabelRole::Boundary);
}

Result<LabelId, EditError> CodeDom::createLabel(InsnId anchor, LabelRole role)
{
    if (anchor != kNoInsn && slotOf(anchor) == nullptr) {
        return Err(EditError::InvalidHandle);
    }
    const ca::u32 slot = static_cast<ca::u32>(labels_.size());
    labels_.push_back(LabelSlot{anchor, true, 0, role});
    return Ok(LabelId{owner_, slot, 0});
}

Result<void, EditError> CodeDom::bindLabel(LabelId label, InsnId anchor)
{
    LabelSlot* slot = labelSlotOf(label);
    if (slot == nullptr) {
        return Err(EditError::InvalidHandle);
    }
    if (anchor != kNoInsn && slotOf(anchor) == nullptr) {
        return Err(EditError::InvalidHandle);
    }
    slot->anchor = anchor;
    return Ok();
}

Result<void, EditError> CodeDom::destroyLabel(LabelId label)
{
    LabelSlot* slot = labelSlotOf(label);
    if (slot == nullptr) {
        return Err(EditError::InvalidHandle);
    }
    slot->live = false;
    ++slot->gen;
    slot->anchor = kNoInsn;
    return Ok();
}

Result<LabelTarget, EditError> CodeDom::resolveLabel(LabelId label) const
{
    const LabelSlot* slot = labelSlotOf(label);
    if (slot == nullptr) {
        return Err(EditError::InvalidHandle);
    }
    if (slot->anchor != kNoInsn && slotOf(slot->anchor) == nullptr) {
        return Err(EditError::InvalidHandle);
    }
    return Ok(slot->anchor == kNoInsn ? LabelTarget::end()
                                      : LabelTarget::before(slot->anchor));
}

bool CodeDom::validLabel(LabelId label) const noexcept
{
    return labelSlotOf(label) != nullptr;
}

std::vector<LabelId> CodeDom::labelsAt(InsnId anchor) const
{
    std::vector<LabelId> out;
    if (slotOf(anchor) == nullptr) {
        return out;
    }
    for (ca::usize i = 0; i < labels_.size(); ++i) {
        const LabelSlot& label = labels_[i];
        if (label.live && label.anchor == anchor) {
            out.push_back(LabelId{owner_, static_cast<ca::u32>(i), label.gen});
        }
    }
    return out;
}

const Insn* CodeDom::peek(InsnId id) const noexcept
{
    const Slot* s = slotOf(id);
    return s == nullptr ? nullptr : &s->insn;
}

Result<Insn*, EditError> CodeDom::get(InsnId id)
{
    Slot* s = slotOf(id);
    if (s == nullptr) {
        return Err(EditError::InvalidHandle);
    }
    return Ok(&s->insn);
}

Result<CodeDom, EditError> CodeDom::importFrom(core::ByteView code)
{
    auto dec = core::decodeInsns(code);
    if (dec.is_err()) {
        return Err(EditError::Decode);
    }
    std::vector<core::RawInsn> raw = std::move(dec).unwrap();

    CodeDom dom;
    // 第一遍：全部入槽（目标暂空），记 pc → 句柄。
    std::unordered_map<ca::u32, InsnId> byPc;
    byPc.reserve(raw.size() * 2);
    for (core::RawInsn& r : raw) {
        const ca::u32 slot = static_cast<ca::u32>(dom.slots_.size());
        Insn in;
        in.op    = r.op;
        in.wide  = r.wide;
        in.srcPc = r.pc;
        in.a     = r.a;
        in.b     = r.b;
        in.cp    = r.cp;
        dom.slots_.push_back(Slot{std::move(in), true, 0});
        const InsnId id{dom.owner_, slot, 0};
        dom.order_.push_back(id);
        byPc.emplace(r.pc, id);
    }
    // 第二遍：解析分支偏移为句柄。目标必须正好是某条指令的起点。
    const auto labelFor = [&](InsnId target) -> Result<LabelId, EditError> {
        if (target.owner != dom.owner_ || target.slot >= dom.slots_.size()) {
            return Err(EditError::InvalidHandle);
        }
        auto created = dom.createLabel(target, LabelRole::ControlFlow);
        if (created.is_err()) {
            return Err(created.unwrap_err());
        }
        return Ok(created.unwrap());
    };
    const ca::i64 end = static_cast<ca::i64>(code.size());
    for (ca::usize i = 0; i < raw.size(); ++i) {
        const core::RawInsn& r = raw[i];
        const core::Shape sh = core::shapeOf(r.op);
        const bool isBranch = sh == core::Shape::Branch2 || sh == core::Shape::Branch4;
        const bool isSwitch = sh == core::Shape::TableSwitch || sh == core::Shape::LookupSwitch;
        if (!isBranch && !isSwitch) {
            continue;
        }
        Insn& in = dom.slots_[dom.order_[i].slot].insn;
        const auto resolve = [&](ca::i32 off) -> Result<LabelId, EditError> {
            const ca::i64 dst = static_cast<ca::i64>(r.pc) + off;
            if (dst < 0 || dst >= end) {
                return Err(EditError::BadTarget);
            }
            const auto it = byPc.find(static_cast<ca::u32>(dst));
            if (it == byPc.end()) {
                return Err(EditError::BadTarget);
            }
            return labelFor(it->second);
        };
        auto t = resolve(r.t);
        if (t.is_err()) {
            return Err(t.unwrap_err());
        }
        in.target = t.unwrap();
        if (isSwitch) {
            if (sh == core::Shape::TableSwitch) {
                in.targets.resize(r.offs.size());
                for (ca::usize k = 0; k < r.offs.size(); ++k) {
                    auto tk = resolve(r.offs[k]);
                    if (tk.is_err()) {
                        return Err(tk.unwrap_err());
                    }
                    in.targets[k] = tk.unwrap();
                }
            } else {
                in.targets.resize(r.cases.size());
                in.keys.resize(r.cases.size());
                for (ca::usize k = 0; k < r.cases.size(); ++k) {
                    auto tk = resolve(r.cases[k].off);
                    if (tk.is_err()) {
                        return Err(tk.unwrap_err());
                    }
                    in.targets[k] = tk.unwrap();
                    in.keys[k]    = r.cases[k].key;
                }
            }
        }
    }
    return Ok(std::move(dom));
}

Result<std::vector<core::RawInsn>, EditError> CodeDom::exportRaw() const
{
    // 第一遍：pc 推进（长度与操作数取值无关，单遍即可）。
    std::unordered_map<ca::u32, ca::u32> pcOf;   // slot → 新 pc
    pcOf.reserve(slots_.size() * 2);
    ca::u32 pc = 0;
    for (const InsnId& id : order_) {
        const Slot* s = slotOf(id);
        if (s == nullptr) {
            return Err(EditError::InvalidHandle);
        }
        pcOf.emplace(id.slot, pc);
        // 尺寸只看 op/wide 与 case 数；用壳 RawInsn 复用同一实现，避免第二套
        // 尺寸表分叉。
        core::RawInsn sz;
        sz.op   = s->insn.op;
        sz.wide = s->insn.wide;
        if (core::shapeOf(s->insn.op) == core::Shape::TableSwitch) {
            sz.offs.resize(s->insn.targets.size());
        } else if (core::shapeOf(s->insn.op) == core::Shape::LookupSwitch) {
            sz.cases.resize(s->insn.targets.size());
        }
        pc += core::rawInsnSize(sz, pc);
    }

    // 第二遍：导出并换算偏移。
    std::vector<core::RawInsn> out;
    out.reserve(order_.size());
    for (const InsnId& id : order_) {
        const Slot* s = slotOf(id);
        if (s == nullptr) {
            return Err(EditError::InvalidHandle);
        }
        const Insn& in = s->insn;
        core::RawInsn r;
        r.op   = in.op;
        r.wide = in.wide;
        r.pc   = pcOf[id.slot];
        r.a    = in.a;
        r.b    = in.b;
        r.cp   = in.cp;

        const core::Shape sh = core::shapeOf(in.op);
        const bool isBranch = sh == core::Shape::Branch2 || sh == core::Shape::Branch4;
        const bool isSwitch = sh == core::Shape::TableSwitch || sh == core::Shape::LookupSwitch;
        if (isBranch || isSwitch) {
            if (sh == core::Shape::TableSwitch) {
                if (in.b < in.a) {
                    return Err(EditError::BadOperand);
                }
                const ca::u64 count = static_cast<ca::u64>(
                    static_cast<ca::i64>(in.b) - static_cast<ca::i64>(in.a)) + 1;
                if (count != in.targets.size()) {
                    return Err(EditError::BadOperand);
                }
            } else if (sh == core::Shape::LookupSwitch &&
                       in.keys.size() != in.targets.size()) {
                return Err(EditError::BadOperand);
            }
            const auto pcOfTarget = [&](LabelId label) -> Result<ca::u32, EditError> {
                const LabelSlot* labelSlot = labelSlotOf(label);
                if (labelSlot == nullptr || labelSlot->anchor == kNoInsn) {
                    return Err(EditError::InvalidHandle);
                }
                const auto it = pcOf.find(labelSlot->anchor.slot);
                if (it == pcOf.end()) {
                    return Err(EditError::InvalidHandle);
                }
                return Ok(it->second);
            };
            auto tp = pcOfTarget(in.target);
            if (tp.is_err()) {
                return Err(tp.unwrap_err());
            }
            const ca::i64 off = static_cast<ca::i64>(tp.unwrap()) - static_cast<ca::i64>(r.pc);
            const bool fits = sh == core::Shape::Branch2
                                  ? off >= -32768 && off <= 32767
                                  : off >= static_cast<ca::i64>(std::numeric_limits<ca::i32>::min()) &&
                                        off <= static_cast<ca::i64>(std::numeric_limits<ca::i32>::max());
            if (!fits) {
                return Err(EditError::BranchOverflow);
            }
            r.t = static_cast<ca::i32>(off);
            if (isSwitch) {
                if (sh == core::Shape::TableSwitch) {
                    r.offs.resize(in.targets.size());
                    for (ca::usize k = 0; k < in.targets.size(); ++k) {
                        auto kp = pcOfTarget(in.targets[k]);
                        if (kp.is_err()) {
                            return Err(kp.unwrap_err());
                        }
                        const ca::i64 off = static_cast<ca::i64>(kp.unwrap()) -
                                            static_cast<ca::i64>(r.pc);
                        if (off < static_cast<ca::i64>(std::numeric_limits<ca::i32>::min()) ||
                            off > static_cast<ca::i64>(std::numeric_limits<ca::i32>::max())) {
                            return Err(EditError::BranchOverflow);
                        }
                        r.offs[k] = static_cast<ca::i32>(off);
                    }
                } else {
                    r.cases.resize(in.targets.size());
                    for (ca::usize k = 0; k < in.targets.size(); ++k) {
                        auto kp = pcOfTarget(in.targets[k]);
                        if (kp.is_err()) {
                            return Err(kp.unwrap_err());
                        }
                        const ca::i64 off = static_cast<ca::i64>(kp.unwrap()) -
                                            static_cast<ca::i64>(r.pc);
                        if (off < static_cast<ca::i64>(std::numeric_limits<ca::i32>::min()) ||
                            off > static_cast<ca::i64>(std::numeric_limits<ca::i32>::max())) {
                            return Err(EditError::BranchOverflow);
                        }
                        r.cases[k] = core::SwitchCase{
                            in.keys[k],
                            static_cast<ca::i32>(off)};
                    }
                }
            }
        }
        out.push_back(std::move(r));
    }
    return Ok(std::move(out));
}

Result<std::vector<ca::u8>, EditError> CodeDom::encode() const
{
    auto raw = exportRaw();
    if (raw.is_err()) {
        return Err(raw.unwrap_err());
    }
    std::vector<core::RawInsn> insns = std::move(raw).unwrap();
    auto bytes = core::encodeInsns(insns);
    if (bytes.is_err()) {
        return Err(EditError::BranchOverflow);
    }
    return Ok(std::move(bytes).unwrap());
}

bool CodeDom::roundtripEquals(core::ByteView code) const
{
    auto raw = exportRaw();
    if (raw.is_err()) {
        return false;
    }
    std::vector<core::RawInsn> insns = std::move(raw).unwrap();
    auto bytes = core::encodeInsns(insns);
    if (bytes.is_err()) {
        return false;
    }
    const std::vector<ca::u8>& b = bytes.unwrap();
    if (b.size() != code.size()) {
        return false;
    }
    return std::equal(b.begin(), b.end(), code.data(), code.data() + code.size());
}

void CodeDom::acceptLoweredLayout(const std::vector<core::RawInsn>& raw) noexcept
{
    assert(raw.size() == order_.size());
    for (ca::usize i = 0; i < order_.size(); ++i) {
        Slot* slot = slotOf(order_[i]);
        assert(slot != nullptr);
        slot->insn.srcPc = raw[i].pc;
    }
}

}   // namespace jbc::ir
