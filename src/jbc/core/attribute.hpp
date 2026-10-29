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
    Signature = 7,         ///< Signature（JVMS 4.7.9，类/方法/字段三级挂载）
    TypeAnnotations = 8,   ///< Runtime{Visible,Invisible}TypeAnnotations（JVMS 4.7.20）
    CharacterRangeTable = 9,  ///< CharacterRangeTable（javac -Xjcov 私有，Code 嵌套）
    SourceFile = 10,       ///< SourceFile（JVMS 4.7.10）
    InnerClasses = 11,     ///< InnerClasses（JVMS 4.7.6）
    EnclosingMethod = 12,  ///< EnclosingMethod（JVMS 4.7.7）
    Exceptions = 13,       ///< Exceptions（JVMS 4.7.5）
    NestHost = 14,             ///< NestHost（JVMS 4.7.29）
    NestMembers = 15,          ///< NestMembers（JVMS 4.7.28）
    PermittedSubclasses = 16,  ///< PermittedSubclasses（JVMS 4.7.31）
    Record = 17,               ///< Record（JVMS 4.7.30，类级，组件带嵌套属性表）
    Module = 18,               ///< Module（JVMS 4.7.25，类级，module-info 专挂）
    ModulePackages = 19,       ///< ModulePackages（JVMS 4.7.26，类级）
    ModuleMainClass = 20,      ///< ModuleMainClass（JVMS 4.7.27，类级）
    ParameterAnnotations = 21, ///< Runtime{Visible,Invisible}ParameterAnnotations
                               ///< （JVMS 4.7.18/4.7.19，方法级，参数下标维注解表）
    AnnotationDefault = 22,    ///< AnnotationDefault（JVMS 4.7.22，方法级，
                               ///< 注解类型方法的单元素值默认值）
    MethodParameters = 23,     ///< MethodParameters（JVMS 4.7.24，方法级，
                               ///< 参数名/旗语表，javac -parameters 产物）
    ConstantValue = 24,        ///< ConstantValue（JVMS 4.7.2，字段级，单 CP 槽）
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

    /// @brief 声明结构之后的非标准尾部字节（trailing 同族）。
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

    /// @brief 声明结构之后的非标准尾部字节（trailing 同族）。
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

    /// @brief 声明结构之后的非标准尾部字节（trailing 同族）。
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

    /// @brief 声明结构之后的非标准尾部字节（trailing 同族）。
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
/// 注解（*TypeAnnotation 家族）另立 typed 模型 @ref TypeAnnotationsAttr
/// （本族 ElemValue 为两族共用）。
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

    /// @brief 声明结构之后的非标准尾部字节（trailing 同族）。
    std::vector<ca::u8> trailing;
};

/// @brief Signature 属性（JVMS 4.7.9）：泛型签名的单 u2 载荷。
///
/// 类/方法/字段三级挂载点各至多一个；属性体是 `u2 signature_index`，签名串
/// 在 CP Utf8，语法与手术语义归 @ref jbc::type::signature，
/// 属性层手术入口见 @ref jbc::ir::sig_attr。
class SignatureAttr : public AttrBody
{
public:
    static constexpr AttrKind kKind = AttrKind::Signature;

    SignatureAttr() noexcept : AttrBody(kKind) {}

    ca::u32 signatureCp = 0;   ///< Utf8 CP 索引（泛型签名串）

    /// @brief 声明结构之后的非标准尾部字节（trailing 同族）。
    ///
    /// 合法 Signature 体恰为 2 字节；超长输入的尾字节奏此保留，重编码原样
    /// 写回，不静默丢弃。
    std::vector<ca::u8> trailing;
};

/// @brief Runtime{Visible,Invisible}TypeAnnotations 属性（JVMS 4.7.20/4.7.21）。
///
/// 类型注解族的全结构建模：target_type + target_info 并取（4.7.20.1 十形态）
/// + type_path（4.7.20.2）+ 注解体（type_index 描述符 + element_value 表，
/// 与 @ref AnnotationsAttr 共用 ElemValue）。可见性只差属性名（AttrSlot.
/// nameCp 承载，体不感知），RuntimeVisibleTypeAnnotations 与
/// RuntimeInvisibleTypeAnnotations 共用本模型。挂载点：类/字段/方法属性表
/// 与 Code 嵌套属性表。类名手术面（type_index 与 'c' 元素值的类描述符）见
/// @ref jbc::ir::type_annotation_attr。
class TypeAnnotationsAttr : public AttrBody
{
public:
    static constexpr AttrKind kKind = AttrKind::TypeAnnotations;

