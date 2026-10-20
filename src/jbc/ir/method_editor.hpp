/// @file        method_editor.hpp
/// @brief       从零建方法的一步编辑面：Result-only 门面，不持有错误状态。
///
/// 从零建一个方法体原本要串 addMethod → promoteMethod → CodeEditor →
/// lowerMethod 四段接线。本文件只压缩接线层数，不改变错误模型：每个可
/// 失败操作仍然当场返回该段自己的 Result，没有任何跨调用保留的失败态，
/// 也没有「出错后静默 no-op」的对象状态。析构不自动 commit——析构函数
/// 无法汇报 LowerError，自动提交会把错误塞进第二条通道。
#pragma once

#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/error.hpp"
#include "jbc/ir/units.hpp"

namespace jbc::ir {

/// @brief 从零建方法的失败记录：段 + 该段原始错误码。
///
/// 与 SessionError 同构：横跨多个错误族的入口如实保留「段 + 码」，不做
/// 跨族有损映射，也不新增错误枚举。本结构只作为返回值载荷出现一次，不
/// 驻留任何对象——它不是第二条汇报通道。
struct MethodBuildError
{
    enum class Stage : ca::u8 {
        Add,      ///< addMethod 失败（lower 字段有效）
        Promote,  ///< promoteMethod 失败（promote 字段有效）
        Edit,     ///< 指令写入/体规格失败（edit + insnIndex 有效）
        Lower,    ///< lowerMethod 失败（lower 字段有效）
    };

    Stage stage = Stage::Add;
    LowerError lower = LowerError::Ok;        ///< stage == Add/Lower 时有效
    PromoteError promote = PromoteError::Ok;  ///< stage == Promote 时有效
    EditError edit = EditError::Ok;           ///< stage == Edit 时有效
    ca::u32 insnIndex = 0;                    ///< stage == Edit 时有效（0 基）

    MethodBuildError() = default;
    MethodBuildError(Stage s, LowerError e) noexcept : stage(s), lower(e) {}
    MethodBuildError(Stage s, PromoteError e) noexcept : stage(s), promote(e) {}
    MethodBuildError(Stage s, EditError e, ca::u32 index) noexcept
        : stage(s), edit(e), insnIndex(index)
    {}
};

const char* to_string(MethodBuildError::Stage s) noexcept;

/// @brief 新建方法并打开编辑面：addMethod + promoteMethod 的一次接线。
///
/// - seed.withCode 必须为 true（abstract/native 没有方法体，不属于本入口）；
///   为 false 时 Err(Stage::Add, InvalidSeed)，不挂入成员。
/// - open 的 add/promote 失败回滚已挂入成员（先算后写纪律）；commit 失败
///   原样返回 LowerError，不摘成员也不回退编辑。CP 追加不回退（常量池
///   只增是会话既有语义）。
/// - 编辑面本身无错误状态：append 失败当场返回 Err，编辑器继续可用。
/// - 成员在 open 时即挂入会话；未 commit 即析构 = 编辑丢弃，成员以空体
///   留在会话，收尸（removeMethod）归调用方。commit 才把编辑写回 backing。
class MethodEditor
{
public:
    static Result<MethodEditor, MethodBuildError> open(ClassSession& s, const MethodSeed& seed);

    MethodId method() const noexcept { return id_; }

    /// @brief 与 CodeEditor::append 同签名同语义：失败当场返回，不污染编辑器。
    Result<InsnId, EditError> append(Insn in) { return code().append(std::move(in)); }

    /// @brief 全量编辑面（标签/分支/switch/批量删除等）。
    ///
    /// 每次返回一个新的轻量视图，全部指向同一个 CodeUnit。
    CodeEditor code() noexcept { return CodeEditor{*unit_}; }

    CodeUnit& unit() noexcept { return *unit_; }
    const CodeUnit& unit() const noexcept { return *unit_; }

    /// @brief 显式终点：lowerMethod。失败返回原始 LowerError，不回滚。
    Result<void, LowerError> commit() { return lowerMethod(*session_, id_); }

private:
    MethodEditor(std::shared_ptr<ClassSession> session, MethodId id, CodeUnitRef unit) noexcept
        : session_(std::move(session))
        , id_(id)
        , unit_(std::move(unit))
    {}

    std::shared_ptr<ClassSession> session_;
    MethodId id_;
    CodeUnitRef unit_;
};

/// @brief 直线体一步入口：addMethod → promote → 逐条 append → lower。
///
/// `body` 是平铺指令序列，不能携带控制流目标（target/targets/keys）——
/// 标签只能在 CodeUnit 的 CodeDom 上创建/绑定，带分支的体请用
/// MethodEditor 的 code() 全量面。违反时 Err(Stage::Edit, BadOperand,
/// insnIndex)，在挂入成员之前就拒绝。
///
/// 逐条 append 遇到第一个错误立即返回（同步折叠，调用结束即清除），不是
/// 跨调用的 sticky 状态。任一步失败都回滚成员；CP 追加不回退。
/// maxStack/maxLocals 由 seed 给定，本入口不代算。
Result<MethodId, MethodBuildError>
addMethodWithBody(ClassSession& s, const MethodSeed& seed, const std::vector<Insn>& body);

Result<MethodId, MethodBuildError>
addMethodWithBody(ClassSession& s, const MethodSeed& seed, std::initializer_list<Insn> body);

}   // namespace jbc::ir
