/// @file        class_file.hpp
/// @brief       单类 DOM：libjbc core 的模型本体。
///
/// 一个 @ref ClassFile 就是一个 class 的完整、可独立析构的所有者。它不持有
/// Jar、不持有 JDK、不持有全局索引——那些是上层消费方的事。
///
/// 模型关系是一条单纯的拥有链，没有 visitor，也不把控制流标签作为低层
/// classfile 对象图的一部分：
///
/// ```text
/// ClassFile ─owns─> ConstantPool / fields / methods / attrs / source bytes
/// MemberInfo ─owns─> AttrSlot[]
/// AttrSlot ─owns─> AttrBody?（lazy，未解码时为空）
/// CodeAttr ─owns─> code bytes / exception table / nested AttrSlot[]
/// ```
///
/// 跳转与异常范围在 classfile 层本来就是 pc 偏移（数字）；高层 IR 的
/// `LabelId`/边界句柄只在 promote 后出现，与低层模型的编码坐标分开。
#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"

#include "jbc/core/attribute.hpp"
#include "jbc/core/bytes.hpp"
#include "jbc/core/constant_pool.hpp"
#include "jbc/core/error.hpp"

namespace jbc::core {

/// @brief 字段或方法的成员条目。
///
/// field_info 与 method_info 的前三项同构（access/name/descriptor），差异只在
/// 属性集合，因此共用一个类型；语义由所在容器区分。
struct MemberInfo
{
    ca::u16 access = 0;
    ca::u32 nameCp = 0;  ///< Utf8 CP 索引
    ca::u32 descCp = 0;  ///< Utf8 CP 索引
    std::vector<AttrSlot> attrs;
};

/// @brief 单类 classfile DOM 的唯一所有者。
///
/// 生命周期只有一条规则：本对象析构，其下一切（常量池、属性体、源字节副本）
/// 一次回收。公共 API 不产生调用方必须 delete 的裸指针。
class ClassFile
{
public:
    // ---- classfile 头 ----

    ca::u16 minor = 0;
    ca::u16 major = 0;
    ca::u16 access = 0;
    ca::u16 thisClass = 0;   ///< CP Class 索引
    ca::u16 superClass = 0;  ///< CP Class 索引，0 表示 java/lang/Object
    std::vector<ca::u16> interfaces;

    // ---- 主体（全部由本对象拥有）----

    ConstantPool cp;
    std::vector<MemberInfo> fields;
    std::vector<MemberInfo> methods;
    std::vector<AttrSlot> attrs;

    /// @brief 类级属性表之后的非标准尾部字节（畸形输入的保真形态）。
    ///
    /// 与 @ref CodeAttr::trailing 同族：合法 classfile 在
    /// 类属性表后应当结束，多余字节属于畸形输入，但结构与边界都可确定——
    /// 因此保留为副本而不是 Err 或静默丢弃。未修改时写回走 copy 路径天然
    /// 无损；任何属性被修改触发重编码后，writer 仍把这段字节原样附在末尾，
    /// 「未修改字节级往返」因此覆盖带尾部垃圾的输入。
    std::vector<ca::u8> trailing;

    // ---- 源字节：lazy 与 copy-through 保真的共同前提 ----

    /// @brief 是否保留源字节（由 ReadOptions 决定）。
    ///
    /// 没有源字节就无法 copy-through，只能整体重编码——这是明确的内存/保真权衡，
    /// 不是隐藏的降级；写回时若要求 Preserve 而源缺失，返回 Err 而非静默重编码。
    bool hasSource() const noexcept { return !source_.empty(); }

    ByteView source() const noexcept { return ByteView{source_}; }

    // ---- 查询 ----

    /// @brief CP Utf8 条目内容（零分配）；索引不指向 Utf8 时返回空。
    std::string_view utf8(ca::u32 i) const noexcept { return cp.utf8Of(i); }

    /// @brief CP Class 条目的内部名（Class→Utf8 一跳解析）；非 Class 索引返回空。
    ///
    /// this/super/接口表存的都是 Class 索引——与 @ref utf8 期望的 Utf8 索引
    /// 是两种坐标系，混用只会得到空串。
    std::string_view className(ca::u32 classCp) const noexcept;

    /// @brief 本类内部名（如 `java/lang/String`）；索引异常时返回空。
    std::string_view thisName() const noexcept;

    /// @brief 父类内部名；无父类（Object）时返回空。
    std::string_view superName() const noexcept;

    /// @brief 成员名 / 描述符（零分配）。
    std::string_view nameOf(const MemberInfo& m) const noexcept { return cp.utf8Of(m.nameCp); }
    std::string_view descOf(const MemberInfo& m) const noexcept { return cp.utf8Of(m.descCp); }

    // ---- 解析状态与诊断 ----

    ParseState state() const noexcept { return state_; }
    const Diagnostics& diagnostics() const noexcept { return diag_; }

    /// @brief 可变诊断收集器（reader 在解析期写入降级记录）。
    Diagnostics& diagnostics() noexcept { return diag_; }

    // ---- 构造期入口（仅由 reader 调用）----

    /// @brief 交入源字节所有权，使模型具备 copy-through 与 lazy 解码能力。
    /// @warning 每个 ClassFile 只应调用一次，且在任何属性解码之前；重复调用
    ///          会使既有 span 指向错误的字节。
    void adoptSource(std::vector<ca::u8> bytes) noexcept { source_ = std::move(bytes); }

    /// @brief 设定解析状态（仅由 reader 在解析结束时调用）。
    void setState(ParseState s) noexcept { state_ = s; }

    // ---- 属性查找 ----

    /// @brief 按 Utf8 CP 索引在属性集合中查找同名槽（O(n) 线性扫描）。
    static AttrSlot* findAttrByCp(std::vector<AttrSlot>& v, ca::u32 nameCp) noexcept;

    /// @brief 按属性名查找类级属性（O(n) 逐条 memcmp，不分配）。
    AttrSlot* findClassAttr(std::string_view name) noexcept;

    // ---- lazy 解码入口（唯一的「从 span 变成对象」通道）----

    /// @brief 按需解码一个属性槽：未解码则解码，已解码直接返回。
    ///
    /// 解码结果挂在槽上，状态推进到 Decoded。未建模的属性解码为 @ref RawAttr
    /// （字节副本），绝不猜语义、绝不丢弃。
    Result<AttrBody*, ReadError> decodeAttr(AttrSlot& slot);

    /// @brief 解码某成员的 Code 属性；不存在或无 Code 时返回 nullptr。
    Result<CodeAttr*, ReadError> decodeCode(MemberInfo& m);

private:
    std::vector<ca::u8> source_;
    ParseState state_ = ParseState::Validated;
    Diagnostics diag_;
};

}   // namespace jbc::core
