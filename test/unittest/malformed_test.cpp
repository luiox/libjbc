/// @file        malformed_test.cpp
/// @brief       降级读路径：畸形输入样本族的契约断言。
///
/// 契约：每个畸形样本的产物必须显式——
/// 1. 结构损坏（边界无法确定）→ reader 阶段 Err，错误码逐族钉死，不产出模型；
/// 2. 结构可确定、语义不合规 → 读出模型标记 Recovered（或 decode 阶段
///    显式 Err），降级事实必须可查询（ParseState/诊断），绝不允许静默成功；
/// 3. 写回侧：读出的模型未触碰时写回必须字节一致（copy-through 与解码成败
///    无关）；降级模型写出必须仍可解析（原样保真），或显式拒绝。
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "jbc/jbc.hpp"
#include "test_class_bytes.hpp"

using namespace jbc::core;
using Bytes = std::vector<ca::u8>;
using jbc::test::AttrSpec;
using jbc::test::ClassBuilder;
using jbc::test::ExcSpec;

namespace {

/// 最小合法类：CP(this/super 的 Utf8+Class) + 空成员表 + 空类属性表。
Bytes minimalClass()
{
    ClassBuilder cb;
    cb.setThis(cb.classRef(cb.utf8("p/T")));
    cb.setSuper(cb.classRef(cb.utf8("java/lang/Object")));
    return cb.build();
}

/// 带一个方法的类：maxStack=1，code 内容与异常表可配。
Bytes classWithCode(const Bytes& code, std::vector<ExcSpec> exc = {})
{
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 codeIdx = cb.utf8("Code");
    const ca::u16 mIdx = cb.utf8("m");
    const ca::u16 voidIdx = cb.utf8("()V");
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setVersion(52);
    cb.addMethod(0x0009, mIdx, voidIdx,
                 {AttrSpec{codeIdx, jbc::test::codeContent(1, 1, code, exc)}});
    return cb.build();
}

/// 读取必须以指定错误码失败（结构损坏族）。
void expectReadErr(const Bytes& bytes, ReadError code, const char* what)
{
    const Result<ReadResult, ReadError> r = readClass(ByteView{bytes});
    ASSERT_TRUE(r.is_err()) << what << ": expected Err, got a model";
    EXPECT_EQ(r.unwrap_err(), code) << what;
}

}   // namespace

// ---------------------------------------------------------------------------
// 族 A：CP 结构损坏 → reader 阶段 Err（不产出模型）
// ---------------------------------------------------------------------------

TEST(MalformedTest, CpCountZeroIsRejected)
{
    Bytes bytes = minimalClass();
    bytes[8] = 0x00;   // constant_pool_count 高字节（magic4+minor2+major2 之后）
    bytes[9] = 0x00;
    expectReadErr(bytes, ReadError::BadConstantPoolCount, "cp count = 0");
}

TEST(MalformedTest, CpSelfReferenceIsRejected)
{
    // NameAndType 自引用：name_index/descriptor_index 指向自身 → tag 不符
    // （NaT ≠ Utf8），validateReferences 必拒。CP 布局钉死：两条 Utf8 +
    // 两条 Class 之后 NaT 落在索引 5。
    ClassBuilder cb;
    cb.setThis(cb.classRef(cb.utf8("p/T")));
    cb.setSuper(cb.classRef(cb.utf8("java/lang/Object")));
    const ca::u16 nat = cb.nameAndType(5, 5);
    ASSERT_EQ(nat, 5);   // 布局假设：自引用恰为自己的索引
    expectReadErr(cb.build(), ReadError::BadConstantPoolRef, "NaT self reference");
}

