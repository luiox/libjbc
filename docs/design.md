# libjbytecode 统一拓扑设计

> **折回说明（2026-10-17）**：本文自 morpher 工作区 `libs/libjbc/doc_ai/design.md` 逐字节折回本仓，作为全量拓扑真源；此后版本演进以本仓为准，morpher 侧原件定格不再更新。读法注意：
>
> 1. §0 快照与正文里的版本/用例数是**历史定格**（正文叙述停在 v0.3.8 时代），当前进度与批次账目以仓内 `dev_plan.md` 为准，后文新增落地记录（§37）以追加方式生长，与全文既有纪律一致；
> 2. 仓内另有 `doc_ai/design.md`（早前的开源快照：仅属性面摘编）。两份文档的 § 编号**大多同义**（§27/§33/§36 逐字可对），唯独 **§32 不同义**——快照的 §32 是 typed 属性面摘编，本文的 §32 是泛型签名层；代码注释里的 §32 系引用（§32.4 Signature 手术入口、§32.5 typed 升格、§32.9 等）一律按本文读。

状态：**core（v0.1–v0.2）+ 高层 IR（v0.3）+ analysis（v0.3.8）已实现并通过测试**；mbase adapter（§16.1 第 7 步的 ClassIndex/ResourceContainer）与产品线迁移（第 8 步）未开工。当前版本 v0.3.8（2026-08-30），逐阶段落地记录见 §18/§21/§22/§23/§24/§25/§26。

## 0. 现状索引（2026-09-09 加；先读这节，再按需进后文）

本文以追加"落地记录"的方式生长（§18–§26），历史条款与现状散在全文。本节是唯一的现状快照，读到与后文冲突处以本节和各落地记录为准。

**已交付（有测试/实测背书）**：

| 层 | 内容 | 真源 |
|---|---|---|
| core（`jbc::core`） | 单类 lazy ClassFile DOM：骨架 eager + 属性四态（Untouched/Decoded/Modified/New，`mutate` 唯一置脏入口）+ CP 零拷贝只追加 + reader/writer 保真写回；指令 codec 字节精确往返 | §3–§5、§18 |
| ir（`jbc::ir`） | CodeDom（`InsnId{owner,slot,gen}` 句柄、跨 DOM 结构性拒绝、erase StillUsed 协议）+ ClassSession promote/lower（relocation contract：LNT pcmap 精确 / SMT 链式重算 / LVT 端点钳制 / 未建模即 Err）+ 语义操作数层（memberOf/classOf/constOf + intern 家族 + 保活孪生）+ CodeEditor（erase 双策略 + moveRange 区间搬移） | §6–§7、§22、§39 |
| analysis（`jbc::analysis`） | 逐指令到达帧 FrameAt；值域搭车 DomainEngine\<V\>（FrameEngine 已降级为壳）；帧自预言 best-effort，真实语料 oracle 100% | §25 |

**验证入口（全部可复现）**：

```sh
xmake f --plat=windows --with_tests=y && xmake build libjbc_unittest && xmake run libjbc_unittest
xmake build libjbc_scan_demo     # 新旧扫描对拍（计数逐类一致 + 计时）
xmake f --plat=windows -m release --with_asan=y && xmake build libjbc_leak_check
JBC_ASAN_SELFTEST=1 xmake run libjbc_leak_check   # 插桩自检防假绿
```

- 单测 105 用例（2026-09-27 实测：92 过、13 语料/环境门控 skip，含 visitor 清零门禁 §26）；其中真实语料 oracle（帧引擎/往返/CP 解析）依赖本机 ares 语料（gitignore 的 `/ares/`，或 `LIBJBC_REAL_CLASS_DIR` 指路），缺席时**skip 而非 fail**；仓库内置 `test_resource/libjbc/annotated`（3 类）供冒烟。
- ASan 只用于 `demo/` 轻量程序（MSVC 下 /fsanitize=address 改 STL ABI 且 gtest 走预编译不插桩，**不能**与 gtest 同链）；`libjbc_leak_check` 接语料目录参数，`JBC_ASAN_SELFTEST=1` 先自证插桩生效。真实类读→全解码→写回零报告 + new/delete 计数平衡（§20/§23）。
- 对拍基准 `libjbc_scan_demo`：同语料四配置（mbase-summary / legacy / jbc-native / jbc-scan）计数逐类一致；大语料计时见 §21（7000 类 3.5–5.6x；小语料绝对值噪声大，以计数一致与方向为准）。

**未开工 / 已知边界**：

- §16.1 第 7 步（summary 扫描已有 demo，但 ClassIndex/ResourceContainer/lazy resolver 属 mbase，见 §9）与第 8 步（产品线迁移）0 进度；旧栈照常是产品线唯一依赖，新库零消费者。
- 帧 Strict 域（§10.1–§10.3：三态策略、异常边精确传播）已落；子程序数据流（§10.6：jsr/ret 建模 + SMT 显式拒绝）已落；Preserve 读侧（C4）、方法级增删（MethodId 域）、CP 压缩逃生口未做（dev_plan「待裁决」）。
- 与 libgarena v2 的关系（任务1 方案 memory-governance-plan.md 裁决）：`InsnId{owner,slot,gen}` 与 libgarena 的 `GHandle{owner,slot,gen}` 语义同构（字段顺序与宽度一致、owner 进程级唯一、删除/清空 gen+1 无 ABA）。libjbc 是该句柄语义在指令域的已验证基线；libgarena 是内存域通用底座。两者不改代码即可保持互操作语义，未来 tree/内存层句柄可无损映射到 libjbc 域。

## 本文其余部分的导读

本文是 `libjbytecode` 重构的拓扑真源。目标不是把 ASM 的 `core`、`tree`、`analysis` 再包装一层，也不是再造一个更大的 `Context`。目标是先确定正确的模型边界，再把两种读取/写出策略、语义变换和外部资源管理放到模型之上。

> v0.1 的落地结果修正了本文若干条款（promote 粒度、CP 重编号策略、relocation 契约），并暴露了三条纸面设计时未预料到的硬约束。见 §18。

## 1. 结论先行

core 与 tree 可以在同一套生产架构中合并，但不能把 classfile 模型、语义模型、读取器和分析器揉成一个对象。最终只保留一个产品库：libjbytecode。它在库内按模块分层；当前工作区里的 libjbc 是该目标的实验 target/名称，不是第二个长期库。最终拓扑如下：

```text
                           mbase
       ResourceContainer / Workspace / Index / Transaction
                              │
             IClassSource / ITypeResolver / staged bytes
                              │
┌─────────────────────────────▼─────────────────────────────┐
│                    libjbytecode                             │
│  classfile DOM | codec | ir | bridge/session | analysis | scan│
│  一个构建 target，模块依赖单向：                           │
│  classfile <- codec；classfile + ir <- bridge；ir <- analysis│
│  classfile <- scan；bridge 不反向改变两套模型的所有权      │
└─────────────────────────────────────────────────────────────┘
```

模型关系不是继承关系，而是组合关系：

```text
ClassFile ───────────────┐
  │ owns                 │ backing
  ▼                      ▼
CP / fields / methods  ClassSession ── owns ── ClassUnit?
                          │                  │
                          │ BridgeState      └─ SymbolPool / CodeUnit / Insn
                          ▼
                   CP/pc/raw 映射与变更足迹

OptimisticReader ───────► ClassFile
DefensiveReader ────────► ClassFile
ClassFile ───────────────► promote ─────────► ClassUnit
ClassUnit ───────────────► lower ───────────► ClassFile
OptimisticWriter ◄─────── ClassFile
DefensiveWriter ◄──────── ClassFile
```

mbase 只依赖 libjbytecode 的公开模块 API；ResourceContainer、索引和事务仍在 mbase。FrameEngine 通过 ITypeResolver 向外查询，IClassSource 让 scanner 读取外部资源；它们都不把外部 owner 带回模型内部。

统一的是：一套架构、一份低层 classfile schema、一份高层语义 schema、一个明确的 bridge 契约、一个明确的所有权制度和一个帧引擎。

不统一的是：乐观与防御式 reader/writer、低层与高层模型、以及 `mbase` 的全局资源事务。这些分叉是输入性质和业务边界决定的，不是重复造轮子。

几个直接裁决：

1. 最终只有一个 libjbytecode 产品库；classfile、ir、codec、bridge/session、analysis、scan 是其内部模块。当前 libjbc target 只承担实验期落地，不形成第二套公共库。
2. `libjclassfile`/cafedude 的 classfile 模型是底层设计来源，最终应吸收到 `libjbytecode::classfile`。过渡期可以保留独立实现或源码副本，但不能让两套低层模型同时成为公共 API。
3. 一个 `ClassFile`、一个 `ClassUnit`、一个 `ClassSession` 都只代表一个 class。没有库级 global `Context`，也没有由任一读写库持有整个 Jar 或 JDK。
4. public API 以模型为中心；libjbc 内**禁止任何 visitor 形状**——无论公共 API、codec 内部细节还是「迁移期 adapter」一律不得引入（§26：判据是数据形态，回调推送即换皮；门禁单测与判据同源）。

## 2. 根因与设计原则

### 2.1 ASM 的问题不是 instruction payload

ASM 的真实拓扑是：

```text
ClassReader -> ClassVisitor -> ClassNode(tree)
ClassWriter <- ClassVisitor
```

`core` 的公共本体是事件协议，`tree` 是事件的物化实现。这个选择在 Java 和 GC 环境中可以工作，但移植到 C++ 后留下四个结构性后果：

- 事件流没有稳定位置，跳转、异常表和局部变量表只能靠 `Label` 令牌补洞；
- 节点没有唯一 owner，`remove`、`detach` 和 `delete` 变成靠纪律配对；
- reader 的 arena、节点和 `Any` 常量之间形成不可检查的借用关系；
- core 为了写 `StackMapTable` 封闭了一套帧算法，analysis 又维护另一套语义。

因此不能靠增加一个 arena、一个 context 或一个 wrapper 解决。必须把“模型是数据，读写和分析是行为”作为拓扑起点。

### 2.2 cafedude 的价值与边界

cafedude/libjclassfile 采取 classfile-first 的低层建模：常量池、属性、偏移、异常表和指令都有明确对象及 owner，适合未知属性、恶意输入和 roundtrip。它解决的是“如何忠实表示 classfile”，不是“如何让优化 pass 方便修改控制流”。

所以最终不是选择 ASM 或 cafedude，而是分层吸收：

```text
classfile model  <- 吸收 cafedude 的结构、所有权、原始布局
semantic IR      <- 吸收 LLVM/现有 tree 的变换人体工学
codec            <- 两种不同的读取/写出策略
bridge           <- 低层表示与高层表示之间的显式契约
```

### 2.3 数据与行为的分离

模型负责值、关系、所有权和局部不变量；行为对象负责解析、写出、提升、降级、编辑、帧计算和索引扫描。模型可以维护必要的内部索引（例如 use chain 的反向表），但不能隐含调用 reader、writer 或外部类型世界。

## 3. 两个模型、一个产品 target

模块和构建依赖固定为：

| target | 能否独立使用 | 依赖 | 主要用途 |
|---|---|---|---|
| libjbytecode::classfile | 是 | libca 等基础库 | classfile 解析、低层编辑、raw patch、无损写出 |
| libjbytecode::ir | 是 | libca 等基础库 | 高层 IR、控制流变换、从零生成 |
| libjbytecode::bridge | 需要前两模块 | classfile + ir | ClassFile 与 ClassUnit 的 promote/lower |

这里的“独立”是 API 和生命周期独立，不要求复制三份 JVMS 定义。构建系统可以拆分静态库以缩短增量编译，但对用户表现为一个 libjbytecode 产品和一套 namespace；不能借拆 target 重新形成两套公共模型或循环依赖。

### 3.1 classfile 模型：字节级真相

`libjbytecode` 是完整的单类底层库。其 `classfile::ClassFile` 模型吸收 `libjclassfile` 的 `ModelStorage`/RAII 思路，并补上未知数据和布局关系：

```text
classfile::ClassFile
├─ Header { magic, minor, major, access }
├─ ConstantPool
│  └─ CpEntry[]                         class-local identity
├─ ThisClass / SuperClass / Interfaces  CpRef
├─ FieldInfo[]
├─ MethodInfo[]
│  └─ CodeBody?
│     ├─ maxStack / maxLocals
│     ├─ InstructionRecord[] { opcode, operands, source pc }
│     ├─ ExceptionTableEntry[] { pc range, handler pc, catch type }
│     ├─ known attributes
│     └─ opaque attributes / raw regions
├─ class/field/method attributes
├─ SourceStore?                          original bytes, optional
├─ LayoutMap?                            offsets and encoded spans
└─ ParseState + Diagnostics
```

所有嵌套数据均由 `ClassFile` 拥有。对外公开的是 `CpRef`、`AttributeId`、`InstructionId` 或受 owner 约束的 view，不公开要求调用方 `delete` 的裸 owner 指针。低层模型可独立构造、移动、写出和析构，不需要 `Workspace` 或其他 class。

低层模型允许“部分结构化”：防御式读取遇到「结构可确定、语义不合规」的区段时，将该区段的原始字节作为同一个 `ClassFile` 中的保留事实（如 `ClassFile::trailing`），并在 `ParseState` 标记为 `Recovered`，降级事实同步落入 `Diagnostics`。这不是第二套 `RawClassFile` 模型，而是低层模型中明确的 tagged fallback。无法解析的 CP index、指令边界或属性长度也必须能以 `InvalidRef`/raw span 加诊断表示；不能为了让类型签名好看而伪造一个有效的 `CpRef`。结构损坏（边界无法确定）不走降级：reader 阶段一律 `Err`，不产出半截模型（§27.3）。

### 3.2 语义模型：变换的本体

libjbytecode::ir 是同一产品内可独立使用的高层语义模块；其 ir::ClassUnit 是高层 pass 直接操作的单类模型，不暴露 CP index、字节偏移或 reader 事件顺序。它有一个明确的能力边界：默认表示可参与语义变换的结构，不承诺能表达每一种 JVM 字节畸形。对于 defensive reader 识别出的“控制流可以读出、但静态语义不完整”的内容，IR 使用 `UnknownInstruction`、`UnresolvedTarget` 或 `OpaqueCodeRegion` 等 tagged 值保留事实。完全不能安全划分的代码根本不会产出模型——reader 已在结构损坏处 `Err`。因此 `Recovered` 不等于“拒绝进入高层”，而是“进入高层但保留不确定性并限制可用操作”：

```text
ir::ClassUnit
├─ SymbolPool                         ClassUnit 自己拥有
├─ ClassMetadata { symbols, access }
├─ FieldUnit[]
└─ MethodUnit[]
   ├─ MethodMetadata { name, desc, signature, access, throws }
   └─ CodeUnit?
      ├─ BasicBlockList
      ├─ InstructionStore + ordered sequence
      ├─ TryRegion[]                  BlockId / boundary handle
      ├─ LocalVariableRange[]         boundary handle
      ├─ LineRange[]                  boundary handle
      ├─ FrameTable?                   input or computed
      └─ opaque attachments            bridge-owned, not guessed semantics
```

两个模型的关键差别：

| 关注点 | `classfile::ClassFile` | `ir::ClassUnit` |
|---|---|---|
| 常量 | CP 条目和 `CpRef` | `ConstValue`、`Handle`、`SymbolId` |
| 指令 | opcode/operand + 原始 pc | typed instruction + block/edge |
| 跳转 | 编码前后的 pc | 目标态使用 owner-scoped `LabelId` 边界句柄；`BasicBlockId` 只属于分析视图 |
| 属性 | 已知结构 + 全部 opaque/raw | 已知语义；未知内容由 session 保管 |
| 栈帧 | 原始 `StackMapTable` 编码 | `FrameTable` / `FrameState` |
| 畸形结构 | 可表达、可诊断、可 roundtrip | 尽量提升；不确定部分用 tagged opaque/unknown，无法安全划分才 raw-only |
| 生命周期 | `ClassFile` 整体 owner | `ClassUnit` 整体 owner |

高层模型不是低层模型的 visitor 包装，也不是低层字段的浅拷贝视图。两者之间必须经过 bridge，bridge 的映射状态必须可被 writer 使用。低层模型仍可单独被 scanner、raw patch 工具和 defensive writer 消费；提升到 IR 不是所有输入的强制步骤。

## 4. 低层模型的核心契约

### 4.1 所有权与身份

```text
ClassFile
├─ owns ByteStore (optional)
├─ owns ConstantPool entries
├─ owns fields / methods / attributes / instructions
└─ owns raw regions and layout metadata
```

`ClassFile` 析构一次回收整棵低层树；成员容器是 owner 的内部组成，不承担第二套释放语义。任何外部引用都必须带有 `ClassFile` 生命周期前提，或使用可检查的 `Id + generation`。调用方不能从一个 class 把低层节点移到另一个 class。

CP index 是 classfile 编码身份，只在 `ClassFile` 及 codec 内有效；它不能成为高层跨 class 的引用，也不能被 mbase 的索引直接保存为全局身份。

### 4.2 属性与未知字节

已知属性由 typed payload 表示；未知属性至少保存：

```text
OpaqueAttribute
├─ name (owned/CP reference)
├─ payload bytes (owned)
├─ source span / enclosing owner
└─ RelocationContract
   ├─ referenced CP indices, if known
   ├─ referenced pc/offset fields, if known
   ├─ length/size dependencies, if known
   └─ unsupported/unknown marker
```

不能只保存 `raw info`。writer 遵守以下硬规则：

1. `ClassFile` 未修改时，若有原始 bytes，defensive writer 可以原样返回。
2. 未知属性没有 relocation contract 时，任何可能改变其 CP 或 pc 依赖的变更都必须失败；不能静默复制导致引用损坏。
3. 追加 CP 且不重编号时，已知未受影响的 opaque CP 引用可以继续有效；需要重编号时必须有明确 relocation handler。
4. 代码布局变化会使 pc 相关 opaque 属性进入受影响集合；没有 handler 就拒绝高层写回，或要求业务选择 raw-only 结果。

这使 roundtrip 的语义成为可检查的“保真或报错”，而不是“尽量保留字节”。

### 4.3 低层状态与能力

```text
ParseState::Validated   全部必要结构通过低层校验，且没有已知的不合规区段
ParseState::Recovered   结构主体可读，存在「结构可确定、语义不合规」的
                        降级区段（trailing 同族等）；降级事实记录在
                        Diagnostics，模型仍可原样写回
```

两态语义（2026-10-13 定形）：结构损坏（边界无法确定）不走本枚举——reader 阶段一律 `Err`，不产出半截模型（§27.3）；「降级」与「拒绝」两类不可混用。`ParseState` 描述能力，不把输入武断地标成“合法”或“非法”。`ClassFile` 可以被 defensive writer 原样写出；能否提升到语义 IR 由 bridge 按 `PromoteMode` 决定。`Recovered` 的唯一现行生产点是类级 trailing（§27.1）：读取产物标记 `Recovered` + warning，降级必须可查询，不允许「只有诊断没有状态」的半吊子形态。

## 5. 两种 reader 和两种 writer

> 现行裁决（2026-10-11 注记）：本节是早期设计草案，实现收敛为单一 reader/writer 策略（readClass eager 骨架 + 按需解码；writeClass 单一保真写回路径，见 README"写回只有一种模式"）。两 reader/两 writer 的 flag 化分叉不再跟进；若未来出现真实需求再按本节草案重启。

### 5.1 reader API

两个 reader 是两种算法和错误策略，不是一个带几十个 flag 的 reader。它们共享低层 schema、值类型和边界检查工具，但不共享 public visitor 协议：

```cpp
struct ReadOptions {
    bool preserveSourceBytes;
    bool preserveSourceSpans;
    bool decodeKnownAttributes;
};

struct ReadResult {
    std::unique_ptr<classfile::ClassFile> model;
    Diagnostics diagnostics;
    ParseState state;
};

class OptimisticReader {
public:
    Result<ReadResult, ReadError> read(ByteView bytes, const ReadOptions&);
};

class DefensiveReader {
public:
    Result<ReadResult, ReadError> read(ByteView bytes, const ReadOptions&);
};
```

`OptimisticReader`：

- 面向正常版本和正常结构；
- 允许直接索引和快速解码；
- 遇到无法满足预期的不变量可以失败；
- 产出的 `ClassFile` 通常为 `Validated`，适合直接 promote。

`DefensiveReader`：

- 每个长度、边界、CP 引用和嵌套属性单独检查；
- 保留 source bytes、source spans、诊断和未知区段；
- 结构损坏返回 `Err`（不产出模型）；「结构可确定、语义不合规」返回 `Recovered` 模型；
- 不承诺所有输入都能进入高层 IR，但承诺“不丢失地表达或明确拒绝”。

reader 内部可以继续使用 ASM 风格的事件分派作为实现优化，但它只能写入 `ClassFileBuilder` 等库内对象，不能要求用户实现 `ClassVisitor`，也不能把事件顺序当成模型身份。

### 5.2 writer API

```cpp
struct WriteOptions {
    FramePolicy framePolicy;
    bool preserveUnchangedRegions;
    bool allowConstantPoolRebuild;
};

class OptimisticWriter {
public:
    Result<ByteBuffer, WriteError> write(const classfile::ClassFile&, const WriteOptions&);
};

class DefensiveWriter {
public:
    Result<ByteBuffer, WriteError> write(const classfile::ClassFile&, const WriteOptions&);
};
```

`OptimisticWriter` 假定输入可编码，允许重建 CP、指令布局和属性顺序，服务合法 class 的正常变换。

`DefensiveWriter` 依赖低层 source/layout/opaque 信息，优先 copy-through 未修改区域，并只对已知 relocation 的区域重编码。它不是“更宽松的 optimistic writer”，而是另一种保真策略。

输入状态和输出能力的关系：

| 输入 | OptimisticWriter | DefensiveWriter |
|---|---|---|
| `Validated` + 正常 IR lower | 支持 | 支持 |
| `Recovered` + 未影响 opaque | 可拒绝或仅写已知部分 | 支持 |
| `Recovered` + 影响未知 CP/pc | 必须显式丢弃/重建，默认拒绝 | 拒绝，除非有 relocation handler |

结构损坏输入没有写出行——reader 已拒绝，模型不存在。

## 6. `ClassSession`：单类转换和保真边界

`ClassFile` 和 `ClassUnit` 都必须能独立存在；但从输入 class 做高层变换时，只返回 `ClassUnit` 会丢失低层的 opaque 属性、source span 和 CP/offset 映射。因此提供一个可选的、仍然只代表一个 class 的 `ClassSession`：

```text
ClassSession (one class only)
├─ backing: classfile::ClassFile
├─ unit: ir::ClassUnit?
└─ BridgeState
   ├─ CP index <-> Symbol/ConstValue map
   ├─ pc/Instruction <-> Block/Insn map
   ├─ opaque attribute store and contracts
   ├─ original layout/source spans
   └─ MutationFootprint
```

`ClassSession` 不是 Workspace，不持有其他 class，不持有 `ResourceContainer`，也不持有 JDK 类型世界。它只是“一个低层 backing 加一个高层投影以及转换状态”。

### 6.1 bridge API

