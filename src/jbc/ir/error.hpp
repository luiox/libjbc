/// @file        error.hpp
/// @brief       ir 层错误码：编辑、promote/lower 的失败原因。
#pragma once

namespace jbc::ir {

/// @brief 结构化编辑失败原因。
enum class EditError {
    Ok = 0,
    InvalidHandle,   ///< 句柄 owner/slot/generation 不符，或指向已删除槽位
    StillUsed,       ///< erase 目标存在 incoming use（分支/switch/边界引用）
    BadTarget,       ///< import 时分支偏移不落在任何指令起点
    Decode,          ///< 底层 code 数组解码失败（截断/保留码等）
    BranchOverflow,  ///< export 时 i16 分支偏移放不下（需显式 goto_w 拓宽）
    BadOperand,      ///< 操作数与指令形状不符（如非法局部变量索引）
};

/// @brief promote 失败原因（classfile 低层 → ir 高层）。
enum class PromoteError {
    Ok = 0,
    NoCode,            ///< 方法没有 Code 属性（abstract/native）
    AttrNotDecoded,    ///< Code 属性尚未解码
    BadInstructions,   ///< 指令序列导入失败（畸形 code 数组）
    NotModeled,        ///< 存在本版本不建模的结构，拒绝升格（该方法保持 raw）
};

/// @brief lower 失败原因（ir 高层 → classfile 低层）。
enum class LowerError {
    Ok = 0,
    BadInstructions,   ///< 导出失败（分支溢出/悬垂目标）
    BranchOverflow,    ///< 同 EditError::BranchOverflow 的 lower 视角
    CpAppendFailed,    ///< CP 只追加写入失败（容量/类型不符）
    NotPromoted,       ///< lower 的方法没有先 promote
    RelocationNotModeled,  ///< 布局改变且存在无 handler 的受影响属性——明确失败
    RelocationBadTarget,   ///< pcmap 无法精确重定向某个 pc 引用
};

const char* to_string(EditError e) noexcept;
const char* to_string(PromoteError e) noexcept;
const char* to_string(LowerError e) noexcept;

}   // namespace jbc::ir
