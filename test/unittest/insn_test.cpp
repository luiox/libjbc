/// @file        insn_test.cpp
/// @brief       指令层测试：codec 往返、CodeDom 句柄/编辑语义、真实类全量往返。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "jbc/jbc.hpp"
#include "corpus_util.hpp"

using namespace jbc::core;
using jbc::ir::CodeDom;
using jbc::ir::EditError;
using jbc::ir::Insn;
using jbc::ir::InsnId;
using jbc::ir::LabelId;
using Bytes = std::vector<ca::u8>;

namespace {

/// 手工字节 → 十六进制字符串（断言失败时人能读）。
std::string hex(Bytes b)
{
    static const char* d = "0123456789abcdef";
    std::string out;
    for (ca::u8 x : b) {
        out.push_back(d[x >> 4]);
        out.push_back(d[x & 0xf]);
        out.push_back(' ');
    }
    return out;
}

/// codec 往返：decode → encode 必须字节一致。
void expectRoundtrip(const Bytes& code)
{
    auto r = decodeInsns(ByteView{code});
    ASSERT_TRUE(r.is_ok()) << "decode failed: " << hex(code);
    std::vector<RawInsn> insns = std::move(r).unwrap();
    auto w = encodeInsns(insns);
    ASSERT_TRUE(w.is_ok()) << "encode failed";
    EXPECT_EQ(w.unwrap(), code) << "roundtrip mismatch: " << hex(code);
}

Bytes mk(std::initializer_list<ca::u8> l)
{
    return Bytes(l);
}

void putS4(Bytes& bytes, ca::usize at, ca::i32 value)
{
    const ca::u32 u = static_cast<ca::u32>(value);
    bytes[at + 0] = static_cast<ca::u8>((u >> 24) & 0xff);
    bytes[at + 1] = static_cast<ca::u8>((u >> 16) & 0xff);
    bytes[at + 2] = static_cast<ca::u8>((u >> 8) & 0xff);
    bytes[at + 3] = static_cast<ca::u8>(u & 0xff);
}

Bytes longGotoWCode()
{
    constexpr ca::usize target = 40000;
    Bytes code(target, 0x00);
    code[0] = 0xc8;   // goto_w
    putS4(code, 1, static_cast<ca::i32>(target));
    code.push_back(0xb1);   // target instruction at pc=40000
    return code;
}

Bytes largeTableSwitchCode()
{
    constexpr ca::usize target = 40000;
    Bytes code(target, 0x00);
    code[0] = 0xaa;   // tableswitch; pc=0 needs three padding bytes
    putS4(code, 4, static_cast<ca::i32>(target));   // default
    putS4(code, 8, 0);                               // low
    putS4(code, 12, 0);                              // high
    putS4(code, 16, static_cast<ca::i32>(target));  // case 0
    code.push_back(0xb1);
    return code;
}

Bytes largeLookupSwitchCode()
{
    constexpr ca::usize target = 40000;
    Bytes code(target, 0x00);
    code[0] = 0xab;   // lookupswitch; pc=0 needs three padding bytes
    putS4(code, 4, static_cast<ca::i32>(target));   // default
    putS4(code, 8, 1);                               // npairs
    putS4(code, 12, 7);                              // match
    putS4(code, 16, static_cast<ca::i32>(target));  // offset
    code.push_back(0xb1);
    return code;
}

}   // namespace

// ---------------------------------------------------------------------------
// codec：形状覆盖
// ---------------------------------------------------------------------------

TEST(InsnCodecTest, SimpleShapesRoundtrip)
{
    // nop / iconst_1 / iadd / return（Shape::None）
    expectRoundtrip(mk({0x00, 0x04, 0x60, 0xb1}));
    // bipush -5 / sipush 1000
    expectRoundtrip(mk({0x10, 0xfb, 0x11, 0x03, 0xe8, 0xb1}));
    // ldc #4 / ldc_w #300
    expectRoundtrip(mk({0x12, 0x04, 0x13, 0x01, 0x2c, 0xb1}));
    // iload 7 / istore 200 / ret 5（Local1，200 是单字节上限内）
    expectRoundtrip(mk({0x15, 0x07, 0x36, 0xc8, 0xa9, 0x05}));
    // iinc 3, -1
    expectRoundtrip(mk({0x84, 0x03, 0xff}));
    // getstatic #10 / new #20 / newarray 6 (T_DOUBLE... 值任意可往返)
    expectRoundtrip(mk({0xb2, 0x00, 0x0a, 0xbb, 0x00, 0x14, 0xbc, 0x06, 0xb1}));
    // invokeinterface #5, 3 args / invokedynamic #7
    expectRoundtrip(mk({0xb9, 0x00, 0x05, 0x03, 0x00, 0xba, 0x00, 0x07, 0x00, 0x00, 0xb1}));
    // multianewarray #2, 3 维
    expectRoundtrip(mk({0xc5, 0x00, 0x02, 0x03, 0xb1}));
    // goto_w +8 / jsr_w（Branch4）
    expectRoundtrip(mk({0xc8, 0x00, 0x00, 0x00, 0x08, 0xb1, 0x01, 0x01, 0x01, 0x01,
                        0x01, 0x01, 0xb1}));
}

