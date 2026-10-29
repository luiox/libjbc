/// @file        domain.hpp
/// @brief       值域搭车引擎：DomainEngine<V> + IValueDomain<V>。
///
/// 架构：引擎持有全部形状迁移知识（控制流 worklist、cat2 槽布局、dup 槽位
/// 置换、get/put/invoke 描述符效应——frame.cpp 时代修出的那一整链），值域 V
/// 作为载荷随槽移动；消费者只实现域钩子（必选五个，可选细分全部有默认）。
/// 旧栈 Interpreter<V> 换个值域要重写整张 per-opcode transfer 表（612 行/
/// 域起跳，三个 interpreter 各自如此）；本层把 transfer 留在引擎。
///
/// FrameEngine（frame.hpp）是本模板的内置实例化：ShapeDomain 产出 VType，
/// 单一起原则不破——实现只有这一份。
#pragma once

#include <cstddef>
#include <deque>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "jbc/analysis/frame.hpp"
#include "jbc/ir/units.hpp"

namespace jbc::analysis {

/// @brief 值域契约：V 必须可默认构造（bottom 行与垫底填充用）、拷贝便宜
///        （引擎按值搬槽）；merge 必须单调（top 最大），否则熔断兜底。
template <typename V>
class IValueDomain
{
public:
    virtual ~IValueDomain() = default;

    // ---- 必选五钩子 ----

    /// 未知值（合并垫底 / 死区填充）。
    virtual V top() = 0;
    /// 域 meet（含冲突吸收策略，域说了算）。
    virtual V merge(const V& a, const V& b) = 0;
    /// 一切产值点：常量、ldc、运算结果（未被 binary/unary 接管时）、
    /// new/checkcast、字段/调用返回值、无局部来源的 load。at 即产生该
    /// 指令的句柄（来源追踪的语义就是它——Insn 本体不带身份，故显式传）。
    virtual V newValue(const ir::Insn& producer, ir::InsnId at, VKind shape) = 0;
    /// 方法入参（按描述符播种 local 槽）。
    virtual V newParameter(ca::u32 localIdx, VKind shape) = 0;
    /// 异常处理器入口的 throwable。
    virtual V newException(const ir::CodeUnit::Exc& entry) = 0;

    // ---- 可选细分（默认退回 newValue / 恒等——不写就是纯搭车）----

    /// 实例方法 this 槽的播种形态。默认退回 newParameter(0, Ref)；
    /// Strict 域覆写：构造器播 UninitThis，其余播精确类名。
    virtual V newThis(bool isCtor, std::string_view thisClass)
    {
        (void)isCtor;
        (void)thisClass;
        return newParameter(0, VKind::Ref);
    }

    /// 按描述符播种引用参数的精确形态（typeText 是描述符里的类型原文，
    /// 如 "Ljava/lang/String;"/"[I"）。默认退回 newParameter。
    virtual V newParameterTyped(ca::u32 localIdx, VKind shape, std::string_view typeText)
    {
        (void)typeText;
        return newParameter(localIdx, shape);
    }

    /// invokespecial <init> 完成：receiver 的已初始化形态（§10.1 翻转）。
    /// 引擎把该值替换进状态里全部与 receiver 相等的行；默认恒等
    /// （形状域无 Uninitialized 语义，翻转即无操作——best-effort 零漂移）。
    virtual V initializedForm(const V& receiver, const ir::Insn& call, ir::InsnId id)
    {
        (void)call;
        (void)id;
        return receiver;
    }

    /// jsr/jsr_w 压入的返回地址形态。默认退回 newValue(Int)——形状域
    /// 不建模返回地址（legacy 行为逐位不变）；Strict 域覆写为携带调用方
    /// jsr pc 的 ReturnAddress。
    virtual V newReturnAddress(const ir::Insn& producer, ir::InsnId at)
    {
        return newValue(producer, at, VKind::Int);
    }

    /// RET 执行点（jsr/ret 遗留；引擎照旧记 hasSubroutine）。默认无操作。
    /// Strict 域建模 ret 数据流（跳回由引擎按返回地址槽计算），
    /// 本钩子仅保留给不建模子程序的域做观测/拒绝。
    virtual void onRet(const ir::Insn& in, ir::InsnId id)
    {
        (void)in;
        (void)id;
    }

    /// 是否建模子程序数据流。默认 false：引擎对 RET 保持 legacy 行为
    /// （degraded 计数，无跳回边）；Strict 域覆写 true：RET 不计 degraded，
    /// 引擎读返回地址槽发跳回边。
    virtual bool modelsSubroutines() const { return false; }

    /// ret 返回地址求解（引擎只对 modelsSubroutines()=true 的域调用）。
    /// 槽值可解析为返回地址（携带调用方 jsr pc）时返回 true；否则 false
    /// （ret 读非返回地址槽——畸形，引擎显式拒绝并定位到 ret）。
    virtual bool retSiteOf(const V& slotValue, ca::u32& jsrPc) const
    {
        (void)slotValue;
        (void)jsrPc;
        return false;
    }

    /// 异常边精确传播开关。关闭（默认）：handler 入口由异常表
    /// 播种（空 locals + [newException]），种子以「待并参与者」形态持有，
    /// 与真实入边同权汇合（verifier 口径：入口帧 = 全部入边 join）——不再
    /// 抢先单跑，真实边缺席时由 worklist 排空后的种子倾泻兜底（异常-only
    /// 处理器语义不变）。开启（Strict 域）：引擎不播种，改为对 try 范围内
    /// 每条可达指令逐覆盖 handler 发异常出口边（§4.10.1.4 保守口径，与
    /// verifier 对有帧覆盖指令的 handler 检查同域）——入口状态 = 指令点
    /// in-state locals + [catch_type 对应类型]，handler 可达 ⟺ 范围内至
    /// 少一条可达指令（死范围的 handler 不可达，verifier 对死代码同样不
    /// 出约束）。
    virtual bool preciseExceptionEdges() const { return false; }

    /// Strict 类域的显式失败通道：域钩子内部遇到「不可算」时置位，引擎
    /// 在每次 transfer / 状态合并后查询——非 None 立即终止并把 failAt
    /// 定位到当前指令（transfer）/ 合并点指令（merge）。默认恒 None。
    virtual FrameSummary::Fail failCode() const { return FrameSummary::Fail::None; }

    /// 清除失败通道（引擎每轮 analyze 入口调用；无失败语义的域为无操作）。
    virtual void clearFail() {}

