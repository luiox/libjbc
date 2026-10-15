/// @file        attr_codec.hpp
/// @brief       属性目录解析与属性内容的按需解码/编码（L2 lazy 的实现）。
///
/// @ref readAttrSlots 是整个 lazy 设计落地的地方，全文只有十几行：
/// 读到 `attribute_info { name_index, length }` 后，记下内容 span 然后
/// **skip(length)**——内容一个字节都不读，也不为它分配任何节点。
///
/// 与之对称，@ref encodeAttr 在未修改时把这段 span 原样 copy 回去。解析与写出
/// 因此都不需要理解属性内容，未知属性既不会被拦住，也不会被改坏。
#pragma once

#include <memory>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"

#include "jbc/core/attribute.hpp"
#include "jbc/core/bytes.hpp"
#include "jbc/core/class_file.hpp"
#include "jbc/core/error.hpp"

namespace jbc::core {

/// @brief 解析属性目录（eager），内容一律跳过（lazy）。
///
/// @param cur        定位在 attributes_count 上的游标
/// @param out        追加到的属性槽集合
/// @param diag       降级记录（属性长度越界等）
/// @param baseOffset 游标视图在源字节流中的绝对起始偏移。
///                   顶层解析传 0；解析 Code 属性内的嵌套属性时必须传该 Code
///                   内容在源流中的偏移——span 一律是绝对偏移，写回时按它直接
///                   取字节，混用相对偏移会静默取到错误的字节。
///
/// 长度声明越出剩余字节时返回 Err(BadAttributeLength)：此时无法确定后续结构
/// 边界，继续解析只会得到错误的目录，宁可失败也不猜。
Result<void, ReadError> readAttrSlots(Cursor& cur,
                                      std::vector<AttrSlot>& out,
                                      Diagnostics& diag,
                                      ca::u32 baseOffset = 0);

/// @brief 按需解码一个属性槽的内容（ClassFile::decodeAttr 的实现）。
///
/// 已知属性（v0.1 仅 Code）走对应解码器；其余一律解为 @ref RawAttr 字节副本。
/// 解码成功即把结果挂上槽并推进状态到 Decoded。
Result<AttrBody*, ReadError> decodeAttrBody(ClassFile& cf, AttrSlot& slot);

/// @brief 递归解码全部属性内容（含 Code 内的嵌套属性）。
///
/// 常规路径不需要它——lazy 的意义就是不解。它的用途是自测与「整体重编码」准备：
/// 解完之后每个槽都有 body，但仍未置脏，写回依旧走 copy-through。
Result<void, ReadError> decodeAllAttrs(ClassFile& cf);

/// @brief InnerClasses 属性体的 typed 直读入口（不触碰槽状态；decodeAttrBody
///        的分派面同走本解码）。
///
/// 形状与既有 typed 解码同铁律：半截体 Err、trailing 保留、span 无效 Err。
Result<std::unique_ptr<AttrBody>, ReadError> decodeInnerClassesBody(ClassFile& cf,
                                                                    const AttrSlot& slot);

/// @brief EnclosingMethod 属性体的 typed 解码（契约同 @ref decodeInnerClassesBody）。
Result<std::unique_ptr<AttrBody>, ReadError> decodeEnclosingMethodBody(ClassFile& cf,
                                                                       const AttrSlot& slot);

/// @brief Exceptions 属性体的 typed 解码（契约同 @ref decodeInnerClassesBody）。
Result<std::unique_ptr<AttrBody>, ReadError> decodeExceptionsBody(ClassFile& cf,
                                                                  const AttrSlot& slot);

/// @brief 编码一个属性槽（含 name_index 与 length）。
///
/// - `state ∈ {Untouched, Decoded}` → 原样 copy 源 span（保真路径）
/// - `state ∈ {Modified, New}`      → 按 body 的 kind 重编码
///
/// 已修改但本版本没有编码器的属性返回 Err(NotModeled)：宁可写失败，也不生成
/// 一个看似完整但 JVM 行为未定义的 class。
Result<void, WriteError> encodeAttr(const ClassFile& cf,
                                    const AttrSlot& slot,
                                    ByteSink& out,
                                    ByteView source);

}   // namespace jbc::core
