/// @file        frame.hpp
/// @brief       帧引擎：操作数栈/局部变量的数据流（/）。
///
/// 单一起原则：写出与 analysis 用同一实现。两条计算档位（FramePolicy）：
/// BestEffort（缺省）是形状数据流——类型只到尺寸类（cat1/cat2/引用），
/// 合并冲突降级为 Top 并计数，足够 maxStack/maxLocals 的重算与自对拍；
/// Strict 档是类型精确数据流——transfer 携带精确类名与
/// Uninitialized 形态，合并经注入的 ITypeResolver 求解，算不动以指令级
/// 诊断显式拒绝（验收口径：可算者与 javac 一致、不可算者显式拒绝并给
/// 指令定位）。ITypeResolver 可注入：公共父类求解需要类层级，默认
/// ShapeResolver 不区分引用子类型；真实现归上层消费方（层次图求解）。
/// Strict 域自 C2 起建模 jsr/ret 子程序数据流（JVMS ：
/// jsr 压返回地址、ret 经局部槽跳回；单调用点精确，多调用点共享入口
/// 显式拒绝——见 ）。
#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "libca/core/result.hpp"

#include "jbc/core/class_file.hpp"
#include "jbc/ir/units.hpp"

namespace jbc::analysis {

/// @brief 值域接口前置声明（domain.hpp；FrameEngine 持其为实现载体）。
template <typename V>
class IValueDomain;

/// @brief 方法签名摘要前置声明（domain.hpp；播种入参用）。
struct MethodSig;

/// @brief 形状/类型格的值类别。
///
/// Top 与 Bottom 是两个语义不同的格元素：Top 是「未知」（合并时被任何已知
/// 类型吸收，单调向下）；Bottom 是「合并冲突」（吸收一切，best-effort 继续
/// 按 1 槽处理，Strict 下则触发显式拒绝）。二者混用会让格失去单调性——
/// 带环方法的数据流不收敛。
enum class VKind : ca::u8 {
    Top = 0,
    Int,
    Float,
    Long,
    Double,
    Ref,
    Null,
    UninitThis,
    Uninitialized,    ///< new 产出的未初始化实例（按 new 指令 pc 判等）
    ReturnAddress,    ///< jsr 压入的返回地址（按调用方 jsr 指令 pc 判等）
    Bottom,
};

/// @brief 栈/局部变量槽的值类型（值语义）。
///
/// - BestEffort：只有 k（+ Ref 的 cls 诊断）有效，name 恒空。
/// - Strict：Ref 携带精确内部名（name，借用 CP 视图，寿命 = 常量池）；
///   Uninitialized 的 cls 字段承载产生方 new 指令的 pc（uninitOffset()）；
///   ReturnAddress 的 cls 字段承载调用方 jsr 指令的 pc（jsrPc()，C2）。
///   UninitThis 只出现在构造器超调用前的 this 槽。
struct VType
{
    VKind k = VKind::Top;
    ca::u32 cls = 0;       ///< Ref 的 CP Class 索引；Uninitialized/ReturnAddress 时为指令 pc
    std::string_view name; ///< Strict：Ref 的精确内部名（借用视图，可为空）

    /// 槽位数（cat2 = 2）。
    ca::u8 slots() const noexcept { return k == VKind::Long || k == VKind::Double ? 2 : 1; }

    /// Uninitialized 形态：产生方 new 指令的 pc。
    ca::u32 uninitOffset() const noexcept { return cls; }

    /// ReturnAddress 形态：调用方 jsr 指令的 pc（ret 据此跳回其顺序后继）。
    ca::u32 jsrPc() const noexcept { return cls; }

    /// 精确类名引用形态（Strict）。
    static VType ref(std::string_view internalName) noexcept
    {
        return VType{VKind::Ref, 0, internalName};
    }

    /// Uninitialized 形态：pc 为产生方 new 指令偏移。
    static VType uninitNew(ca::u32 newPc) noexcept
    {
        return VType{VKind::Uninitialized, newPc, {}};
    }

    /// ReturnAddress 形态：pc 为调用方 jsr 指令偏移。
    static VType retAddr(ca::u32 jsrPc) noexcept
    {
        return VType{VKind::ReturnAddress, jsrPc, {}};
    }

    /// Strict 帧比对语义的相等（k + 名字 + uninit offset；cls 诊断位不计）。
    bool sameAs(const VType& o) const noexcept
    {
        return k == o.k && cls == o.cls && name == o.name;
    }
};

/// @brief Strict 域合并失败的语义分类（ITypeResolver 的错误面）。
enum class TypeMergeError : ca::u8 {
    NoCommonParent,   ///< 两引用类型在注入的类层级下无公共父类
    Unresolvable,     ///< resolver 无法求解（类层级缺席、外部查询失败等）
};

/// @brief 引用合并策略接口（merge 单方法面，A1 契约）。
///
/// 只解决「两个具体引用类型的公共父类」；结构性规则（Top 吸收、Null 底、
/// Uninitialized 按 offset 判等）由计算域内置，不经此接口。实现体归
/// 上层消费方（层次图求解）；libjbc 内置 ShapeResolver 作默认降级。字符串
/// 参数是借用视图（寿命 = 常量池）。
class ITypeResolver
{
public:
    virtual ~ITypeResolver() = default;