    /// 二元运算（算术/位/移位/比较）。默认「结果来源 = 本指令」。
    virtual V binary(core::Op op, const V& a, const V& b, VKind shape,
                     const ir::Insn& at, ir::InsnId id)
    {
        (void)op;
        (void)a;
        (void)b;
        return newValue(at, id, shape);
    }
    /// 一元运算（取负/转换）。默认同上。
    virtual V unary(core::Op op, const V& a, VKind shape, const ir::Insn& at, ir::InsnId id)
    {
        (void)op;
        (void)a;
        return newValue(at, id, shape);
    }
    /// 局部变量读：local 槽里有已存值时走这里（常量传播靠它穿透
    /// store→load）；空槽由引擎 fallback 到 newValue(load, shape)。
    /// 默认恒等（形状域不关心存过什么——形状来自 opcode）。
    virtual V loadLocal(const ir::Insn& load, ir::InsnId id, const V& stored, ca::u32 idx)
    {
        (void)load;
        (void)id;
        (void)idx;
        return stored;
    }
    /// 局部变量写前变换（默认恒等；形状域按 storeKind 归一——栈上形状与
    /// store 形状不一致时以 opcode 为准，如 aconst_null; astore 的 Null→Ref）。
    virtual V storeLocal(const ir::Insn& store, ir::InsnId id, const V& v, ca::u32 idx)
    {
        (void)store;
        (void)id;
        (void)idx;
        return v;
    }
    /// 值宽度（槽位数，cat2=2）。默认 1；VType 域按 slots() 报告。
    virtual ca::u8 width(const V& v)
    {
        (void)v;
        return 1;
    }
    /// fixpoint 判定用相等（默认逐字段需要域自己给；形状域按 k+cls）。
    virtual bool equal(const V& a, const V& b) = 0;
};

/// @brief 槽：值域载荷 + cat2 底槽标记。槽位迁移（dup/swap/弹压）作用于行。
template <typename V>
struct SlotV
{
    V val{};
    bool bottom = false;   // cat2 第二槽：随值整体移动，val 为默认构造的 V
};

/// @brief 一条指令的到达帧（域值形态；与 FrameAt 同构，bottom 保留）。
template <typename V>
struct FrameV
{
    bool reachable = false;
    std::vector<SlotV<V>> locals;
    std::vector<SlotV<V>> stack;
};

/// @brief 域分析结果：summary（同 FrameSummary）+ 逐指令帧（与 order() 对齐）。
template <typename V>
struct DomainResult
{
    FrameSummary summary;
    std::vector<FrameV<V>> frames;
};

/// @brief 方法签名摘要：入参播种需要（access 静态位 + 描述符）。
///
/// name/thisClass 是 Strict 档的精确化输入（借用的 CP 视图）：name 用于
/// 识别构造器（this 槽播 UninitThis，§10.1），thisClass 供 this 槽的
/// 精确类名与 UninitThis 翻转目标；缺席时相应槽位退化为无名 Ref。
struct MethodSig
{
    bool isStatic = false;
    std::string_view desc;
    std::string_view name = {};       ///< 方法名（"<init>" 识别构造器）
    std::string_view thisClass = {};  ///< 实例方法 this 的内部名（可空）
};

namespace detail {

/// 描述符扫描（frame.cpp 同源；模板引擎与公开工具共用一版逻辑，此处在
/// 头内供模板使用，公开包装仍在 frame.cpp）。
inline std::string_view oneType(std::string_view d, ca::usize pos)
{
    if (pos >= d.size()) {
        return {};
    }
    if (d[pos] == '[') {
        ca::usize i = pos;
        while (i < d.size() && d[i] == '[') {
            ++i;
        }
        if (i < d.size() && d[i] == 'L') {
            const ca::usize e = d.find(';', i);
            return e == std::string_view::npos ? std::string_view{}
                                               : d.substr(pos, e + 1 - pos);
        }
        return d.substr(pos, i + 1 - pos);
    }
    if (d[pos] == 'L') {
        const ca::usize e = d.find(';', pos);
        return e == std::string_view::npos ? std::string_view{} : d.substr(pos, e + 1 - pos);
    }
    return d.substr(pos, 1);
}

inline ca::i32 typeSlots(std::string_view t)
{
    if (t.empty()) {
        return -1;
    }
    if (t == "J" || t == "D") {
        return 2;
    }
    if (t == "V") {
        return 0;
    }
    if (t.size() == 1) {
        switch (t[0]) {
        case 'B':
        case 'C':
        case 'F':
        case 'I':
        case 'S':
        case 'Z': return 1;
        default: return -1;
        }
    }
    return (t[0] == '[' || t[0] == 'L') ? 1 : -1;   // 数组与类类型都是 1 槽引用
}

inline VKind kindOfType(std::string_view t)
{
    if (t == "J") {
        return VKind::Long;
    }
    if (t == "D") {
        return VKind::Double;
    }
    if (t == "F") {
        return VKind::Float;
    }
    if (t.size() >= 1 && (t[0] == 'L' || t[0] == '[')) {
        return VKind::Ref;
    }
    return VKind::Int;
}

/// 紧凑 load/store 的局部变量号：load 族与 store 族各以自家 0 号为基。
inline ca::u32 compactLocalIdx(core::Op op) noexcept
{
    const ca::u8 v    = static_cast<ca::u8>(op);
    const ca::u8 base = v <= static_cast<ca::u8>(core::Op::ALOAD_3)
                            ? static_cast<ca::u8>(core::Op::ILOAD_0)
                            : static_cast<ca::u8>(core::Op::ISTORE_0);
    return (v - base) % 4;
}

/// load 族（含紧凑形态）推入的形状。
inline VKind loadKind(core::Op op) noexcept
{
    switch (op) {
    case core::Op::LLOAD:
    case core::Op::LLOAD_0:
    case core::Op::LLOAD_1:
    case core::Op::LLOAD_2:
    case core::Op::LLOAD_3: return VKind::Long;
    case core::Op::FLOAD:
    case core::Op::FLOAD_0:
    case core::Op::FLOAD_1:
    case core::Op::FLOAD_2:
    case core::Op::FLOAD_3: return VKind::Float;
    case core::Op::DLOAD:
    case core::Op::DLOAD_0:
    case core::Op::DLOAD_1:
    case core::Op::DLOAD_2:
    case core::Op::DLOAD_3: return VKind::Double;
    default: return op == core::Op::ALOAD || (op >= core::Op::ALOAD_0 && op <= core::Op::ALOAD_3)
                                        ? VKind::Ref
                                        : VKind::Int;
    }
}

/// store 族弹出的形状（含紧凑形态）。
inline VKind storeKind(core::Op op) noexcept
{
    switch (op) {
    case core::Op::LSTORE:
    case core::Op::LSTORE_0:
    case core::Op::LSTORE_1:
    case core::Op::LSTORE_2:
    case core::Op::LSTORE_3: return VKind::Long;
    case core::Op::FSTORE:
    case core::Op::FSTORE_0:
    case core::Op::FSTORE_1:
    case core::Op::FSTORE_2:
    case core::Op::FSTORE_3: return VKind::Float;
    case core::Op::DSTORE:
    case core::Op::DSTORE_0:
    case core::Op::DSTORE_1:
    case core::Op::DSTORE_2:
    case core::Op::DSTORE_3: return VKind::Double;
    default: return op == core::Op::ASTORE || (op >= core::Op::ASTORE_0 && op <= core::Op::ASTORE_3)
                                        ? VKind::Ref
                                        : VKind::Int;
    }
}

/// 成员引用的描述符（Methodref/InterfaceMethodref/Fieldref → NaT.b；
/// InvokeDynamic 的 b 直接是 NaT）。
inline std::string_view memberDesc(const core::ConstantPool& cp, ca::u32 cpIdx, bool& ok)
{
    ok = false;
    if (!cp.valid(cpIdx)) {
        return {};
    }
    const auto& e = cp.at(cpIdx);
    if (e.tag != core::CpTag::Methodref && e.tag != core::CpTag::InterfaceMethodref &&
        e.tag != core::CpTag::Fieldref && e.tag != core::CpTag::InvokeDynamic) {
        return {};
    }
    if (!cp.valid(e.b)) {
        return {};
    }
    const auto& nat = cp.at(e.b);
    if (nat.tag != core::CpTag::NameAndType || !cp.isUtf8(nat.b)) {
        return {};
    }
    ok = true;
    return cp.utf8Of(nat.b);
}

/// 成员引用的名字（Methodref/InterfaceMethodref/Fieldref → NaT.a）。
/// invokespecial 的 "<init>" 识别（§10.1 翻转触发判定）用。
inline std::string_view memberName(const core::ConstantPool& cp, ca::u32 cpIdx, bool& ok)
{
    ok = false;
    if (!cp.valid(cpIdx)) {
        return {};
    }
    const auto& e = cp.at(cpIdx);
    if (e.tag != core::CpTag::Methodref && e.tag != core::CpTag::InterfaceMethodref &&
        e.tag != core::CpTag::Fieldref) {
        return {};
    }
    if (!cp.valid(e.b)) {
        return {};
    }
    const auto& nat = cp.at(e.b);
    if (nat.tag != core::CpTag::NameAndType || !cp.isUtf8(nat.a)) {
        return {};
    }
    ok = true;
    return cp.utf8Of(nat.a);
}

/// CP Class 条目的内部名；无效/非 Class 返回空。
inline std::string_view classNameOf(const core::ConstantPool& cp, ca::u32 cpIdx)
{
    if (!cp.valid(cpIdx) || cp.tagOf(cpIdx) != core::CpTag::Class) {
        return {};
    }
    const auto& e = cp.at(cpIdx);
    return cp.isUtf8(e.a) ? cp.utf8Of(e.a) : std::string_view{};
}

/// 成员引用 owner 类的内部名（Methodref/InterfaceMethodref/Fieldref）。
inline std::string_view refOwnerOf(const core::ConstantPool& cp, ca::u32 cpIdx, bool& ok)
{
    ok = false;
    if (!cp.valid(cpIdx)) {
        return {};
    }
    const auto& e = cp.at(cpIdx);
    if (e.tag != core::CpTag::Methodref && e.tag != core::CpTag::InterfaceMethodref &&
        e.tag != core::CpTag::Fieldref) {
        return {};
    }
    const std::string_view name = classNameOf(cp, e.a);
    ok = !name.empty();
    return name;
}

/// 描述符类型文本（"Ljava/lang/String;"/"[I"）的引用语义内部名；
/// 非引用类型（原始类别）返回空。
inline std::string_view refNameOfType(std::string_view t)
{
    if (t.size() >= 2 && t.front() == 'L' && t.back() == ';') {
        return t.substr(1, t.size() - 2);
    }
    if (!t.empty() && t.front() == '[') {
        return t;
    }
    return {};
}

/// 数据流状态（域值形态）。
template <typename V>
struct DomState
{
    std::vector<SlotV<V>> locals;
    std::vector<SlotV<V>> stack;
};

/// 单条指令的效应应用器（模板）：形状迁移引擎，值由域供给。
template <typename V>
struct Machine
{
    const core::ConstantPool& cp;
    IValueDomain<V>& dom;
    FrameSummary& out;
    DomState<V> st;

    void pushVal(V v, FrameSummary::Fail& fail)
    {
        (void)fail;
        if (dom.width(v) == 2) {
            st.stack.push_back(SlotV<V>{V{}, true});
        }
        st.stack.push_back(SlotV<V>{std::move(v), false});
        if (st.stack.size() > out.maxStack) {
            out.maxStack = static_cast<ca::u32>(st.stack.size());
        }
    }

    /// 弹一个计算值（cat2 连底槽）；outVal 为值行载荷。
    ///
    /// 值行恒在栈顶：cat2 以 [底槽行, 值行] 成对入栈，dup 家族按槽组
    /// 整体搬移不破坏行序，因此取 back() 对 width 1/2 都是值行。
    void popValue(ca::u8 width, V& outVal, FrameSummary::Fail& fail)
    {
        const ca::usize need = width;
        if (st.stack.size() < need) {
            fail = FrameSummary::Fail::Underflow;
            return;
        }
        outVal = st.stack.back().val;
        st.stack.resize(st.stack.size() - width);
    }

    /// 弹 n 个槽位（机械槽语义；pop/pop2 与已知纯槽效应的场景）。
    void popSlots(ca::u32 n, FrameSummary::Fail& fail)
    {
        if (st.stack.size() < n) {
            fail = FrameSummary::Fail::Underflow;
            return;
        }
        st.stack.resize(st.stack.size() - n);
    }

