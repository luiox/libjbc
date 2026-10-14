/// @file        signature.hpp
/// @brief       JVM 泛型签名系统（JVMS 4.7.9.1）：结构化签名树、解析/反解、
///              类名手术坐标。
///
/// 描述符层（@ref descriptor.hpp）之下是擦除后的运行时类型真相；签名层在其
/// 之上表达 javac 写进 Signature 属性的泛型语义（类型参数/实参/通配符/嵌套
/// 泛型/内部类成员/throws）。本模块与描述符层同一风格：视图进、值出——解析
/// 零分配（树节点字符串全部借用输入串的字节），只在实际产出新串的反解/手术
/// 入口才分配。
///
/// 三条层契约（成文于 doc_ai/design.md §32）：
///   1. **roundtrip 恒等**：合法输入 parse→unparse 逐字节恒等（树不做任何
///      归一化，一切字节各归其位）；
///   2. **畸形显式 Err**：解析失败返回 @ref SigError{码, 字节偏移}，绝不静默
///      吞——「结构可确定」之前不存在降级形态；
///   3. **手术显式调用**：类名手术只经本模块 API 发生（span 坐标 / rename
///      便捷面），改写其余字节原样保留；Signature 属性的定位与写回在本纯
///      函数层不做（copy-through 纪律），属性层接线见 jbc::ir::sig_attr
///      （design.md §32.4）。
///
/// 语法真源 JVMS 4.7.9.1，产生式一一对应（注释逐条标注）。
#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"
#include "libca/str/utf8_string.hpp"

#include "jbc/type/descriptor.hpp"

namespace jbc::type {

using ca::core::Result;
using ca::core::Ok;
using ca::core::Err;

/// @brief 签名形态（Signature 属性按所在成员天然三分，解析需调用方给出）。
enum class SignatureForm : ca::u8 {
    Class,   ///< 类签名：[TypeParameters] Superclass Superinterface*
    Method,  ///< 方法签名：[TypeParameters] ( Params ) Ret Throws*
    Field,   ///< 字段签名：FieldTypeSignature
};

/// @brief 签名解析/手术失败原因（与 @ref SigError::offset 配合定位）。
enum class SigErrorCode : ca::u8 {
    Ok = 0,
    Empty,             ///< 输入为空串
    Truncated,         ///< 结构在闭合前结束（缺 ';' '>' ')' 等）
    TrailingChars,     ///< 结构闭合后仍有剩余字符
    BadSignatureShape, ///< 不像任何签名形态（首字符不在 ( < L [ T 之中）
    BadIdentifier,     ///< 标识符为空或含保留字符（. ; [ / < > :）
    BadTypeParams,     ///< 类型参数表畸形（空表 / 缺名 / 缺冒号 / 缺 '>'）
    BadBound,          ///< 界畸形（接口界缺类型 / 类界后跟非法字符）
    BadTypeArg,        ///< 类型实参表畸形（空表 / 缺 '>' / '*' 后跟类型）
    BadClassType,      ///< 类类型畸形（'.' 后缺段名 / 段位置字符非法）
    BadMethodShape,    ///< 方法签名不以 '(' 开头或缺 ')'
    BadThrows,         ///< '^' 后缺类型或类型非法
    BadPrimitive,      ///< 裸基元出现在纯引用位置（类型实参/界/字段签名）
    BadBaseType,       ///< 类型位置出现未知字符
    NestingTooDeep,    ///< 嵌套（数组维数或类型实参深度）超过 255
    RenameShapeMismatch, ///< rename 的 old/new 内部名 $ 段结构不一致
};

/// @brief 错误码的可读文本（诊断输出用）。
const char* to_string(SigErrorCode e) noexcept;

/// @brief 解析/手术失败的完整事实：原因码 + 输入串中的字节偏移。
struct SigError
{
    SigErrorCode code = SigErrorCode::Ok;
    ca::u32 offset = 0;   ///< 相对输入签名串的字节偏移（错误位置）
};

/// @brief 基类型原子：与描述符层共用 @ref BaseType（同一 8 字符集）。

/// @brief 一个类类型签名的段（SimpleClassTypeSignature）：段名 + 可选类型实参。
///
/// 类类型是段的链：`Lpkg/Outer<TT;>.Inner;` = 段 Outer(实参 TT;) + 段 Inner
/// （无实参），段间以 '.' 连接（JVMS ClassTypeSignatureSuffix）。
struct SigClassSegment
{
    std::string_view name;            ///< 段名（不含 '.'；非空，借用输入）
    std::vector<struct SigTypeArg> type_args;  ///< 空 = 无 <>；非空语法保证 ≥1
};

/// @brief 类类型签名（ClassTypeSignature）：包前缀 + 段链。
///
/// `L` 与结尾 `;` 不入树（反解时重建）。package_prefix 含结尾 '/'（缺省包
/// 为空视图）；首段的内部名 = package_prefix + segments[0].name，后缀段按
/// '$' 连接得完整内部名（JVMS 4.7.9.1 的二进制名映射）。
struct SigClassType
{
    std::string_view package_prefix;       ///< 含结尾 '/'；缺省包为空
    std::vector<SigClassSegment> segments; ///< ≥1 段
};

/// @brief 类型实参（TypeArgument）：无界 '*' / 上界 '+T' / 下界 '-T' / 不变 T。
struct SigTypeArg
{
    enum class Kind : ca::u8 {
        Invariant,  ///< 无通配符指示：实参即界
        UpperBound, ///< '+'：? extends 界
        LowerBound, ///< '-'：? super 界
        Unbounded,  ///< '*'：?（无界通配符，无界载荷）
    };

