# libjbc 人体工学评估（v0.3，2026-08-30）

评估方法：不写「示例级」代码，而是把三件真实的事当外部库做完（`demo/use_case.cpp`，语料 ares 262 类 / 2603 方法），记录写代码时实际遇到的每一处别扭。摩擦点按「是否诱发错误」排序，不按美观。

## 实测基线（demo/use_case.cpp 输出）

| 任务 | 结果 |
|---|---|
| 扫描（方法/异常表/indy 计数 + 帧自预言） | 262 类 2603 方法，frameOk 98.2% |
| 变换（全语料方法入口插 nop → lower → 重读验证） | 2603/2603 成功，253/262 类整类验证通过 |
| 未变换往返（writeClass vs 原字节） | 262/262 逐字节一致 |

整类验证未过的 9 类全部源于帧引擎 best-effort 边界（jsr/混淆形态 underflow），与编辑/写回本身无关。

## 顺手的部分（保持，别改坏）

1. **核心循环只有三行**：promote → 编辑器操作 → lower。插桩不需要知道 LNT/SMT/LVT 存在——relocation 自动处理，这是 v0.3 最值钱的设计。
2. **错误即值**：全程无异常；`.is_ok()` 快路径读起来自然，恶意输入不是崩溃而是 Err。
3. **所有权显式**：unique_ptr 交接进 makeSession，消费者零 delete；「摘链忘删」「label 池旁路」在这套 API 上没有入口（设计目标达成）。
4. **no-op lower 不置脏**：promote+lower 后不编辑，写回字节级一致是免费的（262/262）。
5. **危险决策显式化**：erase 默认 kRejectIfUsed；插桩遇到未建模嵌套属性是明确失败而不是静默丢。

## 摩擦点清单

### F0 CP 操作数是裸索引，没有高层语义（用户指出，已修——2026-08-30 第二轮）
v0.3 首版评估漏了这条：demo 插的是 NOP（无 CP 操作数），用例盲区。真实插桩立刻撞墙——插一条 `invokestatic` 要手工拼 addUtf8/addClass/addNameAndType/addRef 四连并理解两跳布局；读一条 `getfield` 同样要手工走 Methodref→NaT→Utf8。**CP 知识泄漏给每个消费者**，这正是旧库时代「变换代码一半在管常量池」的延续。修复（语义层，operand.cpp）：读路径 `memberOf/classOf/constOf` 一步到语义视图；写路径 `internClass/String/Integer/FieldRef/MethodRef/InterfaceMethodRef` 按内容去重（命中既有池不膨胀，miss 才追加，守住 CP 只追加契约）。连带修掉两个陷阱：LDC 的 u1 域宽（>255 曾会静默截断，现编码器拒绝 + `loadConst` 自动选 LDC_W）；invokestatic 指 InterfaceMethodref 的混淆器交叉形态（真实语料 76 处，语义层两种都收）。实测：全语料 CP 指令 37398 条全部语义可解析（member=27003 class=4891 const=5504 unresolved=0）；带 CP 的真实插桩 2603/2603 通过。

### F1「方法有没有 Code」是错误路径（诱发错误，优先修）
`promoteMethod` 对 abstract/native 返回 `Err(NoCode)`，消费者只能 `if (p.is_err()) continue;`——这会把**真正的失败**（BadInstructions/AttrNotDecoded）和「没有代码」一起吞掉。demo 里就这么写的：解码坏了的方法会被静默跳过而无任何信号。建议：`bool hasCode(methodIdx)` 查询 + 让 promote 的真失败留在错误路径，或提供 `forEachCodeUnit(session, fn)` 迭代器把两种情况分开。**已修（v0.4，2026-09-14；2026-09-27 按 design.md §26 改形态）**：两条建议都落了。`hasCode(session, idx)` 只读查询（不解码属性内容、不置脏，越界探测式 false）；`collectCodeUnits(session)` 按方法序**值返回** `CodeVisit` 序列（F1 当时落的是回调形态 `forEachCodeUnit(session, fn)`，visitor 清零后改为拉形态，语义不变），Kind 三态 NoCode/Failed/Ok 互斥，Failed 携带 PromoteError——想把 Failed 静默跳过仍必须显式写分支才做得到。已 promote 的方法在收集中直接复用现有 CodeUnit 不重建（promoteMethod 的幂等重建语义会失效既有句柄，收集场景刻意避开）。promoteMethod 文档补「先 hasCode 分流」推荐写法。测试：`ergonomics_test.cpp`（关内建字节，含 goto 跳进操作数中间的真失败用例）。

### F2 read→session 三步样板（高频，次优先）
每次都是 readClass → 取 ReadResult.file → makeSession 两段 unwrap。每个消费者都会写一个 openSession 辅助函数（demo/测试里出现了 3 份）。建议：`makeSessionFromBytes(ByteView)` 一步到位；三步版保留给需要检查 ParseState/diagnostics 的场合。**已修（v0.4，2026-09-14）**：`makeSessionFromBytes(ByteView[, ReadOptions])` 落地，产物与三步版同形（shared_ptr 寿命接管，Recovered 照常建会话）。错误横跨 Read/Promote 两族的问题用 `SessionError{stage, read, session}` 如实保留「段 + 码」——不做跨族有损映射，也不新增第六套错误枚举（F3 的收敛裁决照旧）；诊断面不透出，三步版保留给需要检查 ParseState/diagnostics 的场合（与本条建议一致）。测试：ergonomics_test（一步产物等价性、坏魔数/空输入的段+码断言、ReadOptions 透传）；本测试文件的 sessionOf 样板同步被结清。

