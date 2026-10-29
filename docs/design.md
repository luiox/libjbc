# libjbc 统一拓扑设计

本文是 libjbc 的设计真源：只写**现行的**架构拓扑与硬契约，不记过程史。代码注释只述自身行为、不回指本文；全文以两处为锚——visitor 门禁判据（§11）与封闭集契约（§8），改契约先改本文再改测试。

状态行：core / ir / analysis / type 四层已实现并通过测试；当前版本与进度以 README 为准。

## 1. 定位与拓扑

libjbc 是 node-first 的 JVM 字节码单类建模库。三条根纪律：

- **模型是数据，读写是行为**。模型负责值、关系、所有权和局部不变量；解析、写出、提升、降级、编辑、帧计算都是行为对象。库内禁止任何 visitor 形状（判据见 §11）。
- **一个对象只代表一个 class**。没有库级 global `Context`，不持有 Jar、JDK、资源容器或跨类索引。
- **错误走值语义**。全链 `Result`，不抛异常；失败不留半截状态。

```text
                    上层消费方（库外）
          索引 / 事务 / 资源容器 / 跨类编排
                              │
                   ITypeResolver / 注入点
                              │
┌─────────────────────────────▼─────────────────────────────┐
│                    libjbytecode                             │
│  classfile DOM | codec | ir | session | analysis | type     │
│  模块依赖单向：core ← ir ← analysis；type 独立纯函数面       │
└─────────────────────────────────────────────────────────────┘
```

模型关系是拥有链，不是继承：

```text
ClassFile ─owns─> ConstantPool / fields / methods / attrs / source bytes
MemberInfo ─owns─> AttrSlot[]
AttrSlot ─owns─> AttrBody?（lazy，未解码时为空）
CodeAttr ─owns─> code bytes / exception table / nested AttrSlot[]
ClassSession ─owns─> backing ClassFile + ClassUnit + BridgeState
```

## 2. core：单类 lazy classfile DOM

### 2.1 所有权与寿命

- `ClassFile` 拥有一切，析构一次回收；公共 API 不产生调用方 `delete` 的裸指针。借用只发生在调用方手里，模型内部零借用字段。
- 解析来的 Utf8 是指向源字节副本的零拷贝视图（寿命 = ClassFile）；变换新增的 Utf8 intern 进 arena。两条存储路径各配一种生命周期，ClassFile 析构时同死。
- 句柄式身份（ir 层 `InsnId{owner,slot,gen}`）owner 进句柄不进对象；来自另一 DOM 的同号句柄被结构性拒绝；删除或析构后失效。

### 2.2 常量池

- 零拷贝视图 + **只追加，永不重编号**——重排会使未建模属性内嵌的 CP 引用失效；只追加则天然保持有效。
- 语义是**逐条目**的：dirty 条目重编码，未脏条目一律 copy 源 span。`at()` 是纯读取透镜（不置脏），一切 setter 置脏，`mutateAt(i)` 是改写既有条目的唯一规范入口。经 `at()` 改写不置脏会被源 span 静默覆盖（文档化陷阱，测试钉死）。
- 长宽条目（Long/Double）双槽；宽条目第二槽位越界属 CP 结构损坏，reader 阶段 `Err`。

### 2.3 属性模型

`AttrSlot` 四态：Untouched / Decoded / Modified / New。`peek()` 只读不置脏，`mutate()` 是唯一置脏入口——「看过」不等于「改过」，全解码后写回仍逐字节恒等。lazy 解码唯一通道是 `decodeAttrBody`（按属性名分派），`decodeAllAttrs` 递归解码（含 Code 嵌套与 Record 组件嵌套）。

typed 属性面（`AttrKind` 1..24，全件接入分派、保真写回与压缩封闭集）：

