/// @file        attribute.hpp
/// @brief       属性槽：目录 eager、内容 lazy、状态驱动写回。
///
/// 整个 lazy 设计收敛在 @ref AttrSlot 这一个结构体上：
///
/// ```text
/// 解析期：读到 {name_index, length} 就记录 span 并 skip(length)
///         —— 内容一个字节都不看，也不为它分配任何节点
/// 使用期：首次访问才解码，结果挂在 body 上，状态 Untouched → Decoded
/// 写回期：state ∈ {Untouched, Decoded} → 原样 copy 源 span
///         state ∈ {Modified, New}      → 重编码 body
/// ```
///
/// 由此得到两条旧库没有的性质：
///   - **保真不依赖重编码正确**：没碰过的东西根本不经过编码器；
///   - **改动不可能被忘记记录**：可变访问只有 @ref AttrSlot::mutate 一个入口，
///     它顺手置脏，不存在「改了但状态还是 Decoded」的悬空状态。
#pragma once

#include <memory>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/tag_cast.hpp"

#include "jbc/core/bytes.hpp"
#include "jbc/core/error.hpp"

namespace jbc::core {

/// @brief 已解码属性体的 kind 标签（tag_cast 层级）。
enum class AttrKind : int {
    Raw = 0,               ///< 未建模属性：原样字节
    Code = 1,              ///< Code 属性
    BootstrapMethods = 2,  ///< BootstrapMethods 属性
    LineNumberTable = 3,   ///< LineNumberTable 属性（Code 嵌套）
    StackMapTable = 4,     ///< StackMapTable 属性（Code 嵌套）
    Annotations = 5,       ///< Runtime{Visible,Invisible}Annotations（简单形态）
    LocalVariableTable = 6,   ///< LocalVariableTable/LocalVariableTypeTable（Code 嵌套，行布局同构）
};

/// @brief 属性体基类：一切已解码属性内容的共同祖先。
class AttrBody
{
public:
    explicit AttrBody(AttrKind kind) noexcept : kind_(kind) {}
    virtual ~AttrBody() = default;

    AttrBody(const AttrBody&)            = delete;
    AttrBody& operator=(const AttrBody&) = delete;

    /// @brief kind 标签（tag_cast 的 getType 约定，构造时定死）。
    int getType() const noexcept { return static_cast<int>(kind_); }
    AttrKind kind() const noexcept { return kind_; }

private:
    AttrKind kind_;
};

/// @brief 属性槽：一个属性的全部信息，无论它是否已被解码。
///
/// 定义上刻意先于各 AttrBody 派生类：CodeAttr 的嵌套属性也是 AttrSlot，
/// 二者互相需要，槽作为「容器侧」先落地。
struct AttrSlot
{
    /// @brief 槽的状态，决定写回走 copy 还是重编码。
    enum class State : ca::u8 {
        Untouched = 0,  ///< 未解码；写回 copy 源 span
        Decoded = 1,    ///< 已解码未修改；写回仍 copy 源 span（重编码应字节相同）
        Modified = 2,   ///< 已修改；写回必须重编码
        New = 3,        ///< 新建，无源 span；写回必须重编码
    };

    ca::u32 nameCp = 0;  ///< Utf8 CP 索引（属性名）
    ByteSpan data;       ///< 内容在源字节流中的范围（eager 已知——lazy 的索引）
    State state = State::Untouched;
    std::unique_ptr<AttrBody> body;

    /// @brief 内容是否已被解码（lazy 的观察点：测试据此断言「没碰就没解」）。
    bool decoded() const noexcept { return body != nullptr; }

    /// @brief 写回是否必须走重编码。
    bool needsReencode() const noexcept
    {
        return state == State::Modified || state == State::New;
    }

    /// @brief 只读查看已解码内容；未解码返回 nullptr（不触发解码、不置脏）。
    const AttrBody* peek() const noexcept { return body.get(); }

    /// @brief 取可变指针并置脏——修改的唯一入口，改动因此不可能被忘记记录。
    ///
    /// @warning 未解码时返回 nullptr：调用方必须先 decode。
    AttrBody* mutate() noexcept
    {
        if (body == nullptr) {
            return nullptr;
        }
        state = State::Modified;
        return body.get();
    }

    /// @brief 就地放入已解码内容并标记为 Decoded（由解码路径调用）。
    void setDecoded(std::unique_ptr<AttrBody> b) noexcept
    {
        body = std::move(b);
        if (state == State::Untouched) {
            state = State::Decoded;
        }
    }

