/// @file        method_editor.cpp
/// @brief       MethodEditor / addMethodWithBody 实现。
#include "jbc/ir/method_editor.hpp"

namespace jbc::ir {
namespace {

/// 平铺体不允许携带控制流指令：标签只能在 CodeUnit 的 CodeDom 上创建与
/// 绑定，批量入口没有受理标签的位置。按指令形状判定而非「target 非空」——
/// 未绑定分支的目标恰好是 kNoLabel，按目标判会把坏体放行到 append 阶段。
/// 非控制流指令上残留的 targets/keys 同样视为脏形状。
bool hasControlFlow(const Insn& in) noexcept
{
    const core::Shape sh = core::shapeOf(in.op);
    if (sh == core::Shape::Branch2 || sh == core::Shape::Branch4
        || sh == core::Shape::TableSwitch || sh == core::Shape::LookupSwitch) {
        return true;
    }
    return !in.targets.empty() || !in.keys.empty();
}

/// 刚挂入的成员必然可摘除；回滚没有第二条补救通道，保持原始错误返回。
void rollbackMethod(ClassSession& s, MethodId id)
{
    (void)s.removeMethod(id);
}

}   // namespace

const char* to_string(MethodBuildError::Stage s) noexcept
{
    switch (s) {
    case MethodBuildError::Stage::Add: return "add";
    case MethodBuildError::Stage::Promote: return "promote";
    case MethodBuildError::Stage::Edit: return "edit";
    case MethodBuildError::Stage::Lower: return "lower";
    }
    return "?";
}

Result<MethodEditor, MethodBuildError> MethodEditor::open(ClassSession& s, const MethodSeed& seed)
{
    if (!seed.withCode) {
        return Err(MethodBuildError(MethodBuildError::Stage::Add, LowerError::InvalidSeed));
    }
    auto idR = s.addMethod(seed);
    if (idR.is_err()) {
        return Err(MethodBuildError(MethodBuildError::Stage::Add, idR.unwrap_err()));
    }
    const MethodId id = idR.unwrap();

    auto unitR = promoteMethod(s, id);
    if (unitR.is_err()) {
        rollbackMethod(s, id);
        return Err(MethodBuildError(MethodBuildError::Stage::Promote, unitR.unwrap_err()));
    }
    CodeUnitRef unit = std::move(unitR).unwrap();
    std::shared_ptr<ClassSession> keep = unit.session();
    return Ok(MethodEditor(std::move(keep), id, std::move(unit)));
}

Result<MethodId, MethodBuildError> addMethodWithBody(ClassSession& s, const MethodSeed& seed,
                                                     const std::vector<Insn>& body)
{
    if (!seed.withCode) {
        return Err(MethodBuildError(MethodBuildError::Stage::Add, LowerError::InvalidSeed));
    }
    for (ca::usize i = 0; i < body.size(); ++i) {
        if (hasControlFlow(body[i])) {
            return Err(MethodBuildError(MethodBuildError::Stage::Edit, EditError::BadOperand,
                                        static_cast<ca::u32>(i)));
        }
    }

    auto idR = s.addMethod(seed);
    if (idR.is_err()) {
        return Err(MethodBuildError(MethodBuildError::Stage::Add, idR.unwrap_err()));
    }
    const MethodId id = idR.unwrap();

    auto unitR = promoteMethod(s, id);
    if (unitR.is_err()) {
        rollbackMethod(s, id);
        return Err(MethodBuildError(MethodBuildError::Stage::Promote, unitR.unwrap_err()));
    }
    // CodeUnitRef 先于回滚释放：回滚会销毁刚 promote 的 CodeUnit，引用
    // 不得跨过 rollbackMethod 存活。
    bool editFailed = false;
    ca::u32 editIndex = 0;
    EditError editErr = EditError::Ok;
    {
        CodeUnitRef unit = std::move(unitR).unwrap();
        CodeEditor ed{*unit};
        for (ca::usize i = 0; i < body.size(); ++i) {
            auto r = ed.append(body[i]);
            if (r.is_err()) {
                editFailed = true;
                editErr    = r.unwrap_err();
                editIndex  = static_cast<ca::u32>(i);
                break;
            }
        }
    }
    if (editFailed) {
        rollbackMethod(s, id);
        return Err(MethodBuildError(MethodBuildError::Stage::Edit, editErr, editIndex));
    }

    auto lowerR = lowerMethod(s, id);
    if (lowerR.is_err()) {
        rollbackMethod(s, id);
        return Err(MethodBuildError(MethodBuildError::Stage::Lower, lowerR.unwrap_err()));
    }
    return Ok(id);
}

Result<MethodId, MethodBuildError> addMethodWithBody(ClassSession& s, const MethodSeed& seed,
                                                     std::initializer_list<Insn> body)
{
    return addMethodWithBody(s, seed, std::vector<Insn>(body));
}

}   // namespace jbc::ir
