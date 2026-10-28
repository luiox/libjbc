# dev_plan.md

里程碑前的批次堆叠账：版本号不发、tag 不打，全部补丁式开发堆叠在当前版本之上，到里程碑一次发版。

## 版本口径（堆叠期纪律）

- ~~当前版本 `0.4.15`：堆叠期内 **不 bump**——`get_lib_version` 返回值、xmake `set_version`、库版本测试期望均不动，不新增 tag，不动包仓条目。~~
- **v0.4.16 已发版（2026-10-17）**：触发条件两条皆齐（注解族属性面 + 类构建能力面 + 消费者协议面）。堆叠批次随发版说明见下「v0.4.16 发版说明」一节。
- **v0.4.16 之后重新进入堆叠期（2026-10-20 起）**：新增批次不 bump、不打 tag，随下个里程碑一次发版。

## v0.4.18 发版说明

自 `0.4.17` 落地 CodeEditor 区间搬移原语 moveRange（issue #10），版本 bump 至 0.4.18。`CodeEditor::moveRange(first, last, at, policy)` 把 `[first, last)` 连续指令段（段内相对顺序不变）整体搬到锚点之前，补上搬移面的区间粒度：机械层 `CodeDom::moveRange` 单趟重排 `order_`，标签锚定在指令句柄上，段内标签随段迁移与段外引用保持由句柄锚定自然成立，不改写任何字段；策略层 `MoveRangePolicy` 裁决段首边界标签两种语义——`kHeadFollows`（默认，随段走）/`kHeadStays`（留原位承接原 fallthrough，锚点重定向到原段位后继，段居尾改绑方法末边界）。拒绝面：空段/倒置段/锚点在段内 → 新错误码 `EditError::BadRange`；kHeadStays 遇异常表引用的段首标签或段居尾时控制字段仍引用的段首标签 → StillUsed；全部校验先于变更，任一步失败不留半搬状态。尾哨兵 `last == kNoInsn` 表示段延伸到方法末尾（half-open 语义下尾段的可命名形）。消费方：mjbase 块重排类优化 pass（GotoRearranger 迁移件）。连带修复：0.4.17 批次漏更的 `get_lib_version` 与库版本测试期望（停在 0.4.16）随本次 bump 一并对齐 0.4.18。测试 ir_test 新增五件（正向搬移含分支标签随迁 / kHeadStays 承接者改绑与尾段 end 边界 / 坏几何拒绝与原子性 / 异常表边界双策略对照 / 与 insertAtLabel、eraseBatch 组合）；设计记录见 docs/design.md §39。

## v0.4.17 发版说明

自 `0.4.16` 移除 `jbc::consumer` 模块（E3 源/骨架、E1 引用事实、rename 跨类编排）及其闭环 demo（libjbc_deobf_demo）与三支 consumer 测试，版本 bump 至 0.4.17。边界裁决：source/scan 消费者协议面与 rename 跨类编排是上层消费方设施，不属于 libjbc 的单类字节核边界（core/ir/type/analysis 之外不设消费面）；同名能力由上层仓单份持有（mjbase 的 consumer/renamer），两仓平行实现随之消除。连带清理：伞头 jbc.hpp 收编面与 units.hpp 注释中的 consumer 交叉引用。v0.4.16 引入的其余批次（MethodEditor、BSM intern、属性挂载、文档）不受影响。

## v0.4.16 发版说明（2026-10-17）

自 `0.4.15` 一次性 bump，堆叠批次并入本版：

1. **类构建挂载面（批 0+1）**：注解/常量/参数名/注解默认值族的语义规格 → typed 属性合成挂载（attach 家族），full-jvm 反射读回门；
2. **BSM 语义 intern 家族（批 2）**：五件去重 intern + BootstrapMethods 条目面（append/复用/loadable 参数落位 + 锚点闸）；
3. **E-track 消费者协议面（批 3）**：`jbc::consumer` 模块——E3 源/骨架、E1 引用事实、rename 分族应用 + 注入 resolver + fail-closed 跨类驱动，BSM 载体成员规则 fail-loud 闸；
4. **文档归位（批 4a）**：`docs/design.md` 全量拓扑真源 + 兄弟文档，§37 落地记录；
5. **Grunt 实语料验证（批 4b）**：boar-main/origin 双 jar 五支 pass 编组（字节恒等/骨架/引用/全语料改名/fail-loud 实证），语料门 env 注入。