| 组 | 属性 |
|---|---|
| Code 嵌套 | Code、LineNumberTable、LocalVariableTable/TypeTable、StackMapTable、CharacterRangeTable（javac -Xjcov 私有） |
| 注解族 | Runtime{Visible,Invisible}Annotations、ParameterAnnotations、AnnotationDefault、Runtime{Visible,Invisible}TypeAnnotations |
| 结构元数据 | SourceFile、InnerClasses、EnclosingMethod、Exceptions、Signature |
| Nest/Record/Module | NestHost、NestMembers、PermittedSubclasses、Record（组件带嵌套属性表）、Module、ModulePackages、ModuleMainClass |
| BootstrapMethods | 条目面 append/复用/loadable 参数落位（§9） |
| 字段常量 | ConstantValue（JVMS 4.7.2，走 String 条目） |

不建模裁决（显式，非欠账）：

- **Deprecated / Synthetic**：体恒零长，无 CP 槽无手术面，零长透传构造上无损。
- **ModuleHashes / ModuleResolution / ModuleTarget**：JDK 内部件，无稳定规格。Raw 透传保真，压缩封闭集外显式拒绝（点名属性名）。

Raw 名单已整体退役：非零长 Raw 体显式 `Err(AttrNotModeled)`，拒绝面在 javac 级语料上为空。

结构元数据属性的共同 codec 铁律：半截体 decode 显式 `Err(Truncated)`；声明结构后的多余字节奏 `trailing` 原样保留；编码逐字段过 u16 域宽闸门；公开解码入口（`decodeInnerClassesBody` 族）不触碰槽状态。0 值槽位（匿名内部类 outer==0、EnclosingMethod method_index==0）是合法值原样保真。

## 3. 保真写回

只有一种写回策略：**未触碰的 copy 源 span，触碰的重编码**。刻意不提供「全部重编码」模式——那条路遇到未建模属性只能丢数据或猜语义。需要整体重编码的场合，调用方先 `decodeAllAttrs` 再写回，代价与风险显式。

- 解码不置脏；`mutate()` 才推进状态。判据从「编码器正确」降级为「copy 正确」。
- **trailing 同族**：类级（属性表后的多余字节）与属性体级（声明结构后的多余字节）都是畸形输入的可表达事实——保留副本 + warning，不 Err；copy 路径天然无损，重编码路径原样附加。带尾部垃圾的输入也必须完整往返。
- **编码闸门**：一切落不进目标域宽的值显式 `Err(WriteError::Unencodable)`，禁止 `static_cast` 截断。u1 域放下的唯一合法出路是显式拓宽变换（goto→goto_w、LDC→LDC_W、wide 前缀），由编辑器选择。
- `setMember` 改写 invokeinterface 时 count 按新描述符重算（参数槽 + this 槽）。
- writer 失败返回 Err 时输出字节作废，绝不产出半成品 class。错误码四件：MissingSource（要 copy 而源缺席）、BadSpan（模型内部不一致）、Unencodable、NotModeled（已修改但无编码器——拒绝写坏）。
- 从零构建的模型（`makeSession(NewClassSeed)`）无源字节，每个条目都在新编码态，照常写出。

## 4. reader：骨架 eager，内容 lazy

`readClass` 只解析定位结构所必需的部分：头、CP、成员 access/name/descriptor、属性目录。属性内容记 span 后跳过——解析代价与方法体规模无关。

- `ReadOptions.preserveSourceBytes`（默认 true）：关掉省一份等大内存，代价是属性无法 lazy 解码、写回被拒（MissingSource）、字节视图借用人参 buffer。
- `maxSupportedMajor` 默认 66（Java 22），超出只记 warning 不拒读。
- **失败语义**：结构损坏（边界无法确定）reader 阶段 `Err`（十个错误码逐族钉死），不产出模型；「结构可确定、语义不合规」落在 `ParseState::Recovered` + Diagnostics，模型照样可原样写回。`ParseState` 描述能力，不武断标「合法/非法」；降级与拒绝两类不可混用。
- 畸形输入契约：每个样本的产物必须显式——正确错误码的 `Err`，或带状态标记的 `Recovered`，绝不允许静默成功；降级模型未触碰写回字节一致，写出仍可解析或显式拒绝。