TEST(InsnCodecTest, Branch2RoundtripForwardAndBackward)
{
    // goto +3；往后跳的负偏移
    expectRoundtrip(mk({0xa7, 0x00, 0x03, 0xb1}));
    expectRoundtrip(mk({0x01, 0xa7, 0xff, 0xff, 0xb1}));   // goto -1 → 回到开头
}

TEST(InsnCodecTest, WideFormsRoundtrip)
{
    // wide iload 300：0xc4 0x15 0x012c
    expectRoundtrip(mk({0xc4, 0x15, 0x01, 0x2c, 0xb1}));
    // wide iload 5（小索引也必须保留 wide 前缀——混淆器的真实形态）
    expectRoundtrip(mk({0xc4, 0x15, 0x00, 0x05, 0xb1}));
    // wide iinc 700, -2
    expectRoundtrip(mk({0xc4, 0x84, 0x02, 0xbc, 0xff, 0xfe, 0xb1}));
    // wide astore 65535
    expectRoundtrip(mk({0xc4, 0x3a, 0xff, 0xff, 0xb1}));
}

TEST(InsnCodecTest, SwitchesRoundtripWithAlignment)
{
    // tableswitch 在 pc=0（opcode 后需 3 字节 padding 对齐）：
    // low=0 high=2，3 个 case；总长 29，return 在 pc=28
    expectRoundtrip(mk({
        0xaa,
        0x00, 0x00, 0x00,         // padding
        0x00, 0x00, 0x00, 0x1c,   // default +28 → return
        0x00, 0x00, 0x00, 0x00,   // low
        0x00, 0x00, 0x00, 0x02,   // high
        0x00, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x1c,
        0xb1,
    }));
    // lookupswitch 在 pc=1（操作数起点 pos=2 → 2 字节 padding）：
    // 总长 29，return 在 pc=28，default 相对 pc=1 为 +27
    expectRoundtrip(mk({
        0x01,                      // nop
        0xab,
        0x00, 0x00,               // padding
        0x00, 0x00, 0x00, 0x1b,   // default +27 → return
        0x00, 0x00, 0x00, 0x02,   // npairs=2
        0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x1b,
        0x00, 0x00, 0x00, 0x63, 0x00, 0x00, 0x00, 0x1b,
        0xb1,
    }));
}

TEST(InsnCodecTest, ReservedOpcodeRejected)
{
    auto r = decodeInsns(ByteView{mk({0xca})});
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), ReadError::UnsupportedFeature);
    r = decodeInsns(ByteView{mk({0x00, 0xfe})});
    ASSERT_TRUE(r.is_err());
}

TEST(InsnCodecTest, TruncatedOperandsRejected)
{
    // bipush 缺立即数
    EXPECT_TRUE(decodeInsns(ByteView{mk({0x10})}).is_err());
    // ldc2_w 只有 1 字节操作数
    EXPECT_TRUE(decodeInsns(ByteView{mk({0x14, 0x00})}).is_err());
    // wide iinc 尾巴缺 2 字节
    EXPECT_TRUE(decodeInsns(ByteView{mk({0xc4, 0x84, 0x00, 0x05, 0x00})}).is_err());
    // tableswitch 头都不完整
    EXPECT_TRUE(decodeInsns(ByteView{mk({0xaa})}).is_err());
}

TEST(InsnCodecTest, EmptyCodeIsValid)
{
    auto r = decodeInsns(ByteView{});
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.unwrap().size(), 0u);
}

/// 编码闸门：操作数落不进目标域宽必须 Err(Unencodable)，
/// 不得静默截断——截断产出引用错条目 / 立即数错值的 class。u1 域放不下的
/// 唯一合法出路是显式拓宽（wide 前缀 / LDC_W / goto_w）。
TEST(InsnCodecTest, EncodeRejectsOutOfRangeOperands)
{
    auto expectUnencodable = [](std::vector<RawInsn> insns) {
        auto r = encodeInsns(insns);
        ASSERT_TRUE(r.is_err());
        EXPECT_EQ(r.unwrap_err(), WriteError::Unencodable);
    };

    // CP2 族：ldc_w 索引超 u16。
    expectUnencodable({RawInsn{Op::LDC_W, false, 0, 0, 0, 0, 0x10000}});
    // invokeinterface：count 超 u8。
    expectUnencodable({RawInsn{Op::INVOKEINTERFACE, false, 0, 0x100, 0, 0, 5}});
    // multianewarray：维数超 u8。
    expectUnencodable({RawInsn{Op::MULTIANEWARRAY, false, 0, 0x100, 0, 0, 2}});
    // Local1：iload 索引超 u8（宽通道是 wide 前缀）。
    expectUnencodable({RawInsn{Op::ILOAD, false, 0, 0x100}});
    // Imm1：bipush 超 i8。
    expectUnencodable({RawInsn{Op::BIPUSH, false, 0, 200}});
    // Imm2：sipush 超 i16。
    expectUnencodable({RawInsn{Op::SIPUSH, false, 0, 70000}});
    // Iinc1：索引 300 超 u8。
    expectUnencodable({RawInsn{Op::IINC, false, 0, 300, 1}});
    // Iinc1：常量 200 超 i8。
    expectUnencodable({RawInsn{Op::IINC, false, 0, 3, 200}});
    // wide 路径：局部变量索引超 u16。
    {
        RawInsn in;
        in.op = Op::ALOAD;
        in.wide = true;
        in.a = 0x10000;
        expectUnencodable({in});
    }

    // 合法边界值仍可编码：域宽上界不是禁区。
    std::vector<RawInsn> ok = {
        RawInsn{Op::BIPUSH, false, 0, 127},
        RawInsn{Op::SIPUSH, false, 0, 32767},
        RawInsn{Op::ILOAD, false, 0, 255},
        RawInsn{Op::IINC, false, 0, 255, 127},
        RawInsn{Op::INVOKEINTERFACE, false, 0, 255, 0, 0, 255},
        RawInsn{Op::LDC_W, false, 0, 0, 0, 0, 0xFFFF},
    };
    {
        RawInsn in;
        in.op = Op::ALOAD;
        in.wide = true;
        in.a = 0xFFFF;
        ok.push_back(in);
    }
    auto w = encodeInsns(ok);
    ASSERT_TRUE(w.is_ok());
}

