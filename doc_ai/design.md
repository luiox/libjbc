# design.md — libjbc 设计真源

> 全量拓扑真源已折回本仓 `docs/design.md`（2026-10-17，自 morpher 工作区逐字节收拢）；本文是早前的开源快照：仅属性面摘编，章节编号沿用既有代码引用（§27 属性编解码铁律、§33 relocation 封闭集、§36 CP 显式压缩三处与全量版同义）。**§32 两版不同义**——本文 §32 = typed 属性面摘编，全量版 §32 = 泛型签名层；代码注释里的 §32 系引用（§32.4 Signature 手术入口、§32.5 typed 升格、§32.9 等）一律按 `docs/design.md` 读。后续属性面改动仍先改本文再动代码，落地记录则归全量版生长。

## 27 属性编解码铁律

typed 属性体（AttrBody 派生族）的解码/编码共同边界，全部属性一体适用：

1. **半截体显式失败**：声明结构读到一半字节不足，解码返回 Err(Truncated)，不猜测容忍；畸形输入仍可被 lazy copy-through 表达（解码失败不影响写回原样 copy）。
2. **trailing 同族（§27.1）**：合法属性在声明结构之后应正好结束；多余字节属畸形输入但边界可确定——保留进属性体的 trailing 副本并降级记录，置脏重编码时原样写回，不静默丢弃。
3. **域宽闸门（§27.2）**：编码逐字段过 u16（及 u1 表长）域宽闸门，超界返 Err(Unencodable)，禁止 static_cast 静默截断——截断会产出计数错位/引用错条目的 class。
4. **可见性由槽名承载**：成对属性（Runtime{Visible,Invisible} 前缀族）共用同一属性体模型，AttrSlot::nameCp 区分可见性，体不感知。

## 32 typed 属性面

AttrKind 封闭集（建模属性全集）与各自的挂载点、CP 槽面：

| 族 | AttrKind | 挂载点 | CP 槽面 |
|---|---|---|---|
| Code | Code(1) | 方法级 | 指令操作数 + 异常表 catchType + 嵌套属性递归 |
| BootstrapMethods | BootstrapMethods(2) | 类级 | methodRef + args 全槽 |
| 行表族 | LineNumberTable(3)/StackMapTable(4)/LocalVariableTable(6)/CharacterRangeTable(9) | Code 嵌套 | LVT 的 name/desc；SMT 仅 Object 载荷；LNT/CRT 无 |
| 简单注解 | Annotations(5) | 类/字段/方法级 | 见 §32.1 |
| 结构元数据 | Signature(7)/SourceFile(10)/InnerClasses(11)/EnclosingMethod(12)/Exceptions(13) | 类/字段/方法级 | 各单槽或行槽（access_flags 不是索引） |
| 类型注解 | TypeAnnotations(8) | 类/字段/方法级 + Code 嵌套 | 见 §32.2 |
| Nest 族 | NestHost(14)/NestMembers(15)/PermittedSubclasses(16) | 类级 | CP Class 全槽 |
| Record | Record(17) | 类级 | 组件 name/descriptor 两槽 + 嵌套属性递归 |
| Module 族 | Module(18)/ModulePackages(19)/ModuleMainClass(20) | 类级 | 全段槽面见属性体注释（旗语位不是索引） |
| 注解族 | ParameterAnnotations(21)/AnnotationDefault(22)/MethodParameters(23)/ConstantValue(24) | 方法级（前两件）/方法级/字段级 | 见 §32.3 |

### 32.1 简单注解（Annotations）

Runtime{Visible,Invisible}Annotations 共用 AnnotationsAttr：`u2 注解数 + annotation[]`；annotation = `u2 type_index（CP Utf8 字段描述符）+ u2 值数 + {u2 元素名, element_value}*`。element_value 全形态：const 十态（'B' 'C' 'D' 'F' 'I' 'J' 'S' 'Z' 's' 单 u2 常量/类索引）+ 'e'（枚举：type_name + const_name 双 u2）+ 'c'（类字面量，单 u2）+ '@'（嵌套 annotation，递归）+ '['（数组：u2 项数 + element_value*，递归）。

### 32.2 类型注解（TypeAnnotations）