## 5. ir：CodeDom、ClassSession 与语义操作数

### 5.1 CodeDom

- 指令是值语义槽（`slots_` 拥有、`order_` 存程序顺序），无 intrusive 链表、无 LabelNode。分支/switch/异常表边界持 owner-scoped `LabelId`（控制流标签与边界标签角色分离）；libjbc 的分支目标必须是已存在指令的句柄——「删了还有人跳过来」是返回值不是运行期错误，代价是前向跳两步（先锚点后 setTarget）。
- 删除语义是 API 可见的状态机：有 incoming use 默认 `StillUsed`；`replaceAllUsesWith`/retarget 归零才可删；批量删除（`eraseBatch`）**全有或全无**、批内互指不算引用、retarget 安排显式；`CodeEditor` 是策略层（拒绝或改锚到顺序后继）。
- 搬移原语 `moveRange(first, last, at)`：`[first, last)` 连续段整体搬到 at 之前，单趟提出回插重排 `order_`（O(n)），不触指令槽与标签锚定——段内标签随段迁移、段外对段内标签的引用天然有效，区别于「eraseBatch + 重建」（后者面向删除语义，改锚走标签替换）。`at == kNoInsn`/`last == kNoInsn` 尾哨兵与标签末哨兵同源（half-open 端点下尾段只有经哨兵可命名）。`CodeEditor::moveRange(+policy)` 在段几何外只裁决段首标签去向：`kHeadFollows`（默认）随段走；`kHeadStays` 留原位承接原 fallthrough、锚点改绑原段位后继（居尾改绑 end），被异常表 start/end/handler 引用的段首标签 `StillUsed` 拒绝——改边界 = 改保护区语义。几何拒绝（空段/倒置/锚点在段内）与句柄校验全部前置于变更，无半搬状态。
- 迭代面 `collectCodeUnits` 值返回（拉形态），每方法恰一条 `CodeVisit`（NoCode/Failed/Ok 互斥），Failed 恒保留——静默吞错必须显式写分支才做得到；`CodeCollectFilter` 按指令族 × 属性族两维 AND 过滤。

### 5.2 ClassSession：promote / lower

- promote 是**方法级**显式重建，不是缓存：lower 之后再 promote 得到句柄域全新的 CodeUnit。类不会因改一个方法而整类解链。
- relocation contract（布局改变时对 Code 嵌套属性）：「预检 + 暂存 → 提交元数据 → 写回」三阶段全有或全无。LNT 的 start_pc 精确命中 pcmap；LVT/LVTT 行终点允许「恰等旧 code 长」这一合法 miss，其余钳制/Err；SMT 链式 offset_delta 重算，Uninitialized 载荷走同一张 pcmap 而其 Object 载荷是 CP 索引不得误重定向。封闭集见 §8。
- 会话是单类变换的全部状态（backing + unit + bridge 映射），不是 Workspace；生命周期三种：单类读写、单类变换、从零生成。

### 5.3 语义操作数层

- 读：`memberOf/classOf/constOf` 把 CP 索引一步解析成语义视图；invokestatic 指 InterfaceMethodref 的混淆器交叉形态两种 tag 都收。
- 写：`internClass/String/Integer/FieldRef/MethodRef/InterfaceMethodRef` 按内容去重——命中既有池不膨胀，miss 才沿只追加契约分配。五件扩展 intern：MethodHandle（reference_kind 1..9 域闸）/MethodType/Module/Package/Dynamic，SemIndex 三张去重表。
- LDC 操作数域 u1，索引超域编码器拒绝，`CodeEditor::loadConst` 自动选 LDC/LDC_W。

### 5.4 成员增删与身份