// ---------------------------------------------------------------------------
// CodeDom：句柄、编辑、use 语义
// ---------------------------------------------------------------------------

TEST(CodeDomTest, ImportRoundtripEquals)
{
    const Bytes code = mk({0x1a, 0x1b, 0x60, 0xa7, 0x00, 0x03, 0xac});
    // iload_0 iload_1 iadd goto +3 ireturn —— goto(pc=3) 跳到 pc=6 的 ireturn。
    auto d = CodeDom::importFrom(ByteView{code});
    ASSERT_TRUE(d.is_ok()) << jbc::ir::to_string(d.unwrap_err());
    CodeDom dom = std::move(d).unwrap();
    EXPECT_EQ(dom.size(), 5u);
    EXPECT_TRUE(dom.roundtripEquals(ByteView{code}));
}

TEST(CodeDomTest, ImportRejectsMidOperandTarget)
{
    // goto +1：pc=0 → 1，落在 goto 自己操作数中间
    auto r = CodeDom::importFrom(ByteView{mk({0xa7, 0x00, 0x01, 0xb1})});
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), EditError::BadTarget);
}

TEST(CodeDomTest, EraseBranchTargetIsStillUsed)
{
    // goto 跳到结尾的 return；return 被 goto 引用
    const Bytes code = mk({0xa7, 0x00, 0x04, 0x01, 0xb1});
    auto d           = CodeDom::importFrom(ByteView{code});
    CodeDom dom      = std::move(d).unwrap();
    const InsnId ret = dom.order()[2];

    auto e = dom.erase(ret);
    ASSERT_TRUE(e.is_err());
    EXPECT_EQ(e.unwrap_err(), EditError::StillUsed);

    // retarget 到自身后仍被引用（goto → 自己）：换个目标——插入一个新 return
    // 并把 uses 换过去，旧 return 才可删。
    Insn nr;
    nr.op = Op::RETURN;
    auto nrid = dom.insertAfter(ret, std::move(nr));
    ASSERT_TRUE(nrid.is_ok());
    const InsnId newRet = nrid.unwrap();

    auto r2 = dom.replaceAllUses(ret, newRet);
    ASSERT_TRUE(r2.is_ok());
    EXPECT_FALSE(dom.erase(ret).is_err());

    // 导出的字节语义：goto 现在跳到新 return（序列末条），编码偏移正确。
    auto bytes = dom.encode();
    ASSERT_TRUE(bytes.is_ok());
    auto d2 = CodeDom::importFrom(ByteView{bytes.unwrap()});
    ASSERT_TRUE(d2.is_ok());
    CodeDom back = std::move(d2).unwrap();
    ASSERT_EQ(back.size(), dom.size());
    const Insn* g = back.peek(back.order()[0]);
    ASSERT_NE(g, nullptr);
    EXPECT_EQ(g->op, Op::GOTO);
    const auto t = back.resolveLabel(g->target);
    ASSERT_TRUE(t.is_ok());
    ASSERT_FALSE(t.unwrap().isEnd());
    EXPECT_EQ(t.unwrap().insn, back.last());
    EXPECT_EQ(back.peek(back.last())->op, Op::RETURN);
}

TEST(CodeDomTest, ImportedBranchesToSameTargetGetDistinctLabels)
{
    // goto@0 +7 与 goto@4 +3 都跳到 pc7 的 return：两个操作数必须持有
    // 不同的 LabelId，但解析到同一条目标指令。
    const Bytes code = mk({0xa7, 0x00, 0x07, 0x00, 0xa7, 0x00, 0x03, 0xb1});
    auto d = CodeDom::importFrom(ByteView{code});
    ASSERT_TRUE(d.is_ok()) << jbc::ir::to_string(d.unwrap_err());
    CodeDom dom = std::move(d).unwrap();
    ASSERT_EQ(dom.size(), 4u);

    const Insn* g0 = dom.peek(dom.order()[0]);
    const Insn* g1 = dom.peek(dom.order()[2]);
    ASSERT_NE(g0, nullptr);
    ASSERT_NE(g1, nullptr);
    EXPECT_NE(g0->target, g1->target);
    const auto t0 = dom.resolveLabel(g0->target);
    const auto t1 = dom.resolveLabel(g1->target);
    ASSERT_TRUE(t0.is_ok());
    ASSERT_TRUE(t1.is_ok());
    ASSERT_FALSE(t0.unwrap().isEnd());
    EXPECT_EQ(t0.unwrap().insn, t1.unwrap().insn);
    EXPECT_EQ(t0.unwrap().insn, dom.order()[3]);
}