    Kind kind = Kind::Invariant;
    std::unique_ptr<struct SigType> bound;  ///< kind != Unbounded 时有效

    /// @brief 是否携带类型载荷（ '*' 返回 false）。
    bool hasBound() const noexcept { return kind != Kind::Unbounded; }
};

/// @brief 一个签名类型（JavaTypeSignature）：叶子 + 前置数组维数（扁平表示）。
///
/// 数组不递归：`[[Ljava/util/List<TT;>;` = kind=Class + dims=2（对齐描述符层
/// @ref TypeDesc 的扁平口径）。类型实参经 unique_ptr 持有下级类型，树因此
/// 只移动不拷贝；全部字符串借用输入串。
struct SigType
{
    enum class Kind : ca::u8 {
        Primitive,  ///< 基元叶子（B/C/D/F/I/J/S/Z；dims==0 仅在方法参数/返回位合法）
        Class,      ///< 类类型叶子（可带类型实参）
        TypeVar,    ///< 类型变量叶子（T name ;）
    };

    Kind kind = Kind::Class;
    BaseType primitive = BaseType::Int;      ///< kind==Primitive 时有效
    ca::u32 array_dims = 0;                  ///< 前置 '[' 维数（≤255）
    SigClassType class_type;                 ///< kind==Class 时有效
    std::string_view type_var_name;          ///< kind==TypeVar 时有效（非空）
};

/// @brief 一个类型参数（TypeParameter）：名 + 类界（可缺席）+ 接口界*。
///
/// 类界缺席 = 冒号后无类型（`<E::Ljava/lang/Comparable<TE;>;>` 的 javac 形
/// 态）；接口界至少带一个类型（语法强制）。
struct SigTypeParam
{
    std::string_view name;                   ///< 参数名（非空）
    std::unique_ptr<SigType> class_bound;    ///< nullptr = 类界缺席
    std::vector<SigType> interface_bounds;   ///< 每项必有类型
};

/// @brief 类签名（ClassSignature）：[类型参数] 父类 + 接口*。
struct SigClassSignature
{
    std::vector<SigTypeParam> type_params;   ///< 可空
    SigType superclass;                      ///< 语法保证：Class 且 dims==0
    std::vector<SigType> super_interfaces;   ///< 各项 Class 且 dims==0
};

/// @brief 方法签名（MethodSignature）：[类型参数] (参数*) 返回 throws*。
struct SigMethodSignature
{
    std::vector<SigTypeParam> type_params;   ///< 可空
    std::vector<SigType> params;             ///< 顺序保持
    bool ret_void = false;                   ///< 返回类型为 V
    SigType ret;                             ///< ret_void==false 时有效
    std::vector<SigType> throws_list;        ///< 各项 Class 或 TypeVar（^ 项）
};

/// @brief 一个泛型签名的解析树：三形态共用一个包装（对齐 MethodDesc 的
///        扁平风格；form 指明哪个成员有效）。
struct Signature
{
    SignatureForm form = SignatureForm::Field;
    SigClassSignature class_sig;    ///< form==Class
    SigMethodSignature method_sig;  ///< form==Method
    SigType field_sig;              ///< form==Field
};

// ---- 解析 ----

/// @brief 解析类签名。要求恰好一个完整 ClassSignature。
Result<SigClassSignature, SigError> parseClassSignature(std::string_view s);

/// @brief 解析方法签名。要求恰好一个完整 MethodSignature。
Result<SigMethodSignature, SigError> parseMethodSignature(std::string_view s);

/// @brief 解析字段签名（FieldTypeSignature）。裸基元（dims==0 的 Primitive）
///        在此非法——基元只在方法参数/返回位作为 TypeSignature 出现。
Result<SigType, SigError> parseFieldSignature(std::string_view s);

/// @brief 自动分派解析（形态未知的场合）。
///
/// 分派规则（成文）：`(` 开头必为方法；`[`/`T` 开头必为字段；`L`/`<` 开头先
/// 按类形态试，完整消费即类形态（`L...;` 单独出现时类形态是字段形态的语法
/// 超集，按类形态归一），类形态失败后 `L` 回退字段形态、`<` 回退方法形态；
/// 两者皆败时报告第一次尝试（类形态）的错误。
Result<Signature, SigError> parseSignature(std::string_view s);

// ---- 反解 ----

/// @brief 反解类类型到构建器（'L' 前缀与 ';' 由本函数产出）。
void appendClassType(ca::str::Utf8StringBuilder& b, const SigClassType& ct);

/// @brief 反解一个签名类型（数组维数 + 叶子）。
void appendSigType(ca::str::Utf8StringBuilder& b, const SigType& t);

/// @brief 反解一个完整签名树到构建器（按 form 分派三形态）。
void appendSignature(ca::str::Utf8StringBuilder& b, const Signature& sig);

/// @brief 反解为独立字符串。反解不校验（树的合法性由解析面保证；手工构造
///        的树按构造者契约）。
ca::str::Utf8String unparseSignature(const Signature& sig);

/// @brief 反解类签名。要求恰好一个完整 ClassSignature。
ca::str::Utf8String unparseClassSignature(const SigClassSignature& cs);

/// @brief 反解方法签名。
ca::str::Utf8String unparseMethodSignature(const SigMethodSignature& ms);

/// @brief 反解字段签名。
ca::str::Utf8String unparseFieldSignature(const SigType& t);

// ---- 手术面 ----

/// @brief 签名里一个类名的位置（手术坐标，语义对齐描述符层 @ref ClassNameSpan）。
struct SigClassNameSpan
{
    /// @brief span 覆盖的名字形态（决定 replace 时新名的合法字符集）。
    enum class Kind : ca::u8 {
        InternalName,  ///< 首段全名：package_prefix + 首段名（允许 '/'）
        SimpleName,    ///< 内部类后缀段名（禁止 '/'）
    };

