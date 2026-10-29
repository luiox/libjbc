/// @file        code_dom.hpp
/// @brief       可编辑的指令 DOM：InstructionStore + 有序序列 + owner/generation 句柄。
///
/// 这是 v0.2 入口闸门冻结的形状：
///   - 身份 = @ref InsnId {owner, slot, generation}——owner 是每个 CodeDom
///     构造时获得的进程级唯一代号；跨 CodeDom 的同号句柄被结构性拒绝。
///   - 存储 = slot 表（追加式，永不回收槽位）；删除只置死并递增 generation，
///     旧句柄立即失效，不存在 ABA。
///   - 顺序 = 独立的 InsnId 序列（vector），指令顺序是数据不是指针链副作用。
///   - 分支/switch 的操作数是 **LabelId 边界句柄，不是指令句柄或字节偏移**：
///     插入/删除后导出时统一重算，调用方永远不手改偏移。
///
/// use 语义：erase 遇到 incoming use 返回 StillUsed；
/// 先 replaceAllUses 归零引用再删。反向 use 索引当前按需扫描（方法体规模
/// 下足够），CodeEditor 的批量协议在此之上构建。
#pragma once

#include <deque>
#include <utility>
#include <vector>

#include "libca/core/result.hpp"

#include "jbc/core/bytes.hpp"
#include "jbc/core/error.hpp"
#include "jbc/core/insn.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/ir/error.hpp"

namespace jbc::ir {

// 只引入 Result 三件套而非 `using namespace`，避免把 ca::core 的其余名字
// 带进 jbc::ir（与 core 层 bytes.hpp 同款约定）。
using ca::core::Err;
using ca::core::Ok;
using ca::core::Result;

/// @brief 「无来源 pc」哨兵：编辑新建的指令没有源字节位置。
///
/// append/insert 强制写入；pcmap（lower 的重定向索引）据此区分原指令与
/// 合成指令——合成指令不进旧 pc 索引，也不参与属性重定向。
inline constexpr ca::u32 kNoPc = 0xFFFFFFFFu;

/// @brief 指令身份句柄：三个字段全部参与相等性。
struct InsnId
{
    ca::u32 owner = 0;
    ca::u32 slot = 0;
    ca::u32 gen = 0;

    bool operator==(const InsnId& o) const noexcept
    {
        return owner == o.owner && slot == o.slot && gen == o.gen;
    }
    bool operator!=(const InsnId& o) const noexcept { return !(*this == o); }
};

/// @brief 空句柄常量：无指令的显式命名（此前用 InsnId{} 隐式约定）。
inline constexpr InsnId kNoInsn{};

/// @brief 方法内边界标签句柄。
///
/// 标签是 CodeDom 拥有的整数句柄，不是指令节点，也不携带裸指针。分支、
/// switch、异常表和调试范围都通过同一 owner/generation 校验协议引用边界。
struct LabelId
{
    ca::u32 owner = 0;
    ca::u32 slot = 0;
    ca::u32 gen = 0;

    bool operator==(const LabelId& o) const noexcept
    {
        return owner == o.owner && slot == o.slot && gen == o.gen;
    }
    bool operator!=(const LabelId& o) const noexcept { return !(*this == o); }
};

/// @brief 空标签句柄，仅用于表示「没有标签」的查询结果。
inline constexpr LabelId kNoLabel{};

/// @brief 标签解析后的边界种类。
enum class LabelTargetKind : ca::u8 {
    Instruction = 0,  ///< 指向某条现存指令的前置边界
    End = 1,          ///< 方法末尾边界，不对应任何指令
};

/// @brief 标签的结构化解析结果。
///
/// `End` 与无效句柄是两个不同状态：无效标签由 Result 的 Err 表示，合法
/// 的方法末尾由 kind == End 表示。`insn` 在 End 状态下固定为 kNoInsn，
/// 方便过渡期代码读取，但不能再据此判断标签是否合法。
struct LabelTarget
{
    LabelTargetKind kind = LabelTargetKind::End;
    InsnId insn = kNoInsn;