- slot 发号（删槽复用）+ generation 台账（删除自增，旧句柄结构性失配，无 ABA）。死句柄在查询面返回空值、在变更面返回 `Err(InvalidHandle)`——「方法存在但没有体」与「句柄已死」不混码。
- 逻辑下标与物理序解耦：下标视图经 `backingIndex` 转换，只有 MethodId/FieldId 跨变更稳定。
- `addMethod/addField` 种子挂入（预检先于写入：非空、u16 域、CP 容量、名字+描述符唯一）；`removeMethod/removeField` 物理平移 + 身份回收；`adoptMethod/adoptField` 跨类搬移 = **IR 重放不是字节拷贝**（CP 操作数按源语义视图读、目标语义 intern 写，分支/异常边界经标签映射重建，受控延后绑定构建态，全有或全无）。不随迁面显式（非 Code 属性不随迁；v51+ 目标必须随后再生 SMT）。
- 类级元数据提交：`commitClassMetadata`（version/access/this/super/interfaces，intern 去重落位，先算后写）；字段走 `commitFieldMetadata`。
- 从零建类：`makeSession(NewClassSeed)`——CP 引导构造期完成，backing 从第一刻起自洽；writer 从零分支照常写出。

### 5.5 MethodEditor 门面

`open(session, seed)` = addMethod + promoteMethod 一次接线；`append` 转发 CodeEditor；`commit` 显式 lowerMethod（**析构不自动提交**——析构无法汇报 LowerError，自动提交会把错误塞进第二条通道）。`addMethodWithBody` 直线体一步落盘，任一步失败回滚已挂成员；控制流指令按**指令形状**前置拒绝（未绑定分支的 target 恰是 kNoLabel，按目标判会漏放）。复合错误 `MethodBuildError` = Stage + 原族错误码，只作返回值，不构成第二种错误语法。

### 5.6 ChangeSet

每个会话自带观察面（恒开启），各变更通道在**实改发生时**记账。粒度 = 脏检查面：

| 种类 | 记账点 |
|---|---|
| ClassCreated | makeSession(NewClassSeed) 成功 |
| ClassMetadata | commitClassMetadata 实改 |
| MemberAdded / MemberRemoved | add/remove 成功 |
| MemberMetadata | commit{Method,Field}Metadata 实改（携带改写前身份 prior） |
| MethodBody | lowerMethod 实改（全同即 copy-through 不记） |

promote、未 lower 的编辑、失败通道不记账（观察而非变更）。身份坐标是 `(kind, name, desc)` 文本视图（指向会话驻留存储，只增不移）；查询面给折叠终态（同一键按事件序折叠，改名 = prior 消失事件 + 新身份存活事件）。漏记任一粒度必须使「局部重建 == 全量重扫」等价性测试翻红。压缩成功记一条 ClassMetadata（全类 CP 坐标变更）。

## 6. analysis：帧引擎

一套引擎、一个 transfer/merge 语义，core 写出不私藏帧算法。