TEST(CodeDomTest, ReplaceAllUsesRedirectsControlFlowOnly)
{
    // goto@0 +4 跳到 return@4。普通 Boundary 标签与导入的控制流标签锚定
    // 同一条指令：replaceAllUses 后 branch 改指新指令，Boundary 标签留在
    // 原位，旧控制流标签被销毁。
    const Bytes code = mk({0xa7, 0x00, 0x04, 0x01, 0xb1});
    auto d = CodeDom::importFrom(ByteView{code});
    ASSERT_TRUE(d.is_ok());
    CodeDom dom = std::move(d).unwrap();

    const InsnId ret = dom.order()[2];
    const LabelId oldControl = dom.peek(dom.order()[0])->target;
    auto b = dom.createLabel(ret);
    ASSERT_TRUE(b.is_ok());
    const LabelId boundary = b.unwrap();

    Insn nr;
    nr.op = Op::RETURN;
    const InsnId fresh = dom.append(std::move(nr)).unwrap();

    ASSERT_TRUE(dom.replaceAllUses(ret, fresh).is_ok());

    const auto bt = dom.resolveLabel(dom.peek(dom.order()[0])->target);
    ASSERT_TRUE(bt.is_ok());
    ASSERT_FALSE(bt.unwrap().isEnd());
    EXPECT_EQ(bt.unwrap().insn, fresh);
    const auto ut = dom.resolveLabel(boundary);
    ASSERT_TRUE(ut.is_ok());
    ASSERT_FALSE(ut.unwrap().isEnd());
    EXPECT_EQ(ut.unwrap().insn, ret);
    EXPECT_FALSE(dom.validLabel(oldControl));
    EXPECT_TRUE(dom.validLabel(boundary));

    auto bytes = dom.encode();
    ASSERT_TRUE(bytes.is_ok());
    auto d2 = CodeDom::importFrom(ByteView{bytes.unwrap()});
    ASSERT_TRUE(d2.is_ok());
    CodeDom back = std::move(d2).unwrap();
    const auto t = back.resolveLabel(back.peek(back.order()[0])->target);
    ASSERT_TRUE(t.is_ok());
    EXPECT_EQ(t.unwrap().insn, back.last());
    EXPECT_EQ(back.peek(back.last())->op, Op::RETURN);
}

// ---------------------------------------------------------------------------
// CodeDom：eraseBatch 批量删除（全有或全无契约）
// ---------------------------------------------------------------------------

TEST(CodeDomTest, EraseBatchAllOrNothing)
{
    // goto(return); nop; nop; nop; return（偏移相对指令自身 pc：+6 → pc6 return）
    auto d = CodeDom::importFrom(ByteView{mk({0xa7, 0x00, 0x06, 0x00, 0x00, 0x00, 0xb1})});
    ASSERT_TRUE(d.is_ok());
    CodeDom dom = std::move(d).unwrap();
    ASSERT_EQ(dom.size(), 5u);

    // 无引用的两个 nop：批量删除成功。
    const std::vector<InsnId> two{dom.order()[1], dom.order()[2]};
    ASSERT_TRUE(dom.eraseBatch(two).is_ok());
    EXPECT_EQ(dom.size(), 3u);
    EXPECT_EQ(dom.peek(two[0]), nullptr);
    EXPECT_EQ(dom.peek(two[1]), nullptr);
    EXPECT_NE(dom.peek(dom.order()[1]), nullptr);        // 幸存者完好

    // 重建一个新 DOM 做反例：批内含被引用者 → StillUsed，什么都不删。
    auto d2 = CodeDom::importFrom(ByteView{mk({0xa7, 0x00, 0x06, 0x00, 0x00, 0x00, 0xb1})});
    CodeDom dom2 = std::move(d2).unwrap();
    const std::vector<InsnId> snapshot = dom2.order();   // 本 DOM 的句柄做原样对照
    const std::vector<InsnId> mixed{dom2.order()[1], dom2.order()[4]};   // nop + 被引用的 return
    auto r = dom2.eraseBatch(mixed);
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), EditError::StillUsed);
    EXPECT_EQ(dom2.size(), 5u);                          // 一个都没删
    EXPECT_EQ(dom2.order(), snapshot);                   // 顺序原样
    for (const InsnId& id : snapshot) {
        EXPECT_NE(dom2.peek(id), nullptr);               // 句柄全部仍存活
    }

    // 批内含无效句柄 → InvalidHandle，什么都不删。
    const std::vector<InsnId> bad{dom2.order()[1], InsnId{}};
    EXPECT_EQ(dom2.eraseBatch(bad).unwrap_err(), EditError::InvalidHandle);
    EXPECT_EQ(dom2.size(), 5u);

    // 重复句柄按并集处理：同槽只删一次。
    auto d3 = CodeDom::importFrom(ByteView{mk({0x00, 0x00, 0xb1})});
    CodeDom dom3 = std::move(d3).unwrap();
    const std::vector<InsnId> dup{dom3.order()[0], dom3.order()[0]};
    ASSERT_TRUE(dom3.eraseBatch(dup).is_ok());
    EXPECT_EQ(dom3.size(), 2u);
}

