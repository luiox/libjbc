/// @file        insn_codec.hpp
/// @brief       code 数组 ↔ 指令序列的纯函数编解码。
///
/// 约束与承诺：
///   - decode 对每个操作数做边界检查；遇到保留码/越界结构返回 Err，不猜。
///   - encode 是 decode 的精确逆：对未编辑的序列，字节必须逐位一致
///     （switch 的 4 字节对齐 padding 按目标 pc 重算，与源一致）。
///   - 本层不做任何「修改」——插入/删除/重定向属于 ir 层的 CodeDom。
///     在 code 字节上直接做 pass 是设计明确禁止的路径（design.md §15）。
#pragma once

#include <vector>

#include "libca/core/result.hpp"

#include "jbc/core/bytes.hpp"
#include "jbc/core/error.hpp"
#include "jbc/core/insn.hpp"

namespace jbc::core {

/// @brief 把 code 数组解码为指令序列（顺序即 pc 升序）。
///
/// 失败原因：Truncated（操作数越出数组）、UnsupportedFeature（保留码）、
/// BadBranchTarget（分支偏移指向数组外）。
Result<std::vector<RawInsn>, ReadError> decodeInsns(ByteView code);

/// @brief 单条指令在给定 pc 下的编码长度（switch 依赖 4 字节对齐）。
///
/// ir 层导出时需要先知道每条指令的新 pc 才能换算分支偏移；长度与操作数
/// 取值无关（不自动拓宽），因此一遍顺序推进即可完成定位。
ca::u32 rawInsnSize(const RawInsn& in, ca::u32 pc) noexcept;

/// @brief 把指令序列编码回 code 字节。
///
/// 指令按序列顺序紧密排布，pc 字段被重算并写回 insns（调用后可见）。
/// 分支偏移 i16 溢出返回 Err(Unencodable)——本层不做 goto→goto_w 自动拓宽，
/// 拓宽是 editor 的显式变换，静默换码会改变指令身份。
Result<std::vector<ca::u8>, WriteError> encodeInsns(std::vector<RawInsn>& insns);

}   // namespace jbc::core