    /// target_type 值（JVMS 4.7.20.1 表 A；载荷形状见各注释）。
    static constexpr ca::u8 kClassTypeParameter = 0x00;   ///< u1 type_parameter_index
    static constexpr ca::u8 kMethodTypeParameter = 0x01;  ///< u1 type_parameter_index
    static constexpr ca::u8 kSupertype = 0x10;            ///< u2 interfaces[]/super 下标
    static constexpr ca::u8 kClassTypeParameterBound = 0x11;  ///< u1 param + u1 bound
    static constexpr ca::u8 kMethodTypeParameterBound = 0x12; ///< u1 param + u1 bound
    static constexpr ca::u8 kField = 0x13;                ///< empty（字段类型）
    static constexpr ca::u8 kMethodReturn = 0x14;         ///< empty（方法返回类型）
    static constexpr ca::u8 kMethodReceiver = 0x15;       ///< empty（receiver 类型）
    static constexpr ca::u8 kFormalsParameter = 0x16;     ///< u1 formal_parameter_index
    static constexpr ca::u8 kThrows = 0x17;               ///< u2 Exceptions 表下标
    static constexpr ca::u8 kLocalVar = 0x40;             ///< u2 表长 + {u2,u2,u2}*
    static constexpr ca::u8 kLocalVarResource = 0x41;     ///< 同上（try-with-resources）
    static constexpr ca::u8 kCatch = 0x42;                ///< u2 异常表条目下标
    static constexpr ca::u8 kOffsetBase = 0x43;  ///< 0x43..0x46 instanceof/new/方法引用，u2 offset
    static constexpr ca::u8 kOffsetMax = 0x46;
    static constexpr ca::u8 kCast = 0x47;                 ///< u2 offset + u1 type_index（cast 位置）
    static constexpr ca::u8 kTypeArgBase = 0x48;  ///< 0x48..0x4B 调用/引用类型实参，u2 offset + u1 arg
    static constexpr ca::u8 kTypeArgMax = 0x4B;

    TypeAnnotationsAttr() noexcept : AttrBody(kKind) {}

    /// @brief type_path 一段（JVMS 4.7.20.2）：kind 0..3 + 类型实参下标。
    struct TypePathEntry
    {
        ca::u8 kind = 0;         ///< 0 深入数组 1 深入嵌套 2 通配符界 3 类型实参
        ca::u8 typeArgIndex = 0; ///< kind==3 时有效
    };

    /// @brief localvar_target 一行（start_pc/length/index 三 u2，布局同 LVT 行
    ///        的 pc 段 + slot，但无 name/descriptor——引用的是槽位不是名字）。
    struct LocalVarEntry
    {
        ca::u32 startPc = 0;
        ca::u32 length = 0;
        ca::u32 slot = 0;
    };

    /// @brief 一条 type_annotation。target_info 按字段注释分派取用。
    struct Annotation
    {
        ca::u8 targetType = kField;

        ca::u8 typeParameterIndex = 0;    ///< 0x00 / 0x01 / 0x11 / 0x12
        ca::u8 boundIndex = 0;            ///< 0x11 / 0x12
        ca::u32 supertypeIndex = 0;       ///< 0x10（interfaces[] 下标或 65535=superclass）
        ca::u8 formalParameterIndex = 0;  ///< 0x16
        ca::u32 throwsIndex = 0;          ///< 0x17（Exceptions 表下标，CP Class 引用）
        std::vector<LocalVarEntry> localVars;  ///< 0x40/0x41
        ca::u32 catchIndex = 0;           ///< 0x42（异常表条目下标）
        ca::u32 offset = 0;               ///< 0x43..0x47
        ca::u8 typeArgumentIndex = 0;     ///< 0x47 cast 的 type_index / 0x48..0x4B 实参下标

        std::vector<TypePathEntry> typePath;

        ca::u32 typeIndexCp = 0;  ///< 注解类型描述符（字段描述符形态，CP Utf8）
        std::vector<std::pair<ca::u32, AnnotationsAttr::ElemValue>> values;
    };