包仓条目同步更新（luiox-repo）。已知未竟（下里程碑）：rename v1 的 BSM/indy 改写通道、StackMapTable Object 载荷的类名族覆盖等 §37.3 明示的封口；JVM 级语料 define-sweep 已在发版后跟进收口（consumer_corpus_test.JvmDefineSweep）。

## 当前批次（feat/method-editor，未合入）

### 从零建方法门面 MethodEditor（已完成）

- 新文件 `src/jbc/ir/method_editor.*`：`MethodEditor`（open/append/code/unit/commit）与 `addMethodWithBody`（直线体一步落盘），把 addMethod → promoteMethod → CodeEditor → lowerMethod 四段接线压成两次调用或一次调用。只压缩接线层数，错误模型不变：每个可失败操作当场返回 Result，无 sticky 失败态、无析构自动提交、无回调。
- 复合错误 `MethodBuildError`（Stage::Add/Promote/Edit/Lower + 原族错误码 + insnIndex）与 SessionError 同构，只作返回值载荷。批量入口按指令形状拒绝控制流（未绑定分支的 target 恰是 kNoLabel，按目标判会漏放），任一步失败回滚已挂成员；CP 追加不回退。
- 放弃语义：open 即挂入成员，未 commit 即析构只丢弃编辑，成员以空体留在会话，收尸（removeMethod）归调用方；成员挂载与编辑落盘是两件事。
- `MethodSeed.withCode` 必须为 true；maxStack/maxLocals 仍由 seed 给定。类级 ClassDraft、与 ClassBuilder 的字节对拍、mj2x/mjt 消费面迁移留待后续（issue #8 其余验收项）。
- 测试 `test/unittest/method_editor_test.cpp` 六件：批量建体重读 / 编辑器 append-commit 重读 / 放弃编辑器留空体成员 / append 失败不污染编辑器 / 控制流前置拒绝与下标报告 / 种子失败不留成员。

## 已堆叠批次（封账）

### BSM 语义 intern 家族（已完成）

- CP 语义 intern 增量：internMethodHandle / internMethodType / internModule / internPackage / internDynamic——「命中既有池不膨胀，miss 才追加」同一纪律，SemIndex 扩 mhConst（kind+目标）/ mtConst（描述符）/ namedConst（tag+名）三张去重表，Dynamic 并入 ref 表（bsm 下标占 a 槽）。
- MethodHandle 域闸：reference_kind 1..9 封闭表按 kind 分流 Fieldref/Methodref/InterfaceMethodref（JVM 类解析对域外即拒，合成面提前闸下 Err(InvalidSeed)）；type 层一致性（kind 与成员形态匹配）归上层并归运行时。
- BootstrapMethods 属性面 appendBootstrapMethod：attr 缺则建（类级单例）、条目同形全字段一致命中复用（同形条目对 JVM 无可辨差异；ASM 同思路合并）、静态参数按 loadable 十族规格落位（BsmArg：整型一律 Integer 无窄化语义 + String/Class/MethodType/MethodHandle/Dynamic）。Dynamic 参数与孤立 internDynamic 均要求 BSM 锚点在位（attr 在、下标在条目数内）——JVM 对无锚 BSM 引用类解析即拒。
- 全链「先算后写」不变：句柄域闸 → 域宽（条目数/参数数 u2）→ 解码既有槽（MalformedAttr = 模型侧损坏，新增 LowerError 码）→ 锚点/规格合法性 → 规格容量预检（miss 才计、保守超计不漏计，utf8-hypothetical 剪枝与 intern 实现同构）→ 合成 → 同形命中短路 → 条目推送；CP 容量边界拒下池与属性表纹丝不动。
- ChangeSet：实改（属性诞生/条目追加）记 ClassMetadata，纯命中复用不记账。
- 压缩协同：新四 tag（MethodHandle/MethodType/Module/Package/Dynamic）的 markCp 闭包与重排收编既有封闭集覆盖（markCp 对 MH/Dynamic/indy 只标 b）；BSM 条目间同引用去重（两个条目共享同一句柄条目、同一 condy）在压缩后经 SemIndex 重建依旧命中；无消费者的孤件常量按死常量消除语义收掉。
- 测试 `test/unittest/bsm_intern_test.cpp` 九件：纯模型门（去重/形态逐跳/错误族/容量边界/压缩两态/记账）+ JVM 真跑门（BSM+句柄条目类 v52 全域可载；condy 类 v55 按 java.version 分判——JDK8 解析器不识 tag 17，拒收记环境事实）。本地 JDK8 全绿；CI temurin 17 复验。

