# Classfile 基础设施决策矩阵

classfile 库的难点不在于把 JVMS 的字段翻译成 C++ 结构体，而在于先确定哪些信息是编码事实、哪些信息是可编辑身份、哪些信息属于全程序分析。ZKM 的研究报告说明了一个重要事实：它的很多设计服务于“长期驻留、反复原地变换、最后统一输出”的混淆工作台；`libjbc` 需要吸收其中关于身份、回退和验证的机制，但不能把全程序仓库、观察者链和变换引擎一起塞进单类读写库。

这十二个决策点不是十二个互相独立的开关。表示模型决定变更模型，保真策略决定状态追踪和错误处理，句柄决定嵌套容器与引用重写的边界，全程序视图又决定交叉引用、可达性、去重合并和输出重排应该由谁拥有。下面先冻结职责，再讨论每个决策。

## 结论

`libjbc` 应该是“单类、可拥有、可诊断、可编辑、可验证、可保真写回”的 classfile 基础设施；`mbase` 应该是“跨类、跨方法、可重建、可提交”的程序分析和变换基础设施。两者之间通过稳定的符号键、引用事实、变更集和显式的 promote/lower 协议连接，不能通过 `ClassFile*`、`Insn*` 或 CP 数字索引连接。

十二个决策点的归属如下：表示层和变更层主要由 `libjbc` 固化，分析层由 `libjbc` 提供单方法事实而由 `mbase` 形成全程序视图，错误处理和扩展点由两层分别实现但遵守同一套 Result/诊断和能力声明协议。

| 编号 | 决策点 | `libjbc` 的裁决 | 主要拥有者 | 必须保持的不变量 |
| --- | --- | --- | --- | --- |
| 1 | 表示模型 | 混合：低层对象图加 lazy/raw 视图，高层 DOM/IR；不公开 visitor | `libjbc` | 事件顺序不是身份，未触碰字节可以直接复制 |
| 2 | 保真策略 | 未修改区域 copy-through，修改区域按 relocation contract 重编码，无法安全重定位就报错或 raw-only | `libjbc` | 不以“尽量写出”为理由猜测未知依赖 |
| 3 | 常量池模型 | 低层保留 class-local 索引和原始 span；语义层使用稳定 `ConstValue`/`SymbolId`；需要输出重排时显式 remap | `libjbc` + `mbase` | CP 索引不出 `ClassFile`，重排必须有完整映射 |
| 4 | 指令表示 | 低层 `RawInsn`；高层槽表加顺序序列，控制流引用使用 owner-scoped 边界句柄 | `libjbc` | 不保存 pc offset 作为编辑身份，不使用 intrusive `prev/next` |
| 5 | 编辑模型 | 单方法使用立即编辑；批量重排和跨类修改使用显式计划/事务 | `libjbc` + `mbase` | 每个结构性变更都有可检查的提交边界 |
| 6 | 状态追踪 | 属性使用四态 dirty 状态；会话产生 `MutationFootprint`/`ChangeSet`；不使用全局观察者链 | `libjbc` | 修改不会被遗漏，缓存失效由变更集驱动 |
| 7 | 句柄 | IR 使用 `{owner, slot, generation}` 值句柄；低层 CP 数字只在单类模型内有效 | `libjbc` | 跨 DOM、删除后和 ABA 句柄必须被拒绝 |
| 8 | 嵌套容器 | `AttrSlot` 统一承载 typed/raw/lazy 属性；扩展通过能力注册表，不为每种嵌套关系复制容器协议 | `libjbc` | 所有嵌套对象仍由单一 `ClassFile` 所有，未知依赖有足迹或被拒绝 |
| 9 | 全程序视图 | `libjbc` 只输出摘要和引用事实；类图、xref、调用图、可达性、去重合并、输出重排归 `mbase` | `mbase` | 全局索引不保存局部对象地址和 CP 索引 |
| 10 | 验证器 | 分层：classfile 结构验证、方法 IR 验证和帧计算在 `libjbc`；类型层级和全局语义验证在 `mbase` | 两层协作 | 每层只验证自己拥有的不变量，失败有明确层级 |
| 11 | 错误处理 | `Result<T, E>` 传递可恢复失败，`Diagnostics` 记录降级；不以异常表达恶意输入 | 两层 | Err 不产生可消费的半成品，Warning 不伪装成成功无诊断 |
| 12 | 扩展点 | 属性、指令和格式扩展使用 capability/registry；回调只做内部实现适配，不成为公共事件 API | `libjbc` | 扩展必须声明解析、写出、CP 重映射和 pc 重定位能力 |

## 表示层

### 1. 表示模型：低层真相与高层编辑模型分开