    static constexpr LabelTarget before(InsnId id) noexcept
    {
        return LabelTarget{LabelTargetKind::Instruction, id};
    }
    static constexpr LabelTarget end() noexcept
    {
        return LabelTarget{LabelTargetKind::End, kNoInsn};
    }
    bool isEnd() const noexcept { return kind == LabelTargetKind::End; }
};

/// @brief DOM 中的指令：操作数字段语义同 core::RawInsn，但目标是标签句柄。
///
/// target 语义按形状：
///   - Branch2/Branch4：唯一跳转目标
///   - TableSwitch：default；targets[i] 对应 key = a + i（i 从 0 起）
///   - LookupSwitch：default；targets[i] 对应 keys[i]
/// 其余形状 target/targets 无意义（保持默认值）。
struct Insn
{
    core::Op op = core::Op::NOP;
    bool wide = false;
    ca::u32 srcPc = 0;   ///< 对应 backing 布局的 pc；成功 lower 后更新为新布局
    ca::i32 a = 0;
    ca::i32 b = 0;
    ca::u32 cp = 0;
    LabelId target;
    std::vector<LabelId> targets;
    std::vector<ca::i32> keys;   ///< 仅 LookupSwitch 使用
};

/// @brief 单个方法体的指令 DOM。
class CodeDom
{
public:
    CodeDom() noexcept;

    CodeDom(const CodeDom&)            = delete;
    CodeDom& operator=(const CodeDom&) = delete;
    CodeDom(CodeDom&&) noexcept;
    CodeDom& operator=(CodeDom&&) noexcept;

    /// @brief 本 DOM 的 owner 代号（句柄校验用）。
    ca::u32 owner() const noexcept { return owner_; }

    // ---- 构建 ----

    /// @brief 追加到序列末尾；操作数中的句柄必须属于本 DOM（校验）。
    Result<InsnId, EditError> append(Insn in);

    // ---- 编辑 ----

    Result<InsnId, EditError> insertBefore(InsnId at, Insn in);
    Result<InsnId, EditError> insertAfter(InsnId at, Insn in);

    /// @brief 在标签所在边界插入一条指令，并让标签继续指向新指令。
    ///
    /// 标签绑定到指令时，标签表示该指令的前置边界；绑定到
    /// `kNoInsn` 时表示方法末尾。这个接口把“在边界插入”与“在某条
    /// 指令前插入”区分开来，避免调用方依赖列表当前的偶然邻接关系。
    Result<InsnId, EditError> insertAtLabel(LabelId label, Insn in);

    /// @brief 改变两条指令的程序顺序，不改变任何句柄或控制流引用。
    ///
    /// `moveBefore`/`moveAfter` 只操作 `order_`；指令槽和标签身份保持不变。
    /// 把指令移到自身旁边是合法的无操作，跨 DOM 句柄返回 InvalidHandle。
    Result<void, EditError> moveBefore(InsnId id, InsnId at);
    Result<void, EditError> moveAfter(InsnId id, InsnId at);

    /// @brief 区间顺序重排：把 `[first, last)` 的连续指令段整体搬到 at 之前。
    ///
    /// `moveBefore`/`moveAfter` 的区间推广，同样只重排 `order_`，单趟提出
    /// 回插（O(n)），不触碰指令槽与标签锚定——锚在段内指令上的标签和指向
    /// 段内标签的分支字段随句柄自然保持。`at == kNoInsn` 表示搬到方法末尾、
    /// `last == kNoInsn` 表示段延伸到方法末尾（与 createLabel/bindLabel 的
    /// 末哨兵约定同源——half-open 端点语义下尾段只有经哨兵可命名）。
    /// first == last 是空段、last 位于 first 之前是倒置段、锚点落在段内
    /// （锚点自身会被搬移卷走，语义未定义）一律 Err(BadRange)；锚点即段首
    /// 或紧随段尾时段已在锚点位置，是合法的无操作。段策略（段首边界标签
    /// 去向）在 CodeEditor 层。
    Result<void, EditError> moveRange(InsnId first, InsnId last, InsnId at);

    /// @brief 删除一条指令；仍被 branch/switch 字段引用或被存活的
    ///        Boundary 标签锚定时返回 StillUsed。
    ///
    /// 标签保护按角色区分：异常表/调试范围语义的 Boundary 标签必须先
    /// 移动或销毁；字段改写后遗留的历史 ControlFlow 标签不再保护锚点
    /// （其句柄此后解析为 InvalidHandle，也不能被新指令引用）。
    ///
    /// 成本模型：use 检查按需全序扫描（O(n)），erase 自身也走一遍——
    /// 循环里做 N 次 erase 是 O(N×n)。批量场景用 @ref eraseBatch；散点
    /// 删除先一次 usersOf 规划（无引用者成批删，有引用者先 replaceAllUses），
    /// 不要逐条盲删。
    Result<void, EditError> erase(InsnId id);