    /// 复制栈顶 n 个槽，插入到距栈顶再往下 skip 个槽的位置（dup 家族的
    /// 统一槽位语义；cat2 的值槽+底槽随行整体移动）。
    void dupSlots(ca::u32 n, ca::u32 skip, FrameSummary::Fail& fail)
    {
        const ca::usize need = n + skip;
        if (st.stack.size() < need) {
            fail = FrameSummary::Fail::Underflow;
            return;
        }
        const auto first = st.stack.end() - static_cast<std::ptrdiff_t>(n);
        st.stack.insert(first - static_cast<std::ptrdiff_t>(skip), first, st.stack.end());
        if (st.stack.size() > out.maxStack) {
            out.maxStack = static_cast<ca::u32>(st.stack.size());
        }
    }

    void storeLocal(ca::u32 idx, V v, FrameSummary::Fail& fail)
    {
        if (idx > 0xFFFE) {
            fail = FrameSummary::Fail::BadLocal;
            return;
        }
        const ca::u8 w   = dom.width(v);
        const ca::u32 need = idx + w;
        if (need > out.maxLocals) {
            out.maxLocals = need;
        }
        if (st.locals.size() < need) {
            st.locals.resize(need, SlotV<V>{dom.top(), false});
        }
        st.locals[idx] = SlotV<V>{std::move(v), false};
        if (w == 2) {
            st.locals[idx + 1] = SlotV<V>{V{}, true};
        }
    }