### F3 五个错误枚举（认知负担，缓修）
Read/Write/Edit/Promote/Lower 五套枚举，`unwrap_err()` 各是各的类型，统一日志要写五份 switch。诚实但啰嗦。已有的 `to_string` 家族够用；后续若加通用诊断管道再收敛。

### F4 找方法要手写线性扫描（高频小摩擦）
`symbols().str(m.name) == "xxx"` 循环在 demo 和测试里重复出现。建议：`ClassUnit::findMethod(name[, desc]) → 可选下标`。**已修（v0.4，2026-09-14）**：`ClassUnit::findMethod(name)` 与 `findMethod(name, desc)` 两个重载落地，逐字节比较（零分配，不做 UTF8 解码），同名重载命中下标最小者；未命中返回命名哨兵 `kNoMethod`（替代裸魔法值，与 kNoInsn 同一惯例——下标查询没有 nullptr 可用）。参数取 `std::string_view` 而非 Utf8StringRef：IR 层查询面的既有惯例是借用字节视图（SymbolPool::intern/str、语义 setter 全是 string_view），Utf8StringRef 构造要 O(n) 计码点，对热路径查询是纯开销。测试：ergonomics_test（重载分辨、字段同名不干扰、下标直接喂 promote/lower）。

### F5 空句柄哨兵是隐式约定（可读性）
`InsnId{}` 当 null 用（Exc.end、demo 的 first 判断），但没有命名常量。建议：`kNoInsn` 常量 + `CodeDom::first()/last()` 便捷入口。

### F6 promote 语义是「重建」不是「缓存」（需要文档，不是 API 问题）
lower 之后再 promote，拿到的是**从已写回的 backing 重建**的新 CodeUnit（句柄全换）。demo 验证逻辑第一版就在这里错了（拿编辑后的尺寸当基准）。建议：在 units.hpp 的 promoteMethod 文档里加一句显式说明。

### F7 FrameEngine 默认构造要带 resolver 对象（小）
`ShapeResolver res; FrameEngine fe(cp, res);` 两步。建议：`FrameEngine(cp)` 便捷构造（内部默认 resolver），注入式保留。

### F8 方法索引用裸下标（未来风险，记录在案）
promote/lower/methods[i] 全走 usize 下标。v0.3 没有增删方法的 API，下标即稳定；一旦引入方法级增删，需要 MethodId 域（同 InsnId 的 owner/slot 模式）。现在不动。

### F9 测试侧：AttrKind 覆盖计数数组手开长度（已咬过一次）
attr_extra_test 的 `seen[N]` 手写数组长度，新 AttrKind=6 时越界写崩了进程（release 下静默崩溃，exit 127 无输出）。已修为 7；根修是按枚举上界定长或改 map。新增 AttrKind 时记得检查。

## 结论

v0.3 的「危险面」设计（句柄、策略、relocation contract、copy-through）全部达标——消费者在最短路径上做对了危险的事。剩余摩擦集中在**查询/便捷层缺失**（F1/F2/F4/F5/F7），属增量 API，不动架构。建议 v0.4 优先落 F1（防静默吞错）与 F2（入口样板），其余随手。


## F10 ASM `InsnList` 对照（2026-08-30 第三轮，用户提问驱动）

「有 ASM 那种好写吗，还要自己操作 CP 吗」——逐动作对照后的结论与补口：

| ASM 写法 | libjbc v0.3.6 | 状态 |
|---|---|---|
| `new MethodInsnNode(op, owner, name, desc)`（CP 全自动） | `cpInsn(op, s.internMethodRef(...))` 两步 | 接受差一步：intern 显式可观测（去重/追加），ASM 藏在 writer |
| `insn.name = "print"` 改写调用 | `s.setMember(insn, kind, owner, name, desc)` | **已补**（此前要 memberOf→intern→手动赋 cp 三连） |
| 改 ldc 指向新常量 | `s.setString(insn, s)`，超 u1 域自动升格 LDC_W | **已补**（宽度陷阱原来留给调用方） |
| `LabelNode` 先建标签再前向跳 | 句柄必须指向已存在指令；模式=分支先指锚点、`setTarget` 改写 | 固化为测试 `ForwardJumpPatternWithSetTarget`；差异是设计取舍（见下） |
| `list.getFirst()/insertBefore(first, x)` | `dom.first()/last()` + `ed.insertAtStart(x)` | **已补**（F5 结清） |
| `new LdcInsnNode("s")` 不管宽度 | `loadConst(idx)` 自动 LDC/LDC_W | 已有（v0.3.5） |