单一模型无法同时满足畸形 classfile 的保真和优化 pass 的编辑人体工学。低层模型必须能表达原始 CP、属性 span、未知字节和诊断；高层模型必须能表达符号、边界、控制流和可重算布局。`ClassSession` 是两者的桥，而不是第三份长期模型。

(1) 低层 `ClassFile` 继续采用当前的骨架 eager、属性 lazy、未知内容 raw 的混合模型。它适合 summary 扫描、raw patch 和未触碰类的 copy-through 写出，源字节和模型由一个 owner 管理。

(2) 高层 `ClassUnit`/`CodeUnit` 继续采用 DOM-first。`slots_` 拥有指令，`order_` 代表程序顺序，控制流和范围通过值句柄引用。`std::deque` 或等价的分块存储可以稳定普通访问指针，但外部身份永远是句柄而不是地址。

(3) reader 内部可以使用任何高效的扫描器或临时事件分派，但事件不得成为公共 API，也不得决定节点身份。公共 API 返回模型、视图或 Result，不把消费方变成事件接收器。

### 2. 保真策略：保真是能力声明，不是写出愿望

ZKM 的 raw fallback 和当前 `AttrSlot` 四态是正确方向。保真写回必须回答“这次变更影响了哪些编码坐标”，而不是只回答“这个属性有没有被解码”。

(1) 未修改属性、未修改 CP 条目和未受影响的源区间走 copy-through；这保证常量池顺序、未建模属性和非规范但可接受的字节不会被无意义重编码。

(2) 修改指令布局、CP 编号或成员结构时，受影响的已知属性必须由 relocation contract 处理。异常表、行号表、局部变量表、StackMapTable 和后续新增的类型注解都要声明它们依赖的是 pc、CP 还是两者。

(3) 未知属性至少保留原始 payload 和依赖足迹。足迹无法确定时，任何会改变 CP 或 pc 的编辑都应返回结构化错误；“把旧字节照抄过去”只有在其引用坐标没有变化时才安全。

### 3. 常量池：索引是编码坐标，语义引用必须脱钩

当前 class-local、只追加、原始 span 写回的 CP 模型适合保真变换，但它不应成为 IR 或 `mbase` 的身份系统。ZKM 的对象化常量池解决了反复变换的便利，却把所有权、替换通知和写出重建绑得很紧；`libjbc` 应保留身份稳定的好处，避免把父指针和观察者链扩散到所有节点。

(1) `ClassFile::ConstantPool` 的 `u32` 索引只在该 `ClassFile` 存活期间有效。任何跨类索引、方法身份或 xref 边都使用 `ClassKey`、`MemberKey`、`SymbolKey` 或 `ReferenceSite`。

(2) IR 中的常量操作数应逐步从 backing CP index 迁移到 `ConstValue`/`ConstId`。lower 时由 session 维护 `ConstId ↔ CP index` 映射；同一个语义常量可以在输出阶段合并，也可以在需要保留原布局时继续使用旧条目。

(3) 输出重排需要单独的 `ConstantPoolRemap`。它负责旧索引到新索引的全量映射，并向已知属性、未知属性足迹、指令操作数和 BootstrapMethods 传递；没有完整 remap 的模型只能走 append-only copy-through。

## 变更层

### 4. 指令表示：槽表加顺序序列是正确的核心

OW2 ASM 的侵入式双向链表适合 Java GC 和 visitor 管线，却把节点邻接、删除责任和 label 生命周期混在一起。在无 GC C++ 中，最重要的不是模拟 `InsnList`，而是让“拥有指令”“安排顺序”“引用边界”成为三个可独立验证的事实。

`Insn` 不保存 owner、前驱、后继或链表指针。`CodeDom` 拥有槽表和顺序序列；插入、删除和重排只更新这两个容器及引用关系。指令跨 DOM 搬运必须经过显式 detach/attach 映射，不能把包含旧 owner 的 `Insn` 直接塞进新 DOM。

分支和 switch 的最终目标应迁移到 `LabelId`，而不是继续把“跳到某条指令”当成唯一表达。`LabelId` 是 `{owner, slot, generation}` 的整数句柄，表示一个可重算边界；它不是 `LabelNode`，不需要独立释放，也不要求每条指令保存 owner 指针。基本块分析可以在 `mbase` 或 analysis 结果中为多个边界建立临时 `BasicBlockId`，但不应把临时块身份反向塞回 classfile DOM。

方法末尾必须是一等边界。`InsnId{}` 可以继续作为内部兼容哨兵，但不能同时表示“非法句柄”和“合法方法末尾”；正式 API 应返回带 kind 的 `Boundary`/`LabelTarget`，让 `End` 与 `InvalidHandle` 在类型和错误上分开。