    /// 执行一条指令的效应。
    void apply(const ir::Insn& in, ir::InsnId id, FrameSummary::Fail& fail);

private:
    /// Local1 语义族（含紧凑形态）。
    void applyLocal(const ir::Insn& in, ir::InsnId id, FrameSummary::Fail& fail);
};

template <typename V>
void Machine<V>::applyLocal(const ir::Insn& in, ir::InsnId id, FrameSummary::Fail& fail)
{
    using core::Op;
    const Op op = in.op;

    const auto localIdx = [&]() -> ca::u32 {
        const ca::u8 v = static_cast<ca::u8>(op);
        const bool compactL = v >= static_cast<ca::u8>(Op::ILOAD_0) && v <= static_cast<ca::u8>(Op::ALOAD_3);
        const bool compactS = v >= static_cast<ca::u8>(Op::ISTORE_0) && v <= static_cast<ca::u8>(Op::ASTORE_3);
        if (compactL || compactS) {
            return compactLocalIdx(op);
        }
        return static_cast<ca::u32>(in.a);
    };

    const bool loadFamily = op == Op::ILOAD || op == Op::LLOAD || op == Op::FLOAD ||
                            op == Op::DLOAD || op == Op::ALOAD ||
                            (op >= Op::ILOAD_0 && op <= Op::ALOAD_3);
    if (loadFamily) {
        const ca::u32 idx = localIdx();
        const VKind k     = loadKind(op);
        if (idx < st.locals.size() && !st.locals[idx].bottom) {
            pushVal(dom.loadLocal(in, id, st.locals[idx].val, idx), fail);
        } else {
            pushVal(dom.newValue(in, id, k), fail);
        }
    } else if (op == Op::RET) {
        out.hasSubroutine = true;   // 显式标记：方法体含 RET（jsr/ret 遗留）
        if (!dom.modelsSubroutines()) {
            ++out.degraded;         // 不建模子程序的域：保持 legacy 计数
        }
        dom.onRet(in, id);   // Strict 域由引擎读返回地址槽发跳回边
    } else {
        const VKind k = storeKind(op);
        V v{};
        popValue(static_cast<ca::u8>(k == VKind::Long || k == VKind::Double ? 2 : 1), v, fail);
        if (fail == FrameSummary::Fail::None) {
            storeLocal(localIdx(), dom.storeLocal(in, id, v, localIdx()), fail);
        }
    }
}

template <typename V>
void Machine<V>::apply(const ir::Insn& in, ir::InsnId id, FrameSummary::Fail& fail)
{
    using core::Op;
    using core::Shape;
    const Op op = in.op;

    // 二元运算助手：按两个操作数宽度弹值，经域折叠，按结果形状压栈。
    const auto binaryOp = [&](ca::u8 wa, ca::u8 wb, VKind result) {
        V b{};
        V a{};
        popValue(wb, b, fail);
        if (fail == FrameSummary::Fail::None) {
            popValue(wa, a, fail);
        }
        if (fail == FrameSummary::Fail::None) {
            pushVal(dom.binary(op, a, b, result, in, id), fail);
        }
    };
    const auto unaryOp = [&](ca::u8 wa, VKind result) {
        V a{};
        popValue(wa, a, fail);
        if (fail == FrameSummary::Fail::None) {
            pushVal(dom.unary(op, a, result, in, id), fail);
        }
    };

    // 紧凑 load/store 族：操作数布局是 None（单字节无操作数），但语义是
    // Local1——shape 分派前拦截。
    if ((op >= Op::ILOAD_0 && op <= Op::ALOAD_3) ||
        (op >= Op::ISTORE_0 && op <= Op::ASTORE_3)) {
        applyLocal(in, id, fail);
        return;
    }

    switch (shapeOf(op)) {
    case Shape::None:
        switch (op) {
        // 常量
        case Op::ACONST_NULL: pushVal(dom.newValue(in, id, VKind::Null), fail); return;
        case Op::ICONST_M1:
        case Op::ICONST_0:
        case Op::ICONST_1:
        case Op::ICONST_2:
        case Op::ICONST_3:
        case Op::ICONST_4:
        case Op::ICONST_5: pushVal(dom.newValue(in, id, VKind::Int), fail); return;
        case Op::LCONST_0:
        case Op::LCONST_1: pushVal(dom.newValue(in, id, VKind::Long), fail); return;
        case Op::FCONST_0:
        case Op::FCONST_1:
        case Op::FCONST_2: pushVal(dom.newValue(in, id, VKind::Float), fail); return;
        case Op::DCONST_0:
        case Op::DCONST_1: pushVal(dom.newValue(in, id, VKind::Double), fail); return;
        // 栈操作（纯槽位置换）
        case Op::POP: popSlots(1, fail); return;
        case Op::POP2: popSlots(2, fail); return;
        case Op::DUP: dupSlots(1, 0, fail); return;
        case Op::DUP_X1: dupSlots(1, 1, fail); return;
        case Op::DUP_X2: dupSlots(1, 2, fail); return;
        case Op::DUP2: dupSlots(2, 0, fail); return;
        case Op::DUP2_X1: dupSlots(2, 1, fail); return;
        case Op::DUP2_X2: dupSlots(2, 2, fail); return;
        case Op::SWAP: {
            if (st.stack.size() < 2) {
                fail = FrameSummary::Fail::Underflow;
                return;
            }
            std::swap(st.stack[st.stack.size() - 1], st.stack[st.stack.size() - 2]);
            return;
        }
        // 算术/位运算/移位/比较（结果形状与操作数宽度是引擎知识）
        case Op::IADD: case Op::ISUB: case Op::IMUL: case Op::IDIV: case Op::IREM:
        case Op::IAND: case Op::IOR: case Op::IXOR:
            binaryOp(1, 1, VKind::Int);
            return;
        case Op::ISHL: case Op::ISHR: case Op::IUSHR:
            binaryOp(1, 1, VKind::Int);
            return;
        case Op::INEG: unaryOp(1, VKind::Int); return;
        case Op::LADD: case Op::LSUB: case Op::LMUL: case Op::LDIV: case Op::LREM:
        case Op::LAND: case Op::LOR: case Op::LXOR:
            binaryOp(2, 2, VKind::Long);
            return;
        case Op::LSHL: case Op::LSHR: case Op::LUSHR:
            binaryOp(2, 1, VKind::Long);   // 移位量是 int
            return;
        case Op::LNEG: unaryOp(2, VKind::Long); return;
        case Op::DADD: case Op::DSUB: case Op::DMUL: case Op::DDIV: case Op::DREM:
            binaryOp(2, 2, VKind::Double);
            return;
        case Op::DNEG: unaryOp(2, VKind::Double); return;
        case Op::FADD: case Op::FSUB: case Op::FMUL: case Op::FDIV: case Op::FREM:
            binaryOp(1, 1, VKind::Float);
            return;
        case Op::FNEG: unaryOp(1, VKind::Float); return;
        case Op::IINC: {
            storeLocal(static_cast<ca::u32>(in.a), dom.newValue(in, id, VKind::Int), fail);
            return;
        }
        case Op::LCMP: binaryOp(2, 2, VKind::Int); return;
        case Op::DCMPL: case Op::DCMPG: binaryOp(2, 2, VKind::Int); return;
        case Op::FCMPL: case Op::FCMPG: binaryOp(1, 1, VKind::Int); return;
        // 转换
        case Op::I2L: unaryOp(1, VKind::Long); return;
        case Op::I2F: unaryOp(1, VKind::Float); return;
        case Op::I2D: unaryOp(1, VKind::Double); return;
        case Op::L2I: unaryOp(2, VKind::Int); return;
        case Op::L2F: unaryOp(2, VKind::Float); return;
        case Op::L2D: unaryOp(2, VKind::Double); return;
        case Op::F2I: unaryOp(1, VKind::Int); return;
        case Op::F2L: unaryOp(1, VKind::Long); return;
        case Op::F2D: unaryOp(1, VKind::Double); return;
        case Op::D2I: unaryOp(2, VKind::Int); return;
        case Op::D2L: unaryOp(2, VKind::Long); return;
        case Op::D2F: unaryOp(2, VKind::Float); return;
        case Op::I2B: case Op::I2C: case Op::I2S: unaryOp(1, VKind::Int); return;
        // 数组载荷/监视器/返回/抛出（shape 是 None，效应登记在此）
        case Op::IALOAD: case Op::BALOAD: case Op::CALOAD: case Op::SALOAD:
            binaryOp(1, 1, VKind::Int);
            return;
        case Op::LALOAD: binaryOp(1, 1, VKind::Long); return;
        case Op::FALOAD: binaryOp(1, 1, VKind::Float); return;
        case Op::DALOAD: binaryOp(1, 1, VKind::Double); return;
        case Op::AALOAD: binaryOp(1, 1, VKind::Ref); return;
        case Op::IASTORE: case Op::FASTORE: case Op::AASTORE:
        case Op::BASTORE: case Op::CASTORE: case Op::SASTORE:
            popSlots(3, fail);
            return;   // value + index + ref
        case Op::LASTORE: case Op::DASTORE:
            popSlots(4, fail);
            return;   // cat2 value + index + ref
        case Op::ARRAYLENGTH: popSlots(1, fail); pushVal(dom.newValue(in, id, VKind::Int), fail); return;
        case Op::ATHROW: popSlots(1, fail); return;
        case Op::MONITORENTER:
        case Op::MONITOREXIT: popSlots(1, fail); return;
        case Op::IRETURN: case Op::FRETURN: case Op::ARETURN: popSlots(1, fail); return;
        case Op::LRETURN: case Op::DRETURN: popSlots(2, fail); return;
        default: break;
        }
        break;

    case Shape::Imm1:
    case Shape::Imm2: pushVal(dom.newValue(in, id, VKind::Int), fail); return;

    case Shape::Local1:
        applyLocal(in, id, fail);
        return;

    case Shape::Iinc1:
        storeLocal(static_cast<ca::u32>(in.a), dom.newValue(in, id, VKind::Int), fail);
        return;

    case Shape::Atype1:   // newarray
        popSlots(1, fail);
        if (fail == FrameSummary::Fail::None) {
            pushVal(dom.newValue(in, id, VKind::Ref), fail);
        }
        return;

    case Shape::CP2Dims:   // multianewarray
        popSlots(static_cast<ca::u32>(in.a), fail);
        if (fail == FrameSummary::Fail::None) {
            pushVal(dom.newValue(in, id, VKind::Ref), fail);
        }
        return;

    case Shape::Branch2:
    case Shape::Branch4: {
        if (op == Op::IFEQ || op == Op::IFNE || op == Op::IFLT || op == Op::IFGE ||
            op == Op::IFGT || op == Op::IFLE || op == Op::IFNULL || op == Op::IFNONNULL) {
            popSlots(1, fail);
        } else if (op == Op::GOTO || op == Op::GOTO_W || op == Op::JSR || op == Op::JSR_W) {
            if (op == Op::JSR || op == Op::JSR_W) {
                // jsr 压返回地址（JVMS §4.10.2.5/§6.5 jsr——地址 = jsr 的
                // 顺序后继）。Strict 域携带调用方 jsr pc；形状域退 Int（legacy）。
                pushVal(dom.newReturnAddress(in, id), fail);
            }
        } else {
            popSlots(2, fail);   // icmp/acmp 家族
        }
        return;
    }

    case Shape::TableSwitch:
    case Shape::LookupSwitch:
        popSlots(1, fail);
        return;

    // 常量装载（LDC 族）与 invoke/字段访问
    case Shape::CP1:
    case Shape::CP2Count:
    case Shape::CP2Zero2:
    case Shape::CP2: {
        if (op == Op::LDC || op == Op::LDC_W || op == Op::LDC2_W) {
            if (!cp.valid(in.cp)) {
                fail = FrameSummary::Fail::BadCpRef;
                return;
            }
            const core::CpTag t = cp.tagOf(in.cp);
            if (t == core::CpTag::Long) {
                pushVal(dom.newValue(in, id, VKind::Long), fail);
            } else if (t == core::CpTag::Double) {
                pushVal(dom.newValue(in, id, VKind::Double), fail);
            } else if (t == core::CpTag::Integer) {
                pushVal(dom.newValue(in, id, VKind::Int), fail);
            } else if (t == core::CpTag::Float) {
                pushVal(dom.newValue(in, id, VKind::Float), fail);
            } else if (t == core::CpTag::Dynamic) {
                ++out.degraded;   // 常量类别需 BSM 求值
                pushVal(dom.newValue(in, id, VKind::Int), fail);
            } else {
                pushVal(dom.newValue(in, id, VKind::Ref), fail);
            }
            return;
        }
        switch (op) {
        case Op::GETSTATIC: {
            bool ok = false;
            const std::string_view d = memberDesc(cp, in.cp, ok);
            const ca::i32 n = ok ? fieldSlots(d) : -1;
            if (n < 0) {
                fail = FrameSummary::Fail::BadCpRef;
                return;
            }
            if (n == 2) {
                pushVal(dom.newValue(in, id, VKind::Long), fail);
            } else if (n == 1) {
                pushVal(dom.newValue(in, id, VKind::Ref), fail);
            }
            return;
        }
        case Op::PUTSTATIC: {
            bool ok = false;
            const std::string_view d = memberDesc(cp, in.cp, ok);
            const ca::i32 n = ok ? fieldSlots(d) : -1;
            if (n < 0) {
                fail = FrameSummary::Fail::BadCpRef;
                return;
            }
            popSlots(static_cast<ca::u32>(n), fail);
            return;
        }
        case Op::GETFIELD: {
            bool ok = false;
            const std::string_view d = memberDesc(cp, in.cp, ok);
            const ca::i32 n = ok ? fieldSlots(d) : -1;
            if (n < 0) {
                fail = FrameSummary::Fail::BadCpRef;
                return;
            }
            popSlots(1, fail);   // 对象引用
            if (fail != FrameSummary::Fail::None) {
                return;
            }
            if (n == 2) {
                pushVal(dom.newValue(in, id, VKind::Long), fail);
            } else if (n == 1) {
                pushVal(dom.newValue(in, id, VKind::Ref), fail);
            }
            return;
        }
        case Op::PUTFIELD: {
            bool ok = false;
            const std::string_view d = memberDesc(cp, in.cp, ok);
            const ca::i32 n = ok ? fieldSlots(d) : -1;
            if (n < 0) {
                fail = FrameSummary::Fail::BadCpRef;
                return;
            }
            popSlots(static_cast<ca::u32>(n) + 1, fail);
            return;
        }
        case Op::INVOKEVIRTUAL:
        case Op::INVOKESPECIAL:
        case Op::INVOKESTATIC:
        case Op::INVOKEINTERFACE:
        case Op::INVOKEDYNAMIC: {
            bool ok = false;
            const std::string_view d = memberDesc(cp, in.cp, ok);
            const ca::i32 args = ok ? methodArgSlots(d) : -1;
            const ca::i32 ret  = ok ? methodRetSlots(d) : -1;
            if (args < 0 || ret < 0) {
                fail = FrameSummary::Fail::BadCpRef;
                return;
            }
            ca::u32 pops = static_cast<ca::u32>(args);
            if (op != Op::INVOKESTATIC && op != Op::INVOKEDYNAMIC) {
                ++pops;   // 接收者
            }
            // 接收者值先行捕获：invokespecial <init> 的 Uninitialized 翻转
            // 以它为判定载荷（§10.1——同值出现点全部翻转为已初始化形态）。
            V receiver{};
            const bool initCall = op == Op::INVOKESPECIAL && pops >= 1 &&
                                  st.stack.size() >= pops;
            if (initCall) {
                receiver = st.stack[st.stack.size() - pops].val;
            }
            popSlots(pops, fail);
            if (fail != FrameSummary::Fail::None) {
                return;
            }
            if (initCall) {
                bool nameOk = false;
                if (memberName(cp, in.cp, nameOk) == "<init>" && nameOk) {
                    for (SlotV<V>& row : st.stack) {
                        if (!row.bottom && dom.equal(row.val, receiver)) {
                            row.val = dom.initializedForm(receiver, in, id);
                        }
                    }
                    for (SlotV<V>& row : st.locals) {
                        if (!row.bottom && dom.equal(row.val, receiver)) {
                            row.val = dom.initializedForm(receiver, in, id);
                        }
                    }
                }
            }
            if (ret == 2) {
                pushVal(dom.newValue(in, id, VKind::Long), fail);
            } else if (ret == 1) {
                pushVal(dom.newValue(in, id, VKind::Ref), fail);
            }
            return;
        }
        case Op::NEW:
            pushVal(dom.newValue(in, id, VKind::Ref), fail);
            return;
        case Op::ANEWARRAY:
            popSlots(1, fail);
            if (fail == FrameSummary::Fail::None) {
                pushVal(dom.newValue(in, id, VKind::Ref), fail);
            }
            return;
        case Op::CHECKCAST:
            popSlots(1, fail);
            if (fail == FrameSummary::Fail::None) {
                pushVal(dom.newValue(in, id, VKind::Ref), fail);
            }
            return;
        case Op::INSTANCEOF:
            popSlots(1, fail);
            if (fail == FrameSummary::Fail::None) {
                pushVal(dom.newValue(in, id, VKind::Int), fail);
            }
            return;
        default:
            return;   // 其余 CP 族 op 的效应在 None 分支（ARRAYLENGTH 等）
        }
    }
    }
}

/// @brief 可抛异常指令判定（JVM 规范保守全集，异常出口边发射域）。
///
/// 覆盖：CP 装载与类操作（ldc 族/new 家族/checkcast/instanceof）、字段与
/// 调用、数组载荷/存储/长度、除法余（ArithmeticException）、监视器、
/// athrow。纯局部算术/栈操作/load/store/分支/返回等不可能抛异常的指令
/// 不在内——异常边发射域以 javac oracle 校准。
inline bool canThrow(core::Op op) noexcept
{
    using core::Op;
    switch (op) {
    case Op::LDC: case Op::LDC_W: case Op::LDC2_W:
    case Op::NEW: case Op::NEWARRAY: case Op::ANEWARRAY: case Op::MULTIANEWARRAY:
    case Op::CHECKCAST: case Op::INSTANCEOF:
    case Op::GETSTATIC: case Op::PUTSTATIC: case Op::GETFIELD: case Op::PUTFIELD:
    case Op::INVOKEVIRTUAL: case Op::INVOKESPECIAL: case Op::INVOKESTATIC:
    case Op::INVOKEINTERFACE: case Op::INVOKEDYNAMIC:
    case Op::IALOAD: case Op::LALOAD: case Op::FALOAD: case Op::DALOAD:
    case Op::AALOAD: case Op::BALOAD: case Op::CALOAD: case Op::SALOAD:
    case Op::IASTORE: case Op::LASTORE: case Op::FASTORE: case Op::DASTORE:
    case Op::AASTORE: case Op::BASTORE: case Op::CASTORE: case Op::SASTORE:
    case Op::ARRAYLENGTH:
    case Op::IDIV: case Op::IREM: case Op::LDIV: case Op::LREM:
    case Op::MONITORENTER: case Op::MONITOREXIT: case Op::ATHROW:
        return true;
    default:
        return false;
    }
}

}   // namespace detail

/// @brief 值域搭车引擎：控制流/槽迁移/合并/异常播种/fixpoint 熔断全部
///        引擎负责，值由 IValueDomain<V> 供给。
template <typename V>
class DomainEngine
{
public:
    DomainEngine(const core::ConstantPool& cp, IValueDomain<V>& dom) noexcept
        : cp_(cp)
        , dom_(dom)
    {}