    std::vector<Annotation> annotations;

    /// @brief 声明结构之后的非标准尾部字节（trailing 同族）。
    std::vector<ca::u8> trailing;
};

/// @brief CharacterRangeTable 属性（javac -Xjcov 的私有调试属性）。
///
/// 规格不是 JVMS 条目（JVMS 到 4.7.24 为止），以 JDK javac 源
/// （CRTable.writeCRT）与 -Xjcov 产物字节实测为准：`u2 行数 +
/// { u2 start_pc; u2 end_pc; u4 char_start; u4 char_end; u2 flags }*`。
/// start_pc 是范围首指令 pc；end_pc 是**含末指令**的 pc（javac 写
/// entry.endPc-1），二者都是指令起始 pc，重定位各自精确命中 pcmap。
/// char_start/char_end 是源文件位置的 (line << 10) | column 编码（0 起），
/// flags 是 CRT_* 位集——两者与字节码无关，按不透明整数保真。JDK 11+ 的
/// javac 已停发本属性（-Xjcov 被忽略），仅旧编译器产物携带；Code 嵌套挂载。
class CharacterRangeTableAttr : public AttrBody
{
public:
    static constexpr AttrKind kKind = AttrKind::CharacterRangeTable;

    CharacterRangeTableAttr() noexcept : AttrBody(kKind) {}

    struct Row
    {
        ca::u32 startPc   = 0;  ///< 范围首指令 pc（u2 编码）
        ca::u32 endPc     = 0;  ///< 含末指令 pc（u2 编码）
        ca::u32 charStart = 0;  ///< 源起 (line<<10)|column（u4 编码）
        ca::u32 charEnd   = 0;  ///< 源止 (line<<10)|column（u4 编码）
        ca::u32 flags     = 0;  ///< CRT_* 位集（u2 编码，不透明保真）
    };

    std::vector<Row> rows;

    /// @brief 声明结构之后的非标准尾部字节（trailing 同族）。
    std::vector<ca::u8> trailing;
};

/// @brief 结构元数据属性四件（SourceFile/InnerClasses/EnclosingMethod/
///        Exceptions）的共同边界，成文于此避免四处重复：
///
///  1. **只挂类级/成员级属性表，不 Code 嵌套，载荷全是 CP 索引与访问位，
///     无任何 pc 偏移**——relocation 契约（封闭集）不涉及它们，布局
///     改动不需要、也不得对这四件做任何重定位。
///  2. **codec 形状与既有 typed 属性同铁律**：半截体 decode 显式 Err、
///     声明结构后的多余字节奏 trailing、编码逐字段过 u16 域宽
///     闸门。
///  3. **分派现状**：四件均已接 decodeAttrBody 的 typed 分派：
///     公开解码函数
///     （attr_codec.hpp 的 *Body 入口）保留为不触碰槽状态的直接解码面。
class SourceFileAttr : public AttrBody
{
public:
    static constexpr AttrKind kKind = AttrKind::SourceFile;

    SourceFileAttr() noexcept : AttrBody(kKind) {}

    ca::u32 sourceFileCp = 0;   ///< Utf8 CP 索引（源文件名）

    /// @brief 声明结构之后的非标准尾部字节（trailing 同族）。
    ///
    /// 合法 SourceFile 体恰为 2 字节；超长输入的尾字节奏此保留，重编码
    /// 原样写回，不静默丢弃。
    std::vector<ca::u8> trailing;
};

/// @brief InnerClasses 属性（JVMS 4.7.6）：内部类信息表（类级挂载）。
///
/// 行三 CP 槽即类名手术面：inner_class_info/outer_class_info 指向 CP
/// Class 条目（inner_name 语义随 outer 存在性约束），inner_name 是简单名
/// Utf8。既有 Raw 手术路径（上层消费方 remapper）对三槽的改写语义——类名映射
/// 命中才动、outer==0 保 0、简单名随内部类二进制名映射同步且仅在全名命中
/// 旧简单名时改写——与 @ref Row 三槽一一对应，typed 读写可完整表达
/// 。
class InnerClassesAttr : public AttrBody
{
public:
    static constexpr AttrKind kKind = AttrKind::InnerClasses;

