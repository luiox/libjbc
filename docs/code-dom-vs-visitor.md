# CodeDom 还是 visitor：一个已经做完一半的决定的收尾建议

- 对象：`libs/libjbc`（src 33 个 .cpp/.hpp，test 15 个，demo 4 个）。
- 问题：当初为绕开 ASM 式 visitor 而做的 `CodeDom`（`jbc/ir/code_dom.hpp`）这条路要不要继续走下去；如果继续，下一步该补什么。
- 结论先放前面：**继续，不要回头补 visitor 公共协议**；但当前实现有四处缺口，其中「异常表不在 use 域」这一处能让写出的 classfile 静默损坏，应当优先补上。本文所有代码位置均为实测核对。

---

## 1. 三个数据点：这不是品味问题，是拓扑问题

先把"要不要 visitor"从品味问题里拿出来。这个问题在三个代码库里各有一份独立证据，指向同一个方向。

第一份是 ASM 自己，`design.md §2.1` 已经把它的四条结构性后果写清楚了：事件流没有稳定位置，所以跳转、异常表和局部变量表只能靠 `Label` 令牌补洞；节点没有唯一 owner，于是 `remove`、`detach`、`delete` 变成靠纪律配对；reader 的 arena 和节点之间是不可检查的借用关系；帧算法在 core 和 analysis 各维护一套。这四条没有一条是"实现不够好"，全是拓扑选择的必然代价——`ClassReader -> ClassVisitor -> ClassNode` 这个形状决定了事件先行、数据后到，而数据后到就必然要回头补引用。

第二份证据是我这两天在 ZelixKlassMaster-27 里看到的。ZKM 是商业混淆器，它的变换层**完全没有 visitor**：`MethodBytecode` 是一条指令对象的双向链表，`ASTObfuscateStatement` 执行时直接在链上原地增删，用 `forOpcode(89)` 这类构造往栈上打桩（全仓库约 4800 处裸 opcode 站点，热点在 `LongConstantEncryptor`、`StringEncryptor` 这些反复重排指令序列的模块里）。一个要对同一条指令做几十次插桩、改写、回滚的程序，天然长成了 DOM 形态——因为变换需要的是"位置"和"身份"，而 visitor 恰恰两者都不提供，只提供"经过的时机"。

第三份就是 libjbc 自己：`src` 下 33 个文件，`grep 'class \w*Visitor|void visit'` 零命中；`design.md §1.4` 第 4 条已把规则写死——"public API 以模型为中心；libjbc 内**禁止任何 visitor 形状**——无论公共 API、codec 内部细节还是「迁移期 adapter」一律不得引入（§26：判据是数据形态，回调推送即换皮；门禁单测与判据同源）"。所以现状不是"正在尝试绕开 visitor"，而是**已经绕开了，且规则已成文**。我的建议建立在这个前提上：问题从"要不要"变成了"把已经承诺的结构性不变量补完整"。

## 2. 已经验证的收益，先钉住别丢

在讲缺口之前，把这条路已经拿到手的东西说清楚，因为补缺口时不能把它改回去。`ergonomics.md F11` 用 `demo/obf_passes.cpp` 在 ares 262 类上做了四场景双栈对照，结论有四条：其一是保真——同一处改名，libjbc 输出 3559 字节（copy-through，没碰的字节原样保留），旧栈 ClassWriter 全量重编码输出 3487 字节，差 72 字节，而且旧栈对未建模属性的保留没有任何承诺；其二是错误纪律——旧栈每类都要 try/catch（异常即控制流，漏一个进程就死），libjbc 全程错误即值，字符串改写触发 LDC→LDC_W 布局变化时 LNT/SMT/LVT 全语料自动重定向，零拒绝；其三是入口统一——旧栈改一条调用要对四种 node 逐个 `static_cast`，libjbc 只有 `memberOf`/`classOf` 两个入口；其四是字符串纪律——setter 直接收 `string_view`，不存在"忘了 intern 就悬垂"。

