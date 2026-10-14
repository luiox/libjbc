/// @file        insn.hpp
/// @brief       codec 层的指令值类型：字节格式的忠实表达。
///
/// @ref RawInsn 是「字节格式知识」的结构化投影——分支存原始相对偏移、switch
/// 存原始 case 表，pc 记录来源位置。它不懂句柄、不懂所有权，因此可以被自由
/// 拷贝；句柄化（可编辑 DOM）是 ir 层的事，两层之间用 import/export 转换。
#pragma once

#include <vector>

#include "libca/core/datatype.hpp"

#include "jbc/core/opcode.hpp"

namespace jbc::core {

/// @brief lookupswitch 的一个 case（match 值 + 偏移）。
struct SwitchCase
{
    ca::i32 key = 0;
    ca::i32 off = 0;
};

/// @brief 一条已解码指令。
///
/// 字段语义按 @ref shapeOf(op) 分派：
///   - Imm1/Imm2  ：a = 立即数
///   - Local1     ：a = 局部变量索引
///   - Iinc1      ：a = 索引，b = 常量
///   - CP*/Atype1 ：cp = CP 索引；a = atype/dims/count（按形状）
///   - Branch2/Branch4：t = 目标相对偏移
///   - TableSwitch：a = low，b = high，t = default 偏移，
///                 offs = 每个 case 的偏移（key 隐含为 low..high）
///   - LookupSwitch：t = default 偏移，cases = (key, off) 表
///
/// @ref wide 为 true 表示该指令源字节带 wide 前缀（op 已是修饰后的真实指令）。
/// 混淆器会用 wide 编码小索引，编码必须按此标志还原前缀，否则往返字节漂移。
struct RawInsn
{
    Op op = Op::NOP;
    bool wide = false;
    ca::u32 pc = 0;   ///< 指令首字节在 code 数组中的偏移（编码时重算）
    ca::i32 a = 0;
    ca::i32 b = 0;
    ca::i32 t = 0;
    ca::u32 cp = 0;
    std::vector<ca::i32> offs;
    std::vector<SwitchCase> cases;
};

}   // namespace jbc::core