### 注解族属性 typed 建模 + cp_compact 收编（已完成）

- ParameterAnnotations / AnnotationDefault / MethodParameters / ConstantValue 四件 typed 建模 + 分派 + 公开解码面 + 写回保真；注解体与简单注解族共用同一 Annotation/ElemValue 模型与读写通道。
- cp_compact 封闭集收编：四件计划相闭包标记 + 应用相重映射；Raw 名单分派退役收口（非零长 Raw 体显式拒绝）。压缩拒收面清零：自产语料 5（全是 AnnotationDefault）→ 0；env 注入语料 54（全是 MethodParameters）→ 0。
- 语料：新增 annot 小语料（javac `--release 8 -g -parameters` 现场编译，manifest 旗语指纹），保证 MethodParameters/AnnotationDefault/参数注解双保留在自产模式下可复现。
- Deprecated/Synthetic 评估结论：不建模（体恒零长、无 CP 槽无手术面，零长透传已无损，typed 化无净收益）。
- 文档：doc_ai/design.md 随本批重建（开源快照未携带），属性面语义按既有章节编号收拢。

### 类构建挂载面 attr_build（已完成）

- 新文件 `src/jbc/ir/attr_build.cpp`：ClassSession 公开挂载 API——attachConstantValue / attachMethodParameters / attachParameterAnnotations / attachAnnotationDefault / attachAnnotations（方法/字段）×{可见,不可见} / attachAttr（方法/字段）/ attachClassAnnotations / attachClassAttr（类）。规格面语义合成：ConstValueSpec / ParamNameRow / SemElemValue(十 const 形态 + 'e' 枚举 + 'c' 类字面量 + '@' 嵌套 + '[' 数组) / SemAnnotation → typed AttrBody，CP 经 SemIndex 去重 intern。
- 全链「先算后写」：句柄 → 形状/域宽（u1 行数、u16 元素）→ 挂载预检（单例规则封闭表：LNT/LVT/LVTT 多条，"Code" 拒收 → BadMount，其余 AlreadyMounted）→ 规格容量预检 → 合成 → 落位；失败不留半截。
- **描述符 arity 硬契约**：MethodParameters 行数 / ParameterAnnotations 参数组数必须等于方法描述符参数个数（JVM 反射对失配显式抛 MalformedParametersException / HotSpot 类解析显红）——合成面提前闸下（Err(InvalidSeed)），与 addMethod 重名预检同一纪律。
- **JVMS 对照修正（javap + JVM 真跑对照实验锁定）**：注解 element_value 的 's' 与 'c' const_value_index **直接指向 Utf8**（javac 同形）——不隔 CONSTANT_String / CONSTANT_Class（ConstantValue 属性仍走 String 条目，两者不同账）。'c' 载荷为类型描述符形态（基本类 "I"/类 "Ljava/lang/String;"），JVM 解析语义即 class-literal（int.class 等）。
- ChangeSet 记账：成员级挂载记 MemberMetadata，类级记 ClassMetadata。
- 测试 `test/unittest/class_build_attrs_test.cpp` 双模式：纯模型门（合成 → 重读 typed 逐字段断言 / 错误族：AlreadyMounted、BadMount、DomainOverflow、InvalidHandle、InvalidSeed、深度闸 128；压缩收编后语义为保持；ChangeSet 折叠）+ JVM 真跑门（本地 JDK8 通过，比 CI temurin 17 更保守）：反射读回参数名/旗语、参数注解全族（嵌套/数组/枚举/类字面量）、注解默认值、类级注解默认值填充、ConstantValue 静态常量（int/String）；枚举常量由真实 Java 源（bu/Op）经 extraJavaSources 同场编译——合成注解与 javac 产物同 classpath 混合部署互操作实证。