- **三态 `FramePolicy`**：BestEffort（形状数据流，帧自预言 oracle 100%）；Strict（类型精确，未注入 resolver 显式 `Err(NoResolver)` fail loud，算不动以指令级诊断拒绝，不伪造帧）；Preserve（读侧独立入口 `readFramesFromSmt`，不进 compute 路由——用途是与计算帧对拍/自校验）。
- **resolver 契约**：`ITypeResolver::merge(VType, VType)` 单方法面，只解「两个具体引用类型的公共父类」。实现由调用方注入（类层级世界属上层）；默认降级 ShapeResolver（引用族互并退形状 Ref）。libjbc 保持单类职责，不做 SimpleVerifier 式层级校验。
- **两格分离**：BestEffort 形状格 top=占槽未知（吸收，merge(Top,X)=X，服务 maxes 收敛）；Strict verifier 类型格 top=不可用值（支配，merge(Top,X)=Top）。依据是 JVMS 4.10.1.4 帧匹配——生成 SMT 的唯一裁判是 verifier，汇合槽单边 top 陈述为具体类型 verifier 必拒。
- 异常边两档：基础档 handler 由异常表播种（空 locals + [newException]），种子以**待并参与者**形态持有、与真实入边同权汇合（verifier 口径：入口帧 = 全部入边 join），真实边缺席由 worklist 排空后的种子倾泻兜底；精确档引擎不播种，对 try 范围内每条**可达**指令向覆盖它的每个 handler 发异常出口边（可抛判定 = JVM 保守全集），handler 入口 = (抛出点 in-state locals, [catch_type 精确类])，catch_type=0 播 Throwable；层级缺席保守放行全并入；死 try 的 handler 不可达（verifier 对死代码同样不出约束）。
- jsr/ret：Strict 域数据流建模（返回地址载荷 = 调用点 srcPc；单调用点精确，多调用点共享入口显式 `Err(Subroutine)`——精确语义需 touched-bit-vector，不伪造）。含返回地址槽的帧不可编码为 SMT（封闭 tag 集无此形态），生成侧显式 Err。
- **SMT 生成**（`buildStackMapTable`，显式调用出活）：逐字陈述 Strict 域运行值精确类型（不做 javac 的声明投影，比对口径 = 公共前缀逐槽一致）；目标集合 = 分支/switch 目标 + handler 入口；紧凑族选择（same/same1/ chop/append/full）按 javac 极小化策略；cat2 展开垫底槽必须恰为 Top；tag-8 载荷必须命中方法内 new 的 pc；tag-7 经 internClass 落位；主版本 <50 显式 `Err(ClassTooOld)`；无控制流不产出属性。
- **maxes**：`applyComputedMaxes` 显式 API，重算即真值；verifier 地板并入（maxLocals ≥ 参数槽，maxStack ≥ 1 当带异常表）；超 u16 域不采用不改字段。只显式、不隐式。
- **值域搭车**：`DomainEngine<V>` 持有全部形状迁移知识（worklist、dup 槽位置换、cat2 两槽、描述符效应），值域 V 是载荷——消费者实现 `IValueDomain<V>` 五必选钩子（top/merge/newValue/newParameter/newException）+ 可选细分，几十行一个新值域。FrameEngine 是 DomainEngine<VType> 的壳。

## 7. type：描述符与泛型签名

- 描述符层：擦除后的运行时类型真相，槽宽计算、类名手术 span 面。
- 签名层（JVMS 4.7.9.1 全文法结构化树）：三形态 + 类型参数/通配符三态 + 嵌套泛型 + 段链。三条层契约：**roundtrip 恒等**（树不归一化，逐字节各归其位）；**畸形显式 Err**（带字节偏移，不猜测容忍）；**手术显式调用**（只经本模块 API，未命中零改写零追加）。
- 类名手术双形态全名匹配：'.' 段链（`Outer<T>.Inner`）与段名内嵌 '$'（`Outer$Inner`）；扁平段链按全名逐字节比对整段替换，真嵌套链部件越界不命中。
- 属性层手术入口（`jbc::ir::sig_attr` / `type_annotation_attr`，自由函数面）：Signature 属性（typed 单槽）、LVTT 行签名、TypeAnnotations（type_index + 'c' 类字面量，基类型跳过）。全有或全无三阶段（纯读预检 → 容量预检 → intern 落位置脏）；新串 `findUtf8` 查重复用、miss 才追加，逆向 rename 复用回原条目；未触碰属性写回逐字节 copy-through。

## 8. 封闭集：relocation 与 CP 压缩

两个高危操作都用同一哲学收口：**可处理对象是封闭清单，清单之外显式 Err（带定位），不静默丢、不猜、不放行。**

### 8.1 relocation 封闭集

布局改变时 Code 嵌套属性可重定位的种类封闭清单：LineNumberTable / LocalVariableTable·TypeTable / StackMapTable / TypeAnnotations（携带 pc 的目标才动）/ CharacterRangeTable。清单之外 `Err(RelocationNotModeled)`；已建模但 pc 不落指令起点 `Err(RelocationBadTarget)`。失败不留半改：预检全量通过前不触碰槽位。新嵌套属性进工具链的唯一路径是扩封闭集（typed 建模 + 重定向语义 + 测试）。

### 8.2 CP 压缩

`compactConstantPool` 是消费者主动调用的类级后置步骤，不集成进 lower（§5.2 管布局坐标、压缩管索引坐标，正交）。背景：只追加 + 大量改名/搬移会把池撑向 u16 上限。