```cpp
enum class PromoteMode {
    Strict,     // 无法表达的结构直接失败
    Opaque      // 可表达区域进入 IR，其余挂到 session opaque store
};              // 「不提升」是调用方持有低层模型的决策，不是一种模式

struct PromoteOptions {
    PromoteMode mode;
    bool decodeDebugTables;
    bool importInputFrames;
};

Result<ClassSession, PromoteError>
promote(std::unique_ptr<classfile::ClassFile>, const PromoteOptions&);

struct LowerOptions {
    FramePolicy framePolicy;
    bool preserveOpaque;
    bool rebuildConstantPool;
};

Result<void, LowerError>
lower(ClassSession&, const LowerOptions&);
```

另有两个独立场景：

```cpp
ir::ClassUnit unit = ir::ClassUnit::create(...);   // 从零生成
Result<classfile::ClassFile, LowerError>
lower(const ir::ClassUnit&, const LowerContext&);  // 生成新 classfile
```

从已有 class 变换时，推荐始终使用 `ClassSession`；从零生成时不需要虚构一个低层 backing。`lower(session)` 是唯一把 IR 变更提交回 backing 的入口；session 活跃期间直接修改 backing 会使 bridge 映射失效，API 应禁止或显式要求重新 `promote`。

### 6.2 提升边界

`promote` 不是 visitor replay，而是一次显式的数据变换：

```text
CpIndex / source pc / raw attribute
              │
              │ promote
              ▼
SymbolId / ConstValue / BlockId / InsnId
              │
              │ lower + encode
              ▼
new CpIndex / new pc / relocated attributes
```

对 `Validated` class，已知指令、异常表、局部变量范围、行号和帧可以完整进入高层。对 `Recovered` class，`Opaque` 模式提升可划分的控制流和指令，并把 `UnknownInstruction`、`UnresolvedTarget`、opaque code region 等不确定性作为显式值保留，同时记录 owner、原始数据和影响范围；涉及这些值的编辑默认失败，除非 editor 提供专门的 raw/opaque 操作。无法安全确定区段边界的输入在 reader 阶段已被 `Err` 拒绝，不存在「停在低层的半截模型」这一形态；调用方刻意只用低层模型（不 promote）是持有决策，不是状态。

“统一模型”不等于强迫每个畸形字节都变成一个看似合法的高层节点。统一的要求是：所有输入先落在同一个可拥有、可诊断的低层 schema；进入高层的资格和损失边界必须显式，而不能由某个 visitor 的偶然顺序决定。

## 7. 高层 IR 的身份、容器和删除语义

### 7.1 身份不依赖 global Context

高层模型内使用 owner-scoped handle：

```text
ModelId  = construction-time identity of ClassUnit/CodeUnit
InsnId   = { owner: ModelId, slot: u32, generation: u32 }
BlockId  = { owner: CodeUnitId, slot: u32, generation: u32 }
SymbolId = { owner: ClassUnitId, slot: u32 }
```

具体内部存储可以是 slot table、chunked arena 或稳定地址容器，但 public handle 不能依赖裸指针。owner 和 generation 必须参与相等性及查找；来自另一个 `ClassUnit`/`CodeUnit` 的同号句柄必须被结构性拒绝。句柄在删除或 owner 析构后失效，不能继续使用。

这与 job-level `Context` 的差别很重要：每个 `ClassUnit` 自己拥有需要的符号和节点，模型可独立释放；外部若要批量处理 class，使用 mbase 的调度和索引，而不是把所有 class 塞进一个永不释放的 arena。

### 7.2 指令和控制流

```text
ClassUnit owns MethodUnit
MethodUnit owns CodeUnit
CodeUnit owns BasicBlockList and InstructionStore
CodeEditor owns mutation protocol and reverse-use index
```

基本块是分析得到的控制流视图；跳转、switch、异常处理器最终使用 owner-scoped 边界句柄，不再有 `Label`/`LabelNode` 双重身份。指令顺序是 sequence/container 的数据，不是指针链的隐含副作用。调试范围和异常范围使用受 owner 校验的 boundary handle，不能跨方法引用。

操作数是封闭的 typed value：

```text
Insn
├─ opcode/kind
├─ local index / immediate
├─ FieldRef / MethodRef / TypeRef
├─ ConstValue (int/float/long/double/string/class/handle/dynamic)
├─ BlockId target
└─ SwitchCase[] { key, BlockId }
```

`ConstValue` 替代 `ca::core::Any`。字符串和描述符不存指向 reader 临时 arena 的裸视图，而是存 `SymbolId`；对外需要文本时返回受 `ClassUnit` 生命周期约束的 `Utf8StringRef` view。

### 7.3 `CodeEditor` 与 use chain

> **状态注记（目标态草案）**：本节草稿以 BlockId 为块身份；v0.3 落地的是 `InsnId` 句柄 + 按需只读块视图，**没有 BlockId 域**，块操作走指令句柄。引入 BlockId 的前置条件是有真实 pass 需要"块级变更身份"（持块、跨块改写、拆块且引用自动跟随）；在那之前以 `code_editor.hpp` 的现行接口为准，勿照本节签名调 API。

所有结构化变更经过 `CodeEditor`：

```cpp
class CodeEditor {
public:
    BlockId createBlock(CodeUnit&);
    InsnId append(BlockId, Instruction);
    Result<InsnId, EditError> insertBefore(InsnId, Instruction);
    Result<void, EditError> erase(InsnId, ErasePolicy);
    Result<void, EditError> replaceAllUsesWith(BlockId from, BlockId to);
    Result<void, EditError> retargetAndErase(BlockId, RetargetPolicy);
    Result<void, EditError> splitBlock(BlockId, Boundary);
};
```

反向 use index 覆盖 branch、switch、try region、局部变量范围和行号范围等引用。删除规则不是注释约定，而是 API 结果类型可见的状态机：

1. 有 incoming use 时，默认 `erase` 返回 `EditError::StillUsed`。
2. `replaceAllUsesWith` 或显式 retarget 后，引用计数归零才允许删除。
3. `detach` 若需要保留对象，返回 move-only `DetachedInstruction`；它只能被重新插入或显式丢弃，不能变成调用方必须 `delete` 的裸指针。
4. 删除后原 `InsnId`/`BlockId` 立即失效；迭代器和 view 在结构变更后必须通过 generation 检查或重新取得。

因此“摘链但忘记 delete”“label 池旁路接管”“clone 手工维护 label map”这类问题不再是应用层纪律项，而是模型和 editor 的结构性不变量。

## 8. 字符串、常量和内存规模

### 8.1 三个不同的去重域

不能用一个全局 arena 同时解决单类所有权、全 Jar 索引和 JDK 依赖扫描。合理的去重域是分开的：

| 域 | owner | 保存什么 | 生命周期 |
|---|---|---|---|
| classfile CP | 一个 `ClassFile` | class 内编码需要的 UTF8/常量 | 一个 class |
| IR `SymbolPool` | 一个 `ClassUnit` | 该 class 的名字、描述符、常量值 | 一个 class/session |
| mbase `ClassIndex` symbols | 一个索引实例 | 索引实际需要的签名和类名 | 一个索引/事务 |

同一个 class 内部的重复由 CP 或 `SymbolPool` 去重；不同 class 的字符串只有在 mbase 确实要把它们放进全局索引时才进入索引 interner。索引 interner 存拥有型值或 `SymbolId`，不存任何 `ClassFile*`、`ClassUnit*` 或 arena view。

### 8.2 大 Jar 和标准库

典型流程不是“把整个 Jar 和 rt.jar 都解析进一个 Context”：

```text
scan class bytes -> 生成 ClassSummary/Reference edges -> 释放临时模型
                              │
                              ▼
                       mbase ClassIndex

修改某个 class -> open one ClassFile/ClassSession -> lower/write -> stage bytes
                              │
                              ▼
                       释放该 session
```

全局签名修改先用索引找受影响类和成员，再逐类打开。标准库的父类、接口和成员查询由 lazy `ITypeResolver` 提供，缓存摘要而非完整 class。若某个业务明确需要同时保留多个高层模型，那是 mbase 的显式内存预算和生命周期选择，不是 `libjbytecode` 隐式扩张出一个 global owner。

## 9. `mbase`、索引和 `ResourceContainer`

读写库只定义最小 source/resolver 协议，不依赖 `ResourceContainer`：

```cpp
class IClassSource {
public:
    virtual Span<const u8> bytes(ClassKey) const = 0;
    virtual ~IClassSource() = default;
};

class SummaryScanner {
public:
    Result<ClassSummary, ScanError> scan(ByteView);
};

class ReferenceScanner {
public:
    Result<ReferenceSummary, ScanError> scan(ByteView);
};
```

mbase 提供 `ResourceContainerClassSource`，并负责：

- Jar/目录/多资源来源的枚举；
- `ClassSummary`、方法签名和引用边的全局索引；
- 通过索引和 lazy source 实现 `ITypeResolver`；
- 业务 pass 的 stage、统一提交、回滚和缓存失效。

一个真实的跨类修改流程是：

```text
ResourceContainer
  -> SummaryScanner/ReferenceScanner
  -> ClassIndex (只存 ClassKey/MemberKey/SymbolId/引用边)
  -> 生成修改计划
  -> 逐类 read -> promote -> CodeEditor/lower -> writer
  -> ResourceContainer::stage
  -> 所有类成功后 commit，否则 rollback
```

统一提交和回归语义属于业务事务，不属于 `ClassFile`、reader 或 writer。库层只保证单类模型的结果可验证、可写出、可报告错误。

## 10. 帧计算和畸形控制流

帧计算是语义服务，不应藏在 core writer 内部，也不应由 analysis 再维护一份完全不同的引擎。libjbytecode 提供一个可注入 resolver 的 `FrameEngine`：

```cpp
class ITypeResolver {
public:
    virtual Result<ClassHeader, TypeError> header(TypeName) = 0;
    virtual Result<bool, TypeError> isInterface(TypeName) = 0;
    virtual Result<TypeName, TypeError> superClass(TypeName) = 0;
    virtual Result<MemberSummary, TypeError> members(TypeName) = 0;
    virtual ~ITypeResolver() = default;
};

enum class FramePolicy {
    Strict,       // 不一致即错误
    BestEffort,   // 返回 Unknown/diagnostic，不让分析器崩溃
    Preserve      // 尽量使用输入帧；无法保真则报错
};

class FrameEngine {
public:
    Result<FrameTable, FrameError>
    compute(const ir::CodeUnit&, ITypeResolver&, FramePolicy);
};
```

writer 不再拥有一套封闭的 `Frame.java` 翻版。它可以：

- 使用调用方提供的 `FrameTable`；
- 调用同一个 `FrameEngine` 的标准实现；
- 在 `Preserve` 模式下保留输入 StackMapTable，但在布局变化无法映射时报错。

对于 obf 故意制造的 JVM 可运行但静态 verifier 不喜欢的控制流：

1. 防御式读取把「结构可确定、语义不合规」的低层结构读入 `Recovered`；结构损坏一律 `Err`（§27.3）；可划分的 `Recovered` code 允许以带 `Unknown` 的 relaxed IR 继续处理；
2. relaxed IR 不自动获得 `Strict` 验证资格，涉及未知值的编辑和写回必须显式；
3. `BestEffort` 只承诺返回可诊断的 `Unknown`，不承诺伪造一个错误的 frame；
4. 如果业务自己能计算帧，可将结果作为 `FrameTable` 注入 writer；
5. 如果未知结构或 opaque 属性会被代码布局影响，writer 必须明确失败，而不是生成一个看似完整但 JVM 行为未定义的 class。

这样 core 写出对帧的依赖通过显式服务接口反转到 analysis/业务，且整个库只有一套 transfer/merge 语义。

### §10.1 C1 落地裁决（2026-10-13）：resolver 边界与三态策略定形

本节把 §10 草案中 A1（SMT 生成）需要的部分定形为可开工裁决；草案其余部分（四方法 resolver 全量类型世界）保留为 C5 验证 API 的目标态草案，不前置。

1. **resolver 边界 = merge 单方法面，冻结为 A1 契约**。A1 的唯一外部类型查询点在控制流合并：两个具体引用类型 join 需要公共父类。指令 transfer 的类型全部来自 opcode 与描述符（单类内自足），不需要类型世界；接口维度不参与 merge 结果（与 mjbase spi 公共父类求解的 super-chain-only 裁决一致）。因此现实现的 `ITypeResolver` 单方法形态（`merge(VType,VType)`）即为 A1 契约，不为 A1 扩面。
2. **实现体归属 mjbase**（层次图背书的公共父类求解，spi 侧已有）；libjbc 只提供注入点与默认降级实现（ShapeResolver：引用族互并退形状 Ref）。单类职责不变。
3. **三态策略定形**：
   - **BestEffort**（现状）：形状数据流，域引擎当前形态（帧 oracle 100% 即此档）。
   - **Strict**（A1 的生成档）：类型精确——transfer 携带具体类型，merge 经注入 resolver 求解；算不动（含 Uninitialized offset 冲突）以指令级诊断显式拒绝，不伪造帧。验收口径：**可算者与 javac 产物一致、不可算者显式拒绝并给指令定位**；obf 语料允许存在拒绝清单，不追求全量可算。Strict 运行必须注入 resolver，未注入即失败（fail loud）。
   - **Preserve**（读侧 = C4）：从既有 SMT（same/append/chop 链）还原语义帧；首要用途是 A1 的自校验——未修改方法上 decode(generate(x)) 与 x 等价。布局未变的未触碰方法继续走 copy-through，不进重算。
4. **Uninitialized 语义随 A1 落地**：VKind 增加 UninitThis 与 Uninitialized(new 的指令偏移) 两形态；invokespecial \<init\> 后翻转、按 offset 判等、异 offset merge 在 Strict 下为冲突。jsr/ret 当时限显式拒绝（Subroutine 失败码），不在 A1 范围——后随 C2 落地为数据流建模（§10.6）。

### §10.2 Strict 域实现注记（2026-10-13，A1/J4 落地）

§10.1 的实现载体与契约细节，语义层不变量以此为准。

1. **三态 API 面**：`FramePolicy{BestEffort, Strict, Preserve}`；引擎构造矩阵为（池、档位、resolver 三参自由组合）。Strict 未注入 resolver 时 compute 显式 `Err(NoResolver)`（fail loud，不降级）；Preserve 是读侧占位，档位不装配计算域，compute 显式 `Err(NotSupported)`——在 C4/J7 实现前不存在被静默当 BestEffort 跑的路径。
2. **VType 契约**：`name` 是精确内部名的借用视图（寿命 = 常量池或域内驻留表，随引擎存活）；Uninitialized 的 new 指令 pc 承载在 cls 字段（`uninitOffset()`）；Strict 相等语义 = (kind, offset, name)（`sameAs`）。BestEffort 路径不触碰 name，行为与引入前逐位一致。
3. **resolver 契约**：`merge(VType, VType) → Result<VType, TypeMergeError{NoCommonParent, Unresolvable}>`。结构性规则按两格分离（2026-10-13 修订，依据见 §10.4.8）：BestEffort 形状格的 top = 未知形状但占槽，吸收语义（merge(Top, X) = X）服务 maxes/占用统计收敛；Strict verifier 类型格的 top = 不可用值，汇合支配（merge(Top, X) = Top）——JVMS 4.10.1.4 帧匹配的规范性口径，生成 SMT 的唯一裁判是 verifier。null 作万能引用底、Uninitialized 按 offset 判等、UninitThis 与其他并即冲突两格同规；resolver 只解「两个具体引用类型的公共父类」。BestEffort 的桥接域把 resolver 的 Err 降级为 Bottom 继续（宽松合并语义不变）。
4. **引擎可选钩子**（全部带默认实现，既有自定义域零改动）：播种精确化（`newThis`/`newParameterTyped`，MethodSig 相应扩名与 this 类）、`<init>` 翻转（`initializedForm`，引擎把 receiver 的全部同值出现点——栈行与局部行——替换为已初始化形态）、`onRet`（RET 点通知）、`failCode`/`clearFail`（Strict 失败通道：引擎在每次指令 transfer 后与每次状态合并后查询，failAt 分别定位到当前指令与合并点指令）。
5. **不变量**：BestEffort 逐位不变由缺省钩子的恒等/无操作结构性保证（翻转走恒等、失败通道恒空），不依赖档位判断散布在引擎里；Strict 不伪造帧——不可算即 Err + 指令定位，结果帧只交付 ok=true 的方法。
6. **与 javac 的陈述口径差异（A1 生成侧备忘）**：javac 的 SMT 帧按「局部变量声明类型 + 目标类型化表达式类型」陈述，可以是运行值真实 lub 的父类（verifier 按可指派性接受）；Strict 域陈述的是运行值精确类型。二者在 verifier 语义下都合法，但帧字面不同——J6 生成 SMT 时须显式选择陈述口径（精确 lub 或声明类型投影），不得隐式混用。

### §10.3 Strict 异常边精确传播（2026-10-13，A1/J5 落地）

异常出口边的语义层不变量（检查清单 C3）：

1. **边集**：try 范围内每条可抛指令向覆盖它的每个 handler 发异常出口边（按异常表序，多条表项指向同一 handler 时逐条并入）。可抛指令集 = JVM 规范保守全集：CP 装载与类操作（ldc 族/new 家族/checkcast/instanceof）、字段与调用、数组载荷/存储/长度、除法余（Arithmetic-Exception）、监视器、athrow；纯局部算术等不可抛指令不在内。以 javac（--release 8）oracle 校准。
2. **入口状态**：handler 入口 = (抛出点 in-state locals, [catch_type 对应类型])。locals 取指令 transfer 前形态——invokespecial \<init\> 的初始化翻转折算正常后继效应，异常路径不携带；栈整体替换为异常值（JVM verifier 语义）。入口栈取 catch_type 的精确类（CP Class 名）；catch_type=0（any/finally 汇聚 handler）播 java/lang/Throwable。
3. **路径过滤 = 保守放行单向规则**：判定某抛出点的异常类型与 catch_type 是否匹配需要类层级查询；A1 契约的 resolver 是 merge 单方法面，不引入第二个 resolver 面。层级缺席即不判定、一律保守放行（边照常并入）——本层类型匹配不可算，过滤退化为全并入；入栈值恒取 catch_type 精确类，帧字面不受影响。真过滤（按抛出点异常类型裁边）归 resolver 真实现（mjbase 侧）落地后再议，接口不扩。
4. **可达性**：handler 可达 ⟺ 其 try 范围内存在可达的可抛指令且至少一条异常边实际并入；死 try 范围的 handler 不可达。BestEffort 档保持无条件播种（入口空 locals + 万能 Ref、恒可达），逐位不变。
5. **多抛出点合并**：handler 入口 locals 经域合并钩子收敛——同类保持、异类引用走注入 resolver、冲突置 TypeConflict 且 failAt 定位到 handler 入口指令；只在一部分抛点 in-state 被赋值的槽（其余抛点缺席）按 Strict verifier 类型格支配为 Top（§10.2.3 两格分离）。多 catch（多条表项同一 handler）的 catch_type 值逐条并入，入口栈收敛为各表项类型的公共父类（经 resolver），与 javac 的 lub 陈述一致。
6. **与 javac 的 handler 帧陈述口径差异（§10.2.6 的读侧体现，A1 生成侧备忘）**：javac 对 handler 入口做声明变量投影——死变量 chop 截断（如 synchronized 的 monitor 副本），handler 体被自身覆盖的回边（异常表项覆盖 handler 自身首段）不并入；Strict 域陈述的是 verifier 级原始合并（全部覆盖可抛点并入，含回边）。二者按 JVMS 4.10.1.4 可指派性同为合法帧——声明的 locals 可少于计算所得，被截去部分不构成约束；oracle 比对按公共前缀逐槽一致。J6 生成 SMT 时须显式选择陈述口径（原始合并或声明投影），不得隐式混用。
7. **死 try 的 javac 侧事实**：--release 8 下 javac 对源级死 try（if(false) 包裹）整体不生成代码，oracle 无此类样本可比；try 范围内无可抛指令的退化形态（如 try { return 1; }）javac 仍为 handler 陈述帧，与本条可达性规则存在字面分歧——oracle 样本选择排除该退化形态，「死 try 的 handler 不可达」由合成用例覆盖。

### §10.4 SMT 生成器语义不变量（2026-10-13，A1/J6 落地）

Strict 帧 → `StackMapTableAttr` 的编码层不变量。API 为 `buildStackMapTable(session, code, frames)`：显式调用出活（copy-through），不自动接线 lower/写回——写回接线与 maxes 自动采用同批（J7）。

1. **陈述口径（裁决执行）**：生成器逐字陈述 Strict 域帧（运行值精确类型），不做 javac 的声明投影（死变量 chop / 父类声明）。与 javac 的比对口径 = 公共前缀逐槽一致（多出的尾槽 / 更精确类型不算分歧）。
2. **目标集合**：需要帧的位置 = 分支/switch 各目标 + 异常 handler 入口（经 `resolveLabel`，End 不取），按帧可达性过滤；帧序按目标 pc 升序，`offset_delta` 链 arithmetic（首帧 delta = pc，其后 = 相邻 pc 差 - 1）与 relocateSmt/属性编解码共享同一组小函数（单一知识源，零拷贝）。初始帧不编码：链的隐式起点取 `frames[0].locals` 的条目展开（引擎播种口径），显式帧从第一个目标开始。
3. **紧凑形态选择**（javac 极小化策略，JVMS 4.7.4 全族）：locals 条目序列与链上前帧一致 → same 家族（delta ≤ 63 且栈空 = same；栈恰 1 条目 = same1；否则 251/247 扩展形）；栈空且 locals 恰为前帧去掉/追加末尾 k（1..3）条 → chop 251-k / append 251+k（载荷仅追加项）；其余（中部变化、k 溢出、栈非空且非单条目、chop/append 前置不成立）→ full 整体陈述。
4. **cat2 展开**：帧域是槽语义（long/double = 值槽 + Top 垫底槽），SMT locals/stack 是条目语义（cat2 一条）——展开时垫底槽必须恰为 Top，否则帧不自洽，显式拒绝；Top 出现在值位（非垫底）原样陈述。
5. **Uninitialized 载荷**：tag-8 的 offset 必须命中方法内某个 new 指令的 pc（帧域携带 srcPc 口径），否则显式拒绝；tag-7 的 CP 索引经 `ClassSession::internClass` 落位（既有条目复用，miss 才追加；属性名 Utf8 同样复用既有条目）。
6. **版本门与空产出**：backing 主版本 < 50 显式 `Err(ClassTooOld)`（SMT 在 50- 无意义，调用方决定降级策略）；无控制流（无目标）→ `needed=false` 且不产出属性——空 SMT 属性本身不生成；帧数与指令序列不对齐、delta/帧数超 u16 一律显式 Err。
7. **验收**：合成 24 形态表驱动（全紧凑族 + 边界 delta + cat2 + full 化）+ javac（--release 8）oracle（J4/J5 语料 + 循环/switch 扩展）剥原 SMT → 再生 → 展开比对 20 方法 48 帧 0 分歧 + JVM 真跑：探针样本全部自然 javac 形态（异常 handler 嵌循环、main 末尾反射 try/catch——含 §10.4.8 两个暴露形态、switch、构造器链）剥离→再生→ `java` 执行 verifier 与语义一次过，另有手拼 tag-8（Uninitialized）真实帧类经反射加载执行。libjbc_unittest 188 用例 175 过 13 跳（J6 新增 16 全绿），mjbase_unittest 1136/1134/2 零漂移。
8. **Top 支配（已裁决，2026-10-13 主会话）**：Strict verifier 类型格的 Top 合并为支配语义（merge(Top, X) = Top），BestEffort 形状格保持吸收语义不动；工作表合并协议「入边缺席的行」改为显式过域合并——原实现把吸收语义硬编为短路，吸收口径下值恒等，故 BestEffort 逐位不变。裁决依据：(a) JVMS 4.10.1.4 帧匹配是规范性判据——汇合槽单边 top（未赋值/不可用）合并必为 top，top 不可指派到具体类型；生成 SMT 的唯一裁判是 JVM verifier，生成其必拒的帧是正确性问题不是口径问题。(b) 原 §10.2.3 把两个格混为一谈：BestEffort 形状格（top = 未知形状但占槽，吸收语义保 maxes/占用统计收敛）与 Strict verifier 类型格（top = 不可用值，汇合支配）——两格分离后各自语义更准确。(c) javac 对齐性提升：javac full_frame 对单边赋值槽本就陈述 ITEM_Top（声明投影只在尾槽 chop），Top 支配后公共前缀内逐槽一致包括此前被前缀比对掩盖的槽。**暴露形态（修正前 VerifyError 实证）**：汇合槽单边 top × 具体类型被陈述为具体类型，verifier 必拒——两个极简 javac 形态：handler 入口帧（try 体首存前有更早可抛点）、try/catch 后 join 帧（catch 变量槽只在 handler 路径赋值）。公共前缀 oracle 对此结构性失明（javac 尾槽 chop 不构成约束），只有 JVM 真跑门能暴露。**落地**：探针样本恢复自然形态（循环包 try/catch、main 末尾反射 try/catch——两个暴露形态直接在 JVM 真跑覆盖）；4 个钉住吸收口径的合成用例改写（参数/分支汇合样本改双臂同高的 Null × Ref 汇合、handler 入口直测单边 Top + 全边已赋槽具体保持、resolver 合并样本将 try 起点移到首个 store 之后保持真引用 × 真引用）；新增常规边汇合单边赋值槽 → append [top] 直测。libjbc_unittest 188 用例 175 过 13 跳，mjbase_unittest 1136/1134/2 零漂移。