TEST(CodeDomTest, EraseBatchInternalCrossReferenceDiesTogether)
{
    // goto B; goto C; return —— A→B、B→C 全是批内互指：随批消亡，不算引用。
    // （逐条 erase 按顺序先删谁谁挡路；批量语义与成员消亡顺序无关。）
    auto d = CodeDom::importFrom(ByteView{mk({0xa7, 0x00, 0x03, 0xa7, 0x00, 0x03, 0xb1})});
    ASSERT_TRUE(d.is_ok());
    CodeDom dom = std::move(d).unwrap();
    ASSERT_EQ(dom.size(), 3u);
    const std::vector<InsnId> doomed{dom.order()[0], dom.order()[1]};
    ASSERT_TRUE(dom.eraseBatch(doomed).is_ok());
    EXPECT_EQ(dom.size(), 1u);
    EXPECT_NE(dom.peek(dom.order()[0]), nullptr);        // 存活者完好
    EXPECT_EQ(dom.peek(doomed[0]), nullptr);
    EXPECT_EQ(dom.peek(doomed[1]), nullptr);
    auto bytes = dom.encode();
    ASSERT_TRUE(bytes.is_ok());
    EXPECT_EQ(bytes.unwrap(), mk({0xb1}));
}

TEST(CodeDomTest, EraseBatchRetargetRewritesSurvivorFields)
{
    // goto B; nop; nop; return —— A（存活）→ B（批内）：无安排拒绝，有安排改锚。
    auto d = CodeDom::importFrom(ByteView{mk({0xa7, 0x00, 0x03, 0x00, 0x00, 0xb1})});
    ASSERT_TRUE(d.is_ok());
    CodeDom dom = std::move(d).unwrap();
    ASSERT_EQ(dom.size(), 4u);
    const InsnId a = dom.order()[0];
    const InsnId b = dom.order()[1];
    const InsnId c = dom.order()[2];
    const InsnId r = dom.order()[3];

    // 无 retarget 安排：A→B 是存活引用 → StillUsed，什么都不删。
    auto rej = dom.eraseBatch({b, c});
    ASSERT_TRUE(rej.is_err());
    EXPECT_EQ(rej.unwrap_err(), EditError::StillUsed);
    EXPECT_EQ(dom.size(), 4u);
    EXPECT_EQ(dom.resolveLabel(dom.peek(a)->target).unwrap().insn, b);

    // 安排缺失（只安排 B 不安排 C 无碍——C 无引用者；但完全无安排已验）。
    // 正常安排：B→R 改锚后成批删除。
    const std::vector<std::pair<InsnId, InsnId>> plan{{b, r}};
    ASSERT_TRUE(dom.eraseBatch({b, c}, plan).is_ok());
    EXPECT_EQ(dom.size(), 2u);
    const auto t = dom.resolveLabel(dom.peek(a)->target);
    ASSERT_TRUE(t.is_ok());
    EXPECT_EQ(t.unwrap().insn, r);                       // goto 改指 return
    EXPECT_EQ(dom.peek(a)->op, Op::GOTO);

    // 旧控制流标签按 replaceAllUses 同规销毁：编码重导后目标一致。
    auto bytes = dom.encode();
    ASSERT_TRUE(bytes.is_ok());
    auto d2 = CodeDom::importFrom(ByteView{bytes.unwrap()});
    ASSERT_TRUE(d2.is_ok());
    CodeDom back = std::move(d2).unwrap();
    const auto t2 = back.resolveLabel(back.peek(back.order()[0])->target);
    ASSERT_TRUE(t2.is_ok());
    EXPECT_EQ(t2.unwrap().insn, back.last());

    // 安排表本身非法：from 不在批内 / to 是批内成员 → InvalidHandle。
    auto d3 = CodeDom::importFrom(ByteView{mk({0xa7, 0x00, 0x03, 0x00, 0x00, 0xb1})});
    CodeDom dom3 = std::move(d3).unwrap();
    const std::vector<std::pair<InsnId, InsnId>> badFrom{{dom3.order()[3], dom3.order()[1]}};
    EXPECT_EQ(dom3.eraseBatch({dom3.order()[1]}, badFrom).unwrap_err(),
              EditError::InvalidHandle);
    const std::vector<std::pair<InsnId, InsnId>> badTo{{dom3.order()[1], dom3.order()[2]}};
    EXPECT_EQ(dom3.eraseBatch({dom3.order()[1], dom3.order()[2]}, badTo).unwrap_err(),
              EditError::InvalidHandle);
    EXPECT_EQ(dom3.size(), 4u);                          // 失败无变更
}

TEST(CodeDomTest, EraseBatchBoundaryLabelProtectsAnchor)
{
    auto d = CodeDom::importFrom(ByteView{mk({0x00, 0x00, 0xb1})});
    ASSERT_TRUE(d.is_ok());
    CodeDom dom = std::move(d).unwrap();
    const InsnId first = dom.order()[0];
    const LabelId boundary = dom.createLabel(first).unwrap();

    // Boundary 标签锚定批内成员：StillUsed，什么都不删。
    auto r = dom.eraseBatch({first, dom.order()[1]});
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), EditError::StillUsed);
    EXPECT_EQ(dom.size(), 3u);
    EXPECT_TRUE(dom.validLabel(boundary));

    // 标签移到方法末尾后解除保护，批量删除放行。
    ASSERT_TRUE(dom.bindLabel(boundary).is_ok());
    ASSERT_TRUE(dom.eraseBatch({first, dom.order()[1]}).is_ok());
    EXPECT_EQ(dom.size(), 1u);
    EXPECT_TRUE(dom.resolveLabel(boundary).unwrap().isEnd());
}

