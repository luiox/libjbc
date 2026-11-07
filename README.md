# libjbc

node-first 的 JVM 字节码 C++ 建模库：

- **core**：单类 lazy classfile DOM——骨架 eager（头 / CP / 成员元数据 / 属性目录），属性内容按需解码；保真写回：未触碰的字节原样 copy，触碰才重编码。
- **ir**：CodeDom 生成式句柄 + ClassSession promote/lower + 语义操作数层。
- **analysis**：逐指令到达帧（strict 帧域）+ StackMapTable 生成 / 读回。
- **type**：字段/方法描述符解析 + 结构化泛型签名树与类名手术面。

设计取向：模型是数据（DOM），读写是行为（codec）——visitor 事件协议锁在 codec 内部，不进公共 API；错误走值语义（Result），不抛异常。

## 文档

- `docs/design.rst` — 设计真源（现行架构拓扑与硬契约，只写现状不记过程史）。
- `docs/instruction-dom-design.rst` — 指令 DOM 与标签身份的设计依据。
- RST 规格层本地构建：`pip install "sphinx==9.1.1"` 后 `sphinx-build -b html docs docs/_build`。

## 构建

依赖经 xmake 自动拉取（libca / libjdk_finder 来自 [luiox-repo](https://github.com/luiox/luiox-repo)，gtest 来自 xrepo）：

```sh
xmake f                    # 默认 --with_tests=y
xmake build libjbc         # 库本体（静态）
xmake build libjbc_unittest
xmake run libjbc_unittest --gtest_brief=1
```

只构建库本体：`xmake f --with_tests=n`。

## 测试语料

两档解析：`LIBJBC_REAL_CLASS_DIR` 环境变量注入的目录 > 仓内样本（`test_resource/java/jbc_corpus`，测试期 `javac --release 8` 现场编译）；两档全缺席时语料门 skip 而不是硬失败。

## 最小示例

```cpp
#include "jbc/jbc.hpp"

using namespace jbc::core;

auto r = readClass(ByteView{bytes.data(), bytes.size()});
if (r.is_err()) return;               // 错误即值，不抛异常
auto rr = std::move(r).unwrap();
ClassFile& cf = *rr.file;

std::string_view name = cf.thisName(); // 零拷贝视图

AttrSlot& slot = cf.methods[0].attrs[0];
if (cf.decodeAttr(slot).is_ok()) {
    auto* code = static_cast<CodeAttr*>(slot.mutate());   // 唯一修改入口，顺手置脏
    code->maxStack = 8;
}

auto w = writeClass(cf);               // 未触碰 copy 原样，触碰重编码
```

## License

MIT（见 [LICENSE](LICENSE)）。

## 文档规范

三层按**权威来源**分工，扩展名即角色：

| 载体 | 层 | 写什么 | 谁能引用它 |
|---|---|---|---|
| `///` Doxygen | 微观 | 单个符号的契约：参数、返回、失败模式、不变量；单模块的「背景/定位」（`@defgroup` 组注） | 随代码移动 |
| `docs/*.rst` | 宏观 | 跨模块设计、架构拓扑与硬契约 | 代码/测试/构建脚本 |
| `*.md` | 人 + AI 方向 | README 等流程文档 | 只有人和 AI 读 |

理由：markdown 撑不住代码耦合的规格——无 label、无校验引用面，代码改动后引用静默腐烂。RST 白送可校验的引用面，规范才谈得上执行。

硬规则（`docs/check_refs.py` + Sphinx `-W` 在 CI 挡）：

1. 代码/测试/构建脚本只可引用 `docs/*.rst`，路径必须存在；出现 `.md` 设计引用即失败（流程文档白名单除外）。
2. 引用定位用 label，形式 `doc:<label>`；被引用的位置必须有 `.. _<label>:` 声明（不必每节都放），命名 `<doc-slug>[-<section-slug>]`。跨文档的 `§N` 一律换 `:ref:`，编号会随插入章节漂移。
3. RST 里「`X` 头注」指向微观层：`X` 必须是某个 `@defgroup` 组名或头文件名。
4. RST 站点以 `sphinx-build -b html -E -W --keep-going docs docs/_build` 构建，警告即失败；本地验证先 `pip install "sphinx==9.1.1"`（与 CI 钉同版）。
5. 微观层以 `doxygen docs/Doxyfile` 构建，`WARN_AS_ERROR` 生效；`///` 注释是 API 契约的唯一来源，与代码同仓同改。

RST 写作约定（中文正文的坑）：表格一律 `.. list-table::`（禁 grid table，全角字形会破坏边框校验）；行内代码紧邻中文或全角开括号时在闭定界符后补转义（docutils 要求 end-string 后接空白或 ASCII 标点）；标题层级 `=` / `-` / `^`，下划线按东亚列宽计长（全角算 2 列）；跨仓出处带仓名标记（libjbytecode / libca），带标记视为 prose 不校验存在性。