    /// @brief 求解两个引用值的公共父类；不可解时返回 Err（Strict 域据此
    ///        显式拒绝并给指令定位，BestEffort 域降级为 Bottom 继续）。
    virtual ca::core::Result<VType, TypeMergeError> merge(VType a, VType b) = 0;
};

/// @brief 默认宽松合并：Top 被已知类型吸收；引用族互并 → Ref（丢具体类）；
///        尺寸类冲突 → Bottom（吸收一切，best-effort 继续）。永不失败。
class ShapeResolver : public ITypeResolver
{
public:
    ca::core::Result<VType, TypeMergeError> merge(VType a, VType b) override;
};

/// @brief 帧计算档位（三态）。
enum class FramePolicy : ca::u8 {
    BestEffort = 0,   ///< 缺省：形状数据流（现状行为，逐位不变）
    Strict = 1,       ///< 类型精确：必须注入 ITypeResolver；不可算显式 Err + failAt
    Preserve = 2,     ///< 读侧档：既有 SMT → 语义帧视图；实现在
                      ///<   smt_read.hpp 的独立入口（读属性不是数据流），
                      ///<   compute 对本档恒显式拒绝（防止误导性路由）
};

/// @brief 帧计算结果（best-effort 与 strict 共用；strict 失败经 fail+failAt
///        显式交付，不伪造帧）。
struct FrameSummary
{
    bool ok = false;          ///< 数据流是否无异常完成（合并降级不算失败）
    ca::u32 maxStack = 0;     ///< 观测到的最大栈深度（槽位计）
    ca::u32 maxLocals = 0;    ///< 触达的最大局部变量号（含 2 槽扩展）
    ca::usize insns = 0;      ///> 参与计算的指令数
    ca::usize merges = 0;     ///> 合并点次数（诊断）
    ca::usize degraded = 0;   ///> 类型冲突降级为 Top 的次数（best-effort 度量）
    enum class Fail {
        None = 0,
        Underflow,     ///< 栈下溢（数据流不一致——畸形或超出 best-effort 范围）
        BadLocal,      ///< 局部变量索引越界（> 65535 语义外）
        BadCpRef,      ///< CP 引用无法解析（描述符缺失等）
        Wide,
        NoFixpoint,    ///< 访问量超熔断阈值：状态不收敛（建模缺陷的兜底报告）
        Subroutine,    ///< 子程序路径畸形/不可算：ret 读非返回地址槽、
                       ///<   返回点越界（jsr 无顺序后继）、多调用点共享入口的
                       ///<   returnAddress 合并冲突（JVMS 4.10.2.5：只与自身
                       ///<   合并）；jsr/ret 数据流本身已建模，不再整体拒绝
        TypeConflict,  ///< Strict：跨类合并/异 offset Uninit 合并等不可算冲突
        NoResolver,    ///< Strict：未注入 ITypeResolver（fail loud，不降级）
        NotSupported,  ///< Preserve：占位档被选入计算路径（读侧归 smt_read）
    } fail = Fail::None;
    ir::InsnId failAt = ir::kNoInsn;   ///< fail != None 时的现场指令（诊断定位）
    bool hasSubroutine = false;        ///< 方法体含 RET（jsr/ret 遗留；BestEffort
                                       ///<   只计数不建模，Strict 建模数据流）
};

const char* to_string(FrameSummary::Fail f) noexcept;

/// @brief 一条指令「到达帧」的快照：进入该指令时的 locals/stack 形状。
///
/// 值语义、自持有——与旧栈 analysis 的 `Analyzer::getFrames()` 相对（那边
/// 结果借用 Analyzer 寿命，是 ownership-model 六制之⑥；这里拷贝即独立）。
/// reachable=false 表示死代码（无到达帧）。Strict 档的 VType.name 是借用
/// 视图（CP / 域内驻留表），寿命随常量池/引擎，不随本结构体。
struct FrameAt
{
    bool reachable = false;
    std::vector<VType> locals;
    std::vector<VType> stack;
};

/// @brief 帧引擎：对 promote 后的 CodeUnit 做帧数据流。
///
/// v0.3.8 起是 DomainEngine<VType> 的壳（domain.hpp 模板引擎）；档位由
/// FramePolicy 选择：BestEffort 挂 ShapeDomain/ResolverDomain（现状行为，
/// 逐位不变），Strict 挂 StrictDomain（类型精确 + 显式拒绝）。实现单一
/// 起原则不破：本类不含数据流逻辑，只做 DomainResult → FrameAt 的转换。
class FrameEngine
{
public:
    /// @brief 注入引用合并策略（公共父类求解需要类层级时）；档位 BestEffort。
    FrameEngine(const core::ConstantPool& cp, ITypeResolver& resolver);

