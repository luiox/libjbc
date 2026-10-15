/// @file        cp_compact.hpp
/// @brief       A8：CP 显式压缩逃生口（design.md §36）。
///
/// CP 只追加 + 大量改名/搬移会把池撑大逼近 u16 上限；本入口按消费者显式调用
/// 做一次「可达闭包 + 规范 intern 去重 + 清死」的全量重索引，重写类内全部
/// CP 引用面。压缩是类级后置步骤，不集成进 lower（§36 与 §33 正交：一个管
/// CP 坐标、一个管 pc 坐标）。
///
/// 封闭集铁律（§36.1）：可压缩类是封闭集——全部属性可结构化重写时才可用；
/// 封闭集之外显式 Err 带肇事属性名，不静默丢、不猜。清单与算法契约成文于
/// design.md §36，本头文件只放消费者可见的错误面。
#pragma once

#include <string_view>

#include "libca/core/result.hpp"

#include "jbc/ir/units.hpp"

namespace jbc::ir {

/// @brief 压缩失败原因（§36.1 封闭集的显式失败面）。
enum class CompactError {
    Ok = 0,
    AttrNotModeled,  ///< 属性在压缩封闭集之外（未知/厂商属性，体内索引不可定位）
    MalformedAttr,   ///< 封闭集内属性的结构畸形（长度不符/截断/解码失败）
    Internal,        ///< 应用相遇到计划相未预见的失败（实现缺陷，报告给作者）
};

/// @brief 压缩失败记录：错误码 + 定位。
///
/// @ref attrName 仅在 AttrNotModeled/Malformed 时有效——带定位的拒绝面，
/// 消费者能直接点名是哪个属性挡住了压缩。视图指向会话驻留存储（旧池退居
/// 存储 / 源字节副本），存活期 = 会话（§35.2 同款纪律）。
struct CompactFailure
{
    CompactError code = CompactError::Ok;
    std::string_view attrName;   ///< 肇事属性名（视图存活期 = 会话）
};

/// @brief 压缩会话的常量池（A8 入口，design.md §36）。
///
/// 全有或全无：计划相完成封闭集校验、引用面遍历与映射构建（任何 Err 不留
/// 半截模型，写回字节与进入前逐字节一致）；应用相一次性换池 + 重写全部引用
/// 面（类头/成员槽/属性名/typed 体/压缩侧五件/指令操作数/升格 CodeUnit）。
///
/// 行为细节：
/// - 池已紧凑（映射恒等且无死条目）时整体 no-op：不换池、不改槽、不记账。
/// - 成功（非 no-op）向会话 ChangeSet 记一条 ClassMetadata——压缩改写全类
///   CP 坐标，消费者按「重扫整类」失效（§36.3 记账理由成文）。
/// - 已升格方法的 Insn.cp / 异常表 catchCp 由本入口同步重写，语义 intern
///   索引强制重建；旧池退居会话侧，§35.2 文本视图存活期不被破坏。
/// - 指令编码形态不收缩（LDC_W 不降格 LDC；§36.2 挂账），只改操作数字面。
Result<void, CompactFailure> compactConstantPool(ClassSession& s);

const char* to_string(CompactError e) noexcept;

}   // namespace jbc::ir