### §10.5 Preserve 读侧与 maxes 采用（2026-10-13，A1/J7 落地）

1. **读侧定位与入口选型**：Preserve 档的语义是「读属性」不是「跑数据流」，装配点是 `analysis::readFramesFromSmt(session, code)` 独立入口，不进 `FrameEngine::compute` 路由——compute 的契约是逐指令对齐的数据流交付（播种、worklist、可达性），读侧交付不了也不该交付这个形状；compute 对 Preserve 恒显式 `Err(NotSupported)`，语义路由不误导。用途定位：读到的 SMT 与计算帧比对（A1 自校验、pass 预检），不是计算引擎的替代。
2. **链头与链算术**：隐式初始帧 = `initialLocalsOf(sig)`——this 形态（构造器 UninitThis、其余精确类名）+ 描述符参数，与引擎 MethodSig 播种同源同义。链算术在 SMT 条目空间进行（chop/append 按条目计数），不按 maxLocals 预垫 Top——预垫会把 append 载荷推到垫顶之后，错位全部槽号。cat2 读侧交付双槽（值槽 + Top 垫底），与 compute 的帧交付同构，读↔算对拍两侧同形逐槽可比。防御面（无法保真则报错，不猜）：chop 超出链上既有条目、展开槽数超 CodeUnit.maxes、tag-8 载荷不落 new、Object 条目 CP 不可读、形态标签与载荷矛盾、帧目标 pc 不对齐指令，一律显式 Err。属性缺席（无控制流 / 全死目标 / 版本 < 50）= Ok(空视图)，缺席原因的判定归调用方；视图 reachable 恒 true（属性声明即到达，死代码判定归计算侧）。
3. **读↔算对拍口径**：javac 语料逐目标逐槽——locals 尾部 Top 修剪后公共前缀一致 + stack 全量一致（§10.2.6/§10.3.6 口径）。验收：20 方法 48 帧 0 分歧。
4. **maxes 采用（A2 裁决：Strict 显式重算 = 覆盖）**：`applyComputedMaxes(codeUnit, summary, sig)` 显式 API——重算结果即真值，CodeUnit 现值是旧栈产物，低于原值亦采用（Top 支配口径下重算值与 verifier 一致）。两个 verifier 地板并入采用值：maxLocals ≥ 参数槽（this + 描述符参数——引擎 summary 的 maxLocals 只计 store 触达，只读参数的方法观测值低于 verifier 要求，与 mjbase recomputeFrameMaxes 的参数地板同规）；maxStack ≥ 1（方法带异常表时，handler 入口栈恒有异常值）。summary 非 ok 或地板并入后超 u16 编码域：不采用且不改任何字段。只显式、不隐式：BestEffort 手动链路与 promote 拷贝语义零变化，「lower 时自动重算」不做（管线级策略归 mjbase 侧后续批）。
5. **挂账**：引擎 summary 的 maxLocals「触达」口径（只计 store）对只读参数的方法低于 verifier 底线——本片以 API 层地板解决；engine 侧把播种参数并入 maxLocals 会改动 BestEffort 可观测值，留待独立裁决。

### §10.6 Strict 域子程序数据流（2026-10-14，C2 落地）

jsr/ret 从「显式拒绝」改为数据流建模。语义依据（JVMS 当前版）：§4.10.2.5「Exceptions and finally」（jsr 压返回地址 = jsr 顺序后继的地址、子程序把返回地址存入局部槽、ret 从局部槽取回跳转；以及子程序返回点合并的 touched/untouched 局部槽规则）+ §6.5 jsr/jsr_w/ret/astore 指令语义 + §4.9.2（jsr 目标必须是方法内某条指令的 opcode，结构约束）。

1. **值建模**：`VKind::ReturnAddress`，cls 载荷 = 调用方 jsr 指令的 srcPc（`VType::retAddr(pc)` / `jsrPc()`，与 Uninitialized 的 new pc 同款载荷模式）。jsr/jsr_w transfer 经域钩子 `newReturnAddress` 压栈（默认退回 `newValue(Int)`——形状域 legacy 行为逐位不变；Strict 覆写为带调用点载荷的 ReturnAddress）。astore 存返回地址靠 Strict 的 store/load 恒等穿透，无需新规则。
2. **控制流**（仅建模子程序的域，`modelsSubroutines()` 门控）：jsr 不再发顺序边——顺序后继是返回点，只能经 ret 到达；ret 无 transfer 效应，出口态（= 进入态）经标准 propagate 协议并入返回点（返回点 = 返回地址载荷指向的 jsr 的顺序后继，引擎建 srcPc → order 反查表解析）。子程序未改写的局部槽保留调用方值、改写的槽携子程序值——单调用点时这是 §4.10.2.5 合并规则的精确形态（无跨调用点合并，无需 touched 位向量）。BestEffort 域不建模 ret，保留 legacy 双后继传播与 degraded 计数（bit-identical 契约，mjbase 对拍钉死）。
3. **合并规则**：returnAddress 只与自身合并（§4.10.2.5 族）。异调用点的返回地址互并 = 多调用点共享同一子程序入口——精确语义需逐调用点状态快照（JVMS 的 touched-bit-vector 机制），Strict 域显式拒绝（`Fail::Subroutine`，failAt = 入口合并点），fail loud 不伪造帧。验收口径沿用 A1：可算者精确（含嵌套子程序双返回点、改写/保留局部槽对照）、不可算者显式拒绝并给指令定位。
4. **畸形路径行为表**（全部显式拒绝，failAt 定位）：

   | 畸形 | 行为 | 定位 |
   |---|---|---|
   | ret 读非返回地址槽（Int/Null/Ref/Top/空槽/cat2 垫底） | `Subroutine` | ret 指令 |
   | 返回点越界（jsr 是方法末条指令，返回地址无落点） | `Subroutine` | ret 指令 |
   | 多调用点共享入口（异调用点返回地址互并） | `Subroutine` | 入口合并点 |
   | jsr 指向非指令偏移/越出 code | promote 层 `BadInstructions`（§4.9.2 结构约束归 reader/promote，帧域不重复把关） | — |

   已知边界：jsr 在 v51+ 类文件中按 JVMS §4.10.1 不再可验，但帧域版本无关（数据流可算即算），版本合法性归结构校验层；SMT 侧拒绝见下。
5. **SMT 生成裁决（保持拒绝，显式成文）**：full_frame 的 verification_type_info 是封闭 tag 集，无 returnAddress 形态（JVMS §4.7.4，tag 0–8）——含返回地址槽的帧不可编码。不采用「改码 Top/Int」的伪造，也不做 javac 式逐调用点帧内联（需子程序归属分析与逐调用点帧键，超出 single-state 引擎的契约面）：`buildStackMapTable` 对含返回地址槽的帧显式 `Err(SmtBuildError::Subroutine)`。v≤49 的方法本就走版本门 `Err(ClassTooOld)`（SMT 属性无定义；不带 SMT 走旧校验是 §4.10.1 许可行为，不需要也不得生成）——两态都有测试钉死。
6. **验收**：合成面（嵌套子程序双返回点、子程序改写 local 与保留调用方槽对照、入口栈返回地址载荷）+ 畸形面全表 + JVM 真跑门（v49 手造样本 javap 结构核对 + 本机 java 实际加载执行 42/104/13，畸形样本 JVM 侧同样 VerifyError；引擎对拍 JVM 验过的类逐方法可算）。libjbc_unittest 294 用例 293 过 1 跳（C2 新增 11 全绿），mjbase_unittest 1188/1186/2 零漂移（jsr legacy 双后继契约由 BestEffort 门控保留）。测试面在 subroutine_test.cpp。

## 11. 输入性质与处理路径矩阵

“正常/畸形”和“乐观/防御式”是两条不同的轴，不能用一个 `Reader` 状态位代替：

| 输入/目标 | 读取 | 模型状态 | 是否 promote | 写出 |
|---|---|---|---|---|
| 合法 class，追求吞吐 | optimistic | `Validated` | 是，`Strict` | optimistic 或 defensive |
| 合法 class，需要未知属性无损 | defensive | `Validated` + source/layout | 是，`Strict`/`Opaque` | defensive |
| 可划分但含非标准控制流 | defensive | `Recovered` | 是，`Opaque` relaxed IR | 仅在影响范围可证明时；否则报错 |
| 结构可确定、语义不合规（trailing 同族） | defensive | `Recovered` + warning | 按 opaque 语义 | defensive 原样写出（再读仍可解析） |
| 区段边界不可安全确定 | defensive | 无模型 | 否 | reader 阶段 `Err` |
| 从零生成 class | 无 reader | 新建 `ClassUnit` | 不适用 | lower 后任一 writer |

这里的 `promote` 选择的是“是否把低层事实投影为可变语义”，不是“是否相信输入合法”。`Recovered` 可以进入带 unknown/opaque 的高层模型，但不会因此获得严格 verifier 语义；结构损坏则根本不产出模型。这样同一个底层 schema 同时覆盖 happy path 和 hostile path，又不会把高层 IR 变成承载所有字节畸形的垃圾场。

## 12. 公共 API 拓扑

```cpp
namespace jbytecode {

namespace classfile {
    class ClassFile;
    class ConstantPool;
    class OpaqueAttribute;
}

namespace ir {
    class ClassUnit;
    class MethodUnit;
    class CodeUnit;
    class CodeEditor;
}

namespace codec {
    class OptimisticReader;
    class DefensiveReader;
    class OptimisticWriter;
    class DefensiveWriter;
}

namespace session {
    class ClassSession;
}

namespace bridge {
    Result<session::ClassSession, PromoteError>
    promote(std::unique_ptr<classfile::ClassFile>, const PromoteOptions&);
    Result<void, LowerError>
    lower(session::ClassSession&, const LowerOptions&);
}

namespace analysis {
    class FrameEngine;
    class ITypeResolver;
}

namespace scan {
    class SummaryScanner;
    class ReferenceScanner;
}

} // namespace jbytecode
```

facade 可以根据用户选择装配 optimistic 或 defensive strategy，但 facade 不应该重新引入 visitor、tree、所有权和写出流程的大类。若迁移期需要旧 visitor 适配器，它必须位于单独的 `legacy` 目标中，不能被新模型或新算法依赖。

## 13. 三种生命周期

### 13.1 单类读写

```text
bytes
  -> OptimisticReader/DefensiveReader
  -> unique_ptr<ClassFile>
  -> writer
  -> output bytes
  -> ClassFile 析构，所有低层资源一次释放
```

### 13.2 单类变换

```text
bytes
  -> reader
  -> ClassSession(backing + IR + BridgeState)
  -> pass 只通过 ClassUnit/CodeEditor 修改
  -> lower(session)
  -> selected writer
  -> stage output
  -> session 析构
```

### 13.3 全局索引与跨类事务

```text
ResourceContainer
  -> scanner/index (只保存值和引用边)
  -> modification plan
  -> N 次独立 ClassSession
  -> stage all
  -> commit or rollback
```

以上三个生命周期故意没有共同的 global arena。共享的是值级索引和抽象 source，不是节点 owner。

## 14. 必须满足的不变量

### 模型不变量

- `ClassFile` 和 `ClassUnit` 都可独立构造、移动和析构。
- 一个对象的所有子对象由该对象唯一拥有；公共 API 不产生调用方必须 `delete` 的节点。
- CP index、pc offset 和 owner-scoped handle 不得跨模型或跨 class 使用。
- 高层 branch/switch/try/debug 引用必须经过 owner 校验；不允许跨 `CodeUnit` 的 target。
- 高层字符串和常量是拥有型或封闭值，不能借用 reader 临时 arena。

### codec 不变量

- 两种 reader 产出同一个低层 schema，不产出互不兼容的树。
- defensive reader 对无法解释的区段保存 raw/opaque 或返回明确错误。
- unknown attribute 的 CP/pc relocation 能力必须记录；未知依赖不能静默重排。
- 未修改输入在开启 preserve 选项时可以 byte-for-byte roundtrip。
- writer 的帧策略显式可见，不能隐式调用一个不可替换的 core 帧实现。

### 业务边界不变量

- `libjbytecode` 不拥有 Jar、JDK、`ResourceContainer`、全局索引或事务。
- `mbase` 索引不保存完整 class 模型和内部句柄。
- 跨类引用修改通过 index 生成计划，再逐类打开和提交；不依赖节点跨 class 逃逸。

## 15. 旧形状的明确拒绝

- `ClassReader -> ClassVisitor -> ClassNode` 作为公共主路径；
- 用 reader arena 的生命周期支撑节点字段；
- `Label`/`LabelNode` 双重身份和 `FLAG_TAKEN` 手工接管；
- `InsnList::remove()` 摘链后要求调用方另行 `delete`；
- `ca::core::Any` 作为 LDC、帧槽或 bootstrap 参数的无边界堆盒；
- 一个 global `Context` 持有整个 Jar、rt.jar 和所有高层节点；
- 把 `ResourceContainer` 直接放进低层库；
- 用预加载所有依赖类的 `TypeWorld` 代替可注入、可 lazy 的 resolver；
- unknown attribute 只存 bytes，不记录 CP/pc 影响；
- 以“重新走一遍 visitor”代替 classfile 到 IR 的显式 bridge；
- 任何形式的回调推送协议（`std::function`/lambda sink、`visit*()` 事件先行、`forEach*` 把数据推给消费者）——推形态是 visitor 换皮，已改拉形态（§26）；
- 同时维护 core Frame 和 analysis Frame 两套不一致的帧语义。

## 16. 实施顺序和验收

### 16.1 建造顺序

1. **低层模型**：在 `libjbytecode` 内建立 `classfile::ClassFile`，吸收 `libjclassfile` 的 CP、属性、所有权、offset 和 source span 表达；先完成 `ClassFile` 的独立 RAII 与 known/opaque attribute schema。
2. **乐观 codec**：把现有 `libjbytecode core` 的合法 class 读取/写出搬到 `OptimisticReader/Writer`，直接填充和消费低层模型，不公开 visitor。
3. **防御式 codec**：把 cafedude/libjclassfile 的边界检查、异常结构表达和 roundtrip 能力收敛为 `DefensiveReader/Writer`；补齐 relocation contract。
4. **高层 IR**：在独立的 `libjbc` target 建立 `ClassUnit`、`SymbolPool`、`ConstValue`、基本块、owner-scoped handles、容器和 `CodeEditor/UseChain`。
5. **ClassSession/bridge**：实现 `Strict/Opaque` 两种 promote 取态（「不提升」由调用方直接持有低层模型表达），再实现 lower 的 CP、pc、属性和 opaque 影响检查。
6. **单帧引擎**：把 frame transfer/merge 提升为可注入 `FrameEngine`，让写出和 analysis 使用同一实现；用旧 ASM frame 与 analyzer 做差分 oracle。
7. **扫描和 mbase adapter**：实现 summary/reference scanner、`ResourceContainer` source、`ClassIndex` 和 lazy resolver；不让这些反向依赖模型 owner。
8. **产品线迁移**：至少用一个 `mjt-obf` 变换和一个 mbase 全局引用分析跑通人体工学验收，再安排一次性机械迁移；旧库在此之前只做对拍 oracle。

### 16.2 每步的硬验收

- 低层：合法、未知属性、截断/错误 CP、异常表边界和 hostile 样本均有明确 `ParseState`；未修改 defensive roundtrip 可验证。
- 高层：不使用 intrusive `LabelNode`、`Any`、裸 owner delete；边界使用可校验的 `LabelId`，跨 owner handle 测试必须失败；erase/use chain 测试覆盖 branch/switch/try/debug。
- bridge：已知结构 roundtrip；opaque 受影响时必须报错而非静默损坏；结构损坏输入 Err 不产出模型，`Recovered` 降级事实可查询且写回仍可解析。
- 帧：strict/best-effort/preserve 三种策略有明确结果；resolver 可替换且不要求全量加载依赖类。
- 规模：扫描几万个 class 时临时模型按类释放；索引只增长到唯一摘要/引用数据，不增长为完整节点树。
- 对拍：旧 `libjbytecode` 保留为输出和帧行为 oracle，新库不通过兼容 shim 提前绑定旧 API。

## 17. 仍需在实现前冻结的细节

这些是实现选择，不再改变上述拓扑：

1. `ClassFile` 内部 storage 选 `vector<unique_ptr>`、chunked slots 还是值数组；public identity 不得因此改变。
2. opaque attribute 的 relocation handler 覆盖范围，以及哪些属性进入 typed schema；没有 handler 的保守失败规则不能放宽。
3. `ClassSession` 是让 `lower` 原地更新 backing，还是产生新 `ClassFile` 后交换；两者都必须保持 backing/bridge 状态的一致性。
4. 高层 debug range 的 boundary handle 具体命名；它必须与 branch target 一样做 owner/generation 校验。
5. optimistic writer 的默认 frame policy；无论默认值是什么，都必须允许业务注入自己的 `FrameTable` 或 `FrameEngine`。

这些细节可以由原型和基准决定，但不能再借此把 Workspace、visitor 或第二套所有权制度重新塞回库核心。

---

## 18. v0.1 落地记录与由此修订的条款（2026-08-28）

### 18.1 已实现范围

`libs/libjbc` 的 core 层：单类 lazy classfile DOM。

| 组件 | 文件 | 职责 |
|---|---|---|
| 字节底座 | `core/bytes.hpp` | `ByteSpan`（lazy 载体）/ `ByteView` / `Cursor`（全边界检查）/ `ByteSink` |
| 常量池 | `core/constant_pool.*` | 零拷贝 UTF8 视图、宽条目双槽、只追加、原样 span 写回 |
| 属性模型 | `core/attribute.hpp` | `AttrSlot` 四态、`AttrBody` 层级（`Raw`/`Code`）、`mutate()` 唯一修改入口 |
| 属性编解码 | `core/attr_codec.*` | `readAttrSlots`（目录 eager / 内容 skip）、Code 解码、属性写出 |
| 模型 | `core/class_file.*` | `ClassFile` 唯一所有者、lazy 解码入口 |
| 读写 | `core/reader.*` `core/writer.*` | 骨架解析、保真写回 |

未实现（v0.2 起）：指令解码、高层 IR、帧引擎、bridge、scan/summary。

验收：35 个用例全绿，含 8 个真实 class（44KB 级，含 StackMapTable / 注解 / 异常表 / 未知属性）的往返字节一致，且「全解码后写回」同样字节一致。

### 18.2 三条实现暴露的硬约束

纸面设计时未预料到，实现中被测试抓出——它们约束后续所有设计。

**C1｜嵌套 span 必须是绝对偏移。**

`readAttrSlots` 在解析 Code 内嵌套属性时，游标基于 Code 内容的子视图，`cur.pos()` 是相对偏移；而写回时按 span 直接到源字节流取字节。二者混用会**静默取到错误的字节**（不是崩溃，是错数据）。故 span 一律为源流绝对偏移，嵌套解析须传入基址。

**C2｜零拷贝的视图必须落在模型自己的存储上。**

最初实现基于调用方入参解析，UTF8 视图因此指向调用方的 buffer——调用方一释放即悬垂，而模型还宣称自己拥有数据。修法：**先复制源字节，再基于副本解析**，于是所有视图都落在模型内部。（`preserveSourceBytes=false` 时退化为借用入参，已在 `ReadOptions` 显式记录生命周期代价。）

**C3｜「看过」不等于「改过」，必须在类型上可分。**

只读走 `peek()`（不置脏），修改只有 `mutate()` 一个入口且顺手置脏。这使「解码全部属性后写回」仍然字节一致，也使旧库「改了但忘记标记 → 改动被静默丢弃」这一类 bug 在结构上写不出来。

### 18.3 对既有条款的修订

| 条款 | 原设计 | 修订 | 理由 |
|---|---|---|---|
| §6.1 promote 粒度 | `promote(ClassFile) → ClassSession`（**类级全量**） | **方法级**：只有被改的方法进 IR | 类级全量会为改一个方法而解开整类所有 Code，lazy 白做 |
| §4.1 CP 索引 | 未定 | **只追加，永不重编号** | 重排会使未建模属性内嵌的 CP 引用全部失效；只追加则它们天然保持有效 |
| §4.2 RelocationContract | 需为未知属性记录 CP/pc/长度依赖三张表 | **大幅简化**：未触碰即 copy 字节，无需推断影响；只需守住「CP 只追加」 | 保真由 copy-through 提供，不依赖对未知属性的语义猜测 |

保留不变：§4.2 中「没有 handler 就拒绝写回」的保守失败原则——它落到 `WriteError::NotModeled` / `Unencodable`，宁可写失败也不产出半成品 class。

### 18.4 v0.2 起点

指令解码与 IR 提升按 §16.1 第 4–5 步推进，但 promote 入口按 §18.3 设为方法级。在此之前，core 层可独立用于 **summary 扫描**：解析只做 L0/L1，一个 Code 都不解，代价与方法体规模无关。

---

## 19. 资源管理：一个死亡点，三个去重域

### 19.1 唯一死亡点

libjbc 的内存模型只有一条规则：**一个 `ClassFile` 拥有一切，析构一次全部回收。**

