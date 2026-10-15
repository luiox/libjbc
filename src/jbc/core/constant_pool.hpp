/// @file        constant_pool.hpp
/// @brief       常量池：零拷贝 UTF8 视图 + 只追加 + 原样 span 写回。
///
/// 三条设计决定，各对应旧库的一个痛点：
///
/// 1. **UTF8 零拷贝**——Utf8 条目只记 {指针, 长度} 指向源字节流，解析不复制、
///    不解码。扫描几万个类时属性名/类名的比较走 @ref utf8Equals 的 memcmp，
///    既不分配也不算码点。需要文本语义时调用方再显式构造 Utf8StringRef。
/// 2. **只追加**——新条目一律 append，已有 index 永不改变。旧库里「重排 CP
///    导致存量引用失效」这一类问题从结构上不存在，未知属性里的 CP 引用因此
///    天然保持有效。
/// 3. **原样 span 写回**——每个条目记住自己在源字节流中的编码范围 @ref Entry::raw，
///    未修改的条目写回就是 copy 这段字节。保真不依赖重编码正确。
///
/// 字符串的存储分两路，各自匹配一种生命周期：
///
/// ```text
/// 解析来的 Utf8（绝大多数） → 零拷贝视图，指向模型的源字节副本
/// 变换中新增的 Utf8         → intern 进 Utf8StringArena（去重 + 块分配）
/// ```
///
/// 不把所有 Utf8 都 intern 一遍的理由：classfile 的常量池**本身就是一张去重表**，
/// 再在 arena 里去重是冗余的，而解析期复制会毁掉「扫描不付费」的性质。arena
/// 只承担它真正擅长的那段——新增串的去重与批量回收。两条路径的数据都随
/// ClassFile 一起死，没有第二个死亡点。
#pragma once

#include <string_view>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"
#include "libca/str/utf8_string.hpp"
#include "libca/str/utf8_string_arena.hpp"

#include "jbc/core/bytes.hpp"
#include "jbc/core/error.hpp"

namespace jbc::core {

class ByteSink;
class Cursor;

/// @brief 常量池 tag（JVMS 4.4；值为 classfile 编码值）。
enum class CpTag : ca::u8 {
    Utf8 = 1,
    Integer = 3,
    Float = 4,
    Long = 5,
    Double = 6,
    Class = 7,
    String = 8,
    Fieldref = 9,
    Methodref = 10,
    InterfaceMethodref = 11,
    NameAndType = 12,
    MethodHandle = 15,
    MethodType = 16,
    Dynamic = 17,
    InvokeDynamic = 18,
    Module = 19,
    Package = 20,
};

/// @brief Long/Double 占两个 CP 槽位（JVMS 4.4.5）；第二个槽位不可用。
bool cpTagIsWide(CpTag tag) noexcept;

/// @brief 单类常量池的唯一所有者。
///
/// 条目 0 是占位槽（classfile 的 CP 索引从 1 起），@ref at 与 @ref valid 以
/// 真实索引寻址。解析出的条目默认干净（dirty=false）；任何 setter 置脏后，
/// 写回走重编码而非 copy span。
class ConstantPool
{
public:
    /// @brief 单个 CP 条目。
    ///
    /// 字段按 tag 解释：Class/String/MethodType → a；Fieldref/Methodref/
    /// InterfaceMethodref/Dynamic/InvokeDynamic/NameAndType → a,b；
    /// MethodHandle → a(reference_kind), b(reference_index)；Integer/Float
    /// → ival；Long/Double → lval；Utf8 → utf8/utf8Len。
    struct Entry
    {
        CpTag tag = CpTag::Utf8;
        ca::u32 a = 0;  ///< 第一个 u2 操作数（多为 name_index / class_index）
        ca::u32 b = 0;  ///< 第二个 u2 操作数（多为 descriptor_index / name_and_type_index）
        ca::i32 ival = 0;
        ca::i64 lval = 0;

        /// @brief UTF8 内容视图：指向源字节副本（解析来的）或 arena_（追加的），均不拥有。
        const ca::u8* utf8 = nullptr;
        ca::u32 utf8Len = 0;