    /// @brief 域分析（入参不播种——与 FrameEngine 等价形态，自预言用）。
    DomainResult<V> analyze(const ir::CodeUnit& u)
    {
        return analyze(u, nullptr);
    }

    /// @brief 域分析（按签名播种入参 local 槽：常量传播等需要参数初值）。
    DomainResult<V> analyze(const ir::CodeUnit& u, const MethodSig& sig)
    {
        return analyze(u, &sig);
    }

private:
    DomainResult<V> analyze(const ir::CodeUnit& u, const MethodSig* sig);

    const core::ConstantPool& cp_;
    IValueDomain<V>& dom_;
};

template <typename V>
DomainResult<V> DomainEngine<V>::analyze(const ir::CodeUnit& u, const MethodSig* sig)
{
    DomainResult<V> result;
    FrameSummary& out   = result.summary;
    out.insns = u.dom.size();
    dom_.clearFail();   // Strict 域的失败通道按轮复位

    const auto& order = u.dom.order();
    if (order.empty()) {
        out.ok = true;
        return result;
    }

    std::unordered_map<ca::u32, detail::DomState<V>> inState;
    std::deque<ir::InsnId> work;

    detail::DomState<V> entry;
    if (sig != nullptr) {
        // 入参播种：实例方法 local0 = this（构造器播 UninitThis，§10.1）；
        // 随后按描述符逐参入槽（cat2 双槽，引用参数携带精确类名）。
        ca::u32 slot = 0;
        if (!sig->isStatic) {
            entry.locals.push_back(
                SlotV<V>{dom_.newThis(sig->name == "<init>", sig->thisClass), false});
            slot = 1;
        }
        std::string_view d = sig->desc;
        if (!d.empty() && d[0] == '(') {
            ca::usize pos = 1;
            while (pos < d.size() && d[pos] != ')') {
                const std::string_view t = detail::oneType(d, pos);
                if (t.empty()) {
                    break;
                }
                const VKind k = detail::kindOfType(t);
                const V v     = dom_.newParameterTyped(slot, k, t);
                entry.locals.push_back(SlotV<V>{v, false});
                if (dom_.width(v) == 2) {
                    entry.locals.push_back(SlotV<V>{V{}, true});
                }
                ++slot;
                pos += t.size();
            }
        }
    }
    inState[order[0].slot] = std::move(entry);
    work.push_back(order[0]);

    // 异常处理器两档。精确档：handler 由 try 范围内可抛指令的出
    // 口边惰性并入，入口顺序伪边照旧截断（handler 可达 ⟺ 至少一条异常边
    // 实际并入）。基础档：不发异常边，入口种子以「待并参与者」形态持有——
    // 真实边到达时种子与边同权并入，真实边缺席的处理器由排空后种子倾泻兜
    // 底；顺序伪边不再截断，否则 fake-try（handler 即 try 之后的自然续体）
    // 的唯一真实入边被丢弃，种子单跑必然残帧下溢。
    const bool preciseExc = dom_.preciseExceptionEdges();
    std::unordered_set<ca::u32> handlerSlots;   ///< 精确档：入口顺序伪边截断集合
    handlerSlots.reserve(u.exc.size() * 2);

    // 精确档异常边表（异常表序即并入序）：[startPc, endPc) 覆盖范围 +
    // handler 入口 + catch_type 对应的栈值（catch_type 0 = any 播
    // Throwable）。路径过滤不引入第二个 resolver 面：类型匹配判定按
    // 「未注入层级即保守放行」单向规则，本层不可判定 → 一律并入。
    struct ExcEdge
    {
        ca::u32 startPc = 0;
        ca::u32 endPc = 0;   ///< 开区间上界；End 边界 = 方法末尾
        ir::InsnId handler;
        V value{};
    };
    std::vector<ExcEdge> excEdges;
    if (preciseExc) {
        excEdges.reserve(u.exc.size());
        for (const ir::CodeUnit::Exc& e : u.exc) {
            const auto target = u.dom.resolveLabel(e.handler);
            if (target.is_err() || target.unwrap().isEnd()) {
                continue;
            }
            const ir::InsnId handler = target.unwrap().insn;
            if (u.dom.peek(handler) == nullptr) {
                continue;
            }
            handlerSlots.insert(handler.slot);
            const auto start = u.dom.resolveLabel(e.start);
            if (start.is_err() || start.unwrap().isEnd()) {
                continue;   // promote 契约保证 start 非 End；防御式跳过
            }
            const ir::Insn* startInsn = u.dom.peek(start.unwrap().insn);
            if (startInsn == nullptr) {
                continue;
            }
            ExcEdge edge;
            edge.startPc = startInsn->srcPc;
            const auto end = u.dom.resolveLabel(e.end);
            const ir::Insn* endInsn =
                end.is_ok() && !end.unwrap().isEnd() ? u.dom.peek(end.unwrap().insn) : nullptr;
            edge.endPc = endInsn != nullptr ? endInsn->srcPc : 0xFFFFFFFFu;
            edge.handler = handler;
            edge.value = dom_.newException(e);
            excEdges.push_back(std::move(edge));
        }
    }

    // 基础档入口种子表（slot 有序 → 倾泻次序确定）：同一 handler 的多个
    // catch 项种子先并。不入队、不占 inState——等真实边并入，或排空后倾
    // 泻。精确档无种子：handler 可达性由异常边实际并入决定。
    std::map<ca::u32, detail::DomState<V>> handlerSeed;
    if (!preciseExc) {
        for (const ir::CodeUnit::Exc& e : u.exc) {
            const auto target = u.dom.resolveLabel(e.handler);
            if (target.is_err() || target.unwrap().isEnd()) {
                continue;
            }
            const ir::InsnId handler = target.unwrap().insn;
            if (u.dom.peek(handler) == nullptr) {
                continue;
            }
            detail::DomState<V> hs;
            hs.stack.push_back(SlotV<V>{dom_.newException(e), false});
            const auto [it, fresh] = handlerSeed.try_emplace(handler.slot, std::move(hs));
            if (!fresh) {
                // 多 catch 项共享入口：种子并种子（形态同为空 locals + 单槽
                // throwable，仅值过域合并）。
                it->second.stack[0].val =
                    dom_.merge(it->second.stack[0].val, hs.stack[0].val);
            }
        }
    }

    std::unordered_map<ca::u32, ca::usize> posOf;
    posOf.reserve(order.size() * 2);
    for (ca::usize i = 0; i < order.size(); ++i) {
        posOf.emplace(order[i].slot, i);
    }
    // 返回地址求解反查表（jsr 指令 srcPc → order 下标）；ret 据此把
    // 跳回边落到调用方 jsr 的顺序后继。srcPc 逐指令唯一（promote 契约）。
    std::unordered_map<ca::u32, ca::usize> pcToOrder;
    pcToOrder.reserve(order.size() * 2);
    for (ca::usize i = 0; i < order.size(); ++i) {
        const ir::Insn* in = u.dom.peek(order[i]);
        if (in != nullptr) {
            pcToOrder.emplace(in->srcPc, i);
        }
    }

    const auto sameRow = [&](const SlotV<V>& a, const SlotV<V>& b) {
        return a.bottom == b.bottom && (a.bottom || dom_.equal(a.val, b.val));
    };
    const auto sameState = [&](const detail::DomState<V>& a, const detail::DomState<V>& b) {
        if (a.locals.size() != b.locals.size() || a.stack.size() != b.stack.size()) {
            return false;
        }
        for (ca::usize i = 0; i < a.locals.size(); ++i) {
            if (!sameRow(a.locals[i], b.locals[i])) {
                return false;
            }
        }
        for (ca::usize i = 0; i < a.stack.size(); ++i) {
            if (!sameRow(a.stack[i], b.stack[i])) {
                return false;
            }
        }
        return true;
    };

    // 行级并入：短侧补 top 后逐行过域合并（状态副本进，融合结果出）。
    // Strict 域合并失败时置 fail 并定位到合并点指令，返回 false = 整个
    // analyze 终止（不伪造帧，§10.1）。
    const auto mergeInto = [&](detail::DomState<V>& merged, const detail::DomState<V>& incoming,
                               const ir::InsnId& at) -> bool {
        const ca::usize nL = merged.locals.size() > incoming.locals.size()
                                 ? merged.locals.size()
                                 : incoming.locals.size();
        const ca::usize nS = merged.stack.size() > incoming.stack.size()
                                 ? merged.stack.size()
                                 : incoming.stack.size();
        if (merged.locals.size() < nL) {
            merged.locals.resize(nL, SlotV<V>{dom_.top(), false});
        }
        if (merged.stack.size() < nS) {
            merged.stack.resize(nS, SlotV<V>{dom_.top(), false});
        }
        for (ca::usize i = 0; i < nL; ++i) {
            // 行在入边缺席 = 入边该槽为 top：显式过域合并（verifier 格
            // 下支配为 top；形状格吸收下值恒等 = 原值，bottom 标记保持）。
            // 原短路实现把吸收语义硬编在协议里，两格分离后不再成立。
            const V src = i < incoming.locals.size()
                              ? incoming.locals[i].val
                              : dom_.top();
            merged.locals[i].val = dom_.merge(merged.locals[i].val, src);
            if (i < incoming.locals.size()) {
                merged.locals[i].bottom =
                    merged.locals[i].bottom && incoming.locals[i].bottom;
            }
        }
        for (ca::usize i = 0; i < nS; ++i) {
            // 栈行与 locals 同规：缺席行显式与 top 合并（值恒等口径同上）。
            const V src = i < incoming.stack.size()
                              ? incoming.stack[i].val
                              : dom_.top();
            merged.stack[i].val = dom_.merge(merged.stack[i].val, src);
            if (i < incoming.stack.size()) {
                merged.stack[i].bottom =
                    merged.stack[i].bottom && incoming.stack[i].bottom;
            }
        }
        const FrameSummary::Fail mergeFail = dom_.failCode();
        if (mergeFail != FrameSummary::Fail::None) {
            out.fail   = mergeFail;
            out.failAt = at;
            out.ok     = false;
            result.frames.clear();
            return false;
        }
        return true;
    };

    const ca::usize visitCap = order.size() * 64 + 4096;   // 熔断
    ca::usize visits = 0;
    while (!work.empty() || !handlerSeed.empty()) {
        if (++visits > visitCap) {
            out.fail = FrameSummary::Fail::NoFixpoint;
            out.ok   = false;
            return result;
        }
        if (work.empty()) {
            // 种子倾泻：无真实边到达的处理器（异常-only），入口 = 种子本体
            // ——与「种子即唯一入边」等值，只是时机移到正常流收敛之后，种
            // 子不再抢先单跑。inState 已有状态（处理器即方法入口等罕见形
            // 态）则种子并入既有帧后重跑。
            const auto seedIt = handlerSeed.begin();
            const auto posIt  = posOf.find(seedIt->first);
            if (posIt == posOf.end()) {
                handlerSeed.erase(seedIt);
                continue;
            }
            const ir::InsnId handler = order[posIt->second];
            const auto existing = inState.find(seedIt->first);
            if (existing == inState.end()) {
                inState.emplace(seedIt->first, std::move(seedIt->second));
                handlerSeed.erase(seedIt);
                work.push_back(handler);
            } else {
                detail::DomState<V> seed = std::move(seedIt->second);
                handlerSeed.erase(seedIt);
                ++out.merges;
                detail::DomState<V> merged = existing->second;
                if (!mergeInto(merged, seed, handler)) {
                    return result;
                }
                if (!sameState(merged, existing->second)) {
                    existing->second = std::move(merged);
                    work.push_back(handler);
                }
            }
            continue;
        }
        const ir::InsnId id = work.front();
        work.pop_front();
        const ir::Insn* inp = u.dom.peek(id);
        if (inp == nullptr) {
            continue;
        }
        detail::Machine<V> m{cp_, dom_, out, inState[id.slot]};
        FrameSummary::Fail fail = FrameSummary::Fail::None;
        m.apply(*inp, id, fail);
        if (fail != FrameSummary::Fail::None) {
            out.fail  = fail;
            out.failAt = id;
            out.ok   = false;
            result.frames.clear();
            return result;
        }
        // Strict 域的显式拒绝（transfer 中置位，如 jsr/ret）：定位到本指令。
        const FrameSummary::Fail transferFail = dom_.failCode();
        if (transferFail != FrameSummary::Fail::None) {
            out.fail   = transferFail;
            out.failAt = id;
            out.ok     = false;
            result.frames.clear();
            return result;
        }

        // 后继：跳转/switch 目标 + 顺序后继（控制流结束指令无顺序后继）。
        std::vector<ir::InsnId> succ;
        const auto appendTarget = [&](ir::LabelId label) {
            const auto target = u.dom.resolveLabel(label);
            if (target.is_ok() && !target.unwrap().isEnd() &&
                u.dom.peek(target.unwrap().insn) != nullptr) {
                succ.push_back(target.unwrap().insn);
            }
        };
        bool fallthrough = true;
        const core::Shape sh = core::shapeOf(inp->op);
        if (sh == core::Shape::Branch2 || sh == core::Shape::Branch4) {
            appendTarget(inp->target);
            if (inp->op == core::Op::GOTO || inp->op == core::Op::GOTO_W) {
                fallthrough = false;
            }
            if ((inp->op == core::Op::JSR || inp->op == core::Op::JSR_W) &&
                dom_.modelsSubroutines()) {
                // C2（仅建模子程序的域）：jsr 的顺序后继是返回点，只能经 ret
                // 到达——不发顺序边（JVMS §4.10.2.5：jsr 只把控制流交给子
                // 程序入口）。BestEffort 不建模 ret，保留 legacy 双后继传播
                //（返回点经 fallthrough 保持可达——bit-identical 契约）。
                fallthrough = false;
            }
        } else if (sh == core::Shape::TableSwitch || sh == core::Shape::LookupSwitch) {
            appendTarget(inp->target);
            for (const ir::LabelId& t : inp->targets) {
                appendTarget(t);
            }
            fallthrough = false;
        } else {
            const core::Op op = inp->op;
            if (op == core::Op::RET && dom_.modelsSubroutines()) {
                // 子程序返回（JVMS §6.5 ret/§4.10.2.5）：后继 = 返回地址槽
                // 指向的调用方 jsr 的顺序后继。RET 自身无 transfer 效应，后
                // 状态即子程序出口态——子程序未改写的局部槽保留调用方值
                //（单调用点时精确；JVMS §4.10.2.5 合并规则的单调用点退化形态）。
                // 槽空/cat2 垫底/非返回地址（含 Top）= 畸形；返回点越界
                //（jsr 是方法末条指令，返回地址无落点）= 结构畸形：两者显式
                // 拒绝定位到 ret。
                const ca::u32 slot = static_cast<ca::u32>(inp->a);
                ca::u32 jsrPc = 0;
                const bool resolved =
                    slot < m.st.locals.size() && !m.st.locals[slot].bottom &&
                    dom_.retSiteOf(m.st.locals[slot].val, jsrPc);
                const auto site = resolved ? pcToOrder.find(jsrPc) : pcToOrder.end();
                if (resolved && site->second + 1 < order.size()) {
                    succ.push_back(order[site->second + 1]);
                } else {
                    out.fail   = FrameSummary::Fail::Subroutine;
                    out.failAt = id;
                    out.ok     = false;
                    result.frames.clear();
                    return result;
                }
            }
            const bool ends = op == core::Op::RETURN || op == core::Op::IRETURN ||
                              op == core::Op::LRETURN || op == core::Op::FRETURN ||
                              op == core::Op::DRETURN || op == core::Op::ARETURN ||
                              op == core::Op::ATHROW || op == core::Op::RET;
            if (ends) {
                fallthrough = false;
            }
        }
        if (fallthrough) {
            const auto it = posOf.find(id.slot);
            if (it != posOf.end() && it->second + 1 < order.size()) {
                const ir::InsnId& next = order[it->second + 1];
                // 顺序伪边截断仅精确档：基础档 handler 入口是普通汇合点
                // （种子随边并入），截断会丢弃 fake-try 连续体的唯一真实入边。
                if (!preciseExc || handlerSlots.count(next.slot) == 0) {
                    succ.push_back(next);
                }
            }
        }

        // 后继并入：状态缺席即播种入队；已存在则副本合并，未变不入队
        // （fixpoint 终止性）。基础档处理器入口的待并种子同权参与汇合。
        // Strict 合并失败返回 false = 整个 analyze 终止（不伪造帧）。
        const auto propagate = [&](const ir::InsnId& s, detail::DomState<V>& incoming) -> bool {
            const auto seedIt = handlerSeed.find(s.slot);
            const auto it     = inState.find(s.slot);
            if (it == inState.end()) {
                detail::DomState<V> entryState = incoming;
                if (seedIt != handlerSeed.end()) {
                    // 首条真实边到达处理器：入口帧 = 边 ⊔ 待并种子。
                    if (!mergeInto(entryState, seedIt->second, s)) {
                        return false;
                    }
                    handlerSeed.erase(seedIt);
                }
                inState.emplace(s.slot, std::move(entryState));
                work.push_back(s);
                return true;
            }
            ++out.merges;
            detail::DomState<V>& dst = it->second;
            if (dst.stack.size() != incoming.stack.size()) {
                ++out.degraded;   // 深度不一致：畸形或超出 best-effort
            }
            // 副本合并；未变不入队（fixpoint 终止性）。
            detail::DomState<V> merged = dst;
            if (!mergeInto(merged, incoming, s)) {
                return false;
            }
            if (seedIt != handlerSeed.end()) {
                // 既有状态先到、种子尚未并入：本轮补并（此后种子出清）。
                if (!mergeInto(merged, seedIt->second, s)) {
                    return false;
                }
                handlerSeed.erase(seedIt);
            }
            if (!sameState(merged, dst)) {
                dst = std::move(merged);
                work.push_back(s);
            }
            return true;
        };
        // 精确档异常出口边：try 范围内可达指令 → 每个覆盖它的
        // handler（异常表序）。入口状态 = 抛出点 in-state locals（transfer
        // 前形态——<init> 翻转属正常后继效应，异常路径不携带）+
        // [catch_type 对应类型]；栈整体替换为异常值，与 JVM verifier 语义
        // 一致。多抛出点经同一合并协议收敛（异类引用走 resolver）。先于
        // 控制流后继发射，取的是未经自环合并污染的 in-state。
        //
        // 发射域是「范围内全部可达指令」而非仅可抛指令——JVM 校验器对范围
        // 内每条有帧指令都做 handler 可指派性检查（§4.10.1.4 保守口径），
        // 只从可抛指令发边会让不可抛覆盖（if 前跳/纯局部段）的 handler 不
        // 可达，SMT 缺 handler 帧而 JVM 加载期 VerifyError。可达指令缺席
        // （死范围）时无边可发，handler 不可达——与 verifier 对死代码不出
        // 约束的口径一致。
        if (preciseExc && !excEdges.empty()) {
            const ca::u32 pc = inp->srcPc;
            detail::DomState<V> exBase;
            exBase.locals = inState[id.slot].locals;
            for (const ExcEdge& edge : excEdges) {
                if (pc < edge.startPc || pc >= edge.endPc) {
                    continue;
                }
                detail::DomState<V> ex = exBase;
                ex.stack.push_back(SlotV<V>{edge.value, false});
                if (!propagate(edge.handler, ex)) {
                    return result;
                }
            }
        }

        for (const ir::InsnId& s : succ) {
            if (!propagate(s, m.st)) {
                return result;
            }
        }
    }

    // 逐指令帧交付（与 order() 下标对齐）。
    result.frames.assign(order.size(), FrameV<V>{});
    for (ca::usize i = 0; i < order.size(); ++i) {
        const auto it = inState.find(order[i].slot);
        if (it == inState.end()) {
            continue;
        }
        result.frames[i].reachable = true;
        result.frames[i].locals   = it->second.locals;
        result.frames[i].stack    = it->second.stack;
    }
    out.ok = true;
    return result;
}

// ---------------------------------------------------------------------------
// 内置域
// ---------------------------------------------------------------------------

/// @brief 形状域：产出 VType（FrameEngine 的实现载体）。
///
/// merge 与 ShapeResolver::merge 同语义（frame.cpp 单一定义，此处转发）。
class ShapeDomain final : public IValueDomain<VType>
{
public:
    VType top() override { return VType{VKind::Top, 0}; }