    /// @brief 批量删除：一次校验、一趟扫序、单趟重建，替代 N 次
    ///        erase 的 O(N×n)。
    ///
    /// 全有或全无：ids 中任一句柄无效、任一存活指令的字段仍锚定批内成员
    /// 且无 retarget 安排、或任一批内成员被存活 Boundary 标签锚定时，
    /// 返回 Err 且什么都不删。重复句柄按并集处理（同槽只删一次）。
    ///
    /// 批内互指不算引用：同批消亡的指令之间的引用随批一并消失，语义与
    /// 成员消亡顺序无关——这是与逐条 erase 的唯一语义差（逐条版先删谁
    /// 谁先挡路）。
    ///
    /// retarget 是「删除前把存活指令的引用改锚」的安排：from 必须是批内
    /// 成员，to 必须是批外存活指令。被安排覆盖的字段改锚语义与逐条
    /// @ref replaceAllUses 一致（每字段一枚锚定 to 的新 ControlFlow 标签，
    /// 被替换的旧 ControlFlow 标签随后销毁）；未覆盖的存活引用按
    /// StillUsed 拒绝。
    ///
    /// 成本模型：校验 + 扫描 + 重建各一趟，总计 O(n + |ids| + labels)。
    Result<void, EditError> eraseBatch(
        const std::vector<InsnId>& ids,
        const std::vector<std::pair<InsnId, InsnId>>& retarget = {});

    /// @brief 把锚定 from 的 branch/switch 操作数字段重定向到 to。
    ///
    /// 每个被改写的字段获得一枚锚定 to 的新 ControlFlow 标签；锚定同一
    /// 指令的 Boundary 标签（异常表/调试范围语义）保持原位。导入产生的
    /// 旧 ControlFlow 标签在字段全部改写后销毁；from == to 直接成功。
    Result<void, EditError> replaceAllUses(InsnId from, InsnId to);

    // ---- 边界标签原型 ----

    /// @brief 创建一个绑定到指令前置边界的标签；默认绑定到方法末尾。
    ///
    /// 标签由本 DOM 持有，返回值只是一枚带 owner/generation 的整数句柄。
    /// 标签仍然存活时，绑定的指令不能被 erase，避免产生悬垂边界。
    Result<LabelId, EditError> createLabel(InsnId anchor = kNoInsn);

    /// @brief 把已有标签移动到指令前置边界或方法末尾。
    Result<void, EditError> bindLabel(LabelId label, InsnId anchor = kNoInsn);

    /// @brief 销毁一个不再需要的标签；旧句柄立即失效。
    Result<void, EditError> destroyLabel(LabelId label);

    /// @brief 解析标签当前绑定的边界；End 与 InvalidHandle 在类型上分开。
    Result<LabelTarget, EditError> resolveLabel(LabelId label) const;

    /// @brief 查询标签句柄是否属于当前 DOM 且仍然有效。
    bool validLabel(LabelId label) const noexcept;

    /// @brief 锚定到某条指令前置边界的全部存活标签（Boundary 与 ControlFlow
    ///        两角色都含，按槽序）。
    ///
    /// 读侧查询：变换 pass 用它实现旧栈 LabelNode 语义——「两条相邻指令之间
    /// 有没有标签」等价于「后一条指令的前置边界有没有锚定标签」；替换型变换
    /// 借此把边界标签迁到新头指令（bindLabel）。不修改任何状态。
    std::vector<LabelId> labelsAt(InsnId anchor) const;

    // ---- 受控延后绑定（仅整体方法体重放路径） ----

    /// @brief 进入「分支/switch 目标延后绑定」受控构建态；逐条追加期放开
    ///        控制目标须已锚定的校验，@ref endDeferredTargets 收尾全量补验。
    ///
    /// 整体重放（ClassSession::adoptMethod 的 replayMethodBody）按源顺序
    /// 逐条搬运：前向分支的目标指令尚未重放，其占位标签必然未锚定——
    /// append 期的控制目标防呆在该路径上机制性不可满足。
    /// 受控态只放开「分支/switch 目标须已锚定到本 DOM 存活指令」这一条
    /// （switch 形状校验保留），退出时对全量存活指令补同一套校验，仍悬垂
    /// 的目标显式失败。正常编辑路径不进入此态，checkHandles 对外语义不变；
    /// 重复进入幂等。
    void beginDeferredTargets() noexcept;

    /// @brief 退出受控延后绑定态：对全量存活指令补 checkHandles 校验
    ///        （含分支/switch 目标锚定），任何悬垂目标 Err(InvalidHandle)。
    ///
    /// 无论校验成败受控态都退出——失败面不留松弛态，调用方（重放）按
    /// 全有或全无放弃即可。未进入受控态时调用同样安全：全量校验照跑，
    /// 等于一次结构自检。
    Result<void, EditError> endDeferredTargets();