TEST(CodeDomTest, EraseBatchRewritesSwitchCaseTable)
{
    // tableswitch(default→D, case0→B, case1→C); nop(B); nop(C); return(D)
    // SW@0 占 24 字节（op+pad3+default/low/high+2 偏移）→ B@24 C@25 D@26；
    // switch 偏移相对 switch 自身 pc（0）。
    auto buildSwitch = []() {
        Bytes b{0xaa, 0x00, 0x00, 0x00};
        auto putI32 = [&b](ca::i32 v) {
            const ca::u32 u = static_cast<ca::u32>(v);
            b.push_back(static_cast<ca::u8>((u >> 24) & 0xff));
            b.push_back(static_cast<ca::u8>((u >> 16) & 0xff));
            b.push_back(static_cast<ca::u8>((u >> 8) & 0xff));
            b.push_back(static_cast<ca::u8>(u & 0xff));
        };
        putI32(26);   // default +26 → pc26 (return)
        putI32(0);    // low
        putI32(1);    // high
        putI32(24);   // case0 +24 → pc24 (nop B)
        putI32(25);   // case1 +25 → pc25 (nop C)
        b.push_back(0x00);
        b.push_back(0x00);
        b.push_back(0xb1);
        return b;
    };
    auto d = CodeDom::importFrom(ByteView{buildSwitch()});
    ASSERT_TRUE(d.is_ok());
    CodeDom dom = std::move(d).unwrap();
    ASSERT_EQ(dom.size(), 4u);
    const InsnId sw = dom.order()[0];
    const InsnId bNop = dom.order()[1];
    const InsnId cNop = dom.order()[2];
    const InsnId ret = dom.order()[3];

    // 只安排 case0 的 B：case1 的 C 仍被存活 switch 引用 → StillUsed。
    const std::vector<std::pair<InsnId, InsnId>> partial{{bNop, ret}};
    auto rej = dom.eraseBatch({bNop, cNop}, partial);
    ASSERT_TRUE(rej.is_err());
    EXPECT_EQ(rej.unwrap_err(), EditError::StillUsed);
    EXPECT_EQ(dom.size(), 4u);

    // 两个 case 目标都安排 → case 表整表改锚到 return。
    const std::vector<std::pair<InsnId, InsnId>> plan{{bNop, ret}, {cNop, ret}};
    ASSERT_TRUE(dom.eraseBatch({bNop, cNop}, plan).is_ok());
    EXPECT_EQ(dom.size(), 2u);
    const Insn* after = dom.peek(sw);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->targets.size(), 2u);
    for (const LabelId& t : after->targets) {
        EXPECT_EQ(dom.resolveLabel(t).unwrap().insn, ret);
    }
    EXPECT_EQ(dom.resolveLabel(after->target).unwrap().insn, ret);
}

TEST(CodeDomTest, CrossDomHandleRejected)
{
    const Bytes code = mk({0xb1});
    auto d1    = CodeDom::importFrom(ByteView{code});
    auto d2    = CodeDom::importFrom(ByteView{code});
    CodeDom a  = std::move(d1).unwrap();
    CodeDom b  = std::move(d2).unwrap();
    ASSERT_NE(a.owner(), b.owner());

    // b 的句柄对 a 是无效句柄：erase/peek/get 全部拒绝
    const InsnId foreign = b.order()[0];
    EXPECT_EQ(a.peek(foreign), nullptr);
    EXPECT_TRUE(a.erase(foreign).is_err());
    EXPECT_TRUE(a.get(foreign).is_err());

    // append 携带跨域 label target 同样拒绝
    Insn br;
    br.op     = Op::GOTO;
    br.target = b.createLabel(foreign).unwrap();
    EXPECT_TRUE(a.append(std::move(br)).is_err());
}

TEST(CodeDomTest, LabelsAreIntegerBoundariesWithOwnerValidation)
{
    const Bytes code = mk({0x00, 0xb1});
    auto da = CodeDom::importFrom(ByteView{code});
    auto db = CodeDom::importFrom(ByteView{code});
    ASSERT_TRUE(da.is_ok());
    ASSERT_TRUE(db.is_ok());
    CodeDom a = std::move(da).unwrap();
    CodeDom b = std::move(db).unwrap();

    const InsnId first = a.order()[0];
    auto labelResult = a.createLabel(first);
    ASSERT_TRUE(labelResult.is_ok());
    const LabelId label = labelResult.unwrap();
    ASSERT_TRUE(a.validLabel(label));
    EXPECT_EQ(a.resolveLabel(label).unwrap().insn, first);

    // A label from another DOM is rejected even when both DOMs have the same
    // local slot layout.
    EXPECT_FALSE(b.validLabel(label));
    EXPECT_TRUE(b.resolveLabel(label).is_err());
    EXPECT_TRUE(b.bindLabel(label, b.order()[0]).is_err());

    // A live label protects its anchor from deletion.  Moving it to the end
    // makes deletion explicit and leaves the old label identity valid.
    EXPECT_EQ(a.erase(first).unwrap_err(), EditError::StillUsed);
    ASSERT_TRUE(a.bindLabel(label).is_ok());
    ASSERT_TRUE(a.erase(first).is_ok());
    EXPECT_TRUE(a.resolveLabel(label).unwrap().isEnd());

    ASSERT_TRUE(a.destroyLabel(label).is_ok());
    EXPECT_FALSE(a.validLabel(label));
    EXPECT_TRUE(a.resolveLabel(label).is_err());
}

