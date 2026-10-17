/// @file        consumer.hpp
/// @brief       consumer 模块错误族：E-track 消费者协议面（E1/E3 + rename
///              跨类编排）的跨边界失败码。
///
/// design.md §9 把关系定死：libjbc 只提供最小 source/scan 协议，其余交给
/// 上层消费方（ResourceContainer / ClassIndex / remapper）。本模块承载：
///   - E3  只读面：@ref IClassSource + MemoryClassSource + SummaryScanner
///         （design.md §9 草图的 libjbc 本体态：文本形态、无 KeyPool）；
///   - E1  导出面：@ref ReferenceScanner（直译字节码的引用事实族）；
///   - rename 跨类编排：@ref RenamePlan + @ref IRenameResolver +
///         applyClassRename / renameSource——计划 formation 属消费方域，
///         逐类应用与 fail-closed 驱动归本模块。
///
/// 失败纪律（§29 family 一致）：错误 = 闭式枚举 + to_string，不带文本
/// message；跨族不加映射。NotFound fail-closed（取类缺失明 Err，不默
/// 空）；BadClassFile 承 core ReadError 的下位归属（结构损坏在 reader
/// 已分族，跨边界只留一个档位）。
#pragma once

namespace jbc::consumer {

/// @brief consumer 模块错误码（跨边界唯一族）。
enum class ConsumerError {
    Ok = 0,
    NotFound,      ///< 源里没有该类（fail-closed 缺失，不默空）
    BadClassFile,  ///< class 字节解码失败（readClass / 会话构造被拒）
    RenameRule,    ///< 改名计划不合法（坏内部名/重复契约/BSM 触达 v1 不支持）
    RenameFailed,  ///< 单类改名应用失败（族见 rename.hpp @ref RenameError）
    WriteFailed,   ///< writeClass 失败
};

const char* to_string(ConsumerError e) noexcept;

}   // namespace jbc::consumer
