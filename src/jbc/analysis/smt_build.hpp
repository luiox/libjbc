/// @file        smt_build.hpp
/// @brief       StackMapTable 生成器。
///
/// 把 Strict 帧计算的结果（逐指令到达帧）编码为 core 层既有的
/// StackMapTableAttr 模型：目标集合收集（分支/switch 目标 + 异常 handler
/// 入口）、verification_type_info 编码（Object 经 CP intern 落位、
/// Uninitialized 载荷 = 产生方 new 指令 pc）、紧凑形态选择（JVMS 4.7.4
/// 同族 + chop/append/full，与 javac 同取最小形态）。
///
/// 边界（裁决）：显式 API——只有调用本函数才发生生成与 CP 落位；
/// 不自动接线 lower/write（把属性挂回 Code、maxes 自动采用归 J7）。
/// 陈述口径：按 Strict 域陈述运行值精确类型（裁决），不做 javac
/// 的声明投影——死局部不 chop、类型不陈述父类；与 javac 产物按
/// 「公共前缀逐槽一致」比对（口径）。
#pragma once

#include <memory>
#include <vector>

#include "libca/core/result.hpp"

#include "jbc/analysis/frame.hpp"
#include "jbc/core/attribute.hpp"
#include "jbc/ir/units.hpp"

namespace jbc::analysis {

using ca::core::Result;

/// @brief 生成失败语义（每个失败码都是显式定义的行为）。
enum class SmtBuildError : ca::u8 {
    ClassTooOld,     ///< classfile 版本 < 50：SMT 属性无定义（调用方决定跳过或报错）
    FramesNotOk,     ///< 输入帧不是与 CodeUnit 对齐的 ok=true Strict 计算结果
    BottomInFrame,   ///< 帧槽出现 Bottom / 无名引用（冲突残留或播种缺名——防御拒绝）
    TargetNotInsn,   ///< Uninitialized 载荷未命中方法内任何 new 指令 pc
    TooBig,          ///< 帧数 / delta 超 u2 编码域
    Subroutine,      ///< 帧槽含 ReturnAddress：SMT 无法编码子程序（裁决——
                     ///<   verification_type_info 是封闭 tag 集，无 returnAddress
                     ///<   形态（JVMS ）；v≤49 不带 SMT 走旧校验是规范
                     ///<   许可行为，不需要也不得生成 SMT）
};

const char* to_string(SmtBuildError e) noexcept;

/// @brief 生成产物：属性模型 + 是否需要产出的显式标志。
///
/// needed=false = 方法无控制流目标（空表也不写——调用方不得附加空属性）；
/// needed=true 时 attr 非空且已选好紧凑形态，frames 即编码帧数。
/// 属性名的 CP Utf8 不在本 API 落位（挂回 Code 属性归 J7 接线）。
struct SmtBuildResult
{
    bool needed = false;
    ca::u32 frameCount = 0;
    std::unique_ptr<core::StackMapTableAttr> attr;
};

/// @brief 由 Strict 帧结果生成 StackMapTable 属性模型（显式 API）。
///
/// - 输入：@p frames 必须来自 FrameEngine(Strict).compute(u, sig) 的逐指令
///   帧（与 order() 对齐、ok=true、播种经 MethodSig——初始帧名精确、构造器
///   this 槽 UninitThis）。初始帧（链的起点）取首指令到达帧的 locals，
///   与引擎播种同源，调用方无需重复描述符逻辑。
/// - 目标集合 = 方法体内分支/switch 全部目标 + 异常 handler 入口；不可达
///   目标不产帧（verifier 跳过不可达代码）。
/// - CP 落位：Object 条目的类名经 session.internClass 落位（CP 只追加 +
///   既有条目复用，与指令侧同一 intern 面）；Uninitialized 载荷用帧域
///   携带的 new 指令 pc（srcPc 口径——布局已变时由接线方保证帧与新布局
///   一致，归 J7）。
/// - 版本门：backing major < 50 → Err(ClassTooOld)（SMT 属性自版本 50
///   才有定义；是否跳过由调用方决定，本 API 不静默）。
Result<SmtBuildResult, SmtBuildError>
buildStackMapTable(ir::ClassSession& s, const ir::CodeUnit& u,
                   const std::vector<FrameAt>& frames);

/// @brief 测试观测点（表驱动单测入口；非公共 API）。
namespace smt_detail {

/// @brief 槽语义的局部变量/栈（cat2 = 值槽 + 垫底槽）→ verification_type
///        条目（cat2 单条目；垫底槽错位 = 帧不自洽，拒绝）。Object 经
///        session internClass 落位。
ca::core::Result<std::vector<core::StackMapTableAttr::VType>, SmtBuildError>
smtEntriesOf(ir::ClassSession& s, const std::vector<VType>& slots);

/// @brief 紧凑形态选择（JVMS 4.7.4，纯函数）：按链上前一帧 locals 条目与
///        目标全帧选最小编码形态——same/same1（含 extended）/chop/append/
///        full；chop/append 只在栈空且 locals 变化恰为末尾 1..3 条增删时
///        适用（与 javac 同策略：能紧凑则紧凑）。
core::StackMapTableAttr::Frame
chooseSmtFrame(ca::u32 delta,
               const std::vector<core::StackMapTableAttr::VType>& prevLocals,
               const std::vector<core::StackMapTableAttr::VType>& locals,
               const std::vector<core::StackMapTableAttr::VType>& stack);

}   // namespace smt_detail

}   // namespace jbc::analysis
