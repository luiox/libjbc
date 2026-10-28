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
    BadRange,        ///< 区间端点不构成非空前向段，或搬移锚点落在段内
};

/// @brief promote 失败原因（classfile 低层 → ir 高层）。
enum class PromoteError {
    Ok = 0,
    NoCode,            ///< 方法没有 Code 属性（abstract/native）
    AttrNotDecoded,    ///< Code 属性尚未解码
    BadInstructions,   ///< 指令序列导入失败（畸形 code 数组）
    NotModeled,        ///< 存在本版本不建模的结构，拒绝升格（该方法保持 raw）
    InvalidHandle,     ///< 方法身份句柄无效（已删除或来自另一会话）
    InvalidSeed,       ///< 从零建类的种子不合法（空 this 名等）
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
    InvalidHandle,     ///< 成员身份句柄无效（已删除或来自另一会话）
    InvalidSeed,       ///< 新增成员的种子不合法（空名字/描述符、重名等）
    NotModeled,        ///< 搬移/挂入涉及本版本不建模的结构（indy、宽常量等）

    // ---- 类构建挂载面（attr_build：注解族合成/挂载，dev_plan 批 1）----
    AlreadyMounted,    ///< 单例属性重复挂载（替换走 decode + mutate 既有面）
    BadMount,          ///< 挂载点不接受该属性（如挂 Code 属性）或合成规格不合法
    DomainOverflow,    ///< 合成规格的行数域写不下（u1/u16 域宽，§27.2 同源）
    MalformedAttr,     ///< 既有属性槽解码失败/形与名不符（模型侧结构损坏）
};

const char* to_string(EditError e) noexcept;
const char* to_string(PromoteError e) noexcept;
const char* to_string(LowerError e) noexcept;

}   // namespace jbc::ir
