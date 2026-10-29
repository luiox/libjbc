# libjbc

node-first 的 JVM 字节码 C++ 建模库：

- **core**：单类 lazy classfile DOM——骨架 eager（头 / CP / 成员元数据 / 属性目录），属性内容按需解码；保真写回：未触碰的字节原样 copy，触碰才重编码。
- **ir**：CodeDom 生成式句柄 + ClassSession promote/lower + 语义操作数层。
- **analysis**：逐指令到达帧（strict 帧域）+ StackMapTable 生成 / 读回。
- **type**：字段/方法描述符解析 + 结构化泛型签名树与类名手术面。

设计取向：模型是数据（DOM），读写是行为（codec）——visitor 事件协议锁在 codec 内部，不进公共 API；错误走值语义（Result），不抛异常。

## 文档

- `docs/design.md` — 设计真源（现行架构拓扑与硬契约，只写现状不记过程史）。
- `docs/instruction-dom-design.md` — 指令 DOM 与标签身份的设计依据。

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