    /// @brief 默认形态：内置 ShapeDomain，一步构造（等价 BestEffort）。
    explicit FrameEngine(const core::ConstantPool& cp);

    /// @brief 档位形态：Strict 未随附 resolver 时 compute 显式 Err(NoResolver)
    ///        （fail loud）；Preserve 档 compute 恒 Err(NotSupported)——读侧
    ///        语义是读属性不是数据流，装配点在 smt_read.hpp 的独立入口。
    FrameEngine(const core::ConstantPool& cp, FramePolicy policy);

    /// @brief 全形态：档位 + 注入 resolver（Strict 的唯一合法构造）。
    FrameEngine(const core::ConstantPool& cp, ITypeResolver& resolver, FramePolicy policy);

    ~FrameEngine();

    FrameEngine(const FrameEngine&)            = delete;
    FrameEngine& operator=(const FrameEngine&) = delete;

    /// @brief 计算方法帧（summary 快路径；扫描/自预言用；不播种入参）。
    FrameSummary compute(const ir::CodeUnit& u);

    /// @brief 计算并交付逐指令到达帧（对标旧栈 Analyzer::getFrames，但
    ///        结果自持有）。out 与 order() 等长按下标对齐；ok=false 时
    ///        内容不完整，以 summary 为准。
    FrameSummary compute(const ir::CodeUnit& u, std::vector<FrameAt>& out);

    /// @brief 按签名播种入参后计算（Strict 的 javac 对齐形态：引用参数
    ///        携带描述符精确类名、构造器 this 槽播 UninitThis）。
    FrameSummary compute(const ir::CodeUnit& u, const MethodSig& sig);

    /// @brief 播种 + 逐指令帧交付（与 order() 下标对齐）。
    FrameSummary compute(const ir::CodeUnit& u, const MethodSig& sig,
                         std::vector<FrameAt>& out);

private:
    const core::ConstantPool& cp_;
    FramePolicy policy_ = FramePolicy::BestEffort;
    ITypeResolver* resolver_ = nullptr;   ///< 借用；调用期需存活（签名可见）
    std::unique_ptr<IValueDomain<VType>> dom_;
};

/// @brief 描述符的工具函数（帧引擎公开给测试与上层）。
/// 方法描述符参数部分的槽位数（J/D 为 2，其余 1）；解析失败返回 -1。
ca::i32 methodArgSlots(std::string_view desc);
/// 方法描述符返回值槽位数（void=0，J/D=2，其余 1）；解析失败返回 -1。
ca::i32 methodRetSlots(std::string_view desc);
/// 字段描述符槽位数（J/D=2，其余 1）；非法返回 -1。
ca::i32 fieldSlots(std::string_view desc);

/// @brief SMT 链头（隐式初始帧）的 locals 条目序列（entry 口径：cat2 一参
///        一条；）。
///
/// this 形态（构造器 UninitThis / 其余精确类名）与描述符参数逐条入列，与
/// 引擎 MethodSig 播种（domain.hpp analyze）同源同义——那边走域钩子按槽
/// 播种（cat2 双槽 + bottom 行），这边是 SMT 条目空间的同一事实。SMT 读侧
/// （readFramesFromSmt）以本序列为链头；槽级展开（cat2 → 值槽 + Top 垫底）
/// 由交付层按需进行。
std::vector<VType> initialLocalsOf(const MethodSig& sig);

/// @brief 把帧计算观测到的 maxes 采用到 CodeUnit（显式 API）。
///
/// 语义按裁决执行：Strict 档显式重算 = 覆盖——重算结果即真值，CodeUnit 里
/// 的现值只是旧栈产物；低于原值亦采用（Top 支配口径下重算值与 verifier
/// 一致）。本函数只做这一件事，不触碰 lower/管线：BestEffort 手动链路与
/// promote 拷贝语义零变化，「lower 时自动重算」的隐式接线不做（管线级策略
/// 归后续批）。
///
/// 两个 verifier 地板并入采用值（FrameSummary 记的是数据流触达，不含两条
/// 规范性底线； 成文）：
/// - maxLocals ≥ 参数槽地板（this + 描述符参数，按 @p sig 求值）——引擎的
///   maxLocals 只计 store 触达，只读参数的方法观测值会低于 verifier 要求；
/// - maxStack ≥ 1（@p u 带异常表时）——handler 入口栈恒有异常值。
///
/// @return 是否采用。summary 非 ok（失败计算无真值）或地板并入后的取值超
///         出 u16 编码域（Code 属性 maxes 是 u16，截断即伪造）时不采用且
///         不改动任何字段——调用方以返回值为准，不隐默丢。
bool applyComputedMaxes(ir::CodeUnit& u, const FrameSummary& computed,
                        const MethodSig& sig) noexcept;

}   // namespace jbc::analysis