        /// @brief utf8 是否位于 @ref arena_ 内（即本条目是变换中新增的）。
        ///        用于归属断言：解析来的条目为 false，新增的为 true。
        bool utf8InArena = false;

        /// @brief 本条目在源字节流中的原始编码范围（dirty 时无意义）。
        ByteSpan raw;
        bool dirty = false;  ///< 被修改过 → 写回必须重编码

        /// @brief 是 Long/Double 的第二个槽位（JVMS 4.4.5 规定其不可用）。
        bool placeholder = false;
    };

    ConstantPool() { entries_.emplace_back(); }  // 0 号占位

    // ---- 解析 ----

    /// @brief 从游标解析整个常量池；失败即 Err，不产生半截状态。
    Result<void, ReadError> parse(Cursor& cur, Diagnostics& diag);

    // ---- 查询（全部 O(1)，零分配）----

    /// @brief 条目总数（含 0 号占位），等于 classfile 的 constant_pool_count。
    ca::u32 size() const noexcept { return static_cast<ca::u32>(entries_.size()); }

    /// @brief 索引是否有效（在范围内、非 0 号占位、非 wide 条目的续槽）。
    bool valid(ca::u32 i) const noexcept;

    /// @brief 校验常量池内部的索引与 tag 关系。
    Result<void, ReadError> validateReferences(Diagnostics& diag) const;

    /// @brief 只读访问；越界返回静态无效条目（全零、不可写）。
    ///
    /// @warning 返回的 Entry& 是 vector 内部存储：下一次 append（intern 家族 /
    ///          addXxx）可能触发扩容搬迁，届时旧引用悬垂——跨追加请只持 u32
    ///          索引，用前重新 at()。改写字段请走 @ref mutateAt（置脏入口）。
    const Entry& at(ca::u32 i) const noexcept;

    /// @brief 可变访问；越界返回静态无效条目。**不置脏**。
    ///
    /// writeClass 只重编码 dirty 条目，未脏条目一律 copy 源 span——经本函数
    /// 改字段而忘记置脏，写回会静默还原为源字节（copy-through 的既定语义，
    /// 不是 bug，但是消费者陷阱）。语义改写一律走 @ref mutateAt；本重载仅供
    /// 「遍历中需要可变视图、脏标记由改写点统一处理」的场合使用。
    /// @warning 生命周期约束同 const 版：Entry& 不得跨 append 存活。
    Entry& at(ca::u32 i) noexcept;

    /// @brief 改写条目的唯一规范入口：取引用并置脏（与 AttrSlot::mutate 的
    ///        「唯一修改入口且顺手置脏」对称）。
    ///
    /// 经本入口改写字段后，writeClass 自动走重编码而非 copy span。越界时
    /// 返回静态无效条目（写入被丢弃，与 at() 同款探测语义）。
    Entry& mutateAt(ca::u32 i) noexcept;

    CpTag tagOf(ca::u32 i) const noexcept;

    bool isUtf8(ca::u32 i) const noexcept { return valid(i) && entries_[i].tag == CpTag::Utf8; }

    /// @brief Utf8 条目的内容视图（借用，零分配）；非 Utf8 返回空。
    std::string_view utf8Of(ca::u32 i) const noexcept;

    /// @brief Utf8 条目与字面量的逐字节比较（扫描热路径：不分配、不计码点）。
    bool utf8Equals(ca::u32 i, std::string_view lit) const noexcept;

    /// @brief Utf8 条目是否以字面量开头（属性名前缀匹配用）。
    bool utf8StartsWith(ca::u32 i, std::string_view prefix) const noexcept;

    /// @brief 按内容查找 Utf8 条目（O(n) 逐条 memcmp）；未命中返回 0。
    ///
    /// 与 @ref addUtf8 配对构成「命中既有池返回原索引（不膨胀），miss 才
    /// 追加」的 intern 纪律（对齐 §22.4 语义 intern 家族）；0 是占位槽索引，
    /// 天然充当哨兵——没有任何真实条目会命中它。
    ca::u32 findUtf8(std::string_view s) const noexcept;