    VType merge(const VType& a, const VType& b) override;

    VType newValue(const ir::Insn& in, ir::InsnId, VKind shape) override
    {
        // NEW/CHECKCAST 的 Ref 带具体类 CP（诊断载荷）；其余形状不带。
        if (shape == VKind::Ref && (in.op == core::Op::NEW || in.op == core::Op::CHECKCAST)) {
            return VType{VKind::Ref, in.cp};
        }
        return VType{shape, 0};
    }

    VType newParameter(ca::u32, VKind shape) override { return VType{shape, 0}; }

    VType newException(const ir::CodeUnit::Exc& e) override
    {
        return VType{VKind::Ref, e.catchCp};
    }

    /// 形状来自 opcode（与 store 过什么无关——形状域的语义本就如此）。
    VType loadLocal(const ir::Insn& load, ir::InsnId, const VType&, ca::u32) override
    {
        return VType{detail::loadKind(load.op), 0};
    }

    /// 存入形状按 storeKind 归一（aconst_null; astore 存 Ref 不存 Null）。
    VType storeLocal(const ir::Insn& store, ir::InsnId, const VType&, ca::u32) override
    {
        return VType{detail::storeKind(store.op), 0};
    }

    ca::u8 width(const VType& v) override { return v.slots(); }

    bool equal(const VType& a, const VType& b) override
    {
        return a.k == b.k && a.cls == b.cls;
    }
};

/// @brief 旧 ITypeResolver 注入的桥接域（FrameEngine(cp, resolver) 兼容形态）。
class ResolverDomain final : public IValueDomain<VType>
{
public:
    explicit ResolverDomain(ITypeResolver& r) noexcept
        : r_(r)
    {}