- **可压缩类是封闭集**：全部属性可结构化重写时才可用。typed 全件在集内；非零长未知属性 `Err(AttrNotModeled)` + 属性名；零长体（Deprecated/Synthetic）构造上不可能携带 CP 引用，透传放行。
- **两相算法**：计划相只读（解码 → 校验封闭集 → 闭包标记 → 构造新池与映射，任何 Err 不留半截）；应用相按计划写入（每个值都在计划相定形，构造上不可失败）。失败后写回字节与进入前逐字节一致。
- **映射核心**：新池 = 活条目按老索引升序的子序列；内容等价类（Utf8 按字节、数值按位模式——-0.0/NaN 载荷保真、复合条目按 (tag, 子条目类号)，MethodHandle/Dynamic/indy 的 **a 列是键不是引用**，递归只进 b 列——误并会让「同 kind 异目标」句柄并成一条，只有 JVM 真跑能抓）只保留最小老索引作代表。**单调性是结构性结论**：对所有存活条目 newIdx ≤ oldIdx，故 LDC 压缩后必然仍可编码——不允许「按根序边遍历边落位」的方案回潮（会先遍历大索引把小索引挤过 u1）。
- 死条目清除：闭包外一律不落入新池（改名/搬移死链是池膨胀主因）；安全性由封闭集保证（引用面全部结构已知，不可达 ⟺ 不可引用）。
- no-op 早退：池已紧凑时整个压缩退化为不换池不记账，写回继续 copy-through。
- 会话一致性：已升格方法的指令操作数/异常表 catchCp 按映射同步重写；升格体的 DOM 面是新增引用面（陈旧 backing 体既不进闭包也不重写）；SemIndex 换池强制重建；旧池退居 retired 存储（文本视图存活期契约依赖其存活）。
- 验收门：压缩后 JVM 真跑 + 反射面行为恒等；同输入两次压缩逐字节一致；no-op 早退逐字节还原；拒绝面逐点名统计。

## 9. 类构建挂载面与 BSM intern

- **attach 家族**（ClassSession 公开 API）：attachConstantValue / attachMethodParameters / attachParameterAnnotations / attachAnnotationDefault / attachAnnotations ×{可见,不可见} / attachAttr / attachClassAnnotations / attachClassAttr。语义规格（ConstValueSpec / ParamNameRow / SemElemValue 十 const 形态 + 枚举/类字面量/嵌套/数组 / SemAnnotation）向 typed AttrBody 合成，CP 经 SemIndex 去重 intern。全链先算后写，失败不留半截。
- **两条硬契约**：(1) 描述符 arity 闸——MethodParameters 行数 / ParameterAnnotations 参数组数必须等于方法描述符参数个数（JVM 反射对失配显式抛异常），合成面提前 `Err(InvalidSeed)`；(2) 注解 element_value 的 's'/'c' 的 const_value_index **直指 Utf8**（javac 同形），与 ConstantValue 属性走 String 条目分账。
- 挂载单例规则封闭表：LNT/LVT/LVTT 多条合法，"Code" 拒收（BadMount），其余重复挂载 AlreadyMounted。
- **appendBootstrapMethod**：属性缺则建（类级单例）；条目同形全字段一致命中复用；静态参数按 loadable 十族规格落位；Dynamic 参数与孤立 internDynamic 均要求 BSM 锚点在位（JVM 对无锚 BSM 引用类解析即拒）。压缩协同：markCp 对 MH/Dynamic/indy 只标 b；孤立常量按死常量消除收掉。

## 10. 与上层的边界

libjbc 是单类字节核：库不拥有 Jar、不持有跨类索引、不做资源容器、不设跨类消费面（跨类扫描/索引/rename 编排属上层消费方，2026-10-17 裁决移出库边界）。

