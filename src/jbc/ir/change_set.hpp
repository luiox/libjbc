/// @file        change_set.hpp
/// @brief       ChangeSet 协议：会话变更收集与按粒度查询。
///
/// 消费者（引用索引/摘要/调用图）在一次会话/一批变换后按粒度决定失效重建
/// 范围，而不是每次全量重扫。会话的各变更通道在实改发生时向本会话的
/// ChangeSet 记账；查询面给三类出口：类级（classChange）、成员级存活折叠
/// （memberChanges）、成员级消失（removedMembers）。身份坐标是文本三段
/// （指向会话驻留存储，只增不移），不是会话内 MethodId/FieldId。
#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

#include "libca/core/datatype.hpp"

namespace jbc::ir {

/// @brief 变更记录种类（粒度分级）。
enum class ChangeKind : ca::u8 {
    ClassCreated,    ///< 会话从零建出（makeSession(NewClassSeed)）：整类皆新
    ClassMetadata,   ///< 类级元数据提交且实改（version/access/this/super/interfaces）
    MemberAdded,     ///< 成员挂入（addMethod/addField；adopt 两入口经它们记账）
    MemberRemoved,   ///< 成员摘除（removeMethod/removeField）
    MemberMetadata,  ///< 成员名/描述符/访问标志提交且实改（commitXxxMetadata）
    MethodBody,      ///< 方法体写回且实改（code/异常表/maxes 任一；copy-through 不记）
};

/// @brief 变更涉及的成员族；类级记录为 None。
enum class MemberKind : ca::u8 {
    None,
    Method,
    Field,
};

/// @brief 类级变更汇总（查询出口）：Created 优先于 Metadata。
enum class ClassChange : ca::u8 {
    None,
    Created,
    Metadata,
};

/// @brief 一条变更记录。
///
/// 身份视图指向会话驻留存储（SymbolPool arena / backing CP arena，只增不移），
/// 存活期 = 会话——消费者必须在会话存活期内完成折叠与 intern。
struct ChangeRecord
{
    ChangeKind kind = ChangeKind::MemberAdded;
    MemberKind member = MemberKind::None;
    /// @brief 成员身份。MemberRemoved = 被删身份；MemberMetadata = 改写后身份。
    std::string_view name;
    std::string_view desc;
    /// @brief 仅 MemberMetadata：改写前身份。未改名（仅 access 变）时为空视图。
    std::string_view priorName;
    std::string_view priorDesc;
};

/// @brief 折叠后的成员级条目：窗口结束时仍存活的被记账成员。
struct MemberChange
{
    MemberKind member = MemberKind::Method;
    std::string_view name;   ///< 最终身份
    std::string_view desc;
    bool created        = false;  ///< 最终这一代是本窗口挂入（add/adopt）
    bool bodyChanged    = false;  ///< 本窗口内有方法体写回实改
    bool metadataChanged = false; ///< 本窗口内有元数据提交实改
};

/// @brief 窗口结束时已消失的成员身份（含改名让出的旧身份）。
struct RemovedMember
{
    MemberKind member = MemberKind::Method;
    std::string_view name;
    std::string_view desc;
};

/// @brief 单会话变更台账：追加序原始记录 + 按身份键折叠的查询视图。
///
/// 累积与折叠语义：键 = (member, name, desc)，会话内成员唯一性预检
/// 保证任一时刻每键至多一个存活成员，事件序按终态折叠——add 后 remove 再 add
/// 终态存活（created 置位，中间代历史不外泄）；改名折作「旧身份消失事件 +
/// 新身份元数据事件」，改名目标继承原代的 created 旗标。同一键按终态只落
/// memberChanges/removedMembers 一个桶。
class ChangeSet
{
public:
    /// @brief 追加一条记录（库内记账通道：ClassSession 通道与 units.cpp 的
    ///        lowerMethod 调用；消费者只读查询面，不伪造记录）。
    void record(ChangeRecord r) { records_.push_back(r); }

    /// @brief 窗口内无任何实改（查询面三出口全空 ⇔ 本值为真）。
    bool empty() const noexcept { return records_.empty(); }
    /// @brief 原始记录条数。
    ca::usize size() const noexcept { return records_.size(); }

    /// @brief 原始追加序记录（审计面）。
    const std::vector<ChangeRecord>& records() const noexcept { return records_; }

    /// @brief 类级变更汇总；从零建出（Created）优先于元数据实改（Metadata）。
    ClassChange classChange() const noexcept;

    /// @brief 终态存活的被记账成员（首次触碰序输出）。
    std::vector<MemberChange> memberChanges() const;

    /// @brief 终态消失的成员身份（首次触碰序输出）。
    std::vector<RemovedMember> removedMembers() const;

private:
    std::vector<ChangeRecord> records_;
};

}   // namespace jbc::ir