没有第二个 death point，没有引用计数，没有跨对象共享的所有权，也没有需要调用方 `delete` 的裸指针。这条性质使「忘记释放」在结构上不可表达——旧库六套所有权制度（structural-problems.md M1）试图用纪律达成的，正是这一条。

### 19.2 资源归属总表

| 资源 | 持有者 | 生命周期 | 释放 |
|---|---|---|---|
| 源字节副本 | `ClassFile::source_` | = ClassFile | vector 析构 |
| 解析来的 Utf8 | 视图，指向 `source_` | = ClassFile | 无（不拥有） |
| 新增的 Utf8 | `ConstantPool::arena_` | = ClassFile | arena 整批释放 |
| CP 条目 | `ConstantPool::entries_` | = ClassFile | vector 析构 |
| 属性体（`Raw` / `Code`） | `AttrSlot::body`（unique_ptr） | = 属性槽 | unique_ptr |
| `RawAttr` 字节副本 | `RawAttr::bytes_` | = 属性体 | vector 析构 |
| `CodeAttr` 的 code / 异常表 | 该属性体的成员 | = 属性体 | vector 析构 |
| Code 内嵌套属性槽 | `CodeAttr::attrs` | = 属性体 | vector 析构 |

### 19.3 字符串：两条存储路径，各自匹配一种生命周期

```text
解析来的 Utf8（占绝大多数） → 零拷贝视图，指向模型的源字节副本
变换中新增的 Utf8          → intern 进 Utf8StringArena（去重 + 块分配）
```

**不把所有 Utf8 都 intern 一遍**的原因：classfile 的常量池**本身就是一张去重表**，再在 arena 里去重是冗余的；而解析期复制会毁掉「扫描不付费」的性质（§16.2 的规模验收项）。arena 只承担它真正擅长的那一段。两条路径的数据都随 `ClassFile` 一起死，单一死亡点不受影响。

### 19.4 三个去重域与 libca 设施的对应

不同域的寿命差异决定用哪种设施，不能一刀切：

| 域 | owner | 设施 | 寿命 | 选型理由 |
|---|---|---|---|---|
| classfile CP | 一个 `ClassFile` | 零拷贝 span + `Utf8StringArena` | 单类 | 整批共存、一起销毁——arena 的定义场景 |
| IR `SymbolPool` | 一个 `ClassUnit` | `Utf8StringArena` | 单类 / session | 同上 |
| mbase `ClassIndex` | 一个索引实例 | `Utf8StringPool`（引用计数句柄） | 索引 / 事务 | 条目寿命各异——pool 的定义场景 |

libca 自己的选型注释即是这个判据：「一批字符串有共同死亡点（整批共存、一起销毁）用 arena；寿命各异用 `Utf8StringPool`」。

### 19.5 为什么没有全局 arena

「一个全局 Context 持有整个 Jar / rt.jar + 所有字符串」的形状在 §15 已被拒绝，它在三个地方失败：

1. **扫描几万个类时内存单调增长**，且无法在单个类处理完后归还；
2. **寿命绑定错误**——扫完就该扔的类的字符串，与索引要长期持有的签名混在同一个池里；
3. **与 lazy 冲突**：lazy 的前提是「随时可以只解一半」，而全局池要求所有字符串在解析时就落地。

批量处理由 mbase 的调度与索引负责（§8.2），不是让库自己膨胀出一个 global owner。

### 19.6 结构性防线：能断言的就不靠注释

| 旧库靠注释 / 纪律守住的事 | libjbc 的对应机制 |
|---|---|
| 「节点字符串借自 reader 的 arena，reader 必须活得更久」 | 视图指向 `ClassFile` 自己的存储；`arenaOwnsAllAppended()` 可断言 |
| 「remove 后记得 delete」 | 不存在单个对象的 delete；`mutate()` 是唯一修改入口且顺手置脏 |
| 「CP 重排后记得修引用」 | CP 只追加，索引永不改变（§18.3） |
| 「未知属性的 CP/pc 依赖别被破坏」 | 未触碰即 copy 源 span，不经过编码器 |
| 「改了要置 modified 位」 | 无此位；状态由 `mutate()` 推进，不改则不生效（可测） |

每一条都有一个**可被测试断言**的抓手，而不是一段要人记住的文字。

---

## 20. 验证工具与实测（2026-08-28）

### 20.1 三套工具，各管一段

| 工具 | 查什么 | 怎么开 |
|---|---|---|
| AddressSanitizer | 越界、use-after-free、double-free | `xmake f --with_asan=y`（**全局**开关） |
| ASan 自检 | 证明插桩真的生效，排除「假绿」 | `JBC_ASAN_SELFTEST=1 xmake run libjbc_leak_check` |
| CRT 调试堆 | 泄漏 | `xmake f -m debug` + `xmake run libjbc_leak_check` |

三条踩过的坑，都写进构建配置了：

1. **MSVC 的 ASan 不做泄漏检测**（Linux 的 LSan 才做），所以泄漏另走 CRT 调试堆；
2. **ASan 必须在根作用域开启**：`/fsanitize=address` 改变 STL ABI（`annotate_vector` 等注解），只给部分 target 插桩会链接期报 LNK2038；
3. **ASan 构建不能链 gtest**：gtest 走 xrepo 预编译、不参与插桩。故 ASan 只用于 `demo/` 下的轻量程序（仅依赖 libjbc + libca）。

### 20.2 结果

- **ASan**：262 个真实 class 的「读 → 全解码 → 写回」全程零报告；自检确认插桩生效（故意越界能被抓到并给出完整调用栈）。
- **分配平衡**：`demo/leak_check.cpp` 接管全局 `operator new/delete` 统计存活分配数。524 次解析（262 类 × 2 轮）、累计 109,636 次分配，跑完存活数**264 → 264**，完全平衡。release 下即可运行，不依赖 debug 构建。
- **静态**：`src/` 下零处裸 `new` / `delete` / `malloc` / `free`，9 处 `unique_ptr`，全部资源经 RAII 容器或 unique_ptr 持有。
- **CRT 调试堆**：程序已就位（`xmake f -m debug` + `libjbc_leak_check`），但 debug 全链重编在本机超时（>30 分钟），本次未取到数据——上述三条已覆盖同一结论的三个角度。
- **扫描对拍**（262 个 ares 类，只要类名与成员签名，不解方法体）：

| 配置 | 耗时 | 内存增长 | 相对 |
|---|---|---|---|
| libjbytecode 全量 `ClassNode` | 74.6 ms | 1.2 MB | 基线 |
| libjbc（保留源字节，可写回） | 20.0 ms | 0.1 MB | **3.7x 快，9.4x 省** |
| libjbc（不保留源字节，纯扫描） | 18.8 ms | 0.1 MB | **4.0x 快，12.3x 省** |

样本偏小（262 个类、均为中等大小的业务类），绝对值仅供参考；量级差是稳定的。

### 20.3 对拍顺带暴露的旧库问题

都不是本次要修的东西，但都是 R0/R1 的活样本，值得留在案：

1. **4 字节输入让 libjbytecode 栈缓冲区溢出崩溃**（`0xC0000409`，`__security_check_cookie` 失败）。样本就在本仓库：`build/artifacts/.../mjdbg-cli/class-data-output.class`（只有 magic 四个字节）。libjbc 对同一输入返回 `Err(Truncated)`（已固化为单元测试 `ReaderTest.FourByteInputFailsCleanly`）。
2. **`LabelFactory` 必须手动注入**（变体 A 上的观察；该变体已存档于 `archive/libjbytecode-variant-a`，不在本线）：变体 A 的 `TreeContext` 实现了 `LabelFactory`，忘了调 `ClassReader::setLabelFactory` 的后果是解析期栈溢出**崩溃**——不是编译错误、不是可捕获的错误码，是进程直接死。
3. **`libjbytecode_util` 曾在本线编译不过**（变体 A 改 `ClassNode` 需 `TreeContext` 后 `util/check_adapters.cpp` 未同步）。变体 A WIP 剔除后旧库恢复冻结基线；对拍 demo 本身只用 tree + core。

第 2 条尤其值得记住：它精确演示了「所有权/生命周期靠调用方记得」的代价——漏掉一个非类型约束的调用，得到的不是编译错误，而是运行期栈溢出。

## 21. v0.2 summary 扫描实测（2026-08-30）

**基线修正**：§20.2 的对拍以 libjbytecode 全量物化为基线，但 mbase 扫描现状不是全量物化——`workspace.cpp::decodeClassSummary` 用 `SKIP_CODE|SKIP_DEBUG|SKIP_FRAMES` 跳过方法体。lazy 的真实对手是它。

**方法**：`libjbc_scan_demo` 增设 `mbase-summary` 相（镜像 decodeClassSummary：每类新 arena + SKIP 解析 + 计数 visitor + 类名/父名物化 std::string）；输入先全部装载进内存（jar 经 libjzip 解包，与 mbase JarReader 同路），计时只覆盖解析。四相 classes/methods 计数交叉一致，零解析失败。

| 语料（jar） | 类数 | mbase 现状 | libjbc 保留源 | libjbc 纯扫描 | 相对现状 |
|---|---|---|---|---|---|
| ares2（混淆产物） | 4536 | 111.3 ms | 32.0 ms | 28.1 ms | 3.5x / 4.0x |
| cfr-0.152（反编译器） | 1302 | 32.7 ms | 7.6 ms | 6.6 ms | 4.3x / 5.0x |
| jdk8-api-opt（API 面） | 777 | 14.1 ms | 2.7 ms | 2.5 ms | 5.2x / 5.6x |
| vineflower-1.12（反编译器） | 432 | 18.6 ms | 4.6 ms | 3.8 ms | 4.1x / 4.8x |

**结论**：

1. 对 mbase 真实现状（SKIP 路径），libjbc 稳定快 **3.5–5.6x**。全量物化比 SKIP 路径本身慢 4–7x，拿它当扫描基线会高估迁移收益——§20.2 的 3.7–4.0x 是对错误对手的倍率，保留作方法学演进记录。
2. 内存：输入预装载、逐类释放的扫描形态下，两侧 RSS 增长同为亚 MB 量级（分配器即取即还），差异淹没在测量粒度里；§20.2 的 9–12x 内存倍率不随新方法学复现，扫描形态下时间才是稳定信号。lazy 的内存主张改挂「模型驻留再遍历」的场景，不放扫描上。
3. 稳健性：ares2（4536 个混淆类）零解析失败，与旧库计数逐类一致。
4. dev_plan v0.2 判据兑现：lazy 价值主张成立，继续推进指令解码。

## 22. v0.3 落地记录（2026-08-30）

交付：方法级 promote/lower + relocation contract（LNT/SMT/LVT）、CodeEditor（erase 双策略 + 基本块视图）、best-effort 帧引擎。提交 bacf8c529 / 8812cf161。

### 22.1 真实数据抓出的建模缺陷（合成测试全部测不到）

帧引擎首版在真实语料上的可算率只有 5%，逐层修复的过程本身就是建模清单：

1. **整数算术全家漏效应**（iadd 等 11 个 op 落 default:break 无操作）。
2. **L...; 类类型槽位返回 -1**——typeSlots 只认 `[` 不认 `L`，所有对象类型的字段/方法引用解析全灭。
3. **紧凑 load/store 是 Shape::None 但语义是 Local1**——操作数布局（解码视角：单字节无操作数）与语义族（帧视角：局部变量访问）是两个坐标系。aload_N 全部变成无操作，全语料 underflow。
4. **dup 家族必须按槽位置换实现**——cat2 的 dup2_x1（long 字段 ++ 惯用法）在值式弹压下多算一槽。规范对宽度组合的分列措辞在槽位语义下统一。
5. **popN 按槽不按值**——栈按槽存储（cat2 = Top 底槽 + 值槽），按值弹 n 次会弹掉 2n 个槽。
6. **异常处理器入口必须播种** {stack=[throwable]} 且截断落入入口的顺序伪边——否则 catch 首条 astore 必然下溢。
7. **格的单调性**：未知（Top）与冲突（Bottom）必须是两个格元素。混用时 meet(Top,X)=X 会把冲突升回具体类型，带环方法永久 ping-pong（真实现场：173 条指令的方法 inStack 涨到 331）。
8. **工作列表必须做 fixpoint 检查**（合并副本与原状态相同则不入队），并配访问熔断（NoFixpoint 报告而非卡死）。

修复后：2615 方法 98.2% 可算，可算者 maxStack 与声明 **100% 一致**（stackEq=2567/2567）。剩余 1.8% underflow 为 jsr/ret 与混淆边缘形态，best-effort 边界。

### 22.2 relocation contract 的实战裁决

消费者 demo（入口插桩）首跑被 contract 拒绝 97%：全调试信息语料的 Code 嵌套 LocalVariableTable 未建模。裁决：**补 LVT 建模与重定向**，而不是放宽契约——入口插桩是最普通的变换任务，拒绝它等于不可用。LVT 重定向语义：startPc 精确命中 pcmap；区间端点 start+length 允许为方法末尾（非指令 pc），miss 钳到新 code 长度。补口后 2603/2603 全通过。

教训（与 §18 的 SMT u2 教训同型）：**契约的拒绝路径只有拿真实语料跑消费者任务才能暴露是「正确防御」还是「能力缺口」**。

### 22.3 promote 语义的一个显式化

promoteMethod 是**重建**不是缓存：lower 之后再 promote，返回的是从已写回 backing 重新解码的新 CodeUnit（句柄域全部更换）。消费者若拿编辑后的 session 再 promote 当「编辑前基准」会错（demo 验证逻辑第一版即此处出错）。已在 units.hpp 文档化，人体工学记录见 ergonomics.md F6。

### 22.4 指令操作数语义层（v0.3.5，用户评审补口）

v0.3 评审指出：指令 CP 操作数仍是 backing 裸索引，消费者插一条 invokestatic 要手工拼四连 addXxx、读一条 getfield 要手工走两跳——CP 知识泄漏给每个消费者。补语义层（ir/operand.cpp + sem_index.hpp）：

- **读路径**：`memberOf/classOf/constOf` 把 CP 索引一步解析成语义视图（owner/name/desc、类名、常量值）；invokestatic 指 InterfaceMethodref 的混淆器交叉形态（真实语料 76 处）两种 tag 都收。
- **写路径**：`internClass/String/Integer/FieldRef/MethodRef/InterfaceMethodRef` 以内容为键去重——命中既有池返回原索引（不膨胀），miss 才沿 CP 只追加契约分配新链。SemIndex 惰性建表，追加同步登记。
- **域宽护栏**：LDC 操作数域 u1，索引 >255 曾被编码器静默截断——现拒绝编码（Unencodable），`CodeEditor::loadConst` 按索引自动选 LDC/LDC_W。

实测：ares 全语料 CP 指令 37398 条语义解析 unresolved=0；带 CP 常量的真实插桩（ldc marker + pop）2603/2603 通过，重读语义一致。

## 23. 资源管理核销对照（v0.3.6 全量对账，2026-08-30）

问题：「彻底解决资源管理问题了吗」——按 ownership-model.md 立约三原则（结构性 / 类型可见 / grep 可查）逐条对账，全部带机械证据。

### 23.1 R0/R1/R3 → libjbc 机制核销表

| 旧栈制度（ownership-model.md §0） | libjbc 结构性替代 | 证据 |
|---|---|---|
| InsnList 裸指针拥有 + remove 摘链不删 + 12 处 remove/delete 配对纪律 | 指令是**值语义槽**（CodeDom 的 slot 表），erase 即移除，无堆节点可泄漏 | grep：src/ 裸 new/delete/malloc/free = **0**；insn_test 删除句柄失效用例 |
| Label 池（FLAG_TAKEN/回调接管/O(n) 释放）+ Label 双重身份 | **不使用 intrusive LabelNode**：高层边界是 owner-scoped `LabelId`，低层 classfile 仍以 pc 表示 | erase 被引用指令 → `EditError::StillUsed`；generation 防 ABA；边界迁移后统一重算 pc |
| 交叉引用靠「谁 new 归谁」注释 | 唯一拥有链：ClassFile→CP/属性体/源字节副本；ClassSession→backing+IR；析构一次回收 | 头文件裸指针成员 = **0**（全值/unique_ptr/vector）；move-only |
| Utf8StringRef 逃逸靠人记得 re-intern（R3） | 存入模型的字符串必经 intern 进 arena（addUtf8/intern_raw/SymbolPool），arenaOwnsAllAppended() 把归属变成可断言事实 | constant_pool_test 归属断言用例 |
| visitor 事件物化节点（R1） | DOM-first：模型即拥有者，无 visitor 无事件顺序 | 架构本身 |
| reader/writer/analyzer 借用 outlive 靠注释（六制之⑤⑥） | 借用仍在但**类型可见**：FrameEngine(cp&, resolver&)、CodeEditor(CodeUnit&)、promote→CodeUnit*（非拥有，session 持有）——签名即文档 | 头文件签名 |

### 23.2 运行时验证（2026-08-30 重跑，覆盖 v0.3–v0.3.6 全部新路径）

- **泄漏**：leak_check 扩展负载（read→session→promote→insertAtStart/setMember/setString→erase 双策略→FrameEngine→lower→write→重读），ares 262 类 × 2 轮：**5,079,265 次分配，前后存活数一致（264=264），零泄漏**。
- **UAF/越界**：同负载 ASan 全程 **零报告**；JBC_ASAN_SELFTEST 故意越界被当场捕获（红区报告 + abort），排除假绿。
- **静态**：src/ 裸 new/delete/malloc/free = 0；公共头零裸指针成员；返回裸指针的 API 全部是非拥有视图（peek/mutate/find*/promote 的产物），析构即回收，无调用方 delete 义务。

### 23.3 没有结构性解决的（如实列出，全在借用语义下）

1. **string_view 借用**：memberOf/classOf/constOf/symbols().str 返回的视图寿命 = 会话寿命。消费者把视图存到模型外并延长其寿命 → 悬垂。这是 C++ string_view 的标准语义（与旧栈 R3 的差别：旧栈是**模型自己**把借用视图存成字段，libjbc 的模型内部零借用字段，借用只发生在消费者手里）。
2. **引用型参数**：FrameEngine/CodeEditor 持引用，调用期间需存活——签名可见，属类型可见的借用，不靠注释。
3. **SemIndex 的 string_view key**：指向 CP 内部字节（源副本/arena，均不再移动），随池存活——结构性安全，但依赖「arena 地址稳定」这条 libca 契约。

结论：**旧栈六套靠纪律的所有权制度在 libjbc 中全部消失，替代品是结构性的且经机械验证**；残余的借用义务全部类型可见、且从未进入模型内部。

## 24. 寿命接管层（v0.3.7，2026-08-30）

§23.3 如实列出的借用义务里，「string_view 视图寿命 = 会话」与「引用型参数」两条的本质是：**消费者被迫替库管理寿命**。接管层把这两条变成库的职责——「句柄携带保活」三件套：

1. **共享根**：`makeSession` 返回 `shared_ptr<ClassSession>`（`enable_shared_from_this`）。共享的只是**寿命**，不是可变性——并发编辑依旧不被允许（单线程顺序模型），所有权纪律不降级：依然零 delete 义务，且寿命可断言（weak_ptr 观测归零）。
2. **保活句柄**：`promoteMethod` 返回 `CodeUnitRef`（会话保活 + CodeUnit*，operator-> 与裸指针同形，调用点几乎零改动）；语义查询新增保活孪生 `memberSemRef / constSemRef / classSemRef`（快照 + 保活，拷到哪里都安全）。**视图版（memberOf 等）原样保留**——扫描热路径零开销不受引用计数影响，借用从「默认义务」降级为「显式热路径选择」。
3. **默认 resolver**：`FrameEngine(cp)` 一步构造（内置 ShapeResolver），注入式构造保留——少一个调用方必须保活的引用参数。

**代价（如实）**：每个保活句柄一次原子 incref（每方法一次 promote ≈ 每类几十次，相对解析成本为噪声）；「一个死亡点」叙事从「析构即回收」变为「最后引用归零」——§19 的纪律性不变（结构保证 + 可断言），粒度更细。

**验证**：lifetime_test 三用例——保活句柄在会话变量销毁后视图/指针全部安全（weak_ptr 未过期）；句柄全部销毁后会话精确归零（weak_ptr 过期，接管不欠寿命也不漏引用）；跨作用域编辑 + 默认 resolver 帧计算。全量 84 用例 83 绿 + 1 预期跳过；use_case/obf_passes 结果不变；leak_check（负载含保活路径）5M 分配平衡零泄漏。

## 25. analysis 模块对标（2026-08-30，用户问「有对标 analysis 的设计吗」）

旧栈 `libjbytecode/analysis`（ASM tree.analysis 移植，mbase 七个真实消费者）的面与 libjbc 现状对照：

| 旧栈面 | libjbc | 状态 |
|---|---|---|
| `Analyzer::analyze → vector<Frame<V>*>` 逐指令帧 | `FrameEngine::compute(u, vector<FrameAt>&)` | **已落地（§25.1）** |
| 结果寿命：Analyzer 必须活得比结果久（六制之⑥） | FrameAt 值语义自持有 | **顺带消灭** |
| `Interpreter<V>` 值域抽象 + Basic/Simple/Source 变体 | `IValueDomain<V>` 五必选钩子 + 细分默认（§25.2） | **已落地（v0.3.8）** |
| subroutine（jsr/ret） | RET 降级计数 | 差距收窄：语料 0 失败（见 §25.2 根因修正） |
| SimpleVerifier（类层级校验） | — | 不做：需要跨类 ClassIndex（§14，mbase 域） |

### 25.1 已落地：逐指令到达帧

`FrameAt{reachable, locals, stack}` 与 `order()` 下标对齐；失败时清空以 summary 为准。验证：合成用例覆盖合并点类型（Null×Ref→Ref）与死代码可达性；真实语料 2615 方法帧深全部 ≤ 声明 maxStack（unreachableInsns=0——ares 语料流程完备，死代码判定在合成用例验证）。

### 25.2 已落地（v0.3.8）：值域搭车，不重写 transfer

旧 `Interpreter<V>` 的代价：换个值域（来源追踪/常量/别名）要继承并重写**整张 per-opcode transfer 表**（BasicInterpreter 612 行起跳）——mbase 的 destination/jump_predicting/source 三个 interpreter 各自如此。这正是 R2「帧引擎封闭」的镜像：core 帧封闭，analysis 帧开放到要人肉搬运营。

libjbc 方案（`analysis/domain.hpp`）：**引擎持有全部形状迁移知识**（控制流 worklist、dup 槽位置换/pop 槽计数/cat2 两槽、get-put-invoke 描述符效应——P0 修的那一整链），值域 V 作为载荷随槽移动：

```text
消费者实现 IValueDomain<V>（必选五钩子）：
  top / merge / newValue(producer, InsnId, shape) / newParameter / newException
可选细分（默认退回 newValue 或恒等——不写就是纯搭车）：
  binary / unary（算术折叠）/ loadLocal / storeLocal（store→load 穿透）
  / width（cat2 槽位数）/ equal（fixpoint 判定）
引擎（DomainEngine<V>::analyze）负责：worklist、槽迁移（V 跟槽走）、
  合并调度、异常播种、熔断、逐指令帧交付（FrameV<V>，与 order() 对齐）
入参播种：analyze(u, MethodSig{isStatic, desc}) 重载；analyze(u) 不播种
  （FrameEngine 等价形态）。
```