    InnerClassesAttr() noexcept : AttrBody(kKind) {}

    struct Row
    {
        ca::u32 innerClassInfoCp = 0;  ///< CP Class（内部类二进制名）
        ca::u32 outerClassInfoCp = 0;  ///< CP Class；0 = 匿名/无外围类
        ca::u32 innerNameCp = 0;       ///< CP Utf8 简单名；0 = 匿名
        ca::u32 accessFlags = 0;       ///< 内部类访问位（u2 位集）
    };

    std::vector<Row> rows;

    /// @brief 声明结构之后的非标准尾部字节（trailing 同族）。
    std::vector<ca::u8> trailing;
};

/// @brief EnclosingMethod 属性（JVMS 4.7.7）：局部类/匿名类的外围类与
///        直接包围方法（类级挂载）。
///
/// classCp 指向 CP Class（直接外围类，恒非 0）；methodNatCp 指向 CP
/// NameAndType，**0 合法**——外围类是实例初始化器或静态初始化器时不指名
/// 方法。两槽即类名手术面：类名重映射动 classCp；方法重映射动 methodNatCp
/// （解析 NAT 后换新条目，NestHost/NestMembers 不在本属性）。与
/// @ref InnerClassesAttr 同批接 typed 分派（见上）。
class EnclosingMethodAttr : public AttrBody
{
public:
    static constexpr AttrKind kKind = AttrKind::EnclosingMethod;

    EnclosingMethodAttr() noexcept : AttrBody(kKind) {}

    ca::u32 classCp = 0;      ///< CP Class（直接外围类）
    ca::u32 methodNatCp = 0;  ///< CP NameAndType；0 = 无直接包围方法

    /// @brief 声明结构之后的非标准尾部字节（trailing 同族）。
    std::vector<ca::u8> trailing;
};

/// @brief Exceptions 属性（JVMS 4.7.5）：checked 异常类表（方法级挂载）。
///
/// 体是 `u2 count + u2 class_index*`，条目指向 CP Class。异常类重映射 =
/// 逐条目 CP Class 重指向（既有 Raw 手术路径的语义），typed 模型以
/// @ref exceptionCps 直接表达。与 @ref InnerClassesAttr 同批接 typed 分派
/// （见上）。
class ExceptionsAttr : public AttrBody
{
public:
    static constexpr AttrKind kKind = AttrKind::Exceptions;

    ExceptionsAttr() noexcept : AttrBody(kKind) {}

    std::vector<ca::u32> exceptionCps;  ///< CP Class 索引表（checked 异常）

    /// @brief 声明结构之后的非标准尾部字节（trailing 同族）。
    std::vector<ca::u8> trailing;
};

/// @brief NestHost 属性（JVMS 4.7.29）：当前类的 nest 宿主（类级挂载）。
///
/// 体是单 `u2 host_class_index`（CP Class）。与 NestMembers/PermittedSubclasses
/// 三件同边界：只挂类级属性表、不 Code 嵌套、载荷全为 CP 索引、无 pc 偏移
/// —— relocation 封闭集不涉及；codec 同铁律（半截体 Err、trailing 同族、
/// u16 域宽闸门）。hostClassCp 即类名手术面（nest 宿主重映射 = 单 CP Class
/// 重指向，与既有 Raw 手术路径语义一一对应）。
class NestHostAttr : public AttrBody
{
public:
    static constexpr AttrKind kKind = AttrKind::NestHost;

    NestHostAttr() noexcept : AttrBody(kKind) {}

    ca::u32 hostClassCp = 0;   ///< CP Class（nest 宿主）

    /// @brief 声明结构之后的非标准尾部字节（trailing 同族）。
    ///
    /// 合法 NestHost 体恰为 2 字节；超长输入的尾字节奏此保留，重编码
    /// 原样写回，不静默丢弃。
    std::vector<ca::u8> trailing;
};

/// @brief NestMembers 属性（JVMS 4.7.28）：当前类的 nest 成员表（类级挂载）。
///
/// 体是 `u2 number_of_classes + u2 classes[]`，条目全为 CP Class。成员类名
/// 即手术面（nest 成员重映射 = 逐条目 CP Class 重指向）。宿主自身不出现在
/// 表中（JVMS 4.7.28），0 不是合法条目值但解码不做语义校验——结构保真层
/// 原样承载。
class NestMembersAttr : public AttrBody
{
public:
    static constexpr AttrKind kKind = AttrKind::NestMembers;