TEST(CodeDomTest, LabelsSurviveDomMoveAndRejectDeletedAnchors)
{
    const Bytes code = mk({0xb1});
    auto d = CodeDom::importFrom(ByteView{code});
    ASSERT_TRUE(d.is_ok());
    CodeDom source = std::move(d).unwrap();
    const InsnId ret = source.order()[0];
    const LabelId label = source.createLabel(ret).unwrap();
    const ca::u32 owner = source.owner();

    CodeDom moved = std::move(source);
    EXPECT_EQ(moved.owner(), owner);
    EXPECT_TRUE(source.order().empty());
    EXPECT_EQ(moved.resolveLabel(label).unwrap().insn, moved.order()[0]);
    EXPECT_EQ(moved.erase(moved.order()[0]).unwrap_err(), EditError::StillUsed);

    // The moved-from object remains usable, but its new handles belong to a
    // fresh owner and cannot be mistaken for handles now held by `moved`.
    auto fresh = source.append(Insn{});
    ASSERT_TRUE(fresh.is_ok());
    EXPECT_NE(source.owner(), moved.owner());
    EXPECT_EQ(moved.peek(fresh.unwrap()), nullptr);
}

TEST(CodeDomTest, ReorderingPreservesHandlesAndLabelBinding)
{
    auto d = CodeDom::importFrom(ByteView{mk({0x00, 0x00, 0xb1})});
    ASSERT_TRUE(d.is_ok());
    CodeDom dom = std::move(d).unwrap();
    const InsnId first = dom.order()[0];
    const InsnId second = dom.order()[1];
    const InsnId ret = dom.order()[2];
    const LabelId label = dom.createLabel(second).unwrap();

    ASSERT_TRUE(dom.moveAfter(first, ret).is_ok());
    ASSERT_EQ(dom.order()[0], second);
    ASSERT_EQ(dom.order()[1], ret);
    ASSERT_EQ(dom.order()[2], first);
    EXPECT_EQ(dom.resolveLabel(label).unwrap().insn, second);

    auto inserted = dom.insertAtLabel(label, Insn{});
    ASSERT_TRUE(inserted.is_ok());
    const InsnId atLabel = inserted.unwrap();
    ASSERT_EQ(dom.order()[0], atLabel);
    ASSERT_EQ(dom.order()[1], second);
    EXPECT_EQ(dom.resolveLabel(label).unwrap().insn, atLabel);

    ASSERT_TRUE(dom.moveBefore(atLabel, ret).is_ok());
    EXPECT_EQ(dom.resolveLabel(label).unwrap().insn, atLabel);
    EXPECT_EQ(dom.peek(atLabel)->op, Op::NOP);
}

TEST(CodeDomTest, InstructionViewSurvivesAppendAndInsert)
{
    auto d = CodeDom::importFrom(ByteView{mk({0x00, 0xb1})});
    ASSERT_TRUE(d.is_ok());
    CodeDom dom = std::move(d).unwrap();
    const InsnId first = dom.first();
    const Insn* view = dom.peek(first);
    ASSERT_NE(view, nullptr);
    EXPECT_EQ(view->op, Op::NOP);

    for (int i = 0; i < 64; ++i) {
        ASSERT_TRUE(dom.append(Insn{}).is_ok());
    }
    ASSERT_TRUE(dom.insertBefore(dom.last(), Insn{}).is_ok());
    EXPECT_EQ(dom.peek(first), view);
    EXPECT_EQ(dom.peek(first)->op, Op::NOP);
}

TEST(CodeDomTest, InsertShiftsBranchOffsets)
{
    // iconst_1; goto +3; (跳过两个 nop) return
    const Bytes code = mk({0x04, 0xa7, 0x00, 0x03, 0x00, 0x00, 0xb1});
    auto d      = CodeDom::importFrom(ByteView{code});
    CodeDom dom = std::move(d).unwrap();
    ASSERT_TRUE(dom.roundtripEquals(ByteView{code}));

    // 在第一个 nop 前插入 3 条 nop：goto 的目标仍是同一个句柄，字节偏移
    // 由导出重算——语义不变（跳到的还是原来那条指令）。
    const InsnId firstNop = dom.order()[2];
    for (int i = 0; i < 3; ++i) {
        Insn n;
        n.op = Op::NOP;
        auto r = dom.insertBefore(firstNop, std::move(n));
        ASSERT_TRUE(r.is_ok());
    }
    auto bytes = dom.encode();
    ASSERT_TRUE(bytes.is_ok());
    // 重新导入：语义目标一致（都是各自序列的倒数第二条之前的 nop 链尾）
    auto d2 = CodeDom::importFrom(ByteView{bytes.unwrap()});
    ASSERT_TRUE(d2.is_ok());
    CodeDom back = std::move(d2).unwrap();
    ASSERT_EQ(back.size(), dom.size());
}

