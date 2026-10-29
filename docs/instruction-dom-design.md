# 指令 DOM 与标签身份设计

指令编辑的核心不是把 `InsnList` 做成另一种链表，而是让一个对象同时拥有指令存储、顺序、控制流引用和边界引用。这样删除、插入、重排和跨方法复制都经过同一个 owner 校验，指令本身不需要保存链表指针，也不需要由调用方配对 `remove` 与 `delete`。

## 现有实现的判断

当前 `CodeDom` 已经采用了适合无 GC 语言的值语义存储。`slots_` 拥有指令，`order_` 保存程序顺序，`InsnId` 只作为外部句柄；删除只让槽失效并移除顺序项，指令没有 intrusive `prev`/`next`，也没有单独的 label 对象需要释放。`Insn` 不保存 owner 指针，owner 只出现在句柄中，因而不会给每条普通指令增加一个重复指针。

这条路线解决了 OW2 ASM `InsnList` 在 C++ 中最危险的部分：节点从链表摘下后仍然存在、节点销毁责任分散、节点和 label 的生命周期不一致，以及把一个带有旧链表邻接关系的节点直接插入另一个列表。当前的 `CodeDom::erase`、跨 DOM 句柄检查和 `generation` 失效检查已经把这些错误变成了可观察的错误结果。

分支、`switch` 操作数和异常表边界当前已经保存 `LabelId`，不再直接保存 `InsnId`。控制流字段与元数据边界（异常表、调试范围语义）通过标签角色区分：导入时每个控制流操作数获得独立的 `ControlFlow` 角色标签，调用方创建的是 `Boundary` 角色标签。仍待迁移的缺口是调试范围类属性：LNT/LVT/SMT 的 pc 依赖目前由 lower 的 relocation contract 处理（fail-closed 的 pcmap 重定位），尚未建模成 `LabelId`。

## 句柄与 owner

owner 是跨 `CodeDom` 安全所必需的身份信息，但它不应该是指令对象里的指针。一个目标句柄至少需要回答三个问题：它来自哪个指令域、它指向哪个槽、它是否已经失效。当前的 `{owner, slot, generation}` 正好表达这三个问题，且 owner 是每个句柄携带的整数，不是每条 `Insn` 都保存的字段。

把 owner 完全删掉只有两个安全选择。第一种是把句柄变成进程级永不复用的全局整数，并让每个 `CodeDom` 用索引表查找它；第二种是把 owner 作为不透明的外部类型参数传给每一个编辑操作。前者增加哈希表和全局 ID 的管理成本，后者会把校验责任重新推给调用方。对当前库而言，保留 owner-scoped 的整数句柄更简单，也比在每条指令上放指针更省内存。

后续若实测表明控制流引用的句柄占用成为瓶颈，可以把 `{owner, slot, generation}` 压成一个 `u64` token。压缩只改变存储布局，不改变 owner 校验、generation 失效和跨 DOM 拒绝的语义；在没有内存数据证明之前不提前引入位域限制。

## 指令列表的目标形状

`CodeDom` 应继续作为唯一的 `InstructionList`，但内部职责需要明确分成四个互相配合的区域。

(1) `InstructionStore` 负责槽位和 generation。槽位中的 `Insn` 是值对象，分支操作数只保存不透明句柄，不保存邻接指针。槽位地址不能成为外部身份，任何结构变化都必须通过句柄重新查询。

(2) `Order` 负责程序顺序。正常插入和删除只修改 `Order` 与槽位状态，不能让一条指令携带旧列表的链表关系。需要把一段代码从一个方法复制到另一个方法时，必须通过显式的 `DetachedCode` 变换，把内部指令句柄、标签和异常范围一起重映射；库不提供把带有旧句柄的 `Insn` 直接塞进另一个 DOM 的入口。

(3) `LabelTable` 负责控制流和元数据边界。标签是整数句柄指向的边界对象，不是指令节点，也不参与指令所有权。一个标签可以绑定到某条指令的前置边界，也可以绑定到方法末尾；标签的解析结果由所属 `CodeDom` 计算，不在 `Insn` 中缓存裸指针。

(4) `UseIndex` 负责反向引用。当前实现按需全量扫描：`usersOf` 覆盖 branch/switch 操作数字段，存活的 `Boundary` 标签锚点在 `erase` 中独立拦截；异常表边界是 `Boundary` 标签的锚点保护，行号/局部变量范围当前经 lower 的 relocation 契约保护。删除前先查询 use，`StillUsed` 说明需要显式重定向；重定向成功后再删除。后续若建立真正的反向索引，接口和错误语义必须与当前扫描实现一致。

## 标签语义

标签的身份必须独立于指令身份，但标签的语义必须是一个可重算的边界。推荐的最小表示是 `LabelId { owner, slot, generation }`，其中 slot 指向 `LabelSlot { anchor, live, generation }`，`anchor` 是某条 `InsnId` 或方法末尾哨兵。它仍然是整数句柄，没有 Java 对象、C++ 裸指针或 intrusive 节点。