    NestMembersAttr() noexcept : AttrBody(kKind) {}

    std::vector<ca::u32> memberCps;  ///< CP Class 索引表（nest 成员）

    /// @brief 声明结构之后的非标准尾部字节（trailing 同族）。
    std::vector<ca::u8> trailing;
};

/// @brief PermittedSubclasses 属性（JVMS 4.7.31）：sealed 类的允许子类表
///        （类级挂载）。
///
/// 体是 `u2 number_of_classes + u2 classes[]`，形状与 NestMembers 同构，
/// 条目全为 CP Class（class file v61+）。允许子类名即手术面（重映射 =
/// 逐条目 CP Class 重指向）。
class PermittedSubclassesAttr : public AttrBody
{
public:
    static constexpr AttrKind kKind = AttrKind::PermittedSubclasses;

    PermittedSubclassesAttr() noexcept : AttrBody(kKind) {}

    std::vector<ca::u32> permittedCps;  ///< CP Class 索引表（允许子类）

    /// @brief 声明结构之后的非标准尾部字节（trailing 同族）。
    std::vector<ca::u8> trailing;
};

/// @brief Record 属性（JVMS 4.7.30）：record 类的组件表（类级挂载）。
///
/// 体是 `u2 record_component_count + components[]`；每个 component 是
/// `u2 name_index（Utf8）+ u2 descriptor_index（Utf8 字段描述符）+
/// u2 attributes_count + attributes[]`——组件上是**嵌套属性表**，javac 会
/// 在泛型组件上挂 Signature、在带注解组件上挂 Runtime{Visible,Invisible}-
/// Annotations 等嵌套属性。
///
/// 与 @ref CodeAttr 同构的嵌套持有形态：组件嵌套属性也是 @ref AttrSlot，
/// span 是源字节流中的绝对偏移，解码时只读目录（lazy 照旧），写回按槽状态
/// copy-through 或重编码。组件名/描述符两槽是改名手术面；嵌套属性面
/// （Signature/注解）由上层消费方在槽上直接手术。
class RecordAttr : public AttrBody
{
public:
    static constexpr AttrKind kKind = AttrKind::Record;

    RecordAttr() noexcept : AttrBody(kKind) {}

    struct Component
    {
        ca::u32 nameCp = 0;        ///< CP Utf8（组件名）
        ca::u32 descriptorCp = 0;  ///< CP Utf8（字段描述符）

        /// @brief 组件上的嵌套属性（Signature / Runtime{Visible,Invisible}-
        ///        Annotations 等），同样 lazy——只读目录不解内容。
        std::vector<AttrSlot> attrs;
    };

    std::vector<Component> components;

    /// @brief 声明结构之后的非标准尾部字节（trailing 同族）。
    std::vector<ca::u8> trailing;
};

/// @brief Module 属性（JVMS 4.7.25）：模块声明的全部指令表（类级挂载，
///        仅 module-info 类携带，access_flags = ACC_MODULE）。
///
/// 体是 `u2 module_name（CP Module）+ u2 module_flags + u2 module_version
/// （CP Utf8）+ requires[] + exports[] + opens[] + uses[] + provides[]`；
/// requires 行是 `{模块索引（CP Module）, flags, 版本索引（CP Utf8，0 = 无）}`，
/// exports/opens 行是 `{包索引（CP Package）, flags, to[]（CP Module）}`（to
/// 空 = 未限定导出/开放），uses 全为 CP Class，provides 行是
/// `{服务索引（CP Class）, with[]（CP Class）}`。与 @ref ModuleMainClassAttr
/// 的边界：JVMS 4.7.25 的体**不含**主类索引段——主类由独立的
/// ModuleMainClass（4.7.27）承载，javac 17.0.8 实测产物体恰在 provides 后
/// 结束（体尾多出的 2 字节属 trailing，同族保留）。
///
/// 与 Nest 族/Record 同边界：只挂类级属性表、不 Code 嵌套、载荷全为 CP
/// 索引与访问位、无 pc 偏移—— relocation 封闭集不涉及。段旗语
/// （ACC_TRANSITIVE=0x0020 / ACC_STATIC_PHASE=0x0040 / ACC_OPEN=0x0020 /
/// ACC_SYNTHETIC=0x1000 / ACC_MANDATED=0x8000）按不透明 u16 保真，与
/// InnerClasses 的 access_flags 同款处理。
///
/// ModuleHashes/ModuleResolution/ModuleTarget（JDK 内部件，jlink/jmod 侧产
/// 物，非 JVMS 条目、javac 语料构造上不可产）不建模，保持 Raw 透传。
class ModuleAttr : public AttrBody
{
public:
    static constexpr AttrKind kKind = AttrKind::Module;