- 库向外部开放的只有注入点（`ITypeResolver` 等）；外部实现经注入进入，库不反向依赖任何索引或容器类型。
- CP index / pc offset / owner-scoped handle 不得跨模型或跨 class 使用；上层索引不保存完整 class 模型和内部句柄。
- 跨类修改由上层生成计划后逐类打开（每类一次独立的 ClassSession 生命周期）；统一提交/回滚/缓存失效是业务事务，不是库语义。

## 11. visitor 清零

判据不是类名而是数据形态：数据拉回来的是模型，事件推给消费者的是换皮 visitor——`std::function` 回调 sink、`visit*()` 事件先行、`forEach*` 推送，三形状任一即中。libjbc src 内 0 处 visitor。

门禁单测（`no_visitor_gate_test`）掩蔽注释与字符串后对 src 全量正则扫描，命中即 fail 并报文件：行号；门禁缺席也 fail（不 skip）。要放行新形状：先改本节，再改测试——门禁与文档同源。

## 12. 资源管理

- **一个死亡点**：ClassFile（或会话 shared 根）析构/归零一次回收全部。没有引用计数、没有跨对象共享所有权、没有调用方 delete 义务——「忘记释放」在结构上不可表达。
- 借用义务全部类型可见（string_view 寿命 = 会话、引用型参数调用期存活），签名即文档；保活句柄（`CodeUnitRef`、语义查询保活孪生）把「消费者替库管寿命」的义务接管回库，每次一次原子 incref。
- 三个去重域不混用：classfile CP 与 IR SymbolPool 用 arena（整批共存一起销毁）；上层索引用引用计数 pool（条目寿命各异）。没有全局 arena——它同时败于扫描内存单调增长、寿命绑定错误和与 lazy 冲突。

## 13. 语料与验证

### 13.1 语料解析序（三级取第一个可用者）

1. env 注入 `LIBJBC_REAL_CLASS_DIR`：权威指定，目录不存在按「无语料」处理，**不回退**；
2. 仓内自产（源在 `test_resource/java/jbc_corpus/`，测试期 `javac --release 8` 现场编译，增量重编）：保证无私产环境（CI、新机器）可跑全量；
3. ares 本机路径：私有环境专属回退。

纪律：源码入仓、产物不入仓；样本覆盖面与门禁需求对齐（控制流全形态、异常族、泛型桥方法、枚举 switch map、lambda indy、wide、UTF-8、注解、监视器；另有 nest/record/module/annot 独立小语料按版本旗语现场编译）；帧 oracle 断言 methods > 1000 防样本退化；javac 缺席 → 显式 skip（环境能力问题），**仓内样本源编译失败 → 硬红**（入仓源是承诺）；同一套测试在自产与注入语料下全绿。

### 13.2 构建与验证入口

```sh
xmake f && xmake build libjbc && xmake build libjbc_unittest
xmake run libjbc_unittest --gtest_brief=1        # 全量门禁
xmake f --with_tests=n                           # 只构建库本体
```

ASan 纪律：MSVC 的 ASan 不做泄漏检测（泄漏走 CRT 调试堆）；ASan 必须根作用域全开（改 STL ABI，部分插桩链接期 LNK2038）；ASan 构建不链 gtest，只用于 `demo/` 轻量程序。`JBC_ASAN_SELFTEST=1` 先自证插桩生效防假绿。

### 13.3 硬不变量清单

- 未修改输入（含未知属性、尾部垃圾、全解码后）写回逐字节恒等；触碰才重编码。
- 结构损坏 → Err；语义不合规 → Recovered + 诊断；绝无静默成功或静默降级。
- CP 只追加；改写既有条目必须经置脏入口；压缩是唯一重索引出口且为封闭集上的显式调用。
- 高层 branch/switch/try/debug 引用全部 owner 校验；跨 CodeUnit target 结构性拒绝；删除被引用指令显式 StillUsed。
- 每个可失败操作当场返回该段原生 Result；无 sticky 首错、无回调推送、无第二条汇报通道。
- 帧语义全库一套（transfer/merge 在 DomainEngine）；生成 SMT 的唯一裁判是 JVM verifier。