    /// @brief 构造一个全新的属性槽（无源字节，写回必须重编码）。
    static AttrSlot makeNew(ca::u32 name, std::unique_ptr<AttrBody> b) noexcept
    {
        AttrSlot s;
        s.nameCp = name;
        s.data   = ByteSpan{};
        s.state  = State::New;
        s.body   = std::move(b);
        return s;
    }
};

/// @brief 未建模属性：持有内容字节的副本。
///
/// 这是保真的兜底形态——libjbc 不认识的属性不会丢，也不会被猜错语义，
/// 未修改时它们甚至不必存在（写回走 span copy），解码后才是这个对象。
class RawAttr : public AttrBody
{
public:
    static constexpr AttrKind kKind = AttrKind::Raw;

    explicit RawAttr(std::vector<ca::u8> bytes) noexcept
        : AttrBody(kKind), bytes_(std::move(bytes))
    {}

    const std::vector<ca::u8>& bytes() const noexcept { return bytes_; }
    std::vector<ca::u8>& bytes() noexcept { return bytes_; }

private:
    std::vector<ca::u8> bytes_;
};

/// @brief Code 属性（v0.1：解到 code 字节，不解码指令）。
///
/// 指令解码属于 v0.2——code 数组内部没有索引结构，要定位第 n 条指令只能线性
/// 扫过前面所有指令，因此「只解一半方法体」不成立；Code 属性是 L2 lazy 的
/// 最小完整单位。异常表与 Code 内属性在此一并解析（它们都在 Code 的定长头里
/// 有 count，代价极低）。
class CodeAttr : public AttrBody
{
public:
    static constexpr AttrKind kKind = AttrKind::Code;

    CodeAttr() noexcept : AttrBody(kKind) {}

    /// @brief 异常表条目（JVMS 4.7.3）；catchType 为 0 表示 finally（catch 一切）。
    struct ExcEntry
    {
        ca::u32 startPc = 0;    ///< 含
        ca::u32 endPc = 0;      ///< 不含
        ca::u32 handlerPc = 0;
        ca::u32 catchType = 0;  ///< CP Class 索引，0 表示 any
    };

    ca::u16 maxStack = 0;
    ca::u16 maxLocals = 0;

    /// @brief code 数组的原始字节（v0.1 不做指令解码）。
    std::vector<ca::u8> code;

    /// @brief Code 属性定长结构之后的非标准尾部字节。
    ///
    /// 合法 classfile 不应有这段内容，但防御式解码不能把畸形输入静默丢掉。
    /// 当 Code 被修改并重编码时，尾部仍按原样附加；高层 promote 默认把它视为
    /// opaque attachment，而不是假装它属于指令序列。
    std::vector<ca::u8> trailing;

    std::vector<ExcEntry> excTable;

    /// @brief Code 属性内的嵌套属性（StackMapTable / LineNumberTable 等），同样 lazy。
    std::vector<AttrSlot> attrs;
};

/// @brief BootstrapMethods 属性（JVMS 4.7.23）：invokedynamic 的方法句柄表。
class BootstrapMethodsAttr : public AttrBody
{
public:
    static constexpr AttrKind kKind = AttrKind::BootstrapMethods;

    BootstrapMethodsAttr() noexcept : AttrBody(kKind) {}

    struct Entry
    {
        ca::u32 methodRef = 0;         ///< bootstrap_method_ref（CP MethodHandle）
        std::vector<ca::u32> args;     ///< bootstrap_arguments（CP 索引）
    };

    std::vector<Entry> entries;

    /// @brief 声明结构之后的非标准尾部字节（trailing 同族，design.md §27.1）。
    std::vector<ca::u8> trailing;
};

/// @brief LineNumberTable 属性（JVMS 4.7.12）：pc → 源码行。
///
/// startPc 是「pc 受 Code 编辑影响」的典型字段——relocation 由持有 pcmap
/// 的 lower 路径处理，模型本身只存值不存策略。
class LineNumberTableAttr : public AttrBody
{
public:
    static constexpr AttrKind kKind = AttrKind::LineNumberTable;

    LineNumberTableAttr() noexcept : AttrBody(kKind) {}

    struct Row
    {
        ca::u32 startPc = 0;
        ca::u32 line = 0;
    };

    std::vector<Row> rows;