    VType top() override { return VType{VKind::Top, 0}; }
    /// best-effort 桥接：resolver 报 Err 时降级为 Bottom（冲突吸收）继续。
    VType merge(const VType& a, const VType& b) override
    {
        auto r = r_.merge(a, b);
        return r.is_ok() ? std::move(r).unwrap() : VType{VKind::Bottom, 0, {}};
    }
    VType newValue(const ir::Insn& in, ir::InsnId, VKind shape) override
    {
        if (shape == VKind::Ref && (in.op == core::Op::NEW || in.op == core::Op::CHECKCAST)) {
            return VType{VKind::Ref, in.cp};
        }
        return VType{shape, 0};
    }
    VType newParameter(ca::u32, VKind shape) override { return VType{shape, 0}; }
    VType newException(const ir::CodeUnit::Exc& e) override
    {
        return VType{VKind::Ref, e.catchCp};
    }
    VType loadLocal(const ir::Insn& load, ir::InsnId, const VType&, ca::u32) override
    {
        return VType{detail::loadKind(load.op), 0};
    }
    VType storeLocal(const ir::Insn& store, ir::InsnId, const VType&, ca::u32) override
    {
        return VType{detail::storeKind(store.op), 0};
    }
    ca::u8 width(const VType& v) override { return v.slots(); }
    bool equal(const VType& a, const VType& b) override
    {
        return a.k == b.k && a.cls == b.cls;
    }

private:
    ITypeResolver& r_;
};

/// @brief Strict 域：类型精确 + 显式拒绝。
///
/// 与形状域的分工：transfer 的类型全部由 opcode 与描述符字面注入（单类内
/// 自足）——new 压 Uninitialized{该 new 的 pc}、invokespecial \<init\> 翻转、
/// get/put/invoke/checkcast/ldc/aaload 携带精确内部名、aload/store 靠默认
/// 恒等穿透已存值；merge 的结构性规则（Top 吸收、Null 引用底、Uninitialized
/// 按 offset 判等）内置，跨类引用合并交注入 resolver，不可解置 TypeConflict
/// 失败位，由引擎定位到合并点指令。子程序数据流（JVMS §4.10.2.5 族）：
/// jsr 压 ReturnAddress{调用方 jsr pc}、ret 经局部槽跳回调用点顺序后继
///（引擎发边，出口态携子程序改写后的局部槽）；returnAddress 只与自身合并，
/// 异调用点互并（多调用点共享入口）显式拒绝（Subroutine）。
/// 异常边精确传播开启：handler 入口 = 抛出点 locals + [catch_type
/// 精确类（CP Class 名；0 = any 播 Throwable）]，handler 由范围内可达指令
/// 的出口边并入（§4.10.1.4 保守口径），不再无条件可达。
///
/// 合成数组名（newarray/anewarray 的 "[I"/"[L…;"）经内部驻留表交给借用
/// 视图；其余精确名全部直接借用 CP 视图。
class StrictDomain final : public IValueDomain<VType>
{
public:
    StrictDomain(const core::ConstantPool& cp, ITypeResolver& r) noexcept
        : cp_(cp)
        , r_(r)
    {}

    VType top() override { return VType{}; }