插入操作要明确标签策略，而不能依赖链表偶然顺序。`insertBeforeInsn(id, in)` 把新指令放在目标指令之前，同时保留附着在目标前置边界的标签；`insertAtLabel(label, in)` 把新指令放在标签所在边界，标签继续指向插入后的第一条指令；`insertAfterInsn(id, in)` 把新指令放在目标之后。这样“跳转是否包含新插入的代码”由 API 名称决定，不由调用方猜测。

从 classfile 导入时，分支目标 pc 映射为目标指令前的标签，异常表的 `start_pc`、`handler_pc` 和 `end_pc` 也全部映射为标签。每个控制流操作数独立创建标签：两个 goto 跳到同一条指令时各自持有不同的 `LabelId`，只是解析到同一边界——一次重定向因此只影响一个字段，也不会把控制流 use 和元数据边界混在一起。写出时先由标签解析当前边界，再把边界转为 pc；标签没有固定 offset，插入、删除和重排不会留下需要手工修补的数字。

方法末尾标签必须是合法的一等边界。`LabelTarget` 的 `kind == End` 与无效句柄（Result 的 Err）在类型上分开，异常表的开区间端点直接使用 End 标签；`end_pc == code_length` 的导入建立 End 标签，lower 时 End 端点映射到新 code 长度。

## 跨列表复制与删除

单条指令不能在两个 DOM 之间直接移动，因为它的分支和元数据引用仍属于源 owner。跨列表操作应返回一个拥有型的 `DetachedCode`，其中包含指令序列、标签表、异常范围和一张旧句柄到新句柄的映射；插入目标 DOM 时一次性分配新身份并重写所有内部引用。无法解析的引用必须返回错误，不能把旧整数当成目标列表中的合法 slot。

删除指令时，仍被 branch/switch 操作数字段引用（`usersOf`）或被存活 `Boundary` 标签锚定的指令返回 `StillUsed`。`replaceAllUses(from, to)` 只改写 branch/switch 操作数字段：每个字段获得一枚锚定 to 的新 `ControlFlow` 标签，锚定同一指令的 `Boundary` 标签保持原位，导入产生的旧 `ControlFlow` 标签随后销毁。字段改写后遗留的历史控制流标签不再保护锚点——`CodeEditor::setTarget` 替换目标后删除旧目标不应被这类残留标签阻止。若异常表、局部变量表或行号表仍引用某边界，必须显式处理；这个规则保证“删节点后留下悬垂 label”在 API 层不可达。

## 与上层 IR 和上层索引的边界

`libjbc` 只负责单方法 DOM 的身份、顺序、边界和写回重定位。基本块、可达性、调用图、交叉引用和全局重排属于上层索引，它们读取稳定的 `MethodId`、方法签名和引用站点，不保存 `Insn*`、`Label*` 或 `ClassFile*`。

上层索引需要在方法编辑完成后重新扫描受影响 class 的引用事实。未来可以根据 `ChangeSet` 做局部重建，但局部重建必须与全量扫描得到相同的 `ReferenceSite` 和 `SymbolKey`；在这个等价性没有测试证明前，默认使用全量索引重建。

## 迁移顺序

(1) `LabelId` 标签表、末尾边界与跨 DOM 标签测试已落地（`createLabel`/`bindLabel`/`destroyLabel`/`resolveLabel`，`LabelTarget` 区分 End 与 InvalidHandle）。

(2) `CodeUnit::Exc` 已迁移到 `LabelId`；promote 为 `end_pc == code_length` 建立 End 标签，落在指令操作数中间的 `end_pc` 被拒绝。

(3) 分支与 `switch` 的目标已迁移到 `LabelId`：导入时每个操作数获得独立的 `ControlFlow` 标签，`CodeEditor::setTarget`、`replaceAllUses` 与分析器统一经 `resolveLabel` 解析后继。LNT/LVT/SMT 仍由 lower 的 pc relocation 处理，尚未建模成 `LabelId`；后续按消费者需求拉动再迁。

(4) 在真实 pass 使用标签 API 后，再根据测量结果决定是否把句柄压缩为 `u64`。没有内存基线之前，不用裸 `u32` 取代 owner-scoped 句柄，也不恢复 intrusive list。

## 验收不变量

(1) 指令对象不含 owner 指针、前后链表指针或需要调用方释放的资源。

(2) 任意跨 `CodeDom` 的指令句柄或标签句柄都被拒绝；删除后旧句柄和旧标签都被拒绝。

(3) 插入、删除、重排只改变 `Order`、槽位状态和标签边界，不直接写入 pc offset；写出前统一计算 pc。

(4) 分支、`switch` 和异常表的边界通过同一个标签解析协议得到，无法解析时返回结构化错误；局部变量表和行号表当前由 lower 的 fail-closed relocation 保护，尚未建模为标签。

(5) 跨方法复制通过显式 detach/attach 重映射完成，复制后的引用不依赖源 DOM 的 owner 或存储地址。