    ModuleAttr() noexcept : AttrBody(kKind) {}

    /// @brief requires 指令行。
    struct Require
    {
        ca::u32 moduleCp = 0;    ///< CP Module（被依赖模块）
        ca::u16 flags = 0;       ///< 旗语位集（TRANSITIVE/STATIC_PHASE/...）
        ca::u32 versionCp = 0;   ///< CP Utf8（模块版本）；0 = 无版本
    };

    /// @brief exports 指令行（to 空表 = 未限定导出）。
    struct Export
    {
        ca::u32 packageCp = 0;        ///< CP Package（导出包）
        ca::u16 flags = 0;            ///< 旗语位集（SYNTHETIC/MANDATED）
        std::vector<ca::u32> toCps;   ///< CP Module（限定目标）
    };

    /// @brief opens 指令行（与 exports 布局同构，typed 面各归各——开放与
    ///        导出语义不同，消费方按各自类型分派）。
    struct Open
    {
        ca::u32 packageCp = 0;        ///< CP Package（开放包）
        ca::u16 flags = 0;            ///< 旗语位集（SYNTHETIC/MANDATED）
        std::vector<ca::u32> toCps;   ///< CP Module（限定目标）
    };

    /// @brief provides 指令行。
    struct Provide
    {
        ca::u32 serviceCp = 0;        ///< CP Class（服务接口）
        std::vector<ca::u32> withCps; ///< CP Class（服务实现）
    };

    ca::u32 moduleNameCp = 0;     ///< CP Module（模块名）
    ca::u16 moduleFlags = 0;      ///< 模块旗语位集（OPEN/SYNTHETIC/MANDATED）
    ca::u32 moduleVersionCp = 0;  ///< CP Utf8（模块版本）；0 = 无版本

    /// @brief requires 指令表（`requires` 是 C++20 关键字，成员名缀 Entries）。
    std::vector<Require> requiresEntries;
    std::vector<Export> exports;
    std::vector<Open> opens;
    std::vector<ca::u32> usesCps;   ///< CP Class（uses 指令服务表）
    std::vector<Provide> provides;

    /// @brief 声明结构之后的非标准尾部字节（trailing 同族）。
    ///
    /// 合法 Module 体在 provides 之后正好结束；超长输入的尾字节奏此保留，
    /// 重编码原样写回，不静默丢弃。
    std::vector<ca::u8> trailing;
};

/// @brief ModulePackages 属性（JVMS 4.7.26）：模块全部包名表（类级挂载）。
///
/// 体是 `u2 package_count + u2 packages[]`，条目全为 CP Package。形状与
/// NestMembers 同构而条目类型不同（Package 非 Class）；javac 对已知包集合
/// 才写出本属性（单源单模块直译模式实测不产），typed 面照常承载。包名
/// 手术面 = 逐条目 CP Package 重指向。
class ModulePackagesAttr : public AttrBody
{
public:
    static constexpr AttrKind kKind = AttrKind::ModulePackages;

    ModulePackagesAttr() noexcept : AttrBody(kKind) {}

    std::vector<ca::u32> packageCps;  ///< CP Package 索引表（模块包名）

    /// @brief 声明结构之后的非标准尾部字节（trailing 同族）。
    std::vector<ca::u8> trailing;
};

/// @brief ModuleMainClass 属性（JVMS 4.7.27）：模块主类（类级挂载）。
///
/// 体是单 `u2 main_class_index`（CP Class），与 NestHost 同形状。写入方是
/// jlink/jar 工具链（javac 不产——module-info 源语言无主类指令）；主类名
/// 即手术面（重映射 = 单 CP Class 重指向）。
class ModuleMainClassAttr : public AttrBody
{
public:
    static constexpr AttrKind kKind = AttrKind::ModuleMainClass;