    VType merge(const VType& a, const VType& b) override
    {
        if (sameAs(a, b)) {
            return a;   // 同 offset Uninitialized / 同名 Ref：保持
        }
        if (a.k == VKind::Top || b.k == VKind::Top) {
            // Strict verifier 类型格：top = 不可用值，汇合支配（JVMS 4.10.1.4
            // 帧匹配口径——单边未赋值槽的合并不可指派到具体类型）。与
            // BestEffort 形状格的吸收语义（ShapeDomain）格分离。
            return VType{};
        }
        if (a.k == VKind::ReturnAddress || b.k == VKind::ReturnAddress) {
            // returnAddress 只与自身合并（JVMS §4.10.2.5 合并族；同值
            // 已被上方 sameAs 早退吸收）。异值互并 = 不同 jsr 调用点共享同一
            // 子程序入口——精确语义需逐调用点状态快照（JVMS §4.10.2.5 的
            // touched-bit-vector 机制），本域显式拒绝（fail loud，不伪造帧）。
            rejectSubroutine();
            return VType{VKind::Bottom, 0, {}};
        }
        if (a.k == VKind::Null && b.k == VKind::Ref) {
            return b;   // null 是万能引用底
        }
        if (b.k == VKind::Null && a.k == VKind::Ref) {
            return a;
        }
        if (a.k == VKind::Uninitialized || b.k == VKind::Uninitialized ||
            a.k == VKind::UninitThis || b.k == VKind::UninitThis) {
            reject();   // 异 offset Uninit / UninitThis 与其他并：verifier 冲突
            return VType{VKind::Bottom, 0, {}};
        }
        if (a.k == VKind::Ref && b.k == VKind::Ref) {
            auto r = r_.merge(a, b);
            if (r.is_err()) {
                reject();
                return VType{VKind::Bottom, 0, {}};
            }
            return std::move(r).unwrap();
        }
        reject();   // 尺寸类冲突 / Null × 原始类型
        return VType{VKind::Bottom, 0, {}};
    }

    VType newValue(const ir::Insn& in, ir::InsnId, VKind shape) override
    {
        using core::Op;
        switch (in.op) {
        case Op::NEW: return VType::uninitNew(in.srcPc);
        case Op::CHECKCAST: return nameRef(detail::classNameOf(cp_, in.cp));
        case Op::GETFIELD:
        case Op::GETSTATIC: return descValue(fieldDescOf(in), in.cp);
        case Op::INVOKEVIRTUAL:
        case Op::INVOKESPECIAL:
        case Op::INVOKESTATIC:
        case Op::INVOKEINTERFACE:
        case Op::INVOKEDYNAMIC: {
            bool ok = false;
            const std::string_view d = detail::memberDesc(cp_, in.cp, ok);
            if (!ok) {
                return VType{shape, 0, {}};
            }
            const ca::usize close = d.find(')');
            if (close == std::string_view::npos) {
                return VType{shape, 0, {}};
            }
            return descValue(detail::oneType(d, close + 1), in.cp);
        }
        case Op::ANEWARRAY: return arrayRef(detail::classNameOf(cp_, in.cp));
        case Op::NEWARRAY: return primArrayName(in.a);
        case Op::MULTIANEWARRAY: return nameRef(detail::classNameOf(cp_, in.cp));
        case Op::LDC:
        case Op::LDC_W: {
            if (!cp_.valid(in.cp)) {
                return VType{VKind::Ref, 0, {}};
            }
            switch (cp_.tagOf(in.cp)) {
            case core::CpTag::String: return nameRef("java/lang/String");
            case core::CpTag::Class: return nameRef(detail::classNameOf(cp_, in.cp));
            case core::CpTag::MethodType: return nameRef("java/lang/invoke/MethodType");
            case core::CpTag::MethodHandle: return nameRef("java/lang/invoke/MethodHandle");
            case core::CpTag::Dynamic: {
                // condy 的类型即其 NameAndType 描述符，可精确注入
                const auto& e = cp_.at(in.cp);
                bool ok = false;
                const std::string_view d = natDesc(e.b, ok);
                return ok ? descValue(detail::oneType(d, 0), in.cp)
                          : VType{VKind::Ref, 0, {}};
            }
            default: return VType{shape, 0, {}};
            }
        }
        default: return VType{shape, 0, {}};
        }
    }

    VType newParameter(ca::u32, VKind shape) override { return VType{shape, 0, {}}; }

    VType newParameterTyped(ca::u32, VKind, std::string_view typeText) override
    {
        return descValue(typeText, 0);
    }

    VType newThis(bool isCtor, std::string_view thisClass) override
    {
        thisClass_ = thisClass;
        return isCtor ? VType{VKind::UninitThis, 0, {}} : nameRef(thisClass);
    }

    VType newException(const ir::CodeUnit::Exc& e) override
    {
        return e.catchCp != 0 ? nameRef(detail::classNameOf(cp_, e.catchCp))
                              : nameRef("java/lang/Throwable");   // catch_type 0 = any
    }

    VType binary(core::Op op, const VType& a, const VType&, VKind shape,
                 const ir::Insn& at, ir::InsnId id) override
    {
        // aaload 的精确元素类型来自数组操作数描述符（"[L…;"/"[I"）。
        if (op == core::Op::AALOAD && a.k == VKind::Ref && a.name.size() >= 2 &&
            a.name.front() == '[') {
            const std::string_view comp = a.name.substr(1);
            return comp.front() == 'L' ? nameRef(detail::refNameOfType(comp))
                                       : descValue(comp, at.cp);
        }
        return newValue(at, id, shape);
    }

    /// §10.1 翻转：Uninitialized → 被调用 \<init\> 的 owner 类；
    /// UninitThis → 本类（构造器链）；其余不变。
    VType initializedForm(const VType& receiver, const ir::Insn& call, ir::InsnId) override
    {
        if (receiver.k == VKind::UninitThis) {
            return nameRef(thisClass_);
        }
        if (receiver.k == VKind::Uninitialized) {
            bool ok = false;
            return nameRef(detail::refOwnerOf(cp_, call.cp, ok));
        }
        return receiver;
    }

    void onRet(const ir::Insn&, ir::InsnId) override {}

    // ---- 子程序数据流（JVMS §4.10.2.5 族）----

    VType newReturnAddress(const ir::Insn& in, ir::InsnId) override
    {
        return VType::retAddr(in.srcPc);   // 载荷 = 调用方 jsr 的 pc
    }

    bool modelsSubroutines() const override { return true; }

    bool retSiteOf(const VType& v, ca::u32& jsrPc) const override
    {
        if (v.k != VKind::ReturnAddress) {
            return false;
        }
        jsrPc = v.jsrPc();
        return true;
    }

    /// Strict 域启用精确异常边传播（handler 由出口边惰性并入）。
    bool preciseExceptionEdges() const override { return true; }

    FrameSummary::Fail failCode() const override { return fail_; }
    void clearFail() override { fail_ = FrameSummary::Fail::None; }

    ca::u8 width(const VType& v) override { return v.slots(); }

    bool equal(const VType& a, const VType& b) override { return sameAs(a, b); }

private:
    static bool sameAs(const VType& a, const VType& b) noexcept
    {
        return a.k == b.k && a.cls == b.cls && a.name == b.name;
    }

    /// 字段/调用描述符 → 值（引用带精确名；原始类别带形状）。
    VType descValue(std::string_view t, ca::u32) const
    {
        const VKind k = detail::kindOfType(t);
        if (k == VKind::Ref) {
            return nameRef(detail::refNameOfType(t));
        }
        return VType{k, 0, {}};
    }

    std::string_view fieldDescOf(const ir::Insn& in) const
    {
        bool ok = false;
        return detail::memberDesc(cp_, in.cp, ok);
    }

    /// 内部名（可空）→ Ref；空名退化为无名引用。
    static VType nameRef(std::string_view n) noexcept { return VType::ref(n); }

    /// 数组元素名 → 数组描述符名（"[L…;"），经驻留表交付借用视图。
    VType arrayRef(std::string_view element)
    {
        if (element.empty()) {
            return VType{VKind::Ref, 0, {}};
        }
        std::string s("[");
        if (element.front() == '[') {
            s += element;
        } else {
            s += 'L';
            s += element;
            s += ';';
        }
        return VType::ref(intern(std::move(s)));
    }

    /// newarray 的 atype 码 → 原始数组名（"[Z" 等）。
    VType primArrayName(ca::i32 atype)
    {
        static constexpr char kLetters[12] = {'\0', '?', '?', '?', 'Z', 'C',
                                              'F',  'D', 'B', 'S', 'I', 'J'};
        if (atype < 4 || atype > 11) {
            return VType{VKind::Ref, 0, {}};
        }
        std::string s(1, '[');
        s += kLetters[atype];
        return VType::ref(intern(std::move(s)));
    }

    std::string_view intern(std::string s)
    {
        return *interned_.insert(std::move(s)).first;
    }

    void reject() { fail_ = FrameSummary::Fail::TypeConflict; }
    void rejectSubroutine() { fail_ = FrameSummary::Fail::Subroutine; }

    /// NaT 索引 → 描述符（condy 用）。
    std::string_view natDesc(ca::u32 natIdx, bool& ok) const
    {
        ok = false;
        if (!cp_.valid(natIdx)) {
            return {};
        }
        const auto& nat = cp_.at(natIdx);
        if (nat.tag != core::CpTag::NameAndType || !cp_.isUtf8(nat.b)) {
            return {};
        }
        ok = true;
        return cp_.utf8Of(nat.b);
    }

    static std::string_view primLetter(ca::i32 atype) { return {}; }   // 占位防误用

    const core::ConstantPool& cp_;
    ITypeResolver& r_;
    std::string_view thisClass_;                 ///< 最近一次 newThis 的 this 类
    FrameSummary::Fail fail_ = FrameSummary::Fail::None;
    std::unordered_set<std::string> interned_;   ///< 合成数组名的驻留表
};

}   // namespace jbc::analysis