### 5. 编辑模型：立即编辑用于单方法，事务用于全局变更

单个方法的插入、重排、重定向需要低摩擦的立即 API，当前 `CodeEditor` 的方向正确。跨方法改名、删除、去重和输出重排则需要先得到完整影响集合，再统一提交；把全局事务塞进 `CodeDom` 会破坏单类所有权边界。

(1) `CodeDom` 提供单条插入、按边界插入、顺序移动、引用重定向和结构性删除。默认删除遇到 incoming use 返回 `StillUsed`，不做隐式修补。

(2) `CodeEditor` 可以增加批量编辑计划，先收集 use、边界和 relocation 影响，再一次性执行。这样既能避免循环删除的重复扫描，也能让失败发生在提交前。

(3) `mbase` 负责跨类 `ChangeSet`、stage、commit 和 rollback。`libjbc` 不提供 jar 级 COW，也不维护永不释放的全局 owner。

### 6. 状态追踪：dirty 状态加变更足迹足够，不需要观察者网络

ZKM 的观察者链在它的集成式混淆工作台中有价值，因为类图、索引和缓存都挂在同一个对象宇宙里。`libjbc` 采用按需重建和显式 session 更容易证明正确：属性槽用四态，方法编辑产生变更足迹，`mbase` 根据足迹失效并重建索引。

`MutationFootprint` 至少应能表达：成员元数据是否改变、CP 内容是否改变、代码布局是否改变、控制流是否改变、异常/调试边界是否改变、未知属性是否受影响。`ChangeSet` 在 class/session 边界汇总这些事实，并作为 xref、帧、摘要和 writer 选择路径的输入。

### 7. 句柄：owner 应在句柄里，不应在每条指令里

用户担心的 owner 重复存储只在“每条指令保存一个 owner 指针”时成立；当前设计把 owner 放在 `InsnId`/`LabelId` 值中，普通 `Insn` 不保存 owner。这样每条指令没有额外指针，跨 DOM 误用又能在 API 入口被拒绝。

`{owner, slot, generation}` 比裸 `u32` 多占一些引用空间，但它同时解决跨容器校验、删除后失效和 ABA。除非实测证明控制流密度使句柄占用成为瓶颈，否则不应为节省几个字节退回裸 slot。需要压缩时可以把句柄编码为 `u64` token，语义不变，且仍保留 owner 和 generation 的校验。

低层 CP index 不需要 generation，因为它不是可变 IR 的公共句柄；当 CP 支持删除或全量重排时，必须通过 remap 对象一次性更新引用，不能让旧数字继续充当身份。

### 8. 嵌套容器：统一属性槽和能力接口

属性、Code 内属性、注解嵌套值和未来扩展都应遵循同一条拥有链：父容器拥有子容器，子容器不需要知道全局仓库。`AttrSlot` 已经提供了 lazy、typed、raw 和 dirty 状态的共同外壳，这比给每一种属性单独发明生命周期协议更稳。

扩展点应声明四类能力：能否解析、能否编码、依赖哪些 CP 索引、依赖哪些 pc/边界字段。未知属性若只提供 raw，就只能在坐标不变时 copy-through；如果扩展注册了 relocation handler，writer 才能在 CP 或代码布局变化时安全重编码。

## 分析层

### 9. 全程序视图：必须由 `mbase` 拥有

ZKM 把类图、override、名字预留和引用图放进同一个 repository，是因为它本身就是混淆引擎。这个事实不能被误读成“classfile 库也应该持有全局图”。`libjbc` 的职责是提供稳定、可重复扫描的局部事实；`mbase` 才拥有跨类的解释、策略和提交。

`mbase` 应建立以下稳定键和边模型：`ClassKey`、`MemberKey`、`SymbolKey`、`ReferenceSite`、`ReferenceKind`、`ReferenceResolution`。解析结果至少区分 Resolved、Unresolved、Opaque 和 Ambiguous；未知属性中的 CP 足迹也要作为 Opaque 引用事实进入索引，而不是被忽略。

交叉引用图、调用图、override 图、可达性分析、去重/合并和输出重排都建立在这些稳定事实之上。编辑后默认从受影响 class 全量重建事实，再由 `ChangeSet` 缩小重建范围；只有局部重建与全量重建的等价性有测试证明后，才允许把局部路径作为性能优化。

去重和合并不能依赖局部 slot、CP index 或对象地址。它们应使用规范化的语义 key 和确定性 tie-break，输出阶段再把选定的语义对象 lower 成每个 class 自己的 CP 和属性布局。

### 10. 验证器：三层验证，不造一个万能验证器