    Kind kind = Kind::InternalName;
    ca::u32 offset = 0;  ///< 相对输入签名串的字节偏移
    ca::u32 length = 0;  ///< 名字长度
};

/// @brief 收集树中全部类名 span（按字节序：段名先于其实参内的下级类名）。
///
/// 类签名/方法签名/字段签名通用。@p sig 必须解析自 @p input（树节点视图指向
/// 它），span 偏移由视图地址差得出。
void collectClassNameSpans(const Signature& sig, std::string_view input,
                           std::vector<SigClassNameSpan>& out);

/// @brief 列出签名里全部类名的位置。form 必须与属性所在成员一致（形态歧义
///        时不猜）；语法非法即 Err，不产出半份数据。
Result<std::vector<SigClassNameSpan>, SigError>
classNameSpans(std::string_view signature, SignatureForm form);

/// @brief 把签名里 span 指向的类名改写为 new_name，产出新签名串。
///
/// 语义对齐描述符层 @ref replaceClassName：只改 span 覆盖的字节，其余原样
/// 保留。new_name 按 span 形态校验（InternalName 禁 . ; [ < > :，SimpleName
/// 另禁 '/'），越界或非法即 Err。
Result<ca::str::Utf8String, SigError>
replaceClassNameSpan(std::string_view signature, const SigClassNameSpan& span,
                     std::string_view new_name);

/// @brief 签名类名 rename 的结果：命中数 + 改写后的签名串。
struct SigRenameResult
{
    ca::u32 hits = 0;            ///< 命中并改写的类类型出现次数
    ca::str::Utf8String signature;  ///< hits==0 时与输入逐字节相同
};

/// @brief 顶层便捷面：parse→手术→反解一条龙（显式调用，Signature 属性的定
///        位与写回属消费者）。
///
/// 在签名中找内部名恰为 old_internal 的类类型改写为 new_internal。内部名在
/// 签名字节里有两种等价形态——'.' 段链（`Outer<T>.Inner`）与段名内嵌 '$'
/// （`Outer$Inner`，javac 常规产物）——统一按全名相等匹配（见 @ref
/// SigClassType），段链结构与段名内 '$' 层次原样保留；两者 '$' 段结构不一
/// 致即 Err(RenameShapeMismatch)——rename 面不做结构重排（内部类嵌套在字节
/// 码层面不可变）。其余字节原样保留；未命中返回 hits==0 与原串。
Result<SigRenameResult, SigError>
renameClassInSignature(std::string_view signature, SignatureForm form,
                       std::string_view old_internal, std::string_view new_internal);

/// @brief 类类型签名的完整内部名（首段全名 + 后缀段按 '$' 连接）。
///
/// `Lp/O<TT;>.I;` → `p/O$I`。分配产出（视图拼接需要新串）。
ca::str::Utf8String internalNameOf(const SigClassType& ct);

/// @brief 签名标识符合法性：非空且不含 JVMS 4.7.9.1 为 Identifier 保留的
///        7 个 ASCII 字符（. ; [ / < > :）。
bool isValidSigIdentifier(std::string_view name) noexcept;

/// @brief 签名首段全名合法性：@ref isValidSigIdentifier 且允许 '/'（包分隔）。
bool isValidSigInternalName(std::string_view name) noexcept;

/// @brief 签名内部类段名合法性：@ref isValidSigIdentifier 且禁止 '/'。
bool isValidSigSimpleName(std::string_view name) noexcept;

}   // namespace jbc::type
