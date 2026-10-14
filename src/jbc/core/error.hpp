/// @file        error.hpp
/// @brief       错误码、解析状态与诊断收集。
///
/// 与旧库的三点分歧：
///   - 解析失败用返回值（Result）而非异常抛出，恶意输入是预期输入而非异常；
///   - @ref ParseState 描述「这个模型有什么能力」，不把输入武断判成合法/非法；
///   - 诊断是解析产物的一部分，不是 stderr 副作用——调用方决定怎么处理。
#pragma once

#include <vector>

#include "libca/core/datatype.hpp"

namespace jbc::core {

/// @brief 读取失败原因。
enum class ReadError {
    Ok = 0,
    Truncated,             ///< 字节不够读完一个结构
    BadMagic,              ///< 不是 0xCAFEBABE
    UnsupportedVersion,    ///< 主版本号超出可处理范围
    BadConstantPoolCount,  ///< constant_pool_count 为 0 或越界
    BadConstantPoolTag,    ///< 未知/非法的 CP tag
    BadConstantPoolRef,    ///< CP 索引越界或指向类型不符的条目
    BadUtf8,               ///< 修改后的 UTF8 编码非法
    BadAttributeLength,    ///< 属性长度声明越界
    BadMemberCount,        ///< fields/methods 数量声明越界
    BadCodeLength,         ///< code_length 为 0 或越界
    UnsupportedFeature,    ///< 已知但本版本不建模的结构
};

/// @brief 写出失败原因。
enum class WriteError {
    Ok = 0,
    MissingSource,        ///< 需要 copy-through 但没有源字节（未开启保留源字节）
    BadSpan,              ///< 模型的 span 越出源字节范围（内部不一致）
    Unencodable,          ///< 已修改的内容无法编码（如 CP 索引失效）
    NotModeled,           ///< 该属性已修改但本版本无编码器——拒绝写坏，不静默降级
};

/// @brief 模型的解析状态：描述能力，而非合法性。
///
/// 两态语义（design.md §4.3）：
/// - Validated：骨架与全部可见引用通过校验，且没有已知的不合规区段；
/// - Recovered：结构主体可读，但存在「结构可确定、语义不合规」的降级区段
///   （trailing 同族等），降级事实记录在 Diagnostics 里，模型仍可原样写回。
///
/// 结构损坏（边界无法确定）不走本枚举：reader 阶段一律 Err（design.md §27.3），
/// 不产出半截模型——「降级」与「拒绝」两类不可混用。
enum class ParseState {
    Validated,  ///< 全部必要结构通过校验，可安全做任何变换
    Recovered,  ///< 主体可读，部分区段为已保留的降级事实（仍可原样写回）
};

/// @brief 错误码的可读文本（诊断输出用）。
const char* to_string(ReadError e) noexcept;

/// @brief 错误码的可读文本（诊断输出用）。
const char* to_string(WriteError e) noexcept;

/// @brief 解析状态的文本标签（测试断言与诊断输出用）。
const char* to_string(ParseState s) noexcept;

/// @brief 单条诊断：降级/异常的可检索记录。
struct Diagnostic
{
    enum class Level : ca::u8 { Note = 0, Warning = 1, Error = 2 };

    Level level = Level::Note;
    ReadError code = ReadError::Ok;
    ca::u32 offset = 0;  ///< 记录发生位置，便于回溯到源字节

    static Diagnostic note(ReadError c, ca::u32 off) noexcept
    {
        return Diagnostic{Level::Note, c, off};
    }
    static Diagnostic warning(ReadError c, ca::u32 off) noexcept
    {
        return Diagnostic{Level::Warning, c, off};
    }
    static Diagnostic error(ReadError c, ca::u32 off) noexcept
    {
        return Diagnostic{Level::Error, c, off};
    }

    bool isError() const noexcept { return level == Level::Error; }
};

/// @brief 诊断收集器：解析过程中的降级记录，非空不代表解析失败。
class Diagnostics
{
public:
    void add(Diagnostic d) { items_.push_back(d); }
    void note(ReadError c, ca::u32 off) { add(Diagnostic::note(c, off)); }
    void warn(ReadError c, ca::u32 off) { add(Diagnostic::warning(c, off)); }
    void error(ReadError c, ca::u32 off) { add(Diagnostic::error(c, off)); }

    bool empty() const noexcept { return items_.empty(); }
    ca::usize count() const noexcept { return items_.size(); }
    const std::vector<Diagnostic>& items() const noexcept { return items_; }

    /// @brief 是否存在 Error 级诊断（Warning 级表示已降级但结果可用）。
    bool hasError() const noexcept
    {
        for (const Diagnostic& d : items_) {
            if (d.isError()) {
                return true;
            }
        }
        return false;
    }

private:
    std::vector<Diagnostic> items_;
};

}   // namespace jbc::core
