# dev_plan.md

里程碑前的批次堆叠账：版本号不发、tag 不打，全部补丁式开发堆叠在当前版本
之上，到里程碑一次发版。

## 版本口径（堆叠期纪律）

- 当前版本 `0.4.15`：堆叠期内 **不 bump**——`get_lib_version` 返回值、
  xmake `set_version`、库版本测试期望均不动，不新增 tag，不动包仓条目。
- 发版触发条件（里程碑）：注解族属性面齐（已完成，见下）+ 类构建能力面齐
  （未完成）。两者齐后一次性 bump 发版。

## 已堆叠批次

### BSM 语义 intern 家族（本批，已完成）

- CP 语义 intern 增量：internMethodHandle / internMethodType / internModule /
  internPackage / internDynamic——「命中既有池不膨胀，miss 才追加」同一纪律，
  SemIndex 扩 mhConst（kind+目标）/ mtConst（描述符）/ namedConst（tag+名）
  三张去重表，Dynamic 并入 ref 表（bsm 下标占 a 槽）。
- MethodHandle 域闸：reference_kind 1..9 封闭表按 kind 分流 Fieldref/
  Methodref/InterfaceMethodref（JVM 类解析对域外即拒，合成面提前闸下
  Err(InvalidSeed)）；type 层一致性（kind 与成员形态匹配）归上层并归运行时。
- BootstrapMethods 属性面 appendBootstrapMethod：attr 缺则建（类级单例）、
  条目同形全字段一致命中复用（同形条目对 JVM 无可辨差异；ASM 同思路合并）、
  静态
  参数按 loadable 十族规格落位（BsmArg：整型一律 Integer 无窄化语义 +
  String/Class/MethodType/MethodHandle/Dynamic）。Dynamic 参数与孤立
  internDynamic 均要求 BSM 锚点在位（attr 在、下标在条目数内）——JVM 对
  无锚 BSM 引用类解析即拒。
- 全链「先算后写」不变：句柄域闸 → 域宽（条目数/参数数 u2）→ 解码既有槽
  （MalformedAttr = 模型侧损坏，新增 LowerError 码）→ 锚点/规格合法性 →
  规格容量预检（miss 才计、保守超计不漏计，utf8-hypothetical 剪枝与 intern
  实现同构）→ 合成 → 同形命中短路 → 条目推送；CP 容量边界拒下池与属性
  表纹丝不动。
- ChangeSet：实改（属性诞生/条目追加）记 ClassMetadata，纯命中复用不记账。
- 压缩协同：新四 tag（MethodHandle/MethodType/Module/Package/Dynamic）的
  markCp 闭包与重排收编既有封闭集覆盖（markCp 对 MH/Dynamic/indy 只标 b）；
  BSM 条目间同引用去重（两个条目共享同一句柄条目、同一 condy）在压缩后
  经 SemIndex 重建依旧命中；无消费者的孤件常量按死常量消除语义收掉。
- 测试 `test/unittest/bsm_intern_test.cpp` 九件：纯模型门（去重/形态逐跳/
  错误族/容量边界/压缩两态/记账）+ JVM 真跑门（BSM+句柄条目类 v52 全域
  可载；condy 类 v55 按 java.version 分判——JDK8 解析器不识 tag 17，拒收
  记环境事实）。本地 JDK8 全绿；CI temurin 17 复验。

### 注解族属性 typed 建模 + cp_compact 收编（本批，已完成）

- ParameterAnnotations / AnnotationDefault / MethodParameters / ConstantValue
  四件 typed 建模 + 分派 + 公开解码面 + 写回保真；注解体与简单注解族共用
  同一 Annotation/ElemValue 模型与读写通道。
- cp_compact 封闭集收编：四件计划相闭包标记 + 应用相重映射；Raw 名单分派
  退役收口（非零长 Raw 体显式拒绝）。压缩拒收面清零：自产语料 5（全是
  AnnotationDefault）→ 0；env 注入语料 54（全是 MethodParameters）→ 0。
- 语料：新增 annot 小语料（javac `--release 8 -g -parameters` 现场编译，
  manifest 旗语指纹），保证 MethodParameters/AnnotationDefault/参数注解双
  保留在自产模式下可复现。