TEST(CodeDomTest, DeletedHandleInvalidated)
{
    const Bytes code = mk({0x01, 0x57, 0xb1});   // aconst_null; pop; return
    auto d      = CodeDom::importFrom(ByteView{code});
    CodeDom dom = std::move(d).unwrap();
    const InsnId pop = dom.order()[1];

    ASSERT_TRUE(dom.erase(pop).is_ok());
    EXPECT_EQ(dom.peek(pop), nullptr);                       // gen 失配
    EXPECT_TRUE(dom.get(pop).is_err());
    EXPECT_EQ(dom.size(), 2u);
}

TEST(CodeDomTest, Branch4AndSwitchOffsetsUseSigned32Range)
{
    {
        auto r = CodeDom::importFrom(ByteView{longGotoWCode()});
        ASSERT_TRUE(r.is_ok()) << jbc::ir::to_string(r.unwrap_err());
        auto raw = std::move(r).unwrap().exportRaw();
        ASSERT_TRUE(raw.is_ok());
        ASSERT_FALSE(raw.unwrap().empty());
        EXPECT_EQ(raw.unwrap().back().pc, 40000u);
        EXPECT_EQ(raw.unwrap()[0].t, 40000);
    }
    {
        auto r = CodeDom::importFrom(ByteView{largeTableSwitchCode()});
        ASSERT_TRUE(r.is_ok()) << jbc::ir::to_string(r.unwrap_err());
        auto raw = std::move(r).unwrap().exportRaw();
        ASSERT_TRUE(raw.is_ok());
        ASSERT_FALSE(raw.unwrap().empty());
        EXPECT_EQ(raw.unwrap().back().pc, 40000u);
        EXPECT_EQ(raw.unwrap()[0].t, 40000);
        ASSERT_EQ(raw.unwrap()[0].offs.size(), 1u);
        EXPECT_EQ(raw.unwrap()[0].offs[0], 40000);
    }
    {
        auto r = CodeDom::importFrom(ByteView{largeLookupSwitchCode()});
        ASSERT_TRUE(r.is_ok()) << jbc::ir::to_string(r.unwrap_err());
        auto raw = std::move(r).unwrap().exportRaw();
        ASSERT_TRUE(raw.is_ok());
        ASSERT_FALSE(raw.unwrap().empty());
        EXPECT_EQ(raw.unwrap().back().pc, 40000u);
        EXPECT_EQ(raw.unwrap()[0].t, 40000);
        ASSERT_EQ(raw.unwrap()[0].cases.size(), 1u);
        EXPECT_EQ(raw.unwrap()[0].cases[0].off, 40000);
    }
}

TEST(CodeDomTest, LookupSwitchKeyTargetMismatchIsRejected)
{
    auto r = CodeDom::importFrom(ByteView{largeLookupSwitchCode()});
    ASSERT_TRUE(r.is_ok());
    CodeDom dom = std::move(r).unwrap();
    auto insn = dom.get(dom.order()[0]);
    ASSERT_TRUE(insn.is_ok());
    insn.unwrap()->keys.clear();

    auto raw = dom.exportRaw();
    ASSERT_TRUE(raw.is_err());
    EXPECT_EQ(raw.unwrap_err(), EditError::BadOperand);
}

// ---------------------------------------------------------------------------
// 真实 class：全量指令级往返（v0.2 核心验收）
// ---------------------------------------------------------------------------

namespace {

std::vector<std::string> collectClassFiles(const std::string& dir)
{
    return jbc::test::corpus::collectClassFiles(dir);
}

Bytes slurpFile(const std::string& p)
{
    std::ifstream in(p.c_str(), std::ios::binary);
    if (!in) {
        return {};
    }
    return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}   // namespace

TEST(InsnRealClassTest, EveryMethodRoundtripsThroughCodeDom)
{
    const jbc::test::corpus::CorpusSource src = jbc::test::corpus::resolveCorpus();
    if (src.stage == jbc::test::corpus::Stage::None) {
        GTEST_SKIP() << jbc::test::corpus::skipReason(src);
    }
    const std::vector<std::string> files = collectClassFiles(src.dir);
    if (files.empty()) {
        GTEST_SKIP() << jbc::test::corpus::skipReason(src);
    }

    ca::usize methods  = 0;
    ca::usize insnsAll = 0;
    for (const std::string& path : files) {
        const Bytes bytes = slurpFile(path);
        ASSERT_FALSE(bytes.empty()) << path;

        auto r = readClass(ByteView{bytes});
        ASSERT_TRUE(r.is_ok()) << path;
        auto cf = std::move(r).unwrap().file;

        for (MemberInfo& m : cf->methods) {
            for (AttrSlot& s : m.attrs) {
                auto d = cf->decodeAttr(s);
                ASSERT_TRUE(d.is_ok()) << path << " attr decode";
                AttrBody* body = d.unwrap();
                if (body->kind() != AttrKind::Code) {
                    continue;
                }
                CodeAttr& ca = static_cast<CodeAttr&>(*body);
                ++methods;

                auto dom = CodeDom::importFrom(ByteView{ca.code});
                ASSERT_TRUE(dom.is_ok())
                    << path << " insn import: "
                    << jbc::ir::to_string(dom.unwrap_err());
                CodeDom cd = std::move(dom).unwrap();
                insnsAll += cd.size();
                ASSERT_TRUE(cd.roundtripEquals(ByteView{ca.code}))
                    << path << " insn roundtrip mismatch";
            }
        }
    }
    // 防御：真实语料不可能一个方法都没有（目录非空时）。
    EXPECT_GT(methods, 0u);
    EXPECT_GT(insnsAll, 0u);
}