    /// @brief 声明结构之后的非标准尾部字节（trailing 同族，design.md §27.1）。
    std::vector<ca::u8> trailing;
};

/// @brief LocalVariableTable / LocalVariableTypeTable（JVMS 4.7.13/4.7.14）。
///
/// 两个属性行布局完全同构（LVTT 的 desc 是签名），共用一个模型；写出时
/// 按槽上的 nameCp 区分属性名。startPc/length 受 Code 编辑影响，relocation
/// 由 lower 路径处理（fail-closed：终点须命中旧指令 pc 或严格等于旧
/// code_length，其余 miss 显式失败）。
class LocalVariableTableAttr : public AttrBody
{
public:
    static constexpr AttrKind kKind = AttrKind::LocalVariableTable;

    LocalVariableTableAttr() noexcept : AttrBody(kKind) {}

    struct Row
    {
        ca::u32 startPc = 0;
        ca::u32 length  = 0;
        ca::u32 nameCp  = 0;
        ca::u32 descCp  = 0;
        ca::u32 slot    = 0;
    };

    std::vector<Row> rows;

    /// @brief 声明结构之后的非标准尾部字节（trailing 同族，design.md §27.1）。
    std::vector<ca::u8> trailing;
};

/// @brief StackMapTable 属性（JVMS 4.7.4）：结构化保真形态。
///
/// v0.2 的建模边界是**结构性**：保留每个 frame 的原始 type 标签与按类型
/// 分派的载荷（offset/delta、locals/stack 的 verification_type 序列），保证
/// 解码→编码字节一致。语义视图（跨 frame 链式推导出的完整 locals）是
/// 后续帧引擎的 accessor，不进本模型——否则 chop/append 的推导状态机会把
/// 保真层和语义层搅在一起。
///
/// 编码知识单源：verification_type_info 的 tag 常量、帧形态标签边界与
/// offset_delta 链式算术都定义在这里，codec（attr_codec 读写）、重定位
/// （ir::relocateSmt）与生成器（analysis::buildStackMapTable）共用，
/// 不再各写一份裸数字。
class StackMapTableAttr : public AttrBody
{
public:
    static constexpr AttrKind kKind = AttrKind::StackMapTable;

    StackMapTableAttr() noexcept : AttrBody(kKind) {}

    /// @brief verification_type_info：tag 语义见 JVMS 4.7.4。
    struct VType
    {
        /// verification_type_info tag（JVMS 4.7.4 表）。
        static constexpr ca::u8 kTop = 0;
        static constexpr ca::u8 kInteger = 1;
        static constexpr ca::u8 kFloat = 2;
        static constexpr ca::u8 kDouble = 3;
        static constexpr ca::u8 kLong = 4;
        static constexpr ca::u8 kNull = 5;
        static constexpr ca::u8 kUninitializedThis = 6;
        static constexpr ca::u8 kObject = 7;          ///< 载荷 = CP Class 索引
        static constexpr ca::u8 kUninitialized = 8;   ///< 载荷 = 产生方 new 指令 pc

        ca::u8 tag = 0;      ///< 0..8（Top/Integer/Float/Double/Long/Null/UninitThis/Object/Uninit）
        ca::u32 cp = 0;      ///< 仅 Object(7)=CPClass / Uninit(8)=offset 两用

        /// @brief 条目相等（tag + 载荷；Object 的载荷经 intern 后同索引即
        ///        同名，Uninitialized 的载荷同 pc 即同 new）——生成器选形
        ///        与测试共用。
        friend bool operator==(const VType& a, const VType& b) noexcept
        {
            return a.tag == b.tag && a.cp == b.cp;
        }
        friend bool operator!=(const VType& a, const VType& b) noexcept
        {
            return !(a == b);
        }
    };

    /// @brief 一个 stack_map_frame；type 保留原始字节标签（0..255）。
    struct Frame
    {
        /// 帧形态标签与边界（JVMS 4.7.4）。
        static constexpr ca::u8 kSameMax = 63;        ///< same_frame：type 即 delta
        static constexpr ca::u8 kSame1Base = 64;      ///< same_locals_1_stack_item：type = 64 + delta
        static constexpr ca::u8 kSame1Max = 127;
        static constexpr ca::u8 kSame1Ext = 247;      ///< same_locals_1_stack_item_extended
        static constexpr ca::u8 kChopMax = 248;       ///< chop_frame：k = 251 - type（248 = 3 条）
        static constexpr ca::u8 kChopMin = 250;       ///< chop_frame：k = 1
        static constexpr ca::u8 kSameExt = 251;       ///< same_frame_extended
        static constexpr ca::u8 kAppendMin = 252;     ///< append_frame：k = type - 251（252 = 1 条）
        static constexpr ca::u8 kAppendMax = 254;     ///< append_frame：k = 3
        static constexpr ca::u8 kFull = 255;          ///< full_frame