## 待办

- [x] 从零建方法门面（issue #8 方法体部分）：`MethodEditor` + `addMethodWithBody`，Result-only、无 sticky；类级 ClassDraft、字节对拍与消费面迁移未做。
- [x] BSM 语义 intern 家族（internMethodHandle/internDynamic/internModule/internPackage + BSM 属性构建/改写面）——账目见上「已堆叠批次」。
- [x] E-track 消费者协议边界（E1 ReferenceSite 导出面 / E3 IClassSource/SummaryScanner 注入式只读面 / rename 跨类编排）——已落地并全链验收：合成直验 16 例（consumer_scan/rename_test）+ boar 实语料五支 pass（批 4，含 JVM defineClass 载入闸）。（v0.4.17 已随 consumer 模块移除，见顶部发版说明）
- [x] 文档归位：morpher 侧 doc_ai/design.md 全量章节折回独立仓（`docs/design.md` 逐字节收拢 + 折回说明/§编号差异警示 + §37 落地记录；兄弟文档四件随折；误引修正 rename.hpp §9、signature.hpp 指 docs 版）。
- [x] Grunt 实通过验证（对 boar-main*.jar 语料写数个 pass）（v0.4.17 已随 consumer 模块移除，见顶部发版说明）—— consumer_corpus_test 五支 pass 编组于注入语料实跑全绿：ZeroTouchByteExact（3065 混淆 + 775 干净逐字节恒等）/ SkeletonSweep / ReferenceSweep / RenameClassFamilyWholeCorpus（单类名规则全语料驱动，事实族旧名清零+新名引用集守恒，杂项列/operand 随行改写实证）/BsmTouchedFailLoud（混淆语料 carrier 必现 Err）。语料门 = `LIBJBC_GRUNT_JAR`[-`_ORIGIN`] 注入 + 缺席 skip（CI 无注入即跳）。JVM 级载入闸（v0.4.16 已知界限第 4 项，已收口）：改名 stage 全语料 defineClass 格式闸入本套件（consumer_corpus_test.JvmDefineSweep），JDK 版本探针枚举——`LIBJBC_SWEEP_JAVA_HOME` 注入 > `JAVA_HOME` > 标准安装位 glob（Win Adoptium/Microsoft/Java/Zulu、Linux /usr/lib/jvm），取最小 major>=17，缺席即 skip。
- [x] 里程碑发版：一次 bump 版本号 + tag + 包仓条目更新（堆叠批次随发版说明一并列出）——v0.4.16 bump 已提交，tag 在 PR 合并后于 main 打（`v0.4.16`），包仓条目随发版同步。
- [x] deobf 闭环 demo（`demo/deobf_demo.cpp` + xmake `libjbc_deobf_demo`）（v0.4.17 已随 consumer 模块移除，见顶部发版说明）：Grunt（renamers-only worker 作业流，JDK25）混淆 origin → obf.jar + mappings.json（class/methods/fields 全量）→ libjbc 按 dumpMappings 全规则组广播还原（jar 直读经 libca ZipFile，规避 Windows NTFS 大小写塌缩；zlib 包消费随 libca_zip 上游契约自带）→ 三重实证：775/775 全语料逐类改写、deobf 名集合与 origin 集合 775==775 双向零差、JVM defineClass sweep 548 ok + 227 外部缺失豁免 0 fail（与 origin 基线同分布）；`--members` 档如实呈现 v1 fail-closed 边界（BsmTouched，kotlin indy 载体，归 rename v2 口径）。随行扩界：签名手术**扁平段链跨形态**——Grunt 抹平嵌套段数、kotlin `$$inlined$` 连 $ 空段真名两实证对原全名匹配的 shape 守卫双杀；扁平链收敛为全名逐字节比对 + 整段 new 原文拼接（`isValidSigInternalName` 字符集闸），'$' 部件机器只留真嵌套 '.' 段链；`RenameShapeMismatch` 保留值并入不命中语义、现不再产生。demo 诊断通道（失败族定位 + 规则级二分）随行落地。