实现载体：`SlotV<V>{val, bottom}` 行结构（cat2 底槽随行整体移动）；`ShapeDomain`（VType 内置域，merge 转发 ShapeResolver——单一定义）与 `ResolverDomain`（旧 ITypeResolver 桥接，注入构造兼容）。

**FrameEngine 降级为壳**：worklist/效应/合并全部走 DomainEngine<VType>，87 个既有用例一字未改跑绿 = 等价性证明。

**根因修正（顺带）**：旧实现的 LSHL/LSHR/LUSHR 弹 4 槽（与 LADD 同组），实际语义是 long(2)+int(1)=3 槽——此前 oracle 98.2% 的 47 个失败方法全部由此而来（此前 §25 表格归因 jsr/ret 是错的）。模板化时按真语义重写为 binaryOp(2,1)，oracle 升至 **2615/2615 = 100%**，maxStack 仍与 javap 100% 一致。RET 降级现在只影响 degraded 计数，语料无失败。

**换域成本实证**（domain_test.cpp）：OriginDomain（来源追踪，store→load 穿透 + 汇合退化）约 40 行；ConstDomain（int 常量折叠：iconst/bipush/iadd/imul/ineg + 局部穿透）约 60 行——对比旧栈每个 interpreter 612 行 transfer 表重写。非单调域（merge 必变）触发 NoFixpoint 熔断兜底亦有用例。**不做**整层 virtual Interpreter 复刻——那会重新引进旧栈的厚度。

### 25.3 边界

SimpleVerifier 依赖跨类类型层级，属 mbase ClassIndex 域（§14），libjbc 保持单类职责；subroutine 已随 C2 落地为 Strict 域数据流建模（§10.6，单调用点精确/多调用点显式拒绝），不再只是 degraded 计数。

## 26. visitor 清零落地（2026-09-27）

**决策**：在 libjbc 上彻底消灭 visitor——判据不是类名而是数据形态：数据拉回来的是模型，事件推给消费者的是换皮 visitor（`std::function` 回调 sink、`visit*()` 事件先行、`forEach*` 推送，三形状任一即中）。

**落地**（与本节同日生效）：

1. §1.4-4 由「visitor 只能是 codec 内部实现细节或迁移期隔离的 adapter」收紧为绝对禁止——libjbc src 实测 0 处 visitor，豁免已无存在理由，删除以防日后被援引。
2. §15 增补「回调推送协议」拒绝条目，机械对应门禁的三个模式。
3. 唯一存留的推形态 `forEachCodeUnit(session, fn)`（ergonomics F1 的回调迭代器，仅测试引用）改为值返回 `collectCodeUnits(session)`：每方法恰一条 `CodeVisit`（NoCode/Failed/Ok 三态互斥不变），已 promote 的 CodeUnit 仍复用不重建，F1 防静默吞错不回退——调用方拉回序列后仍须显式分支才能静默跳过。旧名只留在 ergonomics.md / dev_plan.md 的历史记录里。
4. 门禁单测 `test/unittest/no_visitor_gate_test.cpp`：掩蔽注释与字符串（保留换行，行号不漂移）后对 `src/` 全量正则扫描——`(class|struct) \w*Visitor`、`\bvisit\w*\s*\(`、`std::function`——命中即 fail 并报 `文件:行号`；找不到 src 根也 fail（门禁缺席必须可见，不 skip）。要放行新形状：先改本节与 §1.4-4，再改测试——门禁与文档同源。

**范围**：仅 `libs/libjbc/src`。libjbytecode 旧库、mbase adapter 与产品线迁移不在本门禁内；双栈过渡与迁移顺序见 `code-dom-vs-visitor.md`。

## 27. 数据保真契约：trailing 同族保留、编码闸门与 CP 结构错误（2026-10-13）

数据保真审计收口的三条契约，全部服务于同一条门禁——「未修改字节级往返 + 合法修改不丢数据」。

### 27.1 trailing 同族：保留 + copy-through，而非 Err

类级尾部与属性体尾部是同一族畸形输入，适用同一条契约：

- **类级 trailing**：类属性表之后 classfile 应当结束；多余字节保留为 `ClassFile::trailing`（字节副本），writer 在输出末尾原样写回。
- **属性体 trailing**：已知属性的声明结构读完之后属性体应结束；多余字节保留为该属性体的 `trailing` 成员（Code、BootstrapMethods、LineNumberTable、LocalVariableTable、StackMapTable、Annotations、Signature、TypeAnnotations 八类同治），编码器在结构输出之后原样追加。

共同语义：合法输入没有 trailing，trailing 非空即畸形输入的**可表达事实**。读取时降级记录 warning，不判 Err（结构与边界都可确定，只是不合规）；未修改走 copy 路径时 trailing 天然无损，`mutate()` 触发重编码时 trailing 原样附加——两条路径都不丢字节。这与 §14「未修改输入 byte-for-byte roundtrip」一致：带尾部垃圾的输入也必须能完整往返，丢弃即违约。

### 27.2 编码闸门统一：超界即 Err，禁止静默截断

一切编码器对落不进目标域宽的值必须显式返回 `Err(WriteError::Unencodable)`，禁止 `static_cast` 截断——截断会产出貌似合法但引用错条目 / 计数错位的 class，违反 §4.2「保真或报错」。覆盖面与既有闸门同型：

- 属性编码器：BootstrapMethods/LNT/LVT/SMT/注解的条目计数、CP 索引、offset 等全部字段，逐字段检查后再写；
- 指令编码器：CP2 族索引超 u16、Imm1/Local1/Iinc/Atype1/count/dims 超 u8/i8、Imm2 超 i16——u1 域放不下时的唯一合法出路是显式拓宽变换（goto→goto_w、LDC→LDC_W、wide 前缀），由编辑器负责选择，编码器只负责拒绝。

### 27.3 CP 结构损坏在 reader 阶段 Err：宽条目越界

Long/Double 条目的第二个槽位越出 `constant_pool_count` 属 CP 结构损坏：无法确定后续条目边界，且宽容写回会使槽位比输入多 1（既不保真也不报错）。契约与其它 CP 结构错误一致——reader 阶段 `Err(BadConstantPoolCount)`，不产出降级模型。降级通道（Recovered + warning）只用于「结构可确定、语义不合规」的形态（如 §27.1 的 trailing），两类不可混用。

### 27.4 invokeinterface 的 count 随描述符重算

`setMember` 改写 invokeinterface 时，count 字段必须按新描述符重算（count = 参数槽位数 + 1 个 this 槽，`jbc::type::paramSlotCount`）；只改 CP 引用不改 count 会产出 JVM 可拒绝的 class。描述符解析失败时不改写指令（与 op 族/kind 不符返回 false 同一纪律）。

## 28. 语料自产机制：无本地私产也能跑全量门禁（2026-10-13）

> 批注（2026-10-13）：本节替代草案中「RealClass 语料挂在本机 ares 路径」的隐式前提；ares 路径降为最后一级回退，语义不变。

最强的一批门禁（>1000 方法帧 oracle、语料级 roundtrip、attr/ir/insn 语料闸门）依赖真实 javac 产物语料。ares 是私有产品代码，绝不入仓；为此建立**仓内自产语料**，让无本地私产的环境（CI、新同事、新机器）也能跑全量：

### 28.1 语料解析序

`resolveCorpus()` 按序三级，取第一个可用者：

1. **env 注入**（`LIBJBC_REAL_CLASS_DIR`）：权威指定，语义不变——设了但目录不存在按「无语料」处理，**不回退**（显式指定失败必须显式暴露）；
2. **仓内自产**（`test/libjbc_corpus/classes`）：源文件在 `mtest_set/test_resource/java/jbc_corpus/`（样本纪律：源入 test_resource，产物属生成物落 `test/`），测试期用 `javac --release 8 -encoding UTF-8` 现场编译；manifest 记录类数，任一 `.java` 比产物新即重编（增量）；
3. **ares 本机路径**：现状默认，私有环境专属回退。

### 28.2 纪律

- **样本纪律**：样本源遵守 mtest_set 纪律（源码入仓、产物不入仓），样本覆盖面与门禁需求对齐——控制流全形态、异常（try/finally/multi-catch/TWR/嵌套）、泛型（含桥方法）、长方法、接口默认方法（含菱形消解）、枚举（常量体 + switch map）、lambda（indy：捕获/方法引用/构造引用/数组引用）、宽指令（wide 前缀）、UTF-8 常量、注解、监视器。
- **规模门禁**：帧 oracle 断言 `methods > 1000`；语料不足即红，防样本退化。
- **skip 语义**：javac/JDK 缺失 → 对应语料测试显式 skip（环境能力问题）；**仓内样本源编译失败 → ADD_FAILURE 硬红**（入仓源是承诺，坏源是缺陷，不允许静默降级成 skip）。
- **双模式等价**：同一套测试在自产语料（1000+ 方法）与 ares 语料（3000+ 方法）下都全绿；ares 专属样本（transformers 族）与自产样本各自成表，按解析到的语料级选择，保证两种环境下 skip 集合一致（仅剩 JDK 缺席类）。

## 29. 畸形输入降级契约：断言面与写回面（2026-10-13）

畸形输入测试的断言纪律：**每个样本的产物必须显式——正确错误码的 `Err`，或带 `ParseState` 标记的 `Recovered`，绝不允许静默成功**。样本全部在测试内字节级拼装（不入 `test_resource/` 二进制），按族组织：

| 族 | 样本形态 | 断言 |
|---|---|---|
| CP 结构损坏 | count=0、自引用、双节点环、成员名非 Utf8、接口非 Class、this_class 越界 | reader `Err(BadConstantPoolCount/BadConstantPoolRef)`，不产出模型 |
| 类级 trailing | 属性表之后尾随垃圾 | `Recovered` + warning（非 error）+ trailing 保留；写回字节一致，再读仍 `Recovered` |
| 属性体截断 | Code/BootstrapMethods/LNT/LVT/SMT/Annotations 半截体 | 骨架 `Validated`，decode `Err(Truncated)`；**未触碰写回字节一致**（copy-through 与解码成败无关） |
| SMT 帧类型保留区 | frame_type 128..246 | decode `Err(UnsupportedFeature)`，不猜语义 |
| 异常表畸形 | handler/start/end 落在指令操作数中间 | 骨架照读（字节事实可表达），promote `Err(BadInstructions)`；`end == code_length` 是合法边界（对照组） |

写回面契约：降级读出的模型未触碰时写回字节一致；降级模型写出必须仍可解析（原样保真），或显式拒绝——两条路之外没有第三条。

## 30. Tier A 小账收口（2026-10-13）

### 30.1 CP 访问面与置脏：`mutateAt` 规范入口

写回语义是**逐条目**的：dirty 条目重编码，未脏条目一律 copy 源 span。审计结论：`at()` 双重载是纯读取透镜，**不置脏**；一切 setter 置脏。由此：

- 经 `at()` 取引用改写、不置脏 → 该条目写回时被源 span 覆盖，改写**静默还原**（文档化陷阱，测试钉死为契约——若有人让 `at()` 自动置脏，对照用例红，倒逼其面对全量重编码的语义变化）；
- 新增 `Entry& mutateAt(i)`：取引用并置脏，与 `AttrSlot::mutate()` 同一对称形态，是**改写既有条目的唯一规范入口**；
- 追加（addUtf8 等）只影响新条目，既有索引稳定（跨 append 持索引、用前重 `at()`，边界卫生收口）。

### 30.2 类级提交：`commitClassMetadata`

方法级有 `lowerMethod → commitMethodMetadata` 自动提交点，类级元数据（version/access/thisClass/superClass/interfaces）此前**没有提交通道**——改高层字段不落 backing，静默不生效。收口为显式入口（无自动点：类级没有 lower 时机）：internClass 去重落位（CP 只追加）、version 按 `(major<<16|minor)` 拆分、super 空名提交为 0；access 超 u16 / this 名为空 / CP 超容量一律 `Err(CpAppendFailed)`，先预检后写入，不留半截追加。

### 30.3 ParseState 两态定形

三态中 `RawOnly` 零生产点、零消费者（DefensiveReader 已随 §5 裁决搁置；「不提升」由调用方持有决策表达），删除。`Recovered` 生产点定形为 trailing 同族（§27.1）：结构可确定、语义不合规 → `Recovered` + warning；结构损坏 → `Err`（§27.3）。降级必须落在 `ParseState` 上可查询，不允许「只有诊断没有状态」的形态。

## 31. 批量删除与按族收集（2026-10-14）

### 31.1 `CodeDom::eraseBatch`：全有或全无的批量删除

循环单删的成本是 O(N×n)（`usersOf` 全序扫描 + `erase` 自身全序扫描各来一遍）；批量 API 一次校验、一趟扫序、单趟重建 `order_`，总计 O(n + |ids| + labels)。契约要点：

- **全有或全无**：任一句柄无效（InvalidHandle）、任一存活指令的 branch/switch 字段锚定批内成员且无改锚安排（StillUsed）、任一批内成员被存活 Boundary 标签锚定（StillUsed）时，什么都不删——失败现场句柄与顺序原样可查。重复句柄按并集处理（同槽只删一次）。
- **批内互指不算引用**：同批消亡的指令之间的引用随批一并消失。这是与逐条 erase 的唯一语义差——逐条版「先删谁谁挡路」，批量语义与成员消亡顺序无关；需要逐条挡路语义的调用方仍走循环单删。
- **retarget 安排**：`{from → to}`（from 必须是批内成员、to 必须是批外存活指令）覆盖存活指令面向 from 的字段改锚，语义与逐条 `replaceAllUses` 一致（每字段一枚锚定 to 的新 ControlFlow 标签、被替换的旧 ControlFlow 标签销毁）；未安排覆盖的存活引用按 StillUsed 拒绝。
- `CodeEditor::eraseBatch(ids, policy)` 是策略层：`kRejectIfUsed` 直通；`kRetargetToFallthrough` 把单条版的「顺序后继」推广为「成员之后第一条批外存活指令」，异常表边界引用与「之后无存活指令」的成员仍整批拒绝，且规划失败发生在任何变更之前。

实测成本（12000 条指令批删 3000）：批量约 0.2ms vs 逐条约 200ms，两个多数量级；语义等价以「批量 vs 逐条各一份会话、lower 落盘字节逐位一致」钉死。

### 31.2 `collectCodeUnits` 按族过滤：`CodeCollectFilter`

收集面从全量形态扩展两维 AND 的过滤器，默认构造即全量语义（既有调用零变化，输出逐项同构）：

- **指令族维** `insnFamilies`：`InsnFamily` 四族（Invoke / FieldAccess / CpConst / Branch），`insnFamilyBit(Op)` 给出掩码位；常规数据搬运（load/store/算术/栈操作）不属任何族——族是调用方关心的「重」指令。
- **属性族维** `attrKinds`：Code 嵌套属性的 `AttrKind` 掩码（嵌套惰性解码后判定；解码失败按未命中，不报错）。
- **分流记录不受过滤**：Failed 恒保留（F1 防静默吞错不回退），NoCode 受 `includeNoCode` 管；过滤只作用于 Ok 记录，维度掩码 0 = 该维不过滤。

动机是 ergonomics F12 的实测摩擦：`memberOf` 一个入口返回五族，统计口径靠调用方逐条判 kind 极易混——现在调用侧可在收集时按族收口（如只要调用、只要分支），判别知识从每个消费者移进库。

## 32. 泛型签名层：结构化签名树（2026-10-14）

`jbc::type::signature` 按 JVMS 4.7.9.1 全文法建结构化树：三形态（ClassSignature / MethodSignature / FieldSignature）+ 类型参数/实参/通配符三态（`*` / `+T` / `-T` / 不变）/ 嵌套泛型 / 数组（扁平 dims 口径，对齐 `TypeDesc`）/ 基元 / 类型变量 / 内部类段链 / throws。解析（Utf8 借用视图，零分配）→ 树 → 反解；类名手术面（span 坐标 + rename 便捷 API）语义对齐描述符层同名设施。

### 32.1 三条层契约

1. **roundtrip 恒等**：合法输入 parse→unparse 逐字节恒等。树不做任何归一化——`'L'`/`';'`不入树反解时重建、`<T:>`空类界、段名内嵌 '$' 等一切字节各归其位。表驱动（每产生式 ≥2 形态+边界）+ 语料门（自产 78 类 53 签名 / env 注入 ares 447 类 787 签名）双模式全量断言。
2. **畸形显式 Err**：解析失败返回 `SigError{码, 字节偏移}`（偏移指向肇事字节或截断发现点），绝不静默吞——「结构可确定」之前不存在降级形态。JVMS 之外的字节（如 TypeArguments 中的逗号）按非法拒绝，不猜测容忍。
3. **手术显式调用**：类名手术只经本模块 API 发生——`classNameSpans`/`replaceClassNameSpan`（span 坐标，语义对齐描述符层 `replaceClassName`）与 `renameClassInSignature`（parse→手术→反解一条龙，显式调用）；只改命中 span 覆盖的字节，其余原样保留。Signature 属性的定位与写回在纯函数层不做（copy-through 纪律：不碰不改写）；属性层接线见 §32.4 手术入口。

### 32.2 与描述符层的关系

描述符（`descriptor.hpp`）之下是擦除后的运行时类型真相；签名层在其之上表达 javac 写进 Signature 属性的泛型语义。两层同风格：视图进、值出，节点字符串全部借用输入串字节，只在实际产出新串的反解/手术入口分配。类名手术坐标（`ClassNameSpan` / `SigClassNameSpan`）与 rename 便捷面语义对齐，但签名层的类类型携带结构（段链/实参），rename 因此能按全名匹配两种字节形态——'.' 段链（`Outer<T>.Inner`）与段名内嵌 '$'（`Outer$Inner`，javac 常规产物）——并把形态纪律收窄到段链结构轴（2026-10-17 扩界）：**扁平段链**（单段名）按全名逐字节比对、整段替换为 new 原文（跨 '$' 形态自由，跨混淆器映射还原的载体）；**真嵌套段链**不做结构重排（部件按段对齐，越界即不命中；`RenameShapeMismatch` 保留值并入不命中语义，现不再产生）。

### 32.3 边界

- 自动分派 `parseSignature` 只服务形态未知的场合：`(` 必方法、`[`/`T` 必字段、`L`/`<` 先类形态试完整消费（类形态是字段形态的语法超集）；消费方已知形态时应走形态特定入口（span/rename 的 form 参数同理，形态歧义不猜）。
- ~~属性层泛型保持管线（promote/lower 携带 Signature 重写）未接线；本层只提供纯函数面，类文件属性模型零改动。~~ **已接线（2026-10-14，§32.4）**：纯函数面之上补属性层手术入口；手术面经 typed 属性体读写（Signature 已立 typed 模型，见 §32.5）。

### 32.4 属性层接线：Signature 手术入口（2026-10-14）

签名树纯函数面（§32.1–32.2）与属性写回之间的管线落地为 `jbc::ir::sig_attr`（自由函数面，不进 `ClassSession`——跨类 rename 编排属 mbase ClassIndex 域，库不发明会话级 rename 概念）：

1. **显式调用出活**：`renameClassInSignatureAttr`（单槽原语，form 显式收参）与 `renameClassInSignatureAttrs`（类/方法/字段三级挂载点全扫，形态按挂载点判定——§32.3 形态歧义不猜）。不触发则槽保持 Untouched，字节逐字节 copy-through（硬门禁，语料级钉死：未触碰写回 + 缺席名全扫零改写两形态）。
2. ~~**属性模型零改动**：Signature 属性体是 `u2 signature_index`（JVMS 4.7.9），**不立 typed AttrKind**——Raw 兜底是全库既定契约（下游 mjbase 的 remapper/reference-index/workspace 按 Raw 字节消费 Signature；typed 模型会使这些消费者静默失效，经 mjbase_unittest 实证 6 用例红）。~~**已升格 typed 属性（2026-10-14，§32.5）**：Signature 立为第 7 个 typed 属性（`core::SignatureAttr`，体 `u2 signature_index`），手术面改经 typed 体读写索引，下游消费方协同迁出 Raw 字节消费——契约详见 §32.5。
3. **手术语义复用 §32 契约**：'.' 段链与 '$' 内嵌双形态全名匹配、扁平段链跨形态整段替换（2026-10-17 扩界，§32.2）、真嵌套链部件越界不命中、未命中零改写零追加。
4. **失败显式 Err，不静默跳过**：失败族如实三分——`SigAttrError{Signature（解析/形态，带字节偏移）, SlotDecode（体半截）, CpCapacity（容量预检）}`。全扫**全有或全无**：先全量预检（纯读；解码只推进 Untouched→Decoded，写回对 Decoded 仍 copy）再统一容量预检，最后才 intern 落位置脏——Err 时不留半截改写（对齐 §30.2 先算后写纪律）。
5. **CP 只追加 + 查重不膨胀**：新签名串经 `ConstantPool::findUtf8`（新增查询原语，O(n) memcmp）命中复用、miss 才 `addUtf8`；重复新串在一次全扫内只追加一份，逆向 rename 复用回原条目；旧串成为孤儿条目（append-only 契约允许，压缩逃生口仍是待裁决项）。
6. **验收**：`sig_attr_test` 表驱动（硬门禁/命中重写/ShapeMismatch 拒绝/未命中零改写/查重不膨胀/尾巴保持/坏签名 fail-closed/挂载点形态/半截体拒绝）；`signature_corpus_test` 扩属性级管线门（自产 18 文件 82 属性改写 / env 注入 ares 190 文件 2101 属性改写，逐名正向→写回→重读逐属性比对→ 逆向逐属性还原）；`rename_probe_test` 的 Signature 处理从文本 token 替换
   + Raw 字节捅换成本入口（JVM 真跑验收保持）。libjbc_unittest 294 用例 293 过 1 跳（双模式各自全绿），mjbase_unittest 1198/1196/2 零漂移。（自产语料数字后因 §32.7 样本入组漂移为 20 文件 85 属性。）
7. **挂账**：~~Runtime{Invisible,Visible}TypeAnnotation 家族的引用类型路径不在本入口覆盖内（即 B2）~~ **已落（§32.7）**；LVTT 签名列已落（§32.6）。

### 32.6 LVTT 签名列手术（2026-10-14）

LocalVariableTypeTable 行的 descriptor 列承载字段形态签名（JVMS 4.7.9.1 FieldTypeSignature）——泛型签名在类文件中的第二处宿主。手术入口 `renameClassInLocalVariableTypeTables`（全方法 Code 内 LVTT 全扫）落在 `jbc::ir::sig_attr`，与 Signature 属性手术同模块同契约：