    /// @brief Utf8 条目的文本视图（项目约定的字符串类型，零拷贝）。
    ///
    /// 与 @ref utf8Of 是同一段字节的两种表示：前者给字节比较的热路径，
    /// 这个给需要 libca 字符串语义的场合（格式化、拼接、跨库传递）。
    ca::str::Utf8StringRef utf8Ref(ca::u32 i) const noexcept;

    // ---- 只追加构造 ----

    /// @brief 追加 Utf8 条目；内容 intern 进本池的 arena，index 一经分配不变。
    ///
    /// modified UTF-8 走 `intern_raw`（跳过 UTF-8 校验、码点数取保守值），
    /// 这是 libca 为 JVM 字节码字符串载体准备的入口。同名串复用同一份存储。
    ca::u32 addUtf8(std::string_view s);

    ca::u32 addClass(ca::u32 nameCp);
    ca::u32 addNameAndType(ca::u32 nameCp, ca::u32 descCp);
    ca::u32 addString(ca::u32 utf8Cp);
    ca::u32 addInteger(ca::i32 v);
    ca::u32 addFloatBits(ca::u32 bits);
    ca::u32 addLong(ca::i64 v);
    ca::u32 addDoubleBits(ca::u64 bits);
    ca::u32 addRef(CpTag tag, ca::u32 classCp, ca::u32 nameAndTypeCp);

    /// @brief 追加 MethodHandle（JVMS 4.4.8）：kind 为 reference_kind（1..9），
    ///        refCp 指向 Fieldref/Methodref/InterfaceMethodref。
    ca::u32 addMethodHandle(ca::u32 kind, ca::u32 refCp);

    /// @brief 追加 MethodType（JVMS 4.4.9）：descCp 指向方法描述符 Utf8。
    ca::u32 addMethodType(ca::u32 descCp);

    /// @brief 追加 Dynamic（JVMS 4.4.10）：bsmIndex 是 BootstrapMethods 属性的
    ///        条目下标（不是 CP 索引），nameAndTypeCp 指向 NameAndType。
    ca::u32 addDynamic(ca::u32 bsmIndex, ca::u32 nameAndTypeCp);

    /// @brief 追加 InvokeDynamic（JVMS 4.4.10）：参数语义与 @ref addDynamic 同。
    ca::u32 addInvokeDynamic(ca::u32 bsmIndex, ca::u32 nameAndTypeCp);

    /// @brief 追加 Module/Package（JVMS 4.4.11）：nameCp 指向 Utf8
    ///        （module-info 专用形态）。
    ca::u32 addModule(ca::u32 nameCp);
    ca::u32 addPackage(ca::u32 nameCp);

    /// @brief 追加起点：写回时 [1, restorePoint) 可 copy 原字节，之后须重编码。
    ca::u32 restorePoint() const noexcept { return restorePoint_; }

    // ---- 字符串归属（调试期防线）----

    /// @brief 全部「新增」Utf8 视图是否都确实指向本池的 arena。
    ///
    /// 这是把「新增串的生命周期」从注释约定变成可断言的事实：视图若指向临时
    /// 对象或已回收的存储，本检查会失败。debug 构建里调用它，等于给字符串
    /// 悬垂上了一道结构性防线，而不是靠人记得。
    bool arenaOwnsAllAppended() const noexcept;

    /// @brief arena 中唯一字符串数（去重效果可观测）。
    ca::usize internedCount() const noexcept { return arena_.size(); }

    /// @brief arena 已分配的 chunk 总容量（含未用空间）。
    ca::usize arenaBytes() const noexcept { return arena_.total_bytes(); }

    // ---- 写出 ----

    /// @brief 编码整个常量池（含 count）；未脏条目 copy 原 span，脏条目重编码。
    Result<void, WriteError> encode(ByteSink& out, ByteView source) const;

private:
    /// @brief 追加宽条目（Long/Double）：占两槽，续槽 tag 记为 Utf8 但 valid() 为假。
    ca::u32 appendWide(CpTag tag, ca::i64 lval);

    /// @brief 新增 Utf8 的存储：块链式扩展、地址稳定、内容去重。
    ///        与源字节副本同生共死（都是 ClassFile 的成员）。
    ca::str::Utf8StringArena arena_;
    std::vector<Entry> entries_;
    ca::u32 restorePoint_ = 0;
};

}   // namespace jbc::core
