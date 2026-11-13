/// @defgroup core_writer 保真写回
///
/// 未触碰 copy 源 span、触碰重编码；契约见 doc:design-writeback。

/// @file        writer.hpp
/// @brief       classfile 保真写回。
///
/// 只有一种写回策略：**未触碰的 copy 源 span，触碰的重编码**。
///
/// 刻意不提供全部重编码模式。那种模式遇到未建模属性（模型里只有
/// RawAttr 字节、或干脆尚未解码）时，只能选择丢数据或猜语义，两者都会
/// 产出看似完整但 JVM 行为未定义的 class，正是旧库事故的来源之一。
/// 需要整体重编码的场合（例如自测重编码应字节相同），调用方先
/// decodeAllAttrs 再写回，代价与风险都显式。
///
/// 从零构建的模型（makeSession(NewClassSeed) 的产物）没有源字节，但其
/// 每个 CP 条目与属性槽都处在新编码态，不包含任何 copy-through 路径，
/// 照常写出。MissingSource 因此只在确实需要 copy 而源缺席时返回
/// （如关闭 preserveSourceBytes 读入后又写回），不再挡从零构建的合法形态。
#pragma once

#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"

#include "jbc/core/bytes.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/error.hpp"

namespace jbc::core {

/// @brief 写回一个 class 为 classfile 字节。
///
/// @return 失败原因见 @ref WriteError ；返回 Err 时输出字节一律作废，绝不产出
///         半成品 class。
Result<std::vector<ca::u8>, WriteError> writeClass(const ClassFile& cf);

}   // namespace jbc::core
