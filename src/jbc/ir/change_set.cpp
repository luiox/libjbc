/// @file        change_set.cpp
/// @brief       ChangeSet 折叠查询实现（语义见 ）。
#include "jbc/ir/change_set.hpp"

#include <cstddef>
#include <string>
#include <unordered_map>

namespace jbc::ir {

namespace {

/// 折叠键：成员族 + 身份两段（内容寻址，非指针相等）。
struct FoldKey
{
    MemberKind member = MemberKind::Method;
    std::string_view name;
    std::string_view desc;

    bool operator==(const FoldKey& o) const noexcept
    {
        return member == o.member && name == o.name && desc == o.desc;
    }
};

struct FoldKeyHash
{
    ca::usize operator()(const FoldKey& k) const noexcept
    {
        std::hash<std::string_view> h;
        const ca::usize a = h(k.name);
        const ca::usize b = h(k.desc);
        return a ^ (b + ca::usize{0x9e3779b97f4a7c15ULL} + (a << 6) + (a >> 2))
               ^ static_cast<ca::usize>(k.member);
    }
};

/// 按键维护的折叠状态（语义见  的事件规则）。
struct FoldState
{
    bool present        = false;
    bool everRecorded   = false;
    bool created        = false;
    bool bodyChanged    = false;
    bool metadataChanged = false;
    bool everRemoved    = false;
};

using FoldMap = std::unordered_map<FoldKey, FoldState, FoldKeyHash>;

/// 取键状态；首次出现时登记进触碰序表（发射按此序，输出确定）。
FoldState& stateFor(FoldMap& fold, std::vector<FoldKey>& touchOrder, const FoldKey& k)
{
    const auto it = fold.find(k);
    if (it != fold.end()) {
        return it->second;
    }
    touchOrder.push_back(k);
    return fold.emplace(k, FoldState{}).first->second;
}

/// 成员级记录的事件折叠：类级记录忽略（classChange 单独出口）。
///
/// MemberMetadata 携带改写前身份：prior 与新身份不同时折作两个原子事件——
/// 先对 prior 发消失事件（旧身份的事实必须失效），再对新身份发元数据事件，
/// created 旗标随「代」迁移（改名不改变这一代是否窗口内挂入）。
void foldRecord(FoldMap& fold, std::vector<FoldKey>& touchOrder, const ChangeRecord& r)
{
    switch (r.kind) {
    case ChangeKind::MemberAdded: {
        FoldState& st = stateFor(fold, touchOrder, FoldKey{r.member, r.name, r.desc});
        st.present = true;
        st.everRecorded = true;
        st.created = true;
        st.bodyChanged = false;
        st.metadataChanged = false;
        break;
    }
    case ChangeKind::MemberRemoved: {
        FoldState& st = stateFor(fold, touchOrder, FoldKey{r.member, r.name, r.desc});
        st.present = false;
        st.everRecorded = true;
        st.everRemoved = true;
        break;
    }
    case ChangeKind::MemberMetadata: {
        const bool hasRename = !r.priorName.empty()
                               && !(r.priorName == r.name && r.priorDesc == r.desc);
        if (hasRename) {
            FoldState& old = stateFor(fold, touchOrder,
                                      FoldKey{r.member, r.priorName, r.priorDesc});
            const bool wasCreated = old.created;
            old.present = false;
            old.everRecorded = true;
            old.everRemoved = true;
            FoldState& st = stateFor(fold, touchOrder, FoldKey{r.member, r.name, r.desc});
            st.present = true;
            st.everRecorded = true;
            st.metadataChanged = true;
            st.created = wasCreated;
        }
        else {
            FoldState& st = stateFor(fold, touchOrder, FoldKey{r.member, r.name, r.desc});
            st.present = true;
            st.everRecorded = true;
            st.metadataChanged = true;
        }
        break;
    }
    case ChangeKind::MethodBody: {
        FoldState& st = stateFor(fold, touchOrder, FoldKey{r.member, r.name, r.desc});
        st.present = true;
        st.everRecorded = true;
        st.bodyChanged = true;
        break;
    }
    case ChangeKind::ClassCreated:
    case ChangeKind::ClassMetadata:
        break;
    }
}

}   // namespace

ClassChange ChangeSet::classChange() const noexcept
{
    bool created = false;
    bool metadata = false;
    for (const ChangeRecord& r : records_) {
        if (r.kind == ChangeKind::ClassCreated) {
            created = true;
        }
        else if (r.kind == ChangeKind::ClassMetadata) {
            metadata = true;
        }
    }
    if (created) return ClassChange::Created;
    if (metadata) return ClassChange::Metadata;
    return ClassChange::None;
}

std::vector<MemberChange> ChangeSet::memberChanges() const
{
    FoldMap fold;
    std::vector<FoldKey> touchOrder;
    fold.reserve(records_.size() * 2);
    for (const ChangeRecord& r : records_) {
        foldRecord(fold, touchOrder, r);
    }
    std::vector<MemberChange> out;
    for (const FoldKey& k : touchOrder) {
        const FoldState& st = fold.find(k)->second;
        if (!st.present || !st.everRecorded) {
            continue;
        }
        MemberChange m;
        m.member          = k.member;
        m.name            = k.name;
        m.desc            = k.desc;
        m.created         = st.created;
        m.bodyChanged     = st.bodyChanged;
        m.metadataChanged = st.metadataChanged;
        out.push_back(m);
    }
    return out;
}

std::vector<RemovedMember> ChangeSet::removedMembers() const
{
    FoldMap fold;
    std::vector<FoldKey> touchOrder;
    fold.reserve(records_.size() * 2);
    for (const ChangeRecord& r : records_) {
        foldRecord(fold, touchOrder, r);
    }
    std::vector<RemovedMember> out;
    for (const FoldKey& k : touchOrder) {
        const FoldState& st = fold.find(k)->second;
        if (st.present || !st.everRemoved) {
            continue;
        }
        RemovedMember m;
        m.member = k.member;
        m.name   = k.name;
        m.desc   = k.desc;
        out.push_back(m);
    }
    return out;
}

}   // namespace jbc::ir