Runtime{Visible,Invisible}TypeAnnotations 共用 TypeAnnotationsAttr：每条 type_annotation = target_type + target_info（4.7.20.1 十形态分派）+ type_path + 注解体（与 §32.1 同一 element_value 建模）。CP 槽面只含 type_index 与元素值面；throwsIndex/supertypeIndex/catchIndex 是表行下标，**不是** CP 索引，压缩不得误重映射。

### 32.3 注解族（本批语义）

- **ParameterAnnotations(21)**：Runtime{Visible,Invisible}ParameterAnnotations（4.7.18/4.7.19）共用一体。体 = `u1 num_parameters + { u2 num_annotations + annotation[] }*`；与简单注解的差异只在多一层参数下标维（第 i 个内层表对应方法第 i 个参数槽，空内层表 = 无注解参数，合法且常见）；注解体与 §32.1 完全同构，复用同一 Annotation/ElemValue 模型与同一条读写通道，不造第二套。
- **AnnotationDefault(22)**：注解类型方法的默认值（4.7.22，仅 @interface 的带默认值方法携带）。体 = 单个 element_value，全形态复用 §32.1 的建模。
- **MethodParameters(23)**：参数名/旗语表（4.7.24，javac `-parameters` 产物，非注解但是参数维相邻小件）。体 = `u1 parameters_count + { u2 name_index, u2 access_flags }*`，行序 = 参数槽位，与 ParameterAnnotations 的外层下标同坐标系。name_index 为 0 合法（无源码名的合成参数不指名）；access_flags 是 ACC_FINAL/SYNTHETIC/MANDATED 位集，按不透明 u16 保真。参数名即手术面。
- **ConstantValue(24)**：static final 字段常量值（4.7.2，字段级）。体 = 单 `u2 constantvalue_index`，指向 CP 的 Integer/Float/Long/Double/String 五形态之一；与字段描述符的类型层一致性归上层语义，结构保真层原样承载。
- **Deprecated/Synthetic 不建模（决策）**：体恒零长，无 CP 槽、无手术面；压缩零长透传已无损放行，decode 落 Raw 字节副本保真无损。typed 化只添空壳类型，不消除任何拒收/保真缺口。

### 32.4 公开解码面

每件 typed 属性提供 `decodeXxxBody(ClassFile&, const AttrSlot&)` 公共直读入口：解码但不触碰槽状态（lazy 语义保持）；decodeAttrBody 的分派面同走这些解码器（分派 = 名字识别 + 解码 + setDecoded）。

## 33 relocation 封闭集

pc 偏移载荷只存在于 Code 嵌套属性族（LNT 行 pc、SMT offset 链、LVT 行 pc、CRT 行 pc）；typed 面其余属性只挂类级/成员级且无 pc 载荷，布局改动不需要、也不得对它们做任何重定位。

## 36 CP 显式压缩（计划/应用两相）

计划相只读：解码全部属性面 → 按 §32 封闭集分派 → 以引用闭包标记活条目 → 等价类归类（叶子按位模式、复合按 (tag, 子类号)、Utf8 按字节内容）→ 活条目按老索引升序赋新秩（代表 = 等价类最小老下标；Long/Double 双槽占两席）。应用相按映射一次性换池并重写全部引用面——每个写入值都在计划相定形，构造上不可失败；换池后校验引用自洽（内部一致性网）。

### 36.1 引用面分层

1. 骨架面：this/super/interfaces/成员名与描述符/属性名。
2. typed 属性体面：§32 各族按各自 CP 槽面闭包标记（注解族的嵌套注解/枚举双槽/类字面量/数组递归全进闭包；MethodParameters 只动 name_index，access_flags 是位集；ConstantValue 单槽）。
3. Raw 兜底：零长体构造上不可能携带 CP 引用，透传放行。

### 36.2 子序 + 代表去重

新池 = 活条目按老索引升序的子序列，等价类只保留最小老索引作代表。map_[k] ≤ k 对一切活条目恒成立，CP1（ldc）操作数域宽从不变宽——宽形态不收缩裁决的安全前提。

### 36.5 Raw 名单退役（收口）

压缩对未知属性的接受面收敛过程：B3 四件（SourceFile/InnerClasses/EnclosingMethod/Exceptions）先接 typed 分派并保留 Raw 名单分派兜底 → ConstantValue 与注解族四件接线后名单清空，Raw 名字分派的本地解析整体退役——非零长 Raw 体一律 Err(AttrNotModeled) 显式拒绝（属性名定位），拒绝面在 javac 级语料上为空（两种来源模式同门）。