TEST(MalformedTest, CpTwoNodeCycleIsRejected)
{
    // 双节点环：Class#6.name = Utf8#7？不构成环——真正的环要 Utf8 参与，
    // 而 Utf8 无引用域。JVMS 的 CP 引用图是无环的（DAG），任何「环」都表现为
    // tag 不符或越界。这里钉一个 tag 不符环：Methodref.class_index 指向另一个
    // Methodref（不是 Class）。
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 thisCls = cb.classRef(thisIdx);
    const ca::u16 superCls = cb.classRef(superIdx);
    (void)thisCls;
    (void)superCls;
    const ca::u16 nat = cb.nameAndType(thisIdx, superIdx);
    const ca::u16 badRef = cb.methodref(nat, nat);   // class_index = NaT
    (void)badRef;
    expectReadErr(cb.build(), ReadError::BadConstantPoolRef, "Methodref -> NaT as class");
}

TEST(MalformedTest, MemberNameNotUtf8IsRejected)
{
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 thisCls = cb.classRef(thisIdx);
    const ca::u16 superCls = cb.classRef(superIdx);
    const ca::u16 descIdx = cb.utf8("()V");
    cb.setThis(thisCls);
    cb.setSuper(superCls);
    cb.addMethod(0x0009, thisCls, descIdx);   // 名字指向 Class 条目，不是 Utf8
    expectReadErr(cb.build(), ReadError::BadConstantPoolRef, "method name not Utf8");
}

TEST(MalformedTest, InterfaceNotClassIsRejected)
{
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 nameIdx = cb.utf8("Runnable");
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.addInterface(nameIdx);   // 接口表存 Utf8，不是 Class
    expectReadErr(cb.build(), ReadError::BadConstantPoolRef, "interface not Class");
}

TEST(MalformedTest, ThisClassOutOfRangeIsRejected)
{
    Bytes bytes = minimalClass();
    // 尾部 14 字节窗：access(2) this(2) super(2) ifcount(2)
    // fields/methods/attrs 计数各 2。this_class 在 offset 2，当前值 = 索引 3。
    const ca::usize thisOff = bytes.size() - 14 + 2;
    ASSERT_EQ(bytes[thisOff], 0x00);
    ASSERT_EQ(bytes[thisOff + 1], 0x02);
    bytes[thisOff]     = 0x7F;
    bytes[thisOff + 1] = 0xFF;
    expectReadErr(bytes, ReadError::BadConstantPoolRef, "this_class out of range");
}

// ---------------------------------------------------------------------------
// 族 B：结构可确定、语义不合规 → 显式 Recovered（降级可查询），写回仍可解析
// ---------------------------------------------------------------------------

TEST(MalformedTest, ClassTrailingMarksRecoveredAndRoundtrips)
{
    Bytes bytes = minimalClass();
    const Bytes tail = {0xDE, 0xAD, 0xBE, 0xEF};
    bytes.insert(bytes.end(), tail.begin(), tail.end());

    Result<ReadResult, ReadError> r = readClass(ByteView{bytes});
    ASSERT_FALSE(r.is_err());
    const ReadResult rr = std::move(r).unwrap();
    EXPECT_EQ(rr.state, ParseState::Recovered);   // 降级必须落在状态上，不能只有诊断
    EXPECT_FALSE(rr.diagnostics.empty());
    EXPECT_FALSE(rr.diagnostics.hasError());      // 降级是 warning 级，不是损坏
    EXPECT_EQ(rr.file->trailing, tail);

    // 写回侧：降级模型原样写出 → 字节一致，且再读仍 Recovered（仍可解析）。
    const Bytes out = jbc::test::writeOk(*rr.file);
    EXPECT_EQ(out, bytes);
    Result<ReadResult, ReadError> r2 = readClass(ByteView{out});
    ASSERT_FALSE(r2.is_err());
    const ReadResult rr2 = std::move(r2).unwrap();
    EXPECT_EQ(rr2.state, ParseState::Recovered);
    EXPECT_EQ(rr2.file->trailing, tail);
}

// ---------------------------------------------------------------------------
// 族 C：骨架可读、属性体损坏 → decode 阶段显式 Err；未触碰写回字节一致
// ---------------------------------------------------------------------------