1. **手术语义逐字复用 §32**：逐行走 `renameClassInSignature`（form 固定 Field），双形态全名匹配、扁平链跨形态 / 嵌套链越界不命中（2026-10-17 扩界同款）、未命中零改写。
2. **全有或全无三阶段**：全量预检（Code/LVTT 槽解码失败 → SlotDecode、行签名解析失败 → Signature，零改写零追加）→ 统一容量预检 → intern 落位置脏。未命中的表不置脏，写回逐字节 copy-through。
3. **CP 只追加 + 查重不膨胀**：新串 findUtf8 命中复用、miss 才追加；逆向 rename 复用回原条目（池不二次膨胀；旧串成孤儿条目同 §32.4.5）。
4. **LVT 不属本入口**：LocalVariableTable 行是擦除后描述符，手术归描述符层 span 面（jbc::type descriptor 的 classNameSpans/replaceClassName），签名/描述符两族不混。
5. **验收**：sig_attr_test 表驱动（硬门禁/双形态改写/类型实参嵌套/逆向复用原条目/ShapeMismatch 拒绝/坏行全扫拒绝/半截体 SlotDecode/trailing 保持）；signature_corpus_test 增 LvttLevelSurgeryPipeline（逐名正向→ 重读逐行对 oracle→逆向逐行还原；自产 7 文件 50 行 50 改写 / env 注入 ares 109 文件 913 行 2005 改写——自产语料编译旗语补 `-g`，manifest 旗语指纹保证旗语升级整批重编；自产数字后因 §32.7 样本入组自 6 文件 49 行 48 改写漂移）；rename_probe_test 的 LVTT 处理从文本 token 替换改本入口（探针内 "L<name>;" 全文替换函数删除；描述符路径同步改描述符层 span 结构化替换）并加 Main 类 LVTT 命中断言，JVM 真跑 2 用例绿（改名后类加载 + 行为输出一致）。libjbc_unittest 303 用例 302 过 1 跳（双模式各自全绿），mjbase_unittest 1198/1196/2 零漂移。

### 32.5 typed Signature 属性模型与消费方迁移契约（2026-10-14）

§32.4.2 的「不立 typed AttrKind」过渡态收束：Signature 立为第 7 个 typed 属性（`core::AttrKind::Signature`），codec、手术面与下游消费方一次迁齐。

1. **codec 形状与既有 typed 属性同款**：decoder 按属性名 `Signature` 分派，体读单个 `u2 signature_index`（JVMS 4.7.9；类/方法/字段三级挂载点各至多一个，签名串在 CP Utf8）；声明结构之后的多余字节奏 `trailing` 原样保留（§27.1 八类同治），重编码写回不丢尾；合法体恰 2 字节，半截体 decode 即 Err（畸形面显式，未触碰写回仍逐字节 copy）。encoder 落不进 u16 域宽即 `Err(WriteError::Unencodable)`（§27.2 闸门同型）。
2. **手术面改 typed 读写**：`jbc::ir::sig_attr` 的 promote（读索引）与 lower（`mutate()` 改索引置脏）在 `SignatureAttr` 上发生，写回走既有 typed 重编码路径；§32.4 契约 3–5（双形态全名匹配 / 失败显式 / CP 只追加）逐字不变，硬门禁（未触碰槽字节逐字节 copy-through）不变。
3. **消费方迁移一次迁齐**：按 Raw 字节消费 Signature 的位置全部改吃 typed 模型（tag_cast 判型，不再手工捅 u2）——mjbase remapper 的 Signature 属性重映射、reference-index 的类/字段/方法三处签名类型索引、workspace 的新增成员 Signature 属性构造与签名读出。
4. **验收**：typed 化后全库门禁零漂移——libjbc_unittest 294/293/1、mjbase_unittest 1198/1196/2（§32.4.2 实证的 6 用例红归零，即消费方迁移完成的判据）；语料双模式数字与 §32.4.6 一致（自产 18 文件 82 属性 / ares 190 文件 2101 属性改写）；rename_probe JVM 真跑 2 用例绿（改名后反射读泛型签名与改前一致）。

### 32.7 typed TypeAnnotations 属性模型与手术契约（2026-10-14）

§32.4.7 挂账的 B2 收口：Runtime{Visible,Invisible}TypeAnnotations 内嵌类名（JVMS 4.7.20 注解类型描述符与 'c' element_value 类字面量）纳入类名手术覆盖，属性模型立为第 8 个 typed 属性。

1. **typed 建模**：`core::TypeAnnotationsAttr`（`AttrKind::TypeAnnotations`），双属性名同分派一个 typed 体；结构为注解列表，每条 = `target_type` + `target_info` 联合（JVMS 4.7.20.1：type_parameter/bound/formals 载 u1，supertype/throws/catch/offset/cast/type_argument 载 u2 或 u2+u1，field/method_return/receiver 空）+ `type_path` + `type_index`（注解类型描述符 CP 引用）+ element 对列表（复用 AnnotationsAttr::ElemValue）；trailing 同 §27.1 八类同治。`type_path` step kind > 3 显式 `Err(UnsupportedFeature)`（不猜新 kind）；半截体 decode 即 Err。
2. **target_type 表以 javap 实证为准**：建模时按记忆手写的表与真实 javac 产物对不上（0x10 是 supertype 而非 type_parameter 变体、0x13 是 field 空载荷等），解码真实产物全红后逐条对照 javap 标签修正；`cast_target`（0x47）载荷为 **u2 offset + u1 type_index**（javap 输出 `CAST, offset=1, type_index=0` + 体长唯一闭合解析双实证）——文档表与记忆冲突时以产物实证为准，此教训入契约。
3. **手术点**：`jbc::ir::type_annotation_attr::renameClassInTypeAnnotations` 全挂载点扫描（类/字段/方法 + Code 嵌套属性），每条注解动两处——`type_index`（注解自身描述符 `L...;`）与 element_value 中 tag `'c'` 的类字面量（`returnDescriptor` 语境，基类型 `BCDFIJSZ` 开头跳过——`void.class` 的 `'c'` 值是 `V`，合法不动）；描述符内命中走 `classNameSpans` 全名逐字节匹配 + 倒序 `replaceClassName`（span 面按全名相等命中，无 '$' 结构解读，kotlin `$$inlined$` 真名天然通过）。
4. **全有或全无三阶段 + CP 只追加**：纯读预检（Descriptor/SlotDecode 显式 Err）→ 容量预检（CpCapacity）→ intern 落位置脏；新串 findUtf8 查重复用、miss 才追加，逆向 rename 复用回原条目（§32.4.5 同款）。未触碰属性写回逐字节 copy-through 硬门禁不变。
5. **语料覆盖与显式挂账**：自产语料补 VolTypeUse.java（TYPE_USE 注解在字段/泛型实参/extends/方法返回/throws/转型/instanceof/局部变量触点，RUNTIME+CLASS 双保留），javac 产物门 `filesWithTa=2 taAttrs=5` 手术流水绿；**ares 447 类全扫 TypeAnnotations 分布 = 0**（Python 独立扫描器与 C++ 门双证），env 注入模式语料门成文 skip（skip 理由注明 ares=0），不静默放空——覆盖面以 javac 产物为准，ares 真实分布不含 TA 是显式事实而非覆盖缺口。
6. **验收**：TaAttrTest 表驱动 8 用例（decode/write 未触碰恒等、typed 判型结构、硬门禁零改写、type_index 与 'c' 值改写、逆向复用原条目、ShapeMismatch 拒绝、半截体拒绝、trailing 保持）；TaCorpus 管线门（正向→重读逐注解对 oracle→逆向还原）；rename_probe JVM 真跑 2 用例绿——探针样本补 `@TypeUse(cls = Expr.class)` TYPE_USE 字段注解，改名后经 `Field::getAnnotatedType().getAnnotation(TypeUse.class).cls().getName()` 反射读出改名后类名，debug strip 后仍成立。libjbc_unittest 312/311/1（双模式各自全绿），mjbase_unittest 1198/1196/2 零漂移。自产语料因 VolTypeUse 入组，§32.4.6/§32.6.5 的签名族数字同步漂移（见各节更新后数字）。

### 32.8 typed CharacterRangeTable 属性模型（2026-10-14）

CharacterRangeTable 是 javac `-Xjcov` 的私有调试属性，**不是 JVMS 条目**（JVMS 属性章节到 4.7.24 为止，不存在 4.7.29），建模规格以 JDK javac 源（`CRTable.writeCRT`）与 JDK8 `-Xjcov` 产物字节实测双证核对。

1. **行形以产物实证为准**：`u2 行数 + { u2 start_pc; u2 end_pc; u4 char_start; u4 char_end; u2 flags }*`（每行 14 字节，`2 + 14n` 与属性 length 唯一闭合）。任务侧曾经的记忆行形「start_pc/end_pc/range_index/flags 各 u2」与产物不符——char 列是 u4 且没有 range_index 字段；char 值是源文件位置 `(line << 10) | column` 编码（0 起），与字节码无关。`start_pc` 是范围首指令 pc，`end_pc` 是**含末指令**的 pc（javac 写 `entry.endPc - 1`），二者都是指令起始 pc。
2. **typed 建模**：`core::CharacterRangeTableAttr`（`AttrKind::CharacterRangeTable`，第 9 个 typed 属性），行结构五字段按 §27.1 trailing 同族九类同治；半截行（声明行数与剩余字节不符）decode 显式 `Err(Truncated)`；u2 编码列（start_pc/end_pc/flags）encode 走 §27.2 域宽闸门，char 列本就是 u4 无闸门。
3. **重定位**：Code 嵌套形态下两列 pc 各自精确命中 pcmap，miss 即 `Err(RelocationBadTarget)`——含末指令语义决定 end_pc 不存在「恰等旧 code 长」的合法 miss；char 列与 flags 不携带 pc，原样保留。
4. **语料分布（显式事实）**：自产语料 82 类与 ares env 语料 447 类的 CRT 出现次数均为 **0**（字节级扫描）——现代 javac（JDK 11+）已停发本属性（`-Xjcov` 被忽略），仅 JDK8 及更早编译产物携带；语料门因此无 CRT 覆盖，行为正确性由 crafted 测试承担（解码保真、半截行拒绝、trailing、u16 闸门、双列 pc 重定向、坏 pc 失败显式）。

### 32.9 B3 四件结构元数据属性：typed 模型与分派挂账（2026-10-14）

SourceFile（JVMS 4.7.10）/ InnerClasses（4.7.6）/ EnclosingMethod（4.7.7）/Exceptions（4.7.5）立为第 10–13 个 typed 属性（`AttrKind` 10..13）。四件共同边界：只挂类级/成员级属性表，**不 Code 嵌套、载荷全为 CP 索引与访问位、无任何 pc 偏移——§33 relocation 封闭集不涉及它们**，布局改动不需要也不得对这四件做重定位。

1. **codec 形状同铁律**：半截体 decode 显式 `Err(Truncated)`；声明结构后的多余字节奏 trailing（§27.1 同族，现十类同治）；编码逐字段过 u16 域宽闸门（§27.2）。体模型：SourceFile 单 `u2` Utf8 索引；InnerClasses 行四槽 `inner_class_info/outer_class_info/inner_name/access_flags`（outer==0 匿名无外围、inner_name==0 匿名无名，0 是合法值原样保真）；EnclosingMethod `class_index + method_index`（NAT，**method_index==0 合法**——外围为初始化器时不指名方法）；Exceptions `u2 count + u2 Class` 表。
2. **分派接线（B3 接线批已落，原挂账结清）**：四件的 typed 体、公开解码函数（`decodeInnerClassesBody` 族，不触碰槽状态）与编码分支就绪后，`decodeAttrBody` 曾对三件保持 Raw 兜底——下游 mjbase 有七处消费面按 `RawAttr` 硬 cast 消费它们（remapper 手术×3：remapExceptionsAttr / remapEnclosingMethodAttr / remapInnerClassesAttr；reference_index×2：EnclosingMethod / InnerClasses 结构元数据引用收集；workspace×2：exceptionNames / innerClassNames），先接 typed 分派会使手术与查询**静默失效**（与 §32.4.2 实证 Signature 6 用例红完全同构）。接线批与消费方迁移同批落地：七处消费面全部改走 typed 体（手术改经公开解码入口读体、逐字段语义对齐旧字节手术、makeNew 置 New 重编码），分派对三件翻到 typed；`attr_extra` 计数数组与分派钉门测试随接线对账（钉门从钉 Raw 形翻为钉 typed 形）。class_mutation_service 新建 Exceptions 同批改立 typed 体——会话内 Raw 形态不复存在，是压缩层 Raw 名单退役的前提（A8 教训：分派、消费面、压缩器必须同批，否则静默失效或诬拒）。
3. **手术面不立第二套**：InnerClasses 三 CP 槽与 EnclosingMethod 两槽的类名重映射，生产手术自接线批起在 mjbase typed 面公开解码入口上（语义与旧字节手术逐字段对齐：类名映射命中才动、outer/name==0 保 0、简单名随内部类二进制名映射同步且仅在全名命中旧简单名时改写、NAT 规则命中换新条目）。typed 模型的 `Row` 三槽 / 两槽与手术语义一一对应；「typed 读出 = 旧字节手术结果」的一致性由测试内 Raw-oracle 对拍钉死（oracle 直接从槽源 span 取体字节手术，不依赖分派形态），且重读 typed 视图看到改写后语义。oracle 是测试内口径镜像，不是生产第二套手术。
4. **验收**：四文件 crafted（分派钉门/typed 直读/半截 Err/trailing/makeNew 重编码字节恒等/u16 闸门/手术对拍）+ 语料门（`StructAttrsCorpusGate`：全语料逐类「未触碰写回逐字节不动」+ 四属性逐槽 typed 解码 → 重编码整类字节恒等；自产 82 类分布 SourceFile 82 / InnerClasses 71 / EnclosingMethod 12 / Exceptions 4 全 >0 硬断言，env 注入模式分布不硬断言——ares 混淆产物可能剥属性，分布是显式事实；分布数字随样本库变动漂移，以当前语料门实测为准）。`attr_extra` 的 decode-all+dirty-all+重编码语料门随分派自动覆盖 typed 面。B3 立模时 libjbc_unittest 386 用例（自产 385 过 1 跳 / env 384 过 2 跳）、mjbase_unittest 1228/1226/2 精确平。分派接线批（第 2/3/5 条的落地件）验收：libjbc_unittest 402 用例——自产 401 过 1 跳 / env 注入 ares 400 过 2 跳（基线 401/400/1 与 401/399/2 + 新增 1 全绿），mjbase_unittest 1268/1266/2 精确平零回归；ares 压缩拒绝面逐点名不变（NestHost=149/NestMembers=52/MethodParameters=54——ares 三件分布 SourceFile 447/InnerClasses 312/EnclosingMethod 17/Exceptions 36 本就处压缩封闭集，接线只换分派层不改集合成员）；red-side：分派摘回 Raw → 三钉门 + 压缩五件读回用例翻红、mjbase 全套保持绿（消费面已解耦）→ 复原复绿。
5. **JVM 门**：本批对三件的读改写路径零改动（decode 是观察侧），字节产出不变；SourceFile 是纯调试元数据，反射不暴露，typed 解码经语料门字节恒等背书。分派接线批按 rename_probe 探针基建补嵌套类改名后反射断言：成员内部类/局部类/匿名类混编样本整体改名（含外围类），InnerClasses 行 outer 槽与 EnclosingMethod class 槽跟随重写、三件经分派 typed 面强制重编码后，`getDeclaringClass` / `isAnonymousClass` / `getEnclosingClass` / `getEnclosingMethod` 反射读出新外围名且匿名/局部判定与包围方法原样存活，`getExceptionTypes` 表原样——反射面消费的即 typed 编码产物。

### 32.10 Nest 族三件结构元数据属性：typed 模型与分派接线（2026-10-15）

NestHost（JVMS 4.7.29）/ NestMembers（4.7.28）/ PermittedSubclasses（4.7.31）立为第 14–16 个 typed 属性（`AttrKind` 14..16）。三件共同边界与 §32.9 B3 四件同款：只挂类级属性表，**不 Code 嵌套、载荷全为 CP Class 索引、无任何 pc 偏移——§33 relocation 封闭集不涉及**，布局改动不需要也不得对这三件做重定位。体模型：NestHost 单 `u2 host_class_index`；NestMembers 与 PermittedSubclasses 均为 `u2 count + u2 classes[]`（形状同构、typed 面各归各，不共享体类型）；宿主自身不出现在 NestMembers 表中，解码不做语义校验——结构保真层原样承载。

1. **codec 形状同铁律**：半截体 decode 显式 `Err(Truncated)`；声明结构后的多余字节奏 trailing（§27.1 同族）；编码逐字段过 u16 域宽闸门（§27.2）。
2. **分派与消费面同批（A8 教训的完整执行）**：三件的 typed 分派、公开解码入口（`decodeNestHostBody` 族，不触碰槽状态）、编码分支、压缩封闭集（§36.1 第 1 层扩容）与 mjbase 消费面迁移（remapper 手术：NestHost 单槽 / NestMembers / PermittedSubclasses 表列；reference_index 结构元数据收集）一个批内落地，不留「分派已翻、下游按 Raw 硬转」的半接线态。Module 族（ModuleMainClass/ModulePackages/Module）不在批内，保持 Raw 路径——remapper 的单 Class 条目字节手术归 ModuleMainClass 专用，NestHost 由 typed 手术接管。
3. **手术面不立第二套**：三件的类名重映射生产手术在 mjbase typed 面公开解码入口上，语义与旧字节手术逐字段对齐（映射命中才 intern 新条目、miss 原样、makeNew 置 New 重编码）；「typed 读出 = 旧字节手术结果」由测试内 Raw-oracle 对拍钉死（oracle 直接从槽源 span 取体字节手术，不经分派），oracle 是测试内口径镜像。
4. **验收**：crafted（分派钉门/typed 直读/半截 Err/trailing/makeNew 重编码字节恒等/u16 闸门/手术对拍）+ 语料门扩展为七属性逐槽 typed 解码 → 重编码整类字节恒等。Nest 族自产覆盖走独立 `--release 17` 小语料（主语料 `--release 8` 产物是 v52，构造上不带这三件）：nestmate 对产出 NestHost+NestMembers、sealed 层级产出 PermittedSubclasses；自产分布 >0 硬断言，env 注入模式分布只记录不硬断言。本批验收：libjbc_unittest 420 用例（自产 419 过 1 跳 / env 注入 ares 418 过 2 跳；基线 403/402/1 与 403/401/2 + 新增 17 全绿）、mjbase_unittest 1269/1267/2（基线 1268/1266/2 + 新增 1 全绿）、deps_gate 0 违规。
5. **JVM 门**：rename_probe 嵌套类族样本带 NestHost+NestMembers（javac 11+ nestmate），改名后 `getNestHost` 读新宿主名、`getNestMembers` 计数原样（成员表条目指向未改名的内部类二进制名）；sealed 样本 `getPermitted-Subclasses` 反射原样。member_remapper 侧另立 JVM 真跑门：nestmate + sealed 样本整体经映射改名（含表内条目），三件 typed 手术后真 JVM 执行，反射读出新名、行为输出与原样本一致。JDK < 17（sealed 编译失败）按环境能力跳过，不引入新环境依赖。
6. **压缩拒绝面收缩**：三件入 §36.1 封闭集后，ares 语料的逐点名拒绝分布从 NestHost=149 / NestMembers=52 / MethodParameters=54 收缩为 MethodParameters=54（§36.6）。

### 32.11 Record 属性：组件表 typed 模型与嵌套属性面（2026-10-16）

Record（JVMS 4.7.30）立为第 17 个 typed 属性（`AttrKind::Record`）。只挂类级属性表，载荷为 CP Utf8 索引 + 嵌套属性表、无 pc 偏移——§33 relocation 封闭集不涉及。体模型：`u2 record_component_count + components[]`；每个 component 是 `u2 name_index（Utf8）+ u2 descriptor_index（Utf8 字段描述符）+ u2 attributes_count + attributes[]`。与其它结构元数据属性的本质差异：**组件上是嵌套属性表**，javac 会在泛型组件上挂 Signature、在带注解组件上挂 Runtime{Visible,Invisible}Annotations——嵌套持有形态与 Code 同构（§32.4）：组件嵌套属性也是 `AttrSlot`，span 是源字节流的绝对偏移，解码只读目录（lazy 照旧），写回按槽状态 copy-through 或重编码；嵌套槽置脏使父体进入重编码（与 Code 嵌套同一递归闸）。

1. **codec 形状同铁律**：半截体（组件表或组件三槽任一读不满、嵌套属性目录越界）decode 显式 `Err`；声明结构后的多余字节奏 trailing 同族保留（§27.1）；编码逐字段过 u16 域宽闸门（§27.2），嵌套 `encodeAttr` 失败向上传播、不产半截父体。公开解码入口 `decodeRecordBody` 不触碰槽状态。
2. **分派与消费面同批（不留半接线态）**：`decodeAttrBody` 分派臂、编码分支、`decodeAllAttrs` 递归解码组件嵌套面、压缩封闭集两相 typed 分支（§36.7）与 mjbase 消费面迁移（remapper 的 `remapRecordAttr`：组件名按字段/访问器规则择一映射、描述符走字段描述符映射面、组件嵌套 Signature/注解槽复用成员级同一条手术路径；reference_index 的 Record 读点翻 typed：组件描述符收结构元数据、嵌套 Signature/注解同源收口）一个批内落地。
3. **验收**：crafted（分派钉门/typed 直读不触槽/三形态显式 Err/trailing 存活重编码/空组件表恒等/u16 闸门逐字段/嵌套槽置脏重编码父体且其余字节不动/组件手术往返）；语料门立独立 `--release 16` record 小语料（泛型 record、带注解组件、紧凑规范构造器三样本，源入共享样本库、产物生成于 test/）：record 属性/组件/组件嵌套 Signature/组件嵌套注解四分布自产模式 >0 硬断言、env 注入只记录；逐槽 typed 解码 → 重编码整类字节恒等。
4. **JVM 门**：member_remapper 侧 JVM 真跑门——record 样本（带注解与泛型组件）整体映射改名（组件名/嵌套泛型类名/注解类型名命中），typed 手术后新名类真 JVM 执行：`isRecord`/`getRecordComponents`/组件 `getName`/`getType`/`getGenericType` 读新名、组件注解反射原样、行为输出与原样本一致。JDK < 16（record 编译失败）按环境能力跳过。
5. **压缩拒绝面不变性**：ares 语料类级 Record 属性分布（语料门 env 实测 57 属性 / 179 组件 / 组件嵌套 Signature 20 / 组件嵌套注解 1），但逐点名压缩拒绝分布由方法级 MethodParameters 先扫决定——Record 入封闭集后逐点名拒绝分布保持不变（MethodParameters=54 唯一残余，§36.7）。record 规范构造器恒带 MethodParameters（javac 行为），故 record 自产语料不并入压缩扫描语料（在到达 Record 前即拒绝，并入无增益），Record 压缩分支由 crafted 用例覆盖。

### 32.12 Module 族三件结构元数据属性：typed 模型与分派接线（2026-10-16）