- Deprecated/Synthetic 评估结论：不建模（体恒零长、无 CP 槽无手术面，零长
  透传已无损，typed 化无净收益）。
- 文档：doc_ai/design.md 随本批重建（开源快照未携带），属性面语义按既有章
  节编号收拢。

### 类构建挂载面 attr_build（本批，已完成）

- 新文件 `src/jbc/ir/attr_build.cpp`：ClassSession 公开挂载 API——
  attachConstantValue / attachMethodParameters / attachParameterAnnotations /
  attachAnnotationDefault / attachAnnotations（方法/字段）×{可见,不可见} /
  attachAttr（方法/字段）/ attachClassAnnotations / attachClassAttr（类）。
  规格面语义合成：ConstValueSpec / ParamNameRow / SemElemValue(
  十 const 形态 + 'e' 枚举 + 'c' 类字面量 + '@' 嵌套 + '[' 数组) /
  SemAnnotation → typed AttrBody，CP 经 SemIndex 去重 intern。
- 全链「先算后写」：句柄 → 形状/域宽（u1 行数、u16 元素）→ 挂载预检
  （单例规则封闭表：LNT/LVT/LVTT 多条，"Code" 拒收 → BadMount，其余
  AlreadyMounted）→ 规格容量预检 → 合成 → 落位；失败不留半截。
- **描述符 arity 硬契约**：MethodParameters 行数 / ParameterAnnotations
  参数组数必须等于方法描述符参数个数（JVM 反射对失配显式抛
  MalformedParametersException / HotSpot 类解析显红）——合成面提前闸下
  （Err(InvalidSeed)），与 addMethod 重名预检同一纪律。
- **JVMS 对照修正（javap + JVM 真跑对照实验锁定）**：注解 element_value 的
  's' 与 'c' const_value_index **直接指向 Utf8**（javac 同形）——不隔
  CONSTANT_String / CONSTANT_Class（ConstantValue 属性仍走 String 条目，
  两者不同账）。'c' 载荷为类型描述符形态（基本类 "I"/类 "Ljava/lang/String;"），
  JVM 解析语义即 class-literal（int.class 等）。
- ChangeSet 记账：成员级挂载记 MemberMetadata，类级记 ClassMetadata。
- 测试 `test/unittest/class_build_attrs_test.cpp` 双模式：纯模型门
  （合成 → 重读 typed 逐字段断言 / 错误族：AlreadyMounted、BadMount、
  DomainOverflow、InvalidHandle、InvalidSeed、深度闸 128；压缩收编后语义
  为保持；ChangeSet 折叠）+ JVM 真跑门（本地 JDK8 通过，比 CI temurin 17
  更保守）：反射读回参数名/旗语、参数注解全族（嵌套/数组/枚举/类字面量）、
  注解默认值、类级注解默认值填充、ConstantValue 静态常量（int/String）；
  枚举常量由真实 Java 源（bu/Op）经 extraJavaSources 同场编译——合成注解
  与 javac 产物同 classpath 混合部署互操作实证。

## 待办（里程碑前）

- [x] BSM 语义 intern 家族（internMethodHandle/internDynamic/internModule/
      internPackage + BSM 属性构建/改写面）——账目见上「已堆叠批次」。
- [ ] E-track 消费者协议边界（E1 ReferenceSite 导出面 / E3
      IClassSource/SummaryScanner 注入式只读面 / rename 跨类编排）——实现
      + 合成类直验已堆叠（`src/jbc/consumer/`，consumer_scan/rename_test
      16 例全绿；JVM 闸与真实语料的 consumer 面回归待批 4 Grunt 验证）。
- [x] 文档归位：morpher 侧 doc_ai/design.md 全量章节折回独立仓
      （`docs/design.md` 逐字节收拢 + 折回说明/§编号差异警示 + §37 落地
      记录；兄弟文档四件随折；误引修正 rename.hpp §9、signature.hpp 指
      docs 版）。
- [ ] Grunt 实通过验证（对 boar-main*.jar 语料写数个 pass）。
- [ ] 里程碑发版：一次 bump 版本号 + tag + 包仓条目更新（堆叠批次随发版说
      明一并列出）。