    ModuleMainClassAttr() noexcept : AttrBody(kKind) {}

    ca::u32 mainClassCp = 0;   ///< CP Class（模块主类）

    /// @brief 声明结构之后的非标准尾部字节（trailing 同族）。
    ///
    /// 合法 ModuleMainClass 体恰为 2 字节；超长输入的尾字节奏此保留，
    /// 重编码原样写回，不静默丢弃。
    std::vector<ca::u8> trailing;
};

/// @brief Runtime{Visible,Invisible}ParameterAnnotations 属性（JVMS
///        4.7.18/4.7.19）：方法参数维的注解表（方法级挂载）。
///
/// 体是 `u1 num_parameters + { u2 num_annotations + annotation[] }*`——与
/// @ref AnnotationsAttr 的差异只在多了一层参数下标维（第 i 个内层表对应方法
/// 第 i 个参数槽，含无注解参数的空表）；注解体（type_index + element_value
/// 表）与 @ref AnnotationsAttr::Annotation 完全同构，直接复用同一模型，不造
/// 第二套。可见性只差属性名（AttrSlot::nameCp 承载，体不感知），两个属性名
/// 共用本模型。
class ParameterAnnotationsAttr : public AttrBody
{
public:
    static constexpr AttrKind kKind = AttrKind::ParameterAnnotations;

    ParameterAnnotationsAttr() noexcept : AttrBody(kKind) {}

    /// @brief 逐参数的注解表：外层下标 = 方法参数槽位（MethodParameters 的
    ///        行序一一对应），空内层表 = 该参数无注解（合法且常见）。
    std::vector<std::vector<AnnotationsAttr::Annotation>> parameters;

    /// @brief 声明结构之后的非标准尾部字节（trailing 同族）。
    std::vector<ca::u8> trailing;
};

/// @brief AnnotationDefault 属性（JVMS 4.7.22）：注解类型方法的默认值
///        （方法级挂载，仅 @interface 的带默认值方法携带）。
///
/// 体是单个 element_value，与 @ref AnnotationsAttr::ElemValue 完全同构（全
/// tag 形态：const/'e' 枚举/'c' 类字面量/'@' 嵌套注解/'[' 数组），直接复用
/// 同一模型。
class AnnotationDefaultAttr : public AttrBody
{
public:
    static constexpr AttrKind kKind = AttrKind::AnnotationDefault;

    AnnotationDefaultAttr() noexcept : AttrBody(kKind) {}

    AnnotationsAttr::ElemValue defaultValue;   ///< 默认值元素（单 element_value）

    /// @brief 声明结构之后的非标准尾部字节（trailing 同族）。
    std::vector<ca::u8> trailing;
};

/// @brief MethodParameters 属性（JVMS 4.7.24）：方法参数名与访问旗语表
///        （方法级挂载）。
///
/// 体是 `u1 parameters_count + { u2 name_index, u2 access_flags }*`，行序与
/// 参数槽位一致（含 synthetic/mandated 参数行，与
/// @ref ParameterAnnotationsAttr 的外层下标同坐标系）。name_index 指向 CP
/// Utf8，**0 合法**——无源码名可用的参数（编译器合成等）不指名；access_flags
/// 是 ACC_FINAL(0x0010)/ACC_SYNTHETIC(0x1000)/ACC_MANDATED(0x8000) 位集，
/// 按不透明 u16 保真（与 InnerClasses 的 access_flags 同款）。参数名即手术
/// 面（改名 = 单 u2 Utf8 重指向）。
class MethodParametersAttr : public AttrBody
{
public:
    static constexpr AttrKind kKind = AttrKind::MethodParameters;

    MethodParametersAttr() noexcept : AttrBody(kKind) {}

    struct Row
    {
        ca::u32 nameCp = 0;       ///< CP Utf8（参数名）；0 = 无名
        ca::u16 accessFlags = 0;  ///< FINAL/SYNTHETIC/MANDATED 位集（u2）
    };

    std::vector<Row> rows;

    /// @brief 声明结构之后的非标准尾部字节（trailing 同族）。
    std::vector<ca::u8> trailing;
};