`F10` 里还有一条被标为"刻意的差异，不改"：libjbc 的分支目标必须是**已存在指令的句柄**，所以不可能出现 ASM 的"label 未访问"运行期错误，删除被引用的指令会被 `erase` 结构性拒绝（而 ASM 删掉 LABEL 节点只会留下断链跳转）；代价是前向跳要两步——先建锚点指令、再 `setTarget`。这个代价是值得付的，它把"删了还有人跳过来"从运行期错误变成了编译期返回值。这四条加起来，就是 DOM 这条路的已兑现收益，下面的四条缺口一条都不能以牺牲它们为代价去补。

## 3. 四个缺口，按风险排序

**缺口一已经闭合：异常表边界统一进入 LabelId 域。** `CodeUnit::Exc` 的 start/end/handler 现在都是 `LabelId`，方法末尾由 `LabelTarget::End` 明确表示。活动标签由 `CodeDom::erase` 统一保护，lower 解析失效标签返回 `RelocationBadTarget`，因此异常表不再依赖 `CodeEditor` 与 `CodeDom` 之间的隐式握手，也不会把死句柄静默写成 pc 0。剩余迁移工作是让调试范围和 branch/switch 目标也使用同一边界协议。

**缺口二（中风险）：`get()` 交出去的是裸指针，而槽表会重分配。** `CodeDom::get` 的签名是 `Result<Insn*, EditError>`（`code_dom.hpp:118`），`order()` 交的是句柄序列——句柄化是这套设计的立身之本，头文件自述"指令顺序是数据不是指针链副作用"。但 `slots_` 是无 reserve 的 `push_back`（`code_dom.cpp:84`、`:106`），`Insn` 本身又含 `std::vector<InsnId> targets` 和 `keys`，一旦真实增长触发 `std::vector` 重分配，先前 `get()` 拿到的所有 `Insn*` 全部悬垂，而且是静默的——编译器不拦，ASan 只在读的时候才抓。`code_dom.hpp:116` 的 `@warning` 只约束"改 op 前字段要匹配新形状"，没约束指针寿命。建议二选一，倾向第一种：给 `get()` 写死"任何结构性变更后 `Insn*` 失效，重新 `get`"的契约并放进函数名/文档（现状最接近的表述在 `code_dom.hpp:120`，那句"迭代中变更序列是未定义行为"只覆盖了 `order()`，没覆盖 `get()`）；或者把 `get` 改成返回带 generation 校验的轻量代理、写操作按值回传。在选定之前，这条属于"文档欠账"，成本极低。

**缺口三（低风险，先量化再动）：use 扫描是 O(n)，且在 erase 路径上每次调用。** `usersOf` 每次线性扫 `order_`，`erase` 内部又调它一次（`code_dom.cpp:164`），`replaceAllUses` 同理；`insertAt` 还要线性找锚点（`code_dom.cpp:108`）。方法体规模下这是对的——`design.md` 明确写了"按需扫描，方法体规模下足够"，而 F11 四场景的实测也说明常见 pass 没有被它拖住。但要意识到它的形状：一个循环里做 N 次 erase 就是 O(N×n)。我的建议是**不要**因此引入常驻反向索引——那正是 visitor/tree 一系要付的维护成本（每次插入、删除、改写目标都要同步更新，漏一次就静默错），按需扫描的"每次现场重算"恰恰是它不容易出错的原因。正确的姿势是在 `CodeEditor` 层给批量场景一个"先计划后执行"的形态（一次 `usersOf` 规划整批删除，再逐个删），并把成本模型写进文档，等真实 pass 测出瓶颈再谈缓存。

**缺口四（决策类）：BlockId 是设计稿有、实现没有的半截。** `design.md §7.3` 的草图整个建立在块身份上：`createBlock`、`append(BlockId, ...)`、`replaceAllUsesWith(BlockId, BlockId)`、`retargetAndErase(BlockId, RetargetPolicy)`、`splitBlock(BlockId, Boundary)`；而 `v0.3` 落地的是 `InsnId` 句柄 + `blocks()` 按需只读视图，`code_editor.hpp:4-7` 自己交代了"v0.3 尚未引入 BlockId 域，块操作走指令句柄"。**我的建议是不要为了"补齐设计稿"而补 BlockId**——先问哪个真实 pass 需要"块级变更身份"（即需要持有块、跨块改写、拆分块并让引用自动跟随），在那之前，`blocks()` 只读视图 + `setTarget` 已经覆盖了 F10/F11 暴露的全部需求；反过来，凭空引入一个和 `InsnId` 平行的句柄域，等于多一套 owner/slot/generation 要维护、多一套跨域校验，收益却是零。配套动作是把 `design.md §7.3` 标注为"目标态草案（未实现，当前以 InsnId 为准）"，避免文档与实现长期分叉——文档分叉本身会制造错误信心，下一个人照着 §7.3 调 `createBlock` 会发现接口根本不存在。

