/// @file        reader.hpp
/// @brief       classfile 骨架解析入口（L0/L1 eager，L2 lazy）。
///
/// @ref readClass 只解析「必须解析才能定位结构」的部分：头、常量池、成员的
/// access/name/descriptor，以及属性**目录**。属性内容一律只记 span 并跳过，
/// 因此解析一个类的代价与它有多少方法体无关——这是全量扫描场景的全部收益来源。
///
/// 失败的语义是「无法确定后续结构边界」，不是「输入不合法」：畸形输入的降级
/// 结果落在 @ref ParseState::Recovered 与 @ref Diagnostics 里，而不是异常。
#pragma once

#include <memory>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"

#include "jbc/core/bytes.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/error.hpp"

namespace jbc::core {

/// @brief 读取选项。
struct ReadOptions
{
    /// @brief 保留源字节副本——lazy 解码与 copy-through 保真的共同前提。
    ///
    /// 开启（默认）时字节被复制进模型，解析产出的一切字节视图（Utf8 条目、属性
    /// span）都指向模型内部，与调用方的 buffer 生命周期解耦；调用方可以在
    /// @ref readClass 返回后立即释放入参。
    ///
    /// 关掉它可省一份与输入等大的内存，代价有三：属性无法按需解码、写回被
    /// 拒绝（@ref WriteError::MissingSource）、且所有字节视图改为借用入参
    /// buffer——调用方必须保证它在模型存活期内有效。扫描场景若只看 summary
    /// 且能管住 buffer 生命周期，关掉是合理选择。
    bool preserveSourceBytes = true;

    /// @brief 主版本号上限；超出只降级记录（warning），不拒绝读取。
    ca::u16 maxSupportedMajor = 66;  // Java 22
};

/// @brief 读取产物：模型 + 状态 + 诊断。
///
/// 诊断非空不代表失败——Recovered 模型照样可用、照样能原样写回。
struct ReadResult
{
    std::unique_ptr<ClassFile> file;
    ParseState state = ParseState::Validated;
    Diagnostics diagnostics;
};

/// @brief 解析一个 class 的骨架。
/// @param bytes 输入字节；在 @ref ReadOptions::preserveSourceBytes 下会被复制进模型，
///              调用方可在返回后立即释放。
Result<ReadResult, ReadError> readClass(ByteView bytes, const ReadOptions& opt = ReadOptions{});

}   // namespace jbc::core