        ca::u8 type = 0;
        ca::u32 offset = 0;          ///< same/full 的 offset；chop/append/same_frame_extended 的 delta
        std::vector<VType> locals;   ///> full 的全部 locals；append 追加的部分
        std::vector<VType> stack;    ///> full / same_locals_1_stack 的栈项
    };

    std::vector<Frame> frames;

    /// @brief 声明结构之后的非标准尾部字节（trailing 同族，design.md §27.1）。
    std::vector<ca::u8> trailing;
};

/// @brief SMT offset_delta 链式算术（JVMS 4.7.4）：帧目标 pc = 前一帧
///        pc + delta + 1；首帧的前一帧 pc 约定记 -1（delta 即 pc 本身）。
///        重定位（ir::relocateSmt）与生成（analysis::buildStackMapTable）
///        共用这一版。
inline ca::u32 smtAbsPc(ca::u32 prevAbsPc, ca::u32 delta) noexcept
{
    return prevAbsPc + delta + 1;
}

/// @brief @ref smtAbsPc 的逆：相邻两目标 pc 的 offset_delta。
inline ca::u32 smtDelta(ca::u32 prevAbsPc, ca::u32 targetPc) noexcept
{
    return targetPc - prevAbsPc - 1;
}

/// @brief Runtime{Visible,Invisible}Annotations（JVMS 4.7.16/4.7.17）。
///
/// 只建模 class/field/method 级的简单形态（无 target_info）。参数/类型级
/// 注解（*TypeAnnotation 家族）结构复杂且用例集中在混淆器少见的路径，
/// 保持 Raw 兜底，需要时再立模型。
class AnnotationsAttr : public AttrBody
{
public:
    static constexpr AttrKind kKind = AttrKind::Annotations;

    AnnotationsAttr() noexcept : AttrBody(kKind) {}

    struct ElemValue;

    struct Annotation
    {
        ca::u32 typeCp = 0;   ///< type_descriptor 的 CP Utf8
        std::vector<std::pair<ca::u32, ElemValue>> values;
    };

    struct ElemValue
    {
        ca::u8 tag = 0;           ///< 'B' 'C' 'D' 'F' 'I' 'J' 'S' 'Z' 's' 'e' 'c' '@' '['
        ca::u32 cp = 0;           ///< 常量 / 类信息（'c'）的 CP 索引
        ca::u32 typeNameCp = 0;   ///< 'e' 的 type_name
        ca::u32 constNameCp = 0;  ///< 'e' 的 const_name
        std::unique_ptr<Annotation> nested;   ///< '@'
        std::vector<ElemValue> items;         ///< '['
    };

    std::vector<Annotation> annos;

    /// @brief 声明结构之后的非标准尾部字节（trailing 同族，design.md §27.1）。
    std::vector<ca::u8> trailing;
};

}   // namespace jbc::core

// tag_cast 接入：TypeOf 特化值必须等于运行期 getType()（libca 约定）。
namespace ca::core::tag_cast {

template <>
struct TypeOf<jbc::core::RawAttr>
{
    static constexpr int value = static_cast<int>(jbc::core::AttrKind::Raw);
};

template <>
struct TypeOf<jbc::core::CodeAttr>
{
    static constexpr int value = static_cast<int>(jbc::core::AttrKind::Code);
};

template <>
struct TypeOf<jbc::core::BootstrapMethodsAttr>
{
    static constexpr int value = static_cast<int>(jbc::core::AttrKind::BootstrapMethods);
};

template <>
struct TypeOf<jbc::core::LineNumberTableAttr>
{
    static constexpr int value = static_cast<int>(jbc::core::AttrKind::LineNumberTable);
};

template <>
struct TypeOf<jbc::core::LocalVariableTableAttr>
{
    static constexpr int value = static_cast<int>(jbc::core::AttrKind::LocalVariableTable);
};

template <>
struct TypeOf<jbc::core::StackMapTableAttr>
{
    static constexpr int value = static_cast<int>(jbc::core::AttrKind::StackMapTable);
};

template <>
struct TypeOf<jbc::core::AnnotationsAttr>
{
    static constexpr int value = static_cast<int>(jbc::core::AttrKind::Annotations);
};

}   // namespace ca::core::tag_cast