## 4. 三个"不要做"

有三条边界我认为比缺口清单更重要，因为它们一旦破了，上面的收益会整批回吐。第一条：**不要把 visitor 加回公共 API**。`§1.4-4` 已经把这条写成规则，`F10` 也记录了"MethodVisitor 式事件流 API：消费者侧暂无需求，不做"；如果哪天确实需要"逐条经过"的人体工学，答案是只读迭代（`order()` + `peek`，或一个 `forEach` 辅助），而不是事件协议——前者是模型的视图，后者会把"位置"和"身份"重新变回需要补洞的东西。第二条：**不要让 DOM 成为全 Jar 扫描路径**。`design.md §8.2` 的分工是对的：只读统计走流式的 `SummaryScanner`/`ReferenceScanner`（扫完即释放，F11 的 S1 场景 43 LOC 证明只读根本不需要 DOM），`CodeDom` 只为"要改的那个方法"建立——一旦让每个类都 promote 成 DOM，copy-through 的保真收益和内存曲线会一起塌。第三条：**不要在迭代中改结构**。`code_dom.hpp:120` 已经把 `order()` 的这条写成未定义行为，但这是最容易踩的一条（读者往往以为自己只是在"看看"），与其靠自觉，不如补一个索引式或快照式的便利迭代入口，让危险路径需要显式选择。

## 5. 优先级与验收

把上面收成一张表（顺序即建议执行顺序）：

| 优先级 | 缺口 | 证据位置 | 风险 | 动作 |
|---|---|---|---|---|
| P0 | 异常表不在 use 域 | `code_dom.cpp:131`、`units.hpp:39`、`units.cpp:193` | 写出坏 classfile，静默 | `CodeEditor::erase` 增查 `u_.exc` 三句柄；`pcOfId` 兜底收紧 |
| P1 | `Insn*` 寿命无契约 | `code_dom.hpp:118`、`code_dom.cpp:84/106` | 悬垂读，难查 | 文档写死失效时机（或改代理/按值） |
| P2 | BlockId 文档分叉 | `design.md §7.3` vs `code_editor.hpp:4-7` | 误导后续开发 | 标注草案状态，等真实需求再实现 |
| P3 | use 扫描成本未写明 | `code_dom.cpp:164`、`code_dom.cpp:108` | 循环场景 O(N×n) | 文档化成本模型 + 批量规划 API |

验收上，仓库已有的脚手架够用：`test/unittest` 里 `roundtrip_test`、`ir_test`、`ergonomics_test`、`domain_test` 加上 `demo/obf_passes.cpp` 的 F11 四场景（262 类、5282 字符串、1533 调用点）就是回归基线。针对 P0 应当新增两个用例：一是"删除恰好是 `exc.handler` 的指令必须返回 `StillUsed`（或经 retarget 策略显式处理）"，二是"处理后 lower + 重读，异常表三元组与处理前逐项相等"——第二个用例是关键，因为 P0 的失败模式恰恰是"不报错但产出坏字节码"，只有重读比对才能抓住它。

总起来说，visitor 与 DOM 的取舍在三个代码库里已经有了三份一致的证据，libjbc 现在的问题不是方向，而是把 §7.3 那句"结构化变更不是注释约定，而是 API 结果类型可见的状态机"兑现到**全部引用域**——branch/switch 已经做到了，异常表是缺的第一块，而且是唯一一块能漏出坏字节码的。先把 P0 补上，其余按"有需求再加"的节奏走，这套 DOM 就比任何 visitor 协议都更接近那句承诺。
