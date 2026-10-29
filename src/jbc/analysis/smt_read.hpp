/// @file        smt_read.hpp
/// @brief       StackMapTable 读侧语义帧视图。
///
/// 从既有 StackMapTableAttr（core 层结构保真形态）链式推导逐目标完整帧：
/// same/same1(含 extended)/chop/append/full 形态展开 + smtAbsPc/smtDelta 链
/// 算术还原绝对 pc，交付与 FrameAt 同构的槽语义帧。不重跑数据流——纯读侧
/// 解码，用途定位是「读到的 SMT 与计算帧比对」（自校验、pass 预检），
/// 不是 FrameEngine 计算引擎的替代。
///
/// 入口是独立函数而非 FramePolicy::Preserve 的 compute 路由（选型）：
/// Preserve 的语义是「读属性」，compute 的契约是逐指令对齐的数据流（播种、
/// worklist、可达性），把读侧塞进 compute 会误导调用方以为帧是算出来的。
/// FrameEngine 选 Preserve 档 compute 仍显式 Err(NotSupported)。
#pragma once

#include <vector>

#include "libca/core/result.hpp"

#include "jbc/analysis/frame.hpp"
#include "jbc/core/attribute.hpp"
#include "jbc/ir/units.hpp"

namespace jbc::analysis {

using ca::core::Result;

/// @brief 读侧失败语义（防御面：每个码都是显式定义的行为）。
enum class SmtReadError : ca::u8 {
    NoCode,            ///< 方法没有 Code 属性（promote 过的 CodeUnit 不应出现）
    BadCpEntry,        ///< Object 条目的 CP 索引不指向可读的 Class 名
    FrameNotAligned,   ///< 帧目标 pc 不落在方法内任何指令上（链算术 miss）
    LocalsUnderflow,   ///< chop 超出链上既有 locals 条目（帧链不自洽）
    LocalsOverflow,    ///< 展开 locals 槽位数超过 CodeUnit.maxLocals
    StackOverflow,     ///< 展开栈槽位数超过 CodeUnit.maxStack
    UninitNotOnNew,    ///< tag-8 载荷不落在 new 指令 pc（与写侧  同规）
    BadFramePayload,   ///< 形态标签与载荷矛盾（same 带栈项 / append 条数不符等）
};

const char* to_string(SmtReadError e) noexcept;

/// @brief 一个 SMT 帧目标的读侧视图：绝对 pc + 命中指令 + 到达帧。
///
/// frame 与 FrameEngine 交付的 FrameAt 同构（槽语义：cat2 = 值槽 + Top 垫底
/// 槽， 交付口径）——读↔算对拍两侧同形，逐槽可比。reachable 恒 true：
/// 属性声明即到达（verifier 只要求可达目标有帧）；死代码判定归计算侧。
struct SmtFrameView
{
    ca::u32 pc = 0;               ///< 帧目标的绝对 pc（链式算术还原）
    ir::InsnId at = ir::kNoInsn;  ///< 命中的指令（对拍定位 / 诊断）
    FrameAt frame;                ///< 该目标的到达帧（槽语义、自持有）
};

/// @brief 读取方法既有 StackMapTable 并展开为逐目标语义帧（显式 API）。
///
/// - 链头（隐式初始帧）= @ref initialLocalsOf（this 形态 + 描述符参数，与
///   引擎 MethodSig 播种同源）；链算术在 SMT 条目空间进行（chop/append 按
///   条目计数），**不按 maxLocals 预垫 Top**——预垫会把 append 载荷推到
///   垫顶之后、错位全部槽号（成文）。
/// - 交付：每个帧目标一条 SmtFrameView，帧序 = 属性帧序（pc 升序）。属性
///   缺席（无控制流 / 全死目标 / 版本 < 50 等合法缺席）返回 Ok(空)——缺席
///   原因的判定归调用方（生成器的目标集逻辑在 build 侧）。
/// - 防御：chop 下溢、locals/stack 槽位数超出 CodeUnit.maxes、
///   tag-8 载荷不落 new、Object 条目 CP 不可读、形态标签与载荷矛盾、目标
///   pc 不对齐指令，一律显式 Err——「无法保真则报错」，不猜。
/// - 惰性解码：查找 Code/SMT 属性会触发 lazy decode（Decoded 态缓存，非
///   语义修改）；VType.name 是借用视图，寿命 = 会话常量池。
Result<std::vector<SmtFrameView>, SmtReadError>
readFramesFromSmt(ir::ClassSession& s, const ir::CodeUnit& u);

}   // namespace jbc::analysis