第一层是 classfile 结构验证：magic、版本、长度、CP tag/引用、属性边界和 writer 编码容量。这一层属于 `libjbc::core`；结构损坏在此层 `Err` 拒绝，「结构可确定、语义不合规」降级为 `Recovered` 加诊断（两态，无 RawOnly）。

第二层是方法 IR 验证：句柄 owner/generation、边界合法性、指令形状、控制流目标、异常范围、栈帧和 relocation contract。这一层属于 `libjbc::ir/analysis`，可根据 `FramePolicy` 选择 Strict、BestEffort 或 Preserve。

第三层是全程序语义验证：类型层级、override 闭包、调用解析、入口根、可达性、反射不确定性和跨类变更的一致提交。这一层属于 `mbase`，需要 `ITypeResolver` 或 ClassIndex，不能由单类 writer 猜测。

## 基础设施

### 11. 错误处理：Result 表达控制流，Diagnostics 表达降级

恶意或畸形 classfile 是正常输入类别，不能用异常作为 reader 的主控制流。当前 `Result<T, E>` 加 `Diagnostics` 的组合正确：无法确定边界的情况返回 Err（不产出半截模型），「结构可确定、语义不合规」的情况返回模型加 `Recovered` 标记和诊断（两态语义）。

错误类型要带阶段和影响范围。比如 `RelocationNotModeled` 表示模型仍然存在但不能安全写回，`RelocationBadTarget` 表示引用已经失效；二者不能统一成一个“编码失败”。`mbase` 还应把未解析引用保留为带状态的边，而不是在索引阶段静默丢弃。

### 12. 扩展点：注册能力，不公开事件回调

属性格式、未来 classfile 版本和厂商扩展需要可注册，但注册表的职责是声明“这个名字如何解码、如何编码、依赖什么、失败时如何回退”，不是把读取过程推给用户。回调可以存在于内部 codec 或 resolver 适配层，但不能重新形成 `visitXxx` 的公共 API。

扩展注册表必须有明确生命周期和版本策略。一个 handler 若不能保证写回或重定位，就只能把内容标为 opaque；它不能通过继承一个基类就自动取得 CP 或 pc 的修改权限。

## 从 ZKM 吸收什么

ZKM 最值得吸收的是分层对象模型、变换期身份稳定、未知属性的 raw fallback、写出期统一重建、方法级数据流验证，以及把 library class 作为类型解析上下文的双轨思路。它说明了长期驻留的编辑器必须把“对象身份”和“磁盘编号”分开，也说明输出重排必须集中在一个明确的写出阶段。

ZKM 不应原样搬进 `libjbc` 的部分包括每个组件的 parent 指针、全库 observer/replaceable 网络、把 CP 共享拆成对象副本的全局策略、把类图和混淆器操作直接挂在 `ProgramClass` 上，以及 intrusive 指令链表。那些机制解决的是 ZKM 的集成工作台问题，会把单类库的所有权和可测试边界重新变复杂。

## `libjbc` 当前最关键的补口

(1) 把 `LabelId` 从原型提升为正式边界模型。分支、switch、异常表、LNT、LVT 和 SMT 的边界要共享同一解析协议；方法末尾要从“空 InsnId 哨兵”升级为明确的 End boundary。

(2) 把 CP index 从 IR 操作数中逐步移出，补 `ConstValue`/`ConstId` 与 session remap。只追加 CP 继续作为保真快速路径，但要为去重、合并和输出重排提供显式全量 remap 路径。

(3) 在 `mbase` 落地 `ReferenceSite`、解析状态、未知属性足迹和 class/session `ChangeSet`。xref、可达性、去重合并和输出重排都依赖这些事实；库不提供算法也可以，但不能让 mbase 只能重新猜。

(4) 增加生成类或无源模型的明确 writer 能力。当前保真 writer 依赖 source bytes 是合理的防线，但从零生成和 raw-only patch 必须有不同的 writer contract，不能让 `MissingSource` 掩盖“模型本来就是新建的”这一合法状态。

(5) 把结构验证、IR 验证、帧计算和全程序验证的边界写成 API，而不是把所有检查散落在 lower 的偶然路径中。验证器不必一次做到 ZKM 的完整程度，但层级和失败语义必须先稳定。

因此，当前 `libjbc` 的总体方向没有搞错：DOM-first、值句柄、lazy/raw 保真和单类边界是正确的基础。真正还不够的是边界标签尚未贯穿所有引用域、CP 语义引用尚未完全脱离数字索引，以及 `mbase` 的稳定引用事实和变更集尚未成为正式契约。先补这三个连接点，再扩展更多 opcode、属性或优化 pass，架构才会稳。