    // ---- 访问 ----

    /// @brief 只读访问；句柄无效返回 nullptr（不抛错，方便探测）。
    const Insn* peek(InsnId id) const noexcept;

    /// @brief 可变访问；句柄无效返回 Err(InvalidHandle)。
    ///
    /// @warning 修改 op/形状前调用方自行保证操作数字段匹配新形状；
    ///          修改跳转目标请走 replaceAllUses 或重写 target（须属本 DOM）。
    ///          返回的指针在该指令被 erase 前保持稳定；它不会因为追加或
    ///          插入其它指令而悬空。结构变更后仍应重新取得句柄视图。
    Result<Insn*, EditError> get(InsnId id);

    /// @brief 程序顺序（结构变更后需重新取得；迭代中变更序列是未定义行为）。
    const std::vector<InsnId>& order() const noexcept { return order_; }

    /// @brief 首条/末条指令句柄；空序列返回 kNoInsn。
    InsnId first() const noexcept { return order_.empty() ? kNoInsn : order_.front(); }
    InsnId last() const noexcept { return order_.empty() ? kNoInsn : order_.back(); }

    ca::usize size() const noexcept { return order_.size(); }

    /// @brief 指向 id 的引用者列表（分支/switch default/case）。
    std::vector<InsnId> usersOf(InsnId id) const;

    // ---- codec 桥 ----

    /// @brief 从 code 字节导入：解码并把字节偏移解析为句柄。
    ///
    /// 分支偏移必须落在某条指令的起点，否则 Err(BadTarget)——这是把
    /// 「跳进操作数中间」的畸形输入挡在 DOM 之外的唯一防线。
    static Result<CodeDom, EditError> importFrom(core::ByteView code);

    /// @brief 导出为 codec 层指令序列：顺序排布、重算 pc 与偏移。
    ///
    /// i16 分支放不下时 Err(BranchOverflow)——调用方决定显式拓宽
    /// （改写为 goto_w 形状）还是放弃。
    Result<std::vector<core::RawInsn>, EditError> exportRaw() const;

    /// @brief 一步导出为 code 字节（exportRaw + encodeInsns）。
    Result<std::vector<ca::u8>, EditError> encode() const;

    /// @brief 未编辑时导出必须与源字节一致（往返自检入口）。
    /// @param code 导入时的源字节
    bool roundtripEquals(core::ByteView code) const;

    /// @brief 成功 lower 后，将每条指令的来源 pc 更新为当前 backing 布局。
    ///
    /// 调用方必须传入本 DOM 当前顺序导出的 RawInsn；用于同一 session 多次
    /// 编辑与 lower 时，让下一轮 relocation 从最新已提交布局开始。
    void acceptLoweredLayout(const std::vector<core::RawInsn>& raw) noexcept;

private:
    enum class LabelRole : ca::u8 {
        Boundary = 0,
        ControlFlow = 1,
    };

    struct Slot
    {
        Insn insn;
        bool live = false;
        ca::u32 gen = 0;
    };

    struct LabelSlot
    {
        InsnId anchor = kNoInsn;
        bool live = false;
        ca::u32 gen = 0;
        LabelRole role = LabelRole::Boundary;
    };

    Result<InsnId, EditError> appendSlot(Insn&& in);
    Result<InsnId, EditError> insertAt(InsnId at, Insn in, bool before);
    Result<LabelId, EditError> createLabel(InsnId anchor, LabelRole role);
    const Slot* slotOf(InsnId id) const noexcept;
    Slot* slotOf(InsnId id) noexcept;
    const LabelSlot* labelSlotOf(LabelId id) const noexcept;
    LabelSlot* labelSlotOf(LabelId id) noexcept;
    Result<void, EditError> checkHandles(const Insn& in) const noexcept;

    ca::u32 owner_;
    /// @brief 受控延后绑定态（begin/endDeferredTargets 翻转）：真值时
    ///        checkHandles 跳过分支/switch 目标的锚定校验。仅整体重放路径
    ///        进入，退出时全量补验（见 beginDeferredTargets 契约）。
    bool deferredTargets_ = false;
    // deque 保证在两端追加时已有元素的引用/指针不因槽表扩容而失效。
    // 槽位只做逻辑删除，因而 get()/peek() 的非拥有视图可安全跨越后续插入。
    std::deque<Slot> slots_;
    std::vector<InsnId> order_;
    std::vector<LabelSlot> labels_;
};

/// @brief 进程级 owner 代号发号器（也供其它 Unit 复用）。
ca::u32 nextOwnerId() noexcept;

}   // namespace jbc::ir