Module（JVMS 4.7.25）/ ModulePackages（4.7.26）/ ModuleMainClass（4.7.27）立为第 18–20 个 typed 属性（`AttrKind` 18..20）。三件共同边界与 §32.10 Nest 族同款：只挂类级属性表，**不 Code 嵌套、载荷全为 CP 索引与旗语位、无任何 pc 偏移——§33 relocation 封闭集不涉及**。体模型：Module 为 `u2 module_name（CP Module）+ u2 module_flags + u2 module_version（CP Utf8，0 = 无）+ requires[] + exports[] + opens[] + uses[]（CP Class）+ provides[]`；requires 行是 `{模块（CP Module）, flags, 版本（CP Utf8，0 = 无）}`，exports/opens 行是 `{包（CP Package）, flags, to[]（CP Module）}`（to 空 = 未限定），provides 行是 `{服务（CP Class）, with[]（CP Class）}`；**体在 provides 后结束、不含主类索引段**——javac 17.0.8 实测产物体恰止于 provides，主类由独立的 ModuleMainClass（单 `u2` CP Class，与 NestHost 同形状）承载。ModulePackages 为 `u2 count + u2 packages[]`（CP Package，形状同 NestMembers 而条目类型不同）。段旗语（ACC_TRANSITIVE/ACC_STATIC_PHASE/ACC_OPEN/ACC_SYNTHETIC/ACC_MANDATED）按不透明 u16 保真，与 InnerClasses 的 access_flags 同款处理。

1. **codec 形状同铁律**：半截体（头三槽或五段任一读不满）decode 显式 `Err(Truncated)`；声明结构后的多余字节奏 trailing（§27.1 同族）；编码逐字段过 u16 域宽闸门（§27.2）。公开解码入口 `decodeModuleBody`/`decodeModulePackagesBody`/`decodeModuleMainClassBody` 不触碰槽状态。
2. **ModuleHashes/ModuleResolution/ModuleTarget 不建模**：三者是 JDK 内部件（jlink/jmod 侧产物），非 JVMS 条目、无稳定规格契约；javac 对 module-info 的产物构造上不含它们（直译与 --module-source-path 模式实测均不产），语料机制不可产真实字节，typed 面无从钉门。三者保持 Raw 透传（保真不丢字节），压缩封闭集外照旧显式拒绝（拒绝点名属性名，类级整体不压缩，无静默改写风险）。
3. **分派与消费面同批（不留半接线态）**：三件的 decodeAttrBody 分派臂、编码分支、压缩封闭集两相 typed 分支（§36.8）与 mjbase 消费面迁移（remapper 的 ModuleMainClass 单槽手术与 Module 的 uses/provides 手术翻 typed 体；reference_index 的 ModuleMainClass/Module 读点翻 typed 解码入口，Raw u2 表读取面退役）一个批内落地。
4. **语料现实（探针覆盖口径）**：javac 17.0.8 实测——module-info 只有 `--release 9+` 才产；Module 属性五段全形态（requires MANDATED 基线行带 JDK 版本串、TRANSITIVE/STATIC_PHASE 旗语、exports±to、opens、uses、provides...with）可产，ModulePackages 与 ModuleMainClass 在直译与 --module-source-path 模式下均不产（前者需 javac 能枚举模块全部包的编译形态、后者是 jlink/jar 工具链的写入面），两件的 typed 面由 crafted 字节用例钉住，module 语料不背其分布。

## 33. relocation contract 的封闭集（2026-10-14）

布局改变（导出字节 ≠ 原 code）时，lower 对 Code 嵌套属性执行「预检 + 暂存 → 提交元数据 → 写回」的三阶段全有或全无重定位。本节成文契约的**封闭集**：可重定位属性种类是封闭清单，清单之外一律显式失败。

1. **封闭清单**（Code 嵌套、codeChanged 时逐槽解码分派）：
   - `LineNumberTable`：每行 start_pc 精确命中 pcmap，miss 即 `Err(RelocationBadTarget)`。
   - `LocalVariableTable` / `LocalVariableTypeTable`（双属性名合流为同一 typed kind）：行 start 精确命中；区间终点 start+length 命中 pcmap，或恰等旧 code 长（唯一合法 miss，映射到新 code 长）；其余 miss 与 u32 溢出即 Err。
   - `StackMapTable`：链式 offset_delta 还原绝对 pc → pcmap → 重算 delta；紧凑形态放不下时升格 extended；`Uninitialized` 载荷的 pc 走同一张 pcmap（`Object` 载荷是 CP 索引，不得误重定位）。
   - `Runtime{Visible,Invisible}TypeAnnotations`：携带 pc 的目标才动——localvar/resourcevar 行走 LVT 同款终点规则，offset/cast/type-arg 族（0x43..0x4B）单偏移精确命中；catch 下标（异常表条目序，重建保序）与无 pc 目标（类/方法级、type_path、元素值）原样保留；未知 target_type 在解码层已显式拒绝。
   - `CharacterRangeTable`：行 start_pc/end_pc 双列精确命中（含末指令语义下不存在「恰等旧 code 长」的合法 miss）；char 列与 flags 不动。
2. **封闭性裁决**：清单之外的嵌套属性（含未知/自定义/未建模的携带偏移属性）一律 `Err(RelocationNotModeled)`——不静默丢、不猜、不改契约放行。这是 §22.2 裁决（补建模不放宽契约）的延伸：**新嵌套属性进工具链的唯一路径是扩封闭集**（typed 建模 + 重定向语义 + crafted 重定位测试 + 重读恒等），不存在绕行开关。
3. **失败显式分级**：`RelocationNotModeled` = 种类未建模（能力边界）；`RelocationBadTarget` = 已建模但 pc 引用不落指令起点（输入畸形或实现缺陷）。两者都不允许半改：预检全量通过前不触碰任何槽位，失败后写回字节与进入前逐字节一致。
4. **行为级门**：布局改动语料扫（每带码方法开头插 nop 强制 codeChanged）断言两错误码计数为 0，且写回 → 重读 → 全解码 → 再写逐字节一致。基线与修复后数字：自产语料 82 类 1066 方法（修复前 1065 通过 + 1 NotModeled，即 TA 触点方法整条被墙拒绝）；ares env 语料 447 类 3458 方法（修复前后均 0）；两模式 `badTarget=0`。

## 34. 成员级增删、身份失效协议与从零建类（2026-10-14，A3/A4/A5）

### 34.1 身份簿记与 generation 失效协议

成员身份（MethodId/FieldId）与逻辑下标解耦的协议在此定形：

1. **slot 发号**：会话构造期为初始成员按序分配 0..n-1；成员级新增优先复用**最小已删槽位**，无空闲槽位才发新槽。slot 是身份坐标，不是物理位置（backingIndex 才是，由增删入口统一平移维护）。
2. **generation 台账**：删除成员时该槽 generation 自增。此后旧句柄（携带旧代）结构性失配——不存在「删后同槽新成员复活旧句柄」的 ABA 形态。重建后的身份携带新代，`!=` 旧身份。
3. **失效的可判别面**：死句柄（已删除/外来会话）在查询面返回空值（`method()/field()` 得 nullptr、`indexOf()` 得 kNoMethod/kNoField、`methodId()/fieldId()` 越界得 kNoMethodId/kNoFieldId，均不抛错）；在变更面返回显式 `Err(InvalidHandle)`（commit/promote/lower/remove/adopt 五通道）——promoteMethod 的死句柄自 NoCode 改列 InvalidHandle，「方法存在但没有体」与「句柄已死」不再混码。
4. **逻辑下标的重定义**：成员增删/reorder 之后，逻辑序与物理序不再恒等。下标视图（promoteMethod(s, idx) / hasCode / collectCodeUnits 的 methodIdx）一律经 MethodUnit::backingIndex 转物理位置；只有 MethodId/FieldId 是跨变更稳定形态。这是本节落地时修正的既有隐患：hasCode 与 matchesAttrKinds 此前直接用同一下标查 backing，reorder 后会读错方法。

### 34.2 增删语义（add/remove/commit 面）

- **addMethod/addField（种子挂入）**：种子 = {name, desc, access(, withCode, maxStack, maxLocals)}。名字/描述符 intern 进 backing CP（命中既有池不膨胀，CP 只追加）；withCode 时随建一个空 Code 属性（New 态），promote → CodeEditor 出活 → lower 写回是既定管线，不新增第二套写回。预检先于写入：非空、u16 域、CP/成员容量、会话内名字+描述符唯一（JVM 对重复成员是加载期错误，挂入时挡掉）——失败不留半截成员。
- **removeMethod/removeField**：物理面摘除 method_info/field_info 并把后续成员的 backingIndex 统一平移（身份不动——物理位置本来就不是身份）；身份面回收槽位并自增 generation，再摘骨架。已 promote 的方法其 CodeUnit 随之销毁，消费者手里的 CodeUnitRef 同步作废（与 CodeDom::erase 后旧 Insn*作废同一纪律）；逻辑下标视图此后指向新顺序。
- **commitFieldMetadata**：字段名/描述符/访问标志的高层改动以它为唯一落盘通道（与 commitMethodMetadata 同构；字段无 lower 时机，不做自动点）。
- **错误码**：InvalidHandle（死句柄）、InvalidSeed（种子非法：空名/超域/重名）、NotModeled（搬移涉及未建模结构）新增于既有两族枚举，不立第三套。

### 34.3 跨类搬移契约（adoptMethod/adoptField 的 CP 迁移语义）

消费场景是 mbase ToolClassCombiner（多 static 工具类并入宿主 + 全 owner 重映射），A3/A4 的接收形态按它设计：

1. **挂载点**：字段/方法条目的 name/desc 常量 intern 进目标 CP（命中复用，只追加）；access 原样随迁。
2. **方法体随迁 = IR 重放，不是字节拷贝**：源方法 promote 后逐指令搬运，指令的 CP 操作数按**源会话语义视图**读出（memberOf/classOf/constOf）、按**目标会话语义 intern** 写入（setMember/setClass/intern 家族）——Methodref→NameAndType→Utf8 的多跳引用结构在目标池内重建，源/目标 CP 布局差异从结构上不可见。三读门只认携带 CP 操作数的三个族（Invoke/FieldAccess/CpConst）；`insnFamilyBit` 对 Branch 族的置位是收集过滤的复用位，分支/switch 无 CP 操作数不入三读（曾误伤分支族，任何含分支方法体的 adopt 必败，已修复并以分支体回归测试钉死）。分支/switch/异常表边界经标签映射重建：前向分支的目标指令尚未重放，占位标签在逐条 append 期必然未锚定，CodeDom 的逐条防呆在该路径机制性不可满足——重放进入**受控延后绑定构建态**（`CodeDom::beginDeferredTargets`/`endDeferredTargets`，仅整体重放路径开放：放行的只有「分支/switch 目标须已锚定」一条，switch 形状校验两态一致保留），全量重放后统一改绑，退出受控态时对全量存活指令补同一套锚定校验，仍悬垂的占位显式失败（fail-closed，不留给 lower）；正常编辑路径不进受控态，防呆语义不变。lookupswitch 的 key 表随指令逐字随迁（丢表即形状校验 BadOperand）；异常表的 catch 类索引按源语义读出、目标 intern；maxStack/maxLocals 随体拷贝。
3. **owner 重映射是消费者职责**：搬移本身逐字保真（体内引用源类自身的成员仍指向源类名）；搬入后由消费者遍历体内指令做 setMember 重写（语义 intern 一步落位）。库不替消费者决定重映射策略。
4. **不随迁面（显式契约，非缺陷）**：源成员的非 Code 属性（LNT/LVT/LVTT/SMT/注解/Exceptions/Signature 等）不随迁。后果与义务：(a) 调试属性丢失不影响行为；(b) 分支体并入 v51+ 目标类时**必须**由消费者随后用 buildStackMapTable 再生帧，否则 VerifyError——这是 E1 ReferenceSite 导出（成员级 CP 引用清单）落地前的显式挂账，届时可升级为属性随迁；(c) Exceptions/注解的语义面丢失由消费者按需重建。
5. **fail-closed 清单**：invokedynamic 调用点与 ldc 的 MethodHandle/MethodType/Dynamic/Long/Float/Double 形态显式 `Err(NotModeled)`（语义 intern 面未覆盖，不猜不降级）；携带 CP 操作数族内指令三读全 miss（池结构坏）显式失败，不按普通指令静默搬运（那会把坏引用洗成 0 号槽）；ldc 装载形态按目标索引自动选 LDC/LDC_W（宽度陷阱在搬移层挡掉）；重放收尾全量补控制目标校验，悬垂占位标签 `Err(BadInstructions)`。字段搬移只有元数据面（字段属性表不随迁，ConstantValue 等同 (4)）。
6. **全有或全无**：搬移失败时目标会话可能留有刚挂入的裸元数据成员——失败返回 Err 由调用方决定收尸（remove）或整体放弃会话；库不在失败路径里做静默回滚（与 lower 的预检纪律一致的显式失败面）。

### 34.4 从零建类与 writer 从零分支（A5）

- **makeSession(NewClassSeed)**：空类起步的会话入口。种子 = {version (major<<16|minor), access, thisName, superName, interfaces}；this 名空/名字超 u16/access 超域显式 `Err(InvalidSeed)`。CP 引导在构造期完成：this/super/接口的 Utf8 + Class 双条目即时落位（池此刻为空，无去重顾虑），backing 从第一刻起就是自洽模型；unit 的元数据镜像走既有构造路径，两套状态天然一致。superName 空串 = superClass 索引 0（Object 自身形态；普通类显式给 "java/lang/Object"）。成员面从零开始，类级元数据改动仍走 commitClassMetadata。
- **writer 从零分支**：MissingSource 收窄为「确实要走 copy-through 而源缺席」——判定 = 任一 CP 条目未脏（copy 原 span）或任一属性槽树含 copy 路径（槽未置脏，或重编码 Code 体里嵌有未触碰槽）。全合成模型每个条目都在新编码态，无源字节照常写出。写回策略仍是「只有一种」：从零模型没有未触碰字节，自然不经过任何 copy；这不是「全部重编码」模式的引入。
- **行为级门**：从零建的类（最小 main）java 真跑出输出；跨类搬移场景（javac 产物两成员甲搬乙 + 消费者 owner 重映射）宿主隔离落盘 java 真跑与搬移前 oracle 等价，且全方法体扫描零残留供体引用。

## 35. ChangeSet 协议：会话变更收集与按粒度查询（2026-10-14，E2）

§9 的消费者（reference_index、摘要、调用图）在一次会话/一批变换后需要知道「什么变了、变到什么粒度」来决定失效重建范围，而不是每次全量重扫。本节定形 libjbc 侧的变更收集与查询协议（`jbc/ir/change_set.hpp`），以及它与 §34 generation 失效协议、E1 引用索引的衔接。

### 35.1 变更粒度分级（脏检查面）

每个会话自带一个 ChangeSet（观察面，恒开启、无开关），由会话的各变更通道在**实改发生时**记账。粒度按 libjbc 实际存在的脏检查面定级：

| 种类 | 记账点 | 实改判定 |
|---|---|---|
| `ClassCreated` | makeSession(NewClassSeed) 成功 | 无条件（整类皆新） |
| `ClassMetadata` | commitClassMetadata | version/access/this/super/interfaces 任一与 backing 旧值不同 |
| `MemberAdded` | addMethod/addField 成功（adopt 两入口经它们记账） | 无条件（挂入即新成员） |
| `MemberRemoved` | removeMethod/removeField 成功 | 无条件（摘除即失成员） |
| `MemberMetadata` | commitMethodMetadata/commitFieldMetadata | 名字/描述符/访问标志任一与 backing 旧值不同 |
| `MethodBody` | lowerMethod 成功 | code/异常表/maxStack/maxLocals 任一实改（三者全同即 copy-through，不记账） |

显式不记账的面（都是观察而非变更）：promote（升格不改模型）、未 lower 的 CodeDom 编辑（写回字节不变即无变更——body 粒度以 lower 落盘点为准，不追 IR 期逐指令事件）、失败通道（预检拒绝不留半截状态，无可记变更）。`adoptMethod/adoptField` 只在**目标**会话记账（挂入面）；源会话不记 remove——搬移是拷贝不是摘除，源成员删除是消费者后续动作。

### 35.2 身份坐标与寿命

记录里的成员身份是 `(MemberKind, name, desc)` 三段文本视图，不用 MethodId/FieldId——§34 的身份句柄是会话内坐标（slot/generation 随会话消亡），消费者的索引按文本内容寻址（与 E1 MemberKey 文本契约同构）。视图指向会话驻留存储（SymbolPool arena / backing CP arena，均只增不移），存活期 = 会话；**消费者必须在会话存活期内完成折叠与 intern**（跨会话持有须自行拷贝）。`MemberMetadata` 记录额外携带改写前身份（prior）：改名时旧身份从 backing CP 原样解析（append-only 契约保证旧条目仍在）。

### 35.3 累积与查询语义

原始记录按发生序追加（`records()`，审计面）。查询面给折叠视图，语义按「同一身份键的事件序」定义：

- **键** = (member kind, name, desc)。会话不变量（成员唯一性预检）保证任一时刻每键至多一个存活成员，因此键上事件序可折叠成终态。
- `MemberAdded(k)`：k 进入存活态，`created` 置位，body/metadata 旗标复位（新的一代从零计）。先 add 后 remove 再 add：终态存活、`created` 置位——中间代的历史不外泄。
- `MemberRemoved(k)`：k 进入消失态；终态消失的键落在 **removals** 桶。
- `MemberMetadata(k_new, prior)`：fold 视作两个原子事件——`prior != k_new` 时先对 prior 发消失事件（旧身份的事实必须失效），再对 k_new 发存活事件（`metadataChanged` 置位）；未改名（仅 access 变）只发后者。
- `MethodBody(k)`：k 存活时 `bodyChanged` 置位。

查询出口三件：`classChange()`（None/Created/Metadata，Created 优先——从零会话的消费者按整类重建）、`memberChanges()`（终态存活且被记过账的键，带 created/bodyChanged/metadataChanged 旗标）、`removedMembers()`（终态消失的键，含改名让出的旧身份）。同一键按终态只落一个桶，消费者无需再排序。

### 35.4 与 §34 / E1 的衔接

- **generation 协议**：ChangeSet 是失效协议的消费侧投影。§34 解决「会话内旧句柄不复活」（slot/generation 结构性失配），ChangeSet 解决「会话外消费者按粒度失效」——两者以文本身份坐标交汇：删除成员时 generation 自增（句柄面）与 MemberRemoved 记账（文本面）同点发生。
- **E1 引用索引**：mjbase ReferenceIndex 提供 `applyChangeSet(类名, 会话, ChangeSet)` 局部重建——method 粒度变更按键精确替换（删旧声明与以该成员为 source 的引用事实、重扫该成员），field/类级变更重扫类的类级事实面（声明 + 类作用域类型引用），`ClassCreated/ClassMetadata` 重扫整类。补丁只动主表，派生查询图统一重建（不重解析任何未变更类的字节码）。
- **硬验收（等价性证明）**：同一批变换后，「按 ChangeSet 局部重建的索引」与「变换后容器全量重扫的索引」六张主表逐项一致（声明/方法引用/字段引用/类型引用按规范化序比较）。此证明在两层钉死：libjbc 单测钉各通道的记账形状；mjbase 侧单元等价 + 真实 pass 流水（NopRemover 管线经 sink 逐类折叠）等价。漏记任一粒度必须使等价性测试翻红（red-side 自检入账）。
- **观察面不动字节**：ChangeSet 只读模型状态，不触碰任何写回路径；字节恒等契约由既有语料门继续兜底。管线接线点（GlobalToClassPassAdaptor 的 changeSink）在会话存活期内回调；作业级原子失败（默认档位抛出、无写回）时已折叠的消费者状态必须整体作废——sink 契约的一部分，由调用方承担。

## 36. CP 显式压缩逃生口（2026-10-14，A8）

CP 只追加 + 大量改名/搬移场景会把池撑大逼近 u16 容量上限（65535），此前只有 writer 编码闸门拒绝、没有压缩入口。本节定形显式压缩契约（`jbc/ir/cp_compact.hpp` 的 `compactConstantPool`）：消费者主动调用的**类级后置步骤**，不集成进 lower——lower 保持「PC 坐标重定位」单一职责，压缩是「CP 坐标全量重写」的另一族变更，两者正交（§33 管布局、本节管索引），入口分开。

### 36.1 铁律与封闭集

「不做 writer 全量重编码（未建模属性静默丢数据）」（dev_plan「明确不做」）是铁律。压缩重索引会触碰所有携带 CP 索引的属性字节，因此与 §33 同哲学：**可压缩类是封闭集**——全部属性可结构化重写时才可用；封闭集之外显式 `Err` 拒绝（带肇事属性名），不静默丢、不猜、不改契约放行。

压缩侧封闭清单（按属性 kind 分派，三层）：

1. **typed 属性**（attribute.hpp 十六种，走既有 typed 体 + 既有编码器）：Code（含异常表 catchType、指令 CP 操作数、嵌套属性递归）、BootstrapMethods、LNT、SMT（仅 `Object` 载荷是 CP 索引；`Uninitialized` 载荷是 pc，不得误重定位）、简单注解、LVT/LVTT（name/desc 列）、Signature、TypeAnnotations、CRT，B3 四件结构元数据（SourceFile 单 u2、InnerClasses 行三 CP 槽——access_flags 是访问位不动、EnclosingMethod 两槽、Exceptions 全槽），以及 Nest 族三件（NestHost 单 u2、NestMembers/PermittedSubclasses 全槽 Class 表，§32.10）。其中 TA 的 `throwsIndex`（Exceptions 表行下标）、`supertypeIndex`（interfaces 下标）、`catchIndex`（异常表条目下标）**不是 CP 索引**，不动。
2. **压缩侧结构化属性**（一件）：ConstantValue。它仍无 typed 模型，压缩层自带其结构知识（u2 常量索引，JVMS 4.7.2），从槽体字节原位重写索引、经 RawAttr 字节面写回。B3 四件曾与它同列 Raw 名单五件；四件立 typed 后其本地解析已删、分支退役，压缩层收编为「走 typed 体」（§36.5）。SourceFile 的 Raw 名单分支随其接线批保留为不可达兜底（typed 分派下 Raw 形不可达）。
3. **零长透传**：任何未知名属性若体为空（Deprecated/Synthetic 的合法形态），结构上不可能携带 CP 引用（无字节），透传不拒——按「构造性安全」放行，非猜测。仅属性名索引参与重映射。

清单之外（非零长未知/厂商属性）一律 `Err(AttrNotModeled)` + 属性名定位；清单内但结构畸形（长度不符/截断）`Err(MalformedAttr)` + 属性名。trailing 保留字节（§27.1）不拒：它们是结构终止后的畸形残留，写回逐字节保留（与 lower 重定位的既有口径一致），其内容按「不可解释」处理而非「按旧坐标系解释」。

### 36.2 算法：闭包标记 + 等价类代表 + 子序映射

全有或全无分两相：**计划相**（只读：解码全部属性面 → 校验封闭集 → 标记引用闭包 → 构造新池与映射；任何 Err 不留下半截模型）与**应用相**（按计划换池 + 重写全部引用面；应用相的每个写入值都在计划相定形，构造上不可失败）。失败后写回字节与进入前逐字节一致（解码不置脏，与 lower 预检纪律同构）。