/// @brief ConstantValue 属性（JVMS 4.7.2）：static final 字段的常量值
///        （字段级挂载）。
///
/// 体是单 `u2 constantvalue_index`，指向 CP 的 Integer/Float/Long/Double/
/// String 五形态之一（boolean/byte/char/short 在 CP 里同 Integer）。单 CP
/// 槽即手术面（值改写 = 换槽重指向）；类型层一致性（槽 tag 与字段描述符
/// 匹配）归上层语义，结构保真层原样承载。
class ConstantValueAttr : public AttrBody
{
public:
    static constexpr AttrKind kKind = AttrKind::ConstantValue;

    ConstantValueAttr() noexcept : AttrBody(kKind) {}

    ca::u32 constantValueCp = 0;   ///< CP 常量槽（Integer/Float/Long/Double/String）

    /// @brief 声明结构之后的非标准尾部字节（trailing 同族）。
    ///
    /// 合法 ConstantValue 体恰为 2 字节；超长输入的尾字节奏此保留，重编码
    /// 原样写回，不静默丢弃。
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

template <>
struct TypeOf<jbc::core::SignatureAttr>
{
    static constexpr int value = static_cast<int>(jbc::core::AttrKind::Signature);
};

template <>
struct TypeOf<jbc::core::TypeAnnotationsAttr>
{
    static constexpr int value = static_cast<int>(jbc::core::AttrKind::TypeAnnotations);
};

template <>
struct TypeOf<jbc::core::CharacterRangeTableAttr>
{
    static constexpr int value = static_cast<int>(jbc::core::AttrKind::CharacterRangeTable);
};

template <>
struct TypeOf<jbc::core::SourceFileAttr>
{
    static constexpr int value = static_cast<int>(jbc::core::AttrKind::SourceFile);
};

template <>
struct TypeOf<jbc::core::InnerClassesAttr>
{
    static constexpr int value = static_cast<int>(jbc::core::AttrKind::InnerClasses);
};

template <>
struct TypeOf<jbc::core::EnclosingMethodAttr>
{
    static constexpr int value = static_cast<int>(jbc::core::AttrKind::EnclosingMethod);
};

template <>
struct TypeOf<jbc::core::ExceptionsAttr>
{
    static constexpr int value = static_cast<int>(jbc::core::AttrKind::Exceptions);
};

template <>
struct TypeOf<jbc::core::NestHostAttr>
{
    static constexpr int value = static_cast<int>(jbc::core::AttrKind::NestHost);
};

template <>
struct TypeOf<jbc::core::NestMembersAttr>
{
    static constexpr int value = static_cast<int>(jbc::core::AttrKind::NestMembers);
};

template <>
struct TypeOf<jbc::core::PermittedSubclassesAttr>
{
    static constexpr int value = static_cast<int>(jbc::core::AttrKind::PermittedSubclasses);
};

template <>
struct TypeOf<jbc::core::RecordAttr>
{
    static constexpr int value = static_cast<int>(jbc::core::AttrKind::Record);
};

template <>
struct TypeOf<jbc::core::ModuleAttr>
{
    static constexpr int value = static_cast<int>(jbc::core::AttrKind::Module);
};

template <>
struct TypeOf<jbc::core::ModulePackagesAttr>
{
    static constexpr int value = static_cast<int>(jbc::core::AttrKind::ModulePackages);
};

template <>
struct TypeOf<jbc::core::ModuleMainClassAttr>
{
    static constexpr int value = static_cast<int>(jbc::core::AttrKind::ModuleMainClass);
};

template <>
struct TypeOf<jbc::core::ParameterAnnotationsAttr>
{
    static constexpr int value = static_cast<int>(jbc::core::AttrKind::ParameterAnnotations);
};

template <>
struct TypeOf<jbc::core::AnnotationDefaultAttr>
{
    static constexpr int value = static_cast<int>(jbc::core::AttrKind::AnnotationDefault);
};

template <>
struct TypeOf<jbc::core::MethodParametersAttr>
{
    static constexpr int value = static_cast<int>(jbc::core::AttrKind::MethodParameters);
};

template <>
struct TypeOf<jbc::core::ConstantValueAttr>
{
    static constexpr int value = static_cast<int>(jbc::core::AttrKind::ConstantValue);
};

}   // namespace ca::core::tag_cast