TEST(MalformedTest, TruncatedCodeBodyFailsDecodeButWritesByteExact)
{
    // Code 体只有 maxStack 半个头：骨架读全（属性只记 span），解码必失败。
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 codeIdx = cb.utf8("Code");
    const ca::u16 mIdx = cb.utf8("m");
    const ca::u16 voidIdx = cb.utf8("()V");
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setVersion(52);
    cb.addMethod(0x0009, mIdx, voidIdx, {AttrSpec{codeIdx, {0x00, 0x01}}});
    const Bytes bytes = cb.build();

    auto cf = jbc::test::readOk(bytes);
    ASSERT_NE(cf, nullptr);
    EXPECT_EQ(cf->state(), ParseState::Validated);   // 骨架无损，状态不变
    auto d = cf->decodeAttr(cf->methods[0].attrs[0]);
    ASSERT_TRUE(d.is_err());
    EXPECT_EQ(d.unwrap_err(), ReadError::Truncated);

    // 写回侧：copy-through 与解码成败无关——未触碰写回仍字节一致。
    EXPECT_EQ(jbc::test::writeOk(*cf), bytes);
}

TEST(MalformedTest, TruncatedKnownAttrBodiesFailDecode)
{
    // 各已知属性各自的「半截体」：声明计数读完、载荷不足。
    // （Signature 已建模（u2 体）：半截体同样 decode Err；未触碰写回逐字节
    //   copy，手术拒绝面在 sig_attr_test。）
    struct Case
    {
        const char* name;
        Bytes body;
    };
    const Bytes lvtRow = {0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01};   // 行截半
    std::vector<Case> cases = {
        {"BootstrapMethods", {0x00, 0x01}},                                  // 1 条目无载荷
        {"LineNumberTable", {0x00, 0x02, 0x00, 0x00, 0x00, 0x01}},           // 2 行只有 1 行
        {"LocalVariableTable", lvtRow},                                      // 行中途截断
        {"StackMapTable", {0x00, 0x01}},                                     // 1 帧无载荷
        {"RuntimeVisibleAnnotations", {0x00, 0x01, 0x00, 0x10, 0x00, 0x01, 0x73}},  // 值截半
        {"Signature", {0x00}},                                               // u2 截半
    };
    for (const Case& c : cases) {
        ClassBuilder cb;
        const ca::u16 thisIdx = cb.utf8("p/T");
        const ca::u16 superIdx = cb.utf8("java/lang/Object");
        const ca::u16 codeIdx = cb.utf8("Code");
        const ca::u16 attrIdx = cb.utf8(c.name);
        const ca::u16 initIdx = cb.utf8("<init>");
        const ca::u16 voidIdx = cb.utf8("()V");
        cb.setThis(cb.classRef(thisIdx));
        cb.setSuper(cb.classRef(superIdx));
        cb.setVersion(52);
        const Bytes code = {0xb1};
        cb.addMethod(0x0001, initIdx, voidIdx,
                     {AttrSpec{codeIdx,
                               jbc::test::codeContent(0, 0, code, {}, {AttrSpec{attrIdx, c.body}})}});
        cb.addClassAttr(attrIdx, c.body);
        const Bytes bytes = cb.build();

        auto cf = jbc::test::readOk(bytes);
        ASSERT_NE(cf, nullptr) << c.name;
        for (AttrSlot& s : cf->attrs) {
            auto d = cf->decodeAttr(s);
            ASSERT_TRUE(d.is_err()) << c.name << ": expected decode Err";
            EXPECT_EQ(d.unwrap_err(), ReadError::Truncated) << c.name;
        }
        EXPECT_EQ(jbc::test::writeOk(*cf), bytes) << c.name << ": untouched write must roundtrip";
    }
}