**刻意的差异，不改**：句柄式目标 vs LabelNode——libjbc 的分支目标必须是已存在指令的句柄，不可能出现 ASM 的「label 未访问」运行期错误，erase 有 incoming use 会被结构性拒绝（ASM 删 LABEL 节点留下断链跳转）；代价是前向跳需要 setTarget 两步。typed node（MethodInsnNode 等）vs 扁平 Insn + 语义视图：C++ 侧用 memberOf/classOf/constOf 读、setMember/setClass/setString 写覆盖同一人体工学，不再引入节点类层级。

**仍未对齐**：invokedynamic 的改写（需 BSM 语义，未建模）；`MethodVisitor` 式的事件流 API（消费者侧暂无需求，不做）。

## F11 真实场景实测：mbase/obf 四 pass 双栈对照（2026-08-30 第四轮）

`demo/obf_passes.cpp`（ares 262 类，ALL-PASS），每场景 libjbc 版与 libjbytecode 旧栈版并排写、并排验证：

| 场景 | 结果 | LOC（jbc/legacy） |
|---|---|---|
| S1 调用者索引（读） | 计数与键**逐项相等**（20324 调用 + 463 indy 单列） | 43 / 54 |
| S2 类重命名（写） | 双栈各 43 处重写、重读残余均 0 | 54 / 55 |
| S3 字符串全语料改写 | 5282 串，262/262 类重读验证 | 44 / —（未写） |
| S4 调用重定向 | 1533/1533 调用点改写验证 | 61 / —（未写） |

**结论：LOC 基本持平——旧栈的 tree API 本来就是 ASM 形态，改写一条 MethodInsnNode.owner 也好写。改进不在这里，在四处实测差异：**

1. **保真**（S2 直接证据）：同一改名，libjbc 输出 3559B（copy-through，未触碰字节原样保留），旧栈 3487B（ClassWriter 全量重编码，差 72B）。旧栈对未建模属性/调试信息的保留没有承诺；libjbc 的 relocation contract 把「改了布局就必须处理嵌套属性」变成硬约束。
2. **错误纪律**：旧栈每类要 try/catch（异常即控制流，漏一个 catch 进程就死）；libjbc 全程错误即值。S3 顺带验证：字符串改写触发 LDC→LDC_W 布局变化，LNT/SMT/LVT 全语料自动重定向，零拒绝零失败。
3. **统一入口 vs 四种 node**：S2 旧栈要对 METHOD_INSN/FIELD_INSN/TYPE_INSN 逐个 static_cast（还有第四种 indy node 没进 rename 面）；libjbc 是 memberOf/classOf 两个入口。
4. **字符串纪律**：旧栈新名字必须 arena.intern 物化再赋 Utf8StringRef（忘了就悬垂）；libjbc setter 直接收 string_view。

**实测同时抓到的新摩擦（记 F12，已收口）**：`memberOf` 一个入口返回五族（字段/方法/接口方法/indy），统计「调用」时把 92 个字段访问混了进去——第一版 demo 与旧栈计数对不上，逐类二分 + javap 第三方裁判才定位是**自己口径错**。收口（design §31.2）：`collectCodeUnits` 增 `CodeCollectFilter`（指令族四族掩码 + 嵌套属性族掩码，两维 AND），调用侧在收集时按族取数，判别知识从每个消费者移进库；`Failed` 恒保留、`NoCode` 受 includeNoCode 管，F1 契约不回退。

**场景边界（如实记录，不是 bug）**：单类 rename 的完整面还包含描述符内嵌类名（`Lcc/ares/...;` 出现在字段/方法签名与注解里）与跨类引用——两栈的 demo 都只改了指令引用面，验证也只覆盖该面；这是 obf rename 管线的真实剩余工作（需要描述符手术 + 跨类映射表）。


## F13 寿命接管（2026-08-30 第五轮，用户问「怎么让库接管生命周期」）

§23.3 的残余借用义务升级：保活句柄（CodeUnitRef / memberSemRef / constSemRef / classSemRef）把会话寿命带在身上，消费者跨作用域持有、存容器、传线程队列都不再需要自己保活会话；视图版保留给扫描热路径（显式选择借用换零开销）。FrameEngine(cp) 默认构造结清 F7。测试形态本身就是人体工学证明：`KeepaliveSupportsCrossScopeEditing` 在会话变量销毁后继续编辑 + 帧计算，代码里没有任何保活样板。

## F14 从零建方法体四段接线（issue #8，2026-10-20）

从零建带体方法要串 addMethod → promoteMethod → CodeEditor → lowerMethod，每段各自传播 Result。收口为 Result-only 的 MethodEditor（open/append/code/commit）与 addMethodWithBody（直线体一步落盘）：门面只减接线层数，不做 sticky 首错（第二条汇报通道，失败后静默 no-op），不收回调（库不执行调用方业务）。带分支的体仍走 open 后的 code() 全量面。open 即挂入成员：不 commit 直接析构只丢弃编辑，成员以空体留在会话，需调用方 removeMethod 收尸。类级 ClassDraft、与 ClassBuilder 的字节对拍与消费面迁移未做，见 issue #8 剩余验收项与 design.md §38。