- **引用面根序**（决定「谁活」，不影响新索引）：类头（this/super/接口）→ fields（name/desc/属性序）→ methods（同）→ 类属性序 → 升格体的 DOM 面。属性体内按声明序：属性名 → 指令（pc 序）→ 异常表 → 嵌套属性。
- **子序 + 代表去重（映射核心）**：新池 = 活条目按**老索引升序**的子序列；内容等价类（Utf8 按字节内容、Integer/Float/Long/Double 按位模式——-0.0 与 NaN 载荷保真、复合条目按 (tag, 子条目类号)；MethodHandle/Dynamic/InvokeDynamic 键含非 CP 的 a 列（reference_kind / bootstrap 下标），递归类号只进引用列）只保留**最小老索引作代表**，重复并入代表槽。等价类号递归带备忘，升序扫旧池赋秩：代表拿新槽（Long/Double 双槽在秩空间同步占两席），重复的新索引 = 代表的新索引。
- **单调性是结构性结论**：代表的秩 ≤ 其老下标（子序），重复并入更小老下标的代表槽——对所有存活条目 **newIdx ≤ oldIdx** 恒成立。推论：LDC（u1 域）压缩后必然仍可编码，不存在「压缩把 ldc 挤出 u1」的失败面。这条不变量**不允许**用「按规范根序边遍历边落位」的方式凑——根序的落位次序与老索引序无关，先遍历大老索引条目会把新索引推高过其后遍历的小老索引条目，ldc 直接溢出（此类形态边遍历边 intern 的方案一律不得回潮）。
- **去重键列纪律**：MethodHandle（a=reference_kind）、Dynamic/InvokeDynamic（a=bootstrap 下标）的 a 列不是 CP 引用——键中原样携带，等价类递归**只进 b 列**。把 a 列误当引用递归等类，会让「同 kind 异目标」的句柄并成一条（lambda bootstrap 的 impl 句柄并入 metafactory 句柄），结构自洽而语义已毁，只有 JVM 真跑能抓住。
- **死条目清除**：可达闭包（根序遍历触及的条目及其传递引用）之外的条目一律不落入新池——改名/搬移留下的旧 Utf8→Class→NameAndType→ref 死链是池膨胀的主因，只去重不清死则压缩无法收敛。Dead 条目的安全性由封闭集保证：引用面全部结构已知，不可达 ⟺ 不可引用。
- **指令形态不收缩（挂账）**：LDC_W/LDC2_W 等 wide 形态指令只改操作数字面，不降格为 LDC——形态变化会让「压缩前后指令序列等价」这一语义面引入第二种坐标（指令身份变化），保守裁决留给未来独立成文（与 §34.3 ldc 自动 LDC/LDC_W 的「写入侧按需选宽」不冲突：那是语义 intern 的新增点，这是既有字节的重写点）。压缩后索引变小令形态冗余合法存在，JVM 按编码形态解码，不受影响。
- **无变化早退**：映射为恒等且无死条目、无重复时（池已紧凑），整个压缩退化为 no-op——不换池、不改槽、不记账。推论：对无死条的 javac 级类，膨胀后压缩逐字节还原原类。池的全量重编码是字节安全的（CP 条目是定长格式，无未建模字节；与属性不同），但 no-op 早退让「压缩过紧池」的写回继续走 copy-through 保真路径。

### 36.3 会话一致性：升格体、语义索引与文本视图存活期

- **promoted CodeUnit 同步**：指令 CP 操作数以 backing 索引为坐标（§6.2），压缩换池后已升格方法的 `Insn.cp` 与 `CodeUnit::exc[].catchCp` 由压缩入口按映射同步重写——会话内不存在「IR 与 backing 索引坐标系分裂」的窗口。
- **升格方法的引用面以 DOM 为准**：升格后 backing Code 属性体的指令流与异常表是**陈旧面**——其中的引用不进闭包（DOM 编辑替换掉的旧常量应当死在压缩里），应用相也不重写陈旧体（writer 对升格槽拒绝直写、lower 整体重建，陈旧体不可达输出）。DOM 的指令操作数与异常表是**新增引用面**：会话期语义 intern 的条目可能只被 DOM 引用，漏标记会让应用相的重映射落 0。嵌套属性面（LNT/LVT 等）在 lower 中按 pc 重定向保留，引用仍活，照常进闭包。
- **SemIndex 失效**：语义 intern 的去重索引缓存 CP 索引，换池后强制重建（惰性），后续 intern 家族在新池上命中/追加。
- **旧池退居（retired）**：换池不是销毁——旧池移入会话的 retired 存储。§35.2 的文本视图存活期契约（「backing CP arena 只增不移」）依赖池的存活：改名记录的 priorName 视图指向旧池 arena/源字节，压缩不得使其悬垂。旧池退居而非拷贝（arena 块链整体移交），成本为一次指针移交。
- **ChangeSet 记账**：压缩成功（非 no-op）记一条 `ClassMetadata`。裁决理由：(a) 语义上压缩改写全类所有 CP 坐标，每个成员级缓存事实都失效，§35.4 的消费者协议对 ClassMetadata 的动作恰是「重扫整类」——与新粒度会得到的行为完全一致；(b) mjbase 侧 `applyChangeSet` 对既有枚举已有等价性证明，新增枚举会让既有消费者静默漏失效（switch 未覆盖 = 不重扫），违背「漏记必翻红」的硬验收。压缩的记账点在会话通道内，与其他通道同款「实改才记」（no-op 不记）。
- **无源类（A5）同样适用**：makeSession(NewClassSeed) 的全合成模型无源字节，压缩走同一入口——换池后所有条目均在新编码态，writer 从零分支照常写出。

### 36.4 验收硬门

- 语义等价：压缩后类 re-parse 恒等（语义面）+ JVM 真跑（构造 CP 膨胀样本，压缩前后行为一致；反射面——签名/注解/泛型——一致）。
- 确定性：同一输入压缩两次产物逐字节一致；膨胀池压缩后条目数下降（去重 + 清死生效）；no-op 早退时写回与原字节逐字节一致。
- 未触碰路径字节恒等：不调压缩，一切照旧（既有语料门兜底）。
- Raw 残留拒绝：手造含未知属性的类 → `Err(AttrNotModeled)` 带属性名（不是静默）；畸形五件结构 → `Err(MalformedAttr)` 带属性名。
- 语料门双模式：自产语料 + env 注入 ares 全语料跑压缩门，拒绝面逐点名统计，只允许出现真未知属性。

### 36.5 Raw 名单收编：B3 四件入 typed 面（2026-10-14，B3 接线批）

§36.1 第 2 层挂账的收编在本批结清：SourceFile/InnerClasses/EnclosingMethod/Exceptions 四件立 typed 模型并接 `decodeAttrBody` 分派后，压缩层两相 switch 补齐四件的 typed 分支（SourceFile 随其接线批先落；三件随 B3 接线批与 mjbase 消费面迁移同批落），本地解析（Raw 名字分派的对应分支）按挂账退役——名单内只剩 ConstantValue 仍走 Raw 字节面。裁决要点：

1. **退役的前提是会话内 Raw 形态灭失**：压缩分派按 body kind 走，槽若被生产者以 `makeNew(name, RawAttr)` 新建（如 class_mutation_service 旧写路径），plan 相会以 Raw 形遇到该属性——名单分支一旦退役即 `AttrNotModeled` 诬拒。因此退役与「最后的生产者改立 typed 体」必须同批（A8 教训的压缩器侧推论：分派接线、消费面迁移、压缩收编、生产者形态四者一体）。
2. **畸形拒绝面语义不变**：四件体畸形（半截/声明超实）在 typed 解码层显式 `Err(Truncated)`，plan 相归一为 `MalformedAttr` + 属性名，与 Raw 名单期的拒绝语义逐点同形。
3. **SourceFile 兜底分支保留**：其 Raw 名单分支随 SourceFile 接线批保留为不可达兜底（typed 分派下 Raw 形不可达），不属本批退役面。

### 36.6 Nest 族三件入封闭集与拒绝面收缩（2026-10-15）

NestHost/NestMembers/PermittedSubclasses 立 typed（§32.10）后，压缩层两相 switch 补三件的 typed 分支（NestHost 单槽、两件 Class 表全槽），走「闭包标记 + 内容等价类 + 老索引升序单调映射」既有通道。会话内 Raw 形态灭失的前提由同批消费面迁移保证（remapper 手术与 reference_index 收集全部改走 typed 体；无生产者以 `makeNew(name, RawAttr)` 新建这三件），Raw 名字分派无对应分支、自然落入名单外拒绝——与 B3 四件的名单退役不同，这三件从未进过 Raw 结构化名单。

ares 语料压缩拒绝面逐点名分布（env 注入模式实测，452 类 = 447 主语料 + 5 自产 nest 语料）：

- 批前（v0.4.12 时点，447 类）：NestHost=149、NestMembers=52、MethodParameters=54，rejected=255、compacted=63、unchanged=129。
- 批后：MethodParameters=54（名单外唯一残余），rejected=54、compacted=84、unchanged=314、malformed=0、dishonest=0。

消失的 201 个拒绝点全部转为压缩成功（149+52），MethodParameters 保持 Raw 不动（不在本批建模面）。自产语料侧 nest 语料 5 类全部可压缩；新分布随语料变动漂移，以语料门实测为准。

### 36.7 Record 入封闭集与拒绝面不变性（2026-10-16）

Record 立 typed（§32.11）后，压缩层两相 switch 补 Record 分支：闭包标记与重映射都下沉到组件粒度（组件名/描述符两 Utf8 槽 + 组件嵌套属性递归走既有 planAttr/applyAttr），Raw 名单外无对应分支、自然落入名单外拒绝——与 Nest 族同款，Record 从未进过 Raw 结构化名单。

ares 语料压缩拒绝面逐点名分布（env 注入模式实测，452 类 = 447 主语料 + 5 自产 nest 语料）：批前后均为 MethodParameters=54（名单外唯一残余）、rejected=54、compacted=84、unchanged=314、malformed=0、dishonest=0——类级 Record 属性（语料门 env 实测 57 个）不出现在逐点名拒绝中——没有任何类因 Record 被拒，Record 入封闭集不改变拒绝面。record 自产语料不并入压缩扫描语料（record 规范构造器恒带 MethodParameters，在到达 Record 前即拒绝，并入无增益）；Record 压缩分支（组件槽去重/重映射 + 嵌套属性递归）由 crafted 用例覆盖并钉住「组件描述符与嵌套 Signature 等值文本去重成同一条目」面。新分布随语料变动漂移，以语料门实测为准。

### 36.8 Module 族三件入封闭集与拒绝面（2026-10-16）

Module/ModulePackages/ModuleMainClass 立 typed（§32.12）后，压缩层两相 switch 补三件的 typed 分支：Module 全段闭包（名/版本 + requires 模块与版本 + exports·opens 的包与 to + uses + provides 服务与实现，旗语位与 trailing 不含 CP 引用）、ModulePackages 全槽（CP Package）、ModuleMainClass 单槽（CP Class），走「闭包标记 + 内容等价类 + 老索引升序单调映射」既有通道。跨条目类型面有一条结构性边界：去重等价类按（tag, 内容）归类，CP Module/Package 条目与其名字 Utf8 永不等价、各归各——等值文本去重只发生在同 tag 条目之间（module-info 产物的 requires 版本串与模块名同文本时，Utf8 侧合一而 Module 条目独立保留）。

ModuleHashes/ModuleResolution/ModuleTarget（JDK 内部件，§32.12 不建模裁决）维持 Raw 透传并留在封闭集外：携带它们的类在计划相显式拒绝（点名属性名），与批前行为一致。ares env 语料无模块面，逐点名拒绝分布不变；module 自产语料（--release 17，module-info + 三类源）并入压缩扫描——全部类无名单外属性，构造上可达压缩成功路径。ModulePackages/MainClass 的压缩分支 javac 不可产，由 crafted 用例覆盖（含 uses 与 ModuleMainClass 同条目共享、exports 与 ModulePackages 跨属性同 Package 条目、等值 Utf8 去重与死条目清除面）。

## 37. 落地窗口：类构建挂载面、BSM intern 家族与消费者协议（2026-10-17）

堆叠期内（基线 v0.4.15，发版前不 bump）三批补丁的临界条款；过程账目见 dev_plan.md，契约锚点在代码头注释。

### 37.1 类构建挂载面（attr_build，批 1）

ClassSession 公开挂载 API（attachConstantValue / attachMethodParameters / attachParameterAnnotations / attachAnnotationDefault / attachAnnotations × {可见,不可见} / attachAttr / attachClassAnnotations / attachClassAttr），语义规格（ConstValueSpec / ParamNameRow / SemElemValue 全形态 / SemAnnotation）向 typed AttrBody 合成，CP 经 SemIndex 去重 intern。全链「先算后写」；挂载预检的类级单例规则封闭表（LNT/LVT/LVTT 多条，"Code" 拒收 → BadMount）在 §27 之上执行。两条硬契约：

1. **描述符 arity 闸**：MethodParameters 行数 / ParameterAnnotations 参数组数 = 方法描述符参数个数（JVM 反射对失配显式抛 MalformedParametersException）——合成面提前 Err(InvalidSeed)。
2. **JVMS 对照修正（javap 对照实验钉死）**：注解 element_value 的 's' 与 'c' 的 const_value_index **直指 Utf8**（javac 同形），不隔 String/Class 条目——与 ConstantValue 属性走 String 条目分账。

JVM 真跑门（反射读回参数名/旗语/全注解族/枚举常量混合部署）已入 class_build_attrs_test。

### 37.2 BSM 语义 intern 家族（批 2）

五件去重 intern（internMethodHandle/MethodType/Module/Package/Dynamic，SemIndex 三张去重表 + Dynamic 入 BSM 下标占 a 槽的 ref 表）；MH 域闸 1..9 封闭表按 kind 分流 Fieldref/Methodref/InterfaceMethodref，域外 Err(InvalidSeed)。appendBootstrapMethod = 属性缺则建（类级单例）+ 条目同形全字段一致复用 + loadable 十族参数落位；Dynamic 与孤立 internDynamic 均要求 BSM 锚点在位。压缩协同收编（markCp 对 MH/Dynamic/indy 只标 b）；孤立常量按死常量消除收掉。记账：实改记 ClassMetadata，纯命中复用不记账。

### 37.3 消费者协议面（consumer，批 3）

§9 最小协议的本体态落进 `jbc::consumer`：E3（IClassSource fail-closed + MemoryClassSource 存续视图 + SummaryScanner 骨架）、E1（ReferenceScanner 引用事实族导出）+ rename 跨类编排（RenamePlan 只读面 / applyClassRename 分族应用 / renameSource fail-closed 驱动）。**计划 formation 属消费方域**（§9）：注入 IRenameResolver，无计划的类原字节透传一字节不动。

1. **rename 分族封闭面（v1）**：类名族（CP Class 条目重指向 + renameClassConstant + 描述符列手术 + 签名/LVTT/TA 三件既有手术面）/成员族（成员行置换 + 代码 operand setMember/setClass）+ invokedynamic 跳过不计。String 条目不碰（字符串常量里的 «L名;» 是字面量事实）。
2. **BSM/MethodHandle 载体 fail-loud 闸**：成员规则命中 BSM 条目或 ldc MethodHandle 载体（且规则实改）即 Err(BsmTouched)——半支持状态的静默改坏拒之于趟前；类名族规则合法穿越（CP 重指向对句柄 owner 无损）。载体分解：CP MethodHandle（a=reference_kind，b=ref；ref.a=Class、ref.b=NAT）→ owner 走 Class 条目、name/desc 走 NAT 两 Utf8——owner 读数走 className(ref.a)，不得直接 utf8Of。
3. **驱动 fail-closed**：会话阶段（stage）产物全量替换，任何一环 Err 即丢弃会话不写半截态；resolver 自身 Err 原样外传。
4. **preserveSourceBytes 坑（探针实证）**：属性槽 raw span 依附源字节内存；只要后续要 lazy 解码属性/Code 体（ReferenceScanner、rename 应用面），read 必须保真源字节（ReadOptions 不得关 preserveSourceBytes）。SummaryScanner 不解剖属性体，保持 preserve=false 的最小占用态成立。
5. 站位坐标系：ReferenceScanner 的 ordinal/pc 是**原始 code 域**（decode 序/字节偏移），与 CodeDom::order() 的 IR 下标不同域（含 label/line 占位），消费方走会话 promote 自行映射。

测试面：consumer_scan_test / consumer_rename_test 合成直验（源契约/骨架/事实族站位/分族计数/BSM 闸两侧/驱动三 fail-closed 簿帐）；JVM 闸与真实语料的 consumer 回归归入 Grunt 验证（待办）。

## 38. 从零建方法门面（2026-10-20，issue #8 方法体部分）

§34.4 的建方法链路是 addMethod → promoteMethod → CodeEditor → lowerMethod 四段接线。门面只压缩接线层数，不改变错误语法：每个可失败操作仍当场返回该段原生 Result；不引入 sticky 首错（Result 之外的第二条汇报通道，失败后静默 no-op），也不收回调（库不执行调用方业务）。

### 38.1 API 面

- `MethodEditor::open(session, seed)`：addMethod + promoteMethod 一次接线，返回编辑面；编辑面持 CodeUnitRef 保活会话。
- `MethodEditor::method()`：新成员下标，可直接喂 promote/lower/removeMethod。
- `MethodEditor::append(Insn)`：直接转发 CodeEditor::append 的 Result<InsnId, EditError>。失败不改变编辑器状态，后续 append 照常。
- `MethodEditor::code()`：全量编辑面（标签/分支/switch/批量删除）。
- `MethodEditor::unit()`：CodeUnit 视图；maxStack/maxLocals 可在 commit 前调整。
- `MethodEditor::commit()`：显式 lowerMethod。析构不自动提交——析构无法汇报 LowerError，自动提交会把错误塞进第二条通道。
- `addMethodWithBody(session, seed, body)`：直线体一步入口，内部按 addMethod → promote → append × N → lower 同步折叠，遇首个错误立即返回；body 不得携带控制流指令。

### 38.2 复合错误

MethodBuildError = Stage{Add, Promote, Edit, Lower} + 该段原族错误码（Edit 段另带 0 基 insnIndex），与 SessionError 同构。它只由复合入口返回一次，不驻留对象，不构成第二种错误语法。单段操作（open 之后的 append、commit）直接返回 CodeEditor / lowerMethod 的原生 Result。

### 38.3 失败面

1. **控制流前置拒绝按指令形状**：Branch2/Branch4/TableSwitch/LookupSwitch，以及非控制流指令上残留的 targets/keys，在挂入成员前 Err(Stage::Edit, BadOperand, insnIndex)。不能按 target 是否为空判断：未绑定分支的 target 恰是 kNoLabel，按目标判会把坏体放行到 append 阶段。
2. **失败回滚成员**：addMethodWithBody 的 addMethod/promote/append/lower 任一步失败都摘除已挂入成员，贴合「失败不留半截」；CP 追加不回退（常量池只增语义）。这与 §34.3(6) adopt 的「调用方收尸」不同：一步入口拥有整条建体管线，失败必须自己收干净。MethodEditor::open 只在 promote 失败时回滚；open 之后的 commit 失败原样返回 LowerError，不摘成员也不回退编辑，调用方可修正后重试或 removeMethod。open 之后的放弃不是失败路径：成员已挂入，不 commit 直接析构会留下空体成员，需调用方 removeMethod 收尸。
3. **种子闸**：open 与 addMethodWithBody 都要求 withCode = true；abstract/native 没有方法体，不属于本入口。maxes 由 seed 给定，本面不代算栈图。

### 38.4 范围与测试

本批只落方法体链路：类级 ClassDraft、与 test::ClassBuilder 的字节对拍、mj2x/mjt 消费面迁移不在内，现有 A4/A5/attach 签名不动。测试 method_editor_test 六件覆盖：批量建体重读、编辑器 append-commit 重读、放弃编辑器留空体成员（收尸归调用方）、append 失败不污染编辑器（无 sticky 状态）、控制流前置拒绝与下标报告、种子失败不留成员。

## 39. 区间搬移原语 moveRange（2026-10-28，issue #10）

CodeEditor 的搬移面原是单指令粒度（`moveBefore`/`moveAfter`）；逐条循环搬段时锚定边界随每条搬移漂移，语义不可组合，eraseBatch + 重建又会打断标签锚定。`moveRange(first, last, at, policy)` 把 `[first, last)` 连续指令段（段内相对顺序不变）整体搬到 at 之前，补上区间粒度。

### 39.1 分层与标签随迁

- **`CodeDom::moveRange(first, last, at)` 是机械层**：单趟提出回插重排 `order_`（O(n)），不触碰指令槽与标签锚定。标签锚在指令句柄上，段内标签随段迁移、段外分支指向段内标签的引用保持有效，均由句柄锚定自然成立，无需改写任何字段——这也是它区别于「eraseBatch + 重建」的根本：后者面向删除语义，引用改锚走新标签替换；前者不动任何引用。
- **`CodeEditor::moveRange(first, last, at, policy)` 是策略层**：段几何之外只裁决段首边界标签的去向。段内（非段首）标签不进策略——它们锚定段内指令，随段迁移本就是顺序重排的默认语义；歧义只在段首（它既可读作段的入口，也可读作原段位的标记）。
- **尾哨兵**：`at == kNoInsn` = 搬到方法末尾、`last == kNoInsn` = 段延伸到方法末尾，与 createLabel/bindLabel 的末哨兵约定同源——half-open 端点语义下尾段只有经哨兵可命名，而尾段恰是块搬移最常见的形态（GotoRearranger 换位消除 GOTO）。

### 39.2 段首边界策略（MoveRangePolicy）

- **kHeadFollows（默认）**：段首标签随段迁移，锚定关系不动。段内含标签且被段外分支引用时，搬移后引用仍指向段内同一逻辑位置。
- **kHeadStays**：段首标签全部（Boundary 与 ControlFlow 两角色）留在原位承接原 fallthrough，锚点重定向到**原段位后继**（原 exclusive 端指令，即搬走后顶替原段位的指令）；引用段首标签的分支跟着重定向。标签句柄身份不变。段居尾（last 为尾哨兵）时承接者是方法末边界（改绑 end）。无操作（锚点即段首或紧随段尾）整体不触发改绑——没有位移就没有「留原地」可执行。

### 39.3 拒绝面与原子性

- **BadRange**：空段（first == last）、倒置段（last 在 first 之前）、锚点落在段内（锚点自身会被搬移卷走，语义未定义）。不连续性由构造排除：段 = order 中 pos(first) 到 pos(last) 的全部指令，无从 clamp。
- **StillUsed（kHeadStays 专属）**：异常表 start/end/handler 引用的段首标签不可机械改绑（改边界 = 改保护区间语义，与 erase 的 fallthrough 策略同规，调用方先显式 bindLabel/destroyLabel）；段居尾时仍被 branch/switch 控制字段引用的段首标签同样拒绝——分支目标须锚定存活指令是 checkHandles/exportRaw 的 standing 不变量，end 锚定的控制目标连导出都无法表达。kHeadFollows 无 StillUsed 面：不改写任何引用，异常边界随段整体迁移是标签随迁的自然结果（区间与异常保护区的一致性归调用方，与单条 moveBefore 同责）。
- **原子性**：句柄存活（InvalidHandle）、段几何、策略冲突全部在变更前校验；变更阶段只有标签改绑与单趟重排，两者的失败条件已被校验排除，任一步失败不留半搬状态。
- 测试 ir_test 五件：正向搬移含分支标签随迁（lower 重读布局逐位成立）、kHeadStays 承接者改绑与尾段 end 边界、坏几何拒绝与原子性（失败后布局原样可继续用）、异常表边界双策略对照、与 insertAtLabel/eraseBatch 组合。
