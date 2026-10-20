/// @file        jbc.hpp
/// @brief       libjbc 统一入口头。
///
/// core 层：单类 lazy classfile DOM（解析骨架 → 按需解码 → 保真写回）+ 指令
/// codec（code 数组 ↔ 指令序列，字节精确往返）。
/// ir 层：可编辑指令 DOM（owner/generation 句柄 + 有序序列）；高层
/// Unit/promote/帧引擎按建造顺序推进，见 doc_ai/design.md。
/// type 层：类型/描述符纯函数（解析/类名手术坐标/槽宽/名字换算）；泛型签名
/// 结构化树（JVMS 4.7.9.1 三形态 + 解析/反解/类名手术）。
/// analysis 层：域/帧引擎读面。consumer 层：E-track 消费者协议面（E1 引用
/// 导出 / E3 源与骨架只读面 / rename 跨类编排）。
#pragma once

#include "jbc/core/attr_codec.hpp"
#include "jbc/core/attribute.hpp"
#include "jbc/core/bytes.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/constant_pool.hpp"
#include "jbc/core/error.hpp"
#include "jbc/core/insn.hpp"
#include "jbc/core/insn_codec.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/core/writer.hpp"

#include "jbc/ir/change_set.hpp"
#include "jbc/ir/code_dom.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/cp_compact.hpp"
#include "jbc/ir/error.hpp"
#include "jbc/ir/method_editor.hpp"
#include "jbc/ir/sig_attr.hpp"
#include "jbc/ir/symbol_pool.hpp"
#include "jbc/ir/type_annotation_attr.hpp"
#include "jbc/ir/units.hpp"

#include "jbc/analysis/domain.hpp"
#include "jbc/analysis/frame.hpp"
#include "jbc/analysis/smt_build.hpp"
#include "jbc/analysis/smt_read.hpp"

#include "jbc/consumer/consumer.hpp"
#include "jbc/consumer/reference.hpp"
#include "jbc/consumer/rename.hpp"
#include "jbc/consumer/source.hpp"
#include "jbc/consumer/summary.hpp"

#include "jbc/type/descriptor.hpp"
#include "jbc/type/signature.hpp"

namespace jbc {

/// 库版本（语义版本字符串，随 dev_plan 的版本节点推进）。
const char* get_lib_version() noexcept;

}   // namespace jbc
