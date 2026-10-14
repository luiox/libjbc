/// @file        descriptor.hpp
/// @brief       JVM 类型/描述符系统：解析、类名手术坐标、槽宽与名字换算。
///
/// 描述符是跨 pass 的公共货币（rename 改描述符、xref 按描述符匹配、插桩按
/// 槽宽布置局部变量），本模块把它定型为一组纯函数：视图进、值出，零分配
/// 解析（视图指向输入串），只在实际产生新串的手术/换算入口才分配。
/// 语法真源 JVMS 4.3；旧库 Type/Remapper 的描述符知识由此接管，旧库保持
/// 冻结 oracle。
#pragma once

#include <string_view>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"
#include "libca/str/utf8_string.hpp"

namespace jbc::type {

using ca::core::Result;
using ca::core::Ok;
using ca::core::Err;

/// @brief 描述符解析/手术失败原因。
enum class DescError {
    Ok = 0,
    Empty,            ///< 输入为空串
    Truncated,        ///< 结构在闭合前结束（缺 ';' / 缺 ')' / 孤立 '['）
    TrailingChars,    ///< 结构闭合后仍有剩余字符
    BadBaseType,      ///< 非法基类型字符（含 V 出现在字段/参数位）
    BadClassName,     ///< L';' 之间为空或含保留字符（'.' '[' ';'）
    BadMethodShape,   ///< 方法描述符不以 '(' 开头
    NestingTooDeep,   ///< 数组维数超过 classfile 上限（255）
};

/// @brief 错误码的可读文本（诊断输出用）。
const char* to_string(DescError e) noexcept;

/// @brief 基类型原子（JVMS 4.3 的 B/C/D/F/I/J/S/Z）。
enum class BaseType : ca::u8 {
    Byte,     ///< B
    Char,     ///< C
    Double,   ///< D
    Float,    ///< F
    Int,      ///< I
    Long,     ///< J
    Short,    ///< S
    Boolean,  ///< Z
};

/// @brief 一个字段/参数/返回值类型（扁平表示：叶子 + 维数）。
///
/// 数组不递归：`[Ljava/lang/String;` = kind=Class + dims=1，`[[I` =
/// kind=Primitive(Int) + dims=2。视图指向输入描述符串，输入失效即失效。
struct TypeDesc
{
    enum class Kind : ca::u8 { Primitive, Class };

    Kind        kind       = Kind::Primitive;
    BaseType    primitive  = BaseType::Int;   ///< kind==Primitive 时有效
    std::string_view class_name;              ///< kind==Class：内部名（'/' 分隔）
    ca::u32     array_dims = 0;               ///< 前置 '[' 维数
};

/// @brief 解析一个字段类型描述符（JVMS 4.3.2）。输入必须恰好是完整类型；
///        V 不是字段类型（只在方法返回位出现），在此返回 Err。
Result<TypeDesc, DescError> parseFieldType(std::string_view s) noexcept;

/// @brief 方法描述符（JVMS 4.3.3）：参数类型视图指向输入串。
struct MethodDesc
{
    std::vector<TypeDesc> params;  ///< 各参数类型（顺序保持）
    bool                  ret_void = false;  ///< 返回类型为 V
    TypeDesc              ret;               ///< ret_void==false 时有效
};

/// @brief 解析一个方法描述符。
Result<MethodDesc, DescError> parseMethodDescriptor(std::string_view s);

/// @brief 描述符内一个类内部名的位置（rename 手术坐标，不含 L 与 ';'）。
struct ClassNameSpan
{
    ca::u32 offset;  ///< 相对输入描述符串的字节偏移
    ca::u32 length;  ///< 内部名长度
};

/// @brief 列出描述符里全部类内部名的位置。方法/字段描述符自动分派
///        （以 '(' 开头视为方法描述符）；语法非法即 Err，不产出半份数据。
Result<std::vector<ClassNameSpan>, DescError>
classNameSpans(std::string_view descriptor);

/// @brief 把描述符里 span 指向的类名改写为 new_name，产出新描述符串。
///        span 越界或 new_name 非法内部名即 Err；其余字节原样保留。
Result<ca::str::Utf8String, DescError>
replaceClassName(std::string_view descriptor, const ClassNameSpan& span,
                 std::string_view new_name);

/// @brief 类型占几个局部变量/操作数栈槽（long/double = 2，其余 = 1）。
ca::u32 slotWidth(const TypeDesc& t) noexcept;

/// @brief 方法参数共占多少槽（不含 this；静态/实例由调用方自行加一）。
ca::u32 paramSlotCount(const MethodDesc& m) noexcept;

/// @brief 内部名合法性：非空且不含 '.' '[' ';'（'/' 是合法分隔符）。
bool isValidInternalName(std::string_view name) noexcept;

/// @brief 内部名（a/b/C）→ 二进制名（a.b.C）。机械换算，不校验语法。
ca::str::Utf8String internalToBinary(std::string_view internal_name);

/// @brief 二进制名（a.b.C）→ 内部名（a/b/C）。机械换算，不校验语法。
ca::str::Utf8String binaryToInternal(std::string_view binary_name);

}   // namespace jbc::type
