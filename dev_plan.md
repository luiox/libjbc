# dev_plan.md

里程碑前的批次堆叠账：版本号不发、tag 不打，全部补丁式开发堆叠在当前版本
之上，到里程碑一次发版。

## 版本口径（堆叠期纪律）

- 当前版本 `0.4.15`：堆叠期内 **不 bump**——`get_lib_version` 返回值、
  xmake `set_version`、库版本测试期望均不动，不新增 tag，不动包仓条目。
- 发版触发条件（里程碑）：注解族属性面齐（已完成，见下）+ 类构建能力面齐
  （未完成）。两者齐后一次性 bump 发版。

## 已堆叠批次

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

## 待办（里程碑前）

- [ ] 类构建能力面（NewClassSeed 侧补齐注解族属性的合成/挂载入口）。
- [ ] 里程碑发版：一次 bump 版本号 + tag + 包仓条目更新（堆叠批次随发版说
      明一并列出）。