TEST(MalformedTest, ReservedSmtFrameTypeFailsDecode)
{
    // SMT 帧类型 128..246 是 JVMS 保留区：解码即显式拒绝（不猜语义）。
    const Bytes smtBody = {0x00, 0x01, 0xC8};
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 codeIdx = cb.utf8("Code");
    const ca::u16 smtIdx = cb.utf8("StackMapTable");
    const ca::u16 mIdx = cb.utf8("m");
    const ca::u16 voidIdx = cb.utf8("()V");
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setVersion(52);
    const Bytes code = {0x04, 0x57, 0xb1};
    cb.addMethod(0x0009, mIdx, voidIdx,
                 {AttrSpec{codeIdx, jbc::test::codeContent(1, 0, code, {}, {AttrSpec{smtIdx, smtBody}})}});
    const Bytes bytes = cb.build();

    auto cf = jbc::test::readOk(bytes);
    ASSERT_NE(cf, nullptr);
    // Code 属性解码只编目嵌套属性（只读目录）；SMT 体解码要显式下钻。
    auto dc = cf->decodeAttr(cf->methods[0].attrs[0]);
    ASSERT_TRUE(dc.is_ok());
    auto& decoded = static_cast<CodeAttr&>(*dc.unwrap());
    ASSERT_EQ(decoded.attrs.size(), 1u);
    auto d = cf->decodeAttr(decoded.attrs[0]);
    ASSERT_TRUE(d.is_err());
    EXPECT_EQ(d.unwrap_err(), ReadError::UnsupportedFeature);
    EXPECT_EQ(jbc::test::writeOk(*cf), bytes);
}

// ---------------------------------------------------------------------------
// 族 D：异常表区间畸形 → 骨架/解码照读（字节如实），promote 阶段显式拒绝
// ---------------------------------------------------------------------------

TEST(MalformedTest, ExceptionHandlerMidInstructionRejectedAtPromote)
{
    // sipush@0(3B) pop@3 return@4；handler=1 落在 sipush 操作数中间。
    const Bytes code = {0x11, 0x00, 0x01, 0x57, 0xb1};
    const Bytes bytes = classWithCode(code, {ExcSpec{0, 3, 1, 0}});

    auto s = jbc::ir::makeSessionFromBytes(ByteView{bytes});
    ASSERT_TRUE(s.is_ok()) << "skeleton must read; byte facts are expressible";
    auto p = jbc::ir::promoteMethod(*std::move(s).unwrap(), 0);
    ASSERT_TRUE(p.is_err());
    EXPECT_EQ(p.unwrap_err(), jbc::ir::PromoteError::BadInstructions);
}

TEST(MalformedTest, ExceptionStartMidInstructionRejectedAtPromote)
{
    const Bytes code = {0x11, 0x00, 0x01, 0x57, 0xb1};
    const Bytes bytes = classWithCode(code, {ExcSpec{2, 5, 3, 0}});
    auto s = jbc::ir::makeSessionFromBytes(ByteView{bytes});
    ASSERT_TRUE(s.is_ok());
    auto p = jbc::ir::promoteMethod(*std::move(s).unwrap(), 0);
    ASSERT_TRUE(p.is_err());
    EXPECT_EQ(p.unwrap_err(), jbc::ir::PromoteError::BadInstructions);
}

TEST(MalformedTest, ExceptionEndMidInstructionRejectedAtPromote)
{
    // end=5 恰等于 code 长度是合法边界；end=2（sipush 操作数中间）必须拒绝。
    const Bytes code = {0x11, 0x00, 0x01, 0x57, 0xb1};
    const Bytes bytes = classWithCode(code, {ExcSpec{0, 2, 3, 0}});
    auto s = jbc::ir::makeSessionFromBytes(ByteView{bytes});
    ASSERT_TRUE(s.is_ok());
    auto p = jbc::ir::promoteMethod(*std::move(s).unwrap(), 0);
    ASSERT_TRUE(p.is_err());
    EXPECT_EQ(p.unwrap_err(), jbc::ir::PromoteError::BadInstructions);
}

TEST(MalformedTest, ExceptionEndAtCodeLengthIsLegal)
{
    // 对照组：end == code_length 是「方法末尾」合法形态，promote 必须成功。
    const Bytes code = {0x11, 0x00, 0x01, 0x57, 0xb1};
    const Bytes bytes = classWithCode(code, {ExcSpec{0, 5, 3, 0}});
    auto s = jbc::ir::makeSessionFromBytes(ByteView{bytes});
    ASSERT_TRUE(s.is_ok());
    auto p = jbc::ir::promoteMethod(*std::move(s).unwrap(), 0);
    EXPECT_TRUE(p.is_ok());
}
