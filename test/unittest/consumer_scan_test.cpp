/// @file        consumer_scan_test.cpp
/// @brief       consumer 批 3 · E3/E1 扫描面（MemoryClassSource /
///              SummaryScanner / ReferenceScanner）的合成类直验。
///
/// 合成类经会话从零构造（类构建挂载面）→ writeClass 出字节，扫描产物
/// 断言骨架与引用事实族。站位断言同时文档化「原始 code 域坐标系」：
/// ordinal = decode 指令序，pc = Code 体字节偏移（GETFIELD 三字节、LDC
/// 两字节等宽度事实在这里复核）。
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "jbc/consumer/reference.hpp"
#include "jbc/consumer/summary.hpp"
#include "jbc/consumer/source.hpp"
#include "jbc/core/insn_codec.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

using namespace jbc::core;

namespace ir       = jbc::ir;
namespace consumer = jbc::consumer;

using consumer::ClassSummary;
using consumer::MemoryClassSource;
using consumer::ReferenceScanner;
using consumer::ReferenceSummary;
using consumer::SummaryScanner;

namespace {

/// @brief 会话 → writeClass 字节（失败即断言崩）。
std::vector<ca::u8> writeOf(ir::ClassSession& s)
{
    const auto w = writeClass(s.backing());
    if (w.is_err()) {
        ADD_FAILURE() << "writeClass 失败";
        return {};
    }
    return std::move(w).unwrap();
}

/// @brief demo/B：字段 v:J + 抽象方法 nil()V（无 Code）。
std::vector<ca::u8> buildB()
{
    ir::NewClassSeed seed;
    seed.thisName  = "demo/B";
    seed.superName = "java/lang/Object";
    auto s = ir::makeSession(seed);
    if (s.is_err()) {
        ADD_FAILURE() << "makeSession(B) 失败";
        return {};
    }
    const std::shared_ptr<ir::ClassSession> ses = s.unwrap();   // 具名持活
    ir::MethodSeed ms;
    ms.name   = "nil";
    ms.desc   = "()V";
    ms.access = 0x0401;   // abstract：无 Code 体
    if (ses->addMethod(ms).is_err()) {
        ADD_FAILURE() << "addMethod 失败";
        return {};
    }
    ir::FieldSeed fs;
    fs.name = "v";
    fs.desc = "J";
    if (ses->addField(fs).is_err()) {
        ADD_FAILURE() << "addField 失败";
        return {};
    }
    return writeOf(*s.unwrap());
}

/// @brief demo/A：method call()V（Code：getfield/invokevirtual/checkcast/
///        ldc string 引用 demo/B）+ method witness(Ldemo/B;)V。
std::vector<ca::u8> buildA()
{
    ir::NewClassSeed seed;
    seed.thisName  = "demo/A";
    seed.superName = "java/lang/Object";
    auto s = ir::makeSession(seed);
    if (s.is_err()) {
        ADD_FAILURE() << "makeSession(A) 失败";
        return {};
    }
    const std::shared_ptr<ir::ClassSession> sesHolder = s.unwrap();
    ir::ClassSession& ses = *sesHolder;

    ir::MethodSeed ms;
    ms.name      = "call";
    ms.desc      = "()V";
    ms.access    = 0x0001;
    ms.withCode  = true;
    ms.maxStack  = 3;
    ms.maxLocals = 1;
    auto call = ses.addMethod(ms);
    if (call.is_err()) {
        ADD_FAILURE() << "addMethod(call) 失败";
        return {};
    }

    ir::MethodSeed ms2;
    ms2.name   = "witness";
    ms2.desc   = "(Ldemo/B;)V";
    ms2.access = 0x0001;
    if (ses.addMethod(ms2).is_err()) {
        ADD_FAILURE() << "addMethod(witness) 失败";
        return {};
    }

    auto p = ir::promoteMethod(ses, call.unwrap());
    if (p.is_err()) {
        ADD_FAILURE() << "promoteMethod 失败";
        return {};
    }
    ir::CodeEditor ed{*p.unwrap()};

    // [0]ALOAD_0 [1]GETFIELD [2]INVOKEVIRTUAL [3]CHECKCAST [4]LDC(str)
    // [5]POP [6]RETURN —— 栈形态不自洽不碍事：写回只管编码。
    const auto idGf = ed.append(ir::CodeEditor::simple(Op::ALOAD_0));
    const auto idG  = ed.append(ir::CodeEditor::simple(Op::GETFIELD));
    const auto idIv = ed.append(ir::CodeEditor::simple(Op::INVOKEVIRTUAL));
    const auto idCc = ed.append(ir::CodeEditor::simple(Op::CHECKCAST));
    const auto idLc = ed.append(ir::CodeEditor::simple(Op::LDC));
    if (idGf.is_err() || idG.is_err() || idIv.is_err() || idCc.is_err() || idLc.is_err()
        || ed.append(ir::CodeEditor::simple(Op::POP)).is_err()
        || ed.append(ir::CodeEditor::simple(Op::RETURN)).is_err()) {
        ADD_FAILURE() << "append 失败";
        return {};
    }
    auto get = [&](const ir::InsnId id) -> ir::Insn* {
        auto g = ed.unit().dom.get(id);
        if (g.is_err()) {
            return nullptr;
        }
        return g.unwrap();
    };

    if (ir::Insn* in = get(idG.unwrap()); in == nullptr
        || !ses.setMember(*in, ir::ClassSession::MemberSem::Kind::Field, "demo/B", "v", "J")) {
        ADD_FAILURE() << "setMember(field) 失败";
        return {};
    }
    if (ir::Insn* in = get(idIv.unwrap()); in == nullptr
        || !ses.setMember(*in, ir::ClassSession::MemberSem::Kind::Method, "demo/B", "m", "()V")) {
        ADD_FAILURE() << "setMember(method) 失败";
        return {};
    }
    if (ir::Insn* in = get(idCc.unwrap()); in == nullptr || !ses.setClass(*in, "demo/B")) {
        ADD_FAILURE() << "setClass 失败";
        return {};
    }
    if (ir::Insn* in = get(idLc.unwrap()); in == nullptr || !ses.setString(*in, "LB;")) {
        ADD_FAILURE() << "setString 失败";
        return {};
    }
    if (ir::lowerMethod(ses, call.unwrap()).is_err()) {
        ADD_FAILURE() << "lowerMethod 失败";
        return {};
    }
    return writeOf(ses);
}

}   // namespace

// ---------------------------------------------------------------------------
// MemoryClassSource：E3 源契约
// ---------------------------------------------------------------------------

TEST(ConsumerSourceTest, MemorySourceBasics)
{
    const std::vector<ca::u8> aBytes = buildA();
    const std::vector<ca::u8> bBytes = buildB();
    ASSERT_FALSE(aBytes.empty());
    ASSERT_FALSE(bBytes.empty());

    MemoryClassSource src;
    src.add("demo/B", bBytes);
    src.add("demo/A", aBytes);
    EXPECT_EQ(src.classCount(), static_cast<ca::usize>(2));

    std::vector<std::string_view> names;
    src.listClasses(names);
    ASSERT_EQ(names.size(), static_cast<ca::usize>(2));
    EXPECT_EQ(std::string(names[0]), "demo/A");   // 升序（map 序）
    EXPECT_EQ(std::string(names[1]), "demo/B");

    // 取字节：内容等值 + 同键重新取回为同一段存续字节。
    const auto ra = src.bytes("demo/A");
    ASSERT_TRUE(ra.is_ok());
    EXPECT_EQ(ra.unwrap().size(), aBytes.size());
    EXPECT_EQ(0, std::memcmp(ra.unwrap().data(), aBytes.data(), aBytes.size()));

    // upsert：同键覆盖，键序不重排。
    const std::vector<ca::u8> a2(aBytes.size(), 0x5A);
    src.add("demo/A", a2);
    EXPECT_EQ(src.classCount(), static_cast<ca::usize>(2));
    const auto ra2 = src.bytes("demo/A");
    ASSERT_TRUE(ra2.is_ok());
    EXPECT_EQ(0, std::memcmp(ra2.unwrap().data(), a2.data(), a2.size()));

    // fail-closed：缺失显式 Err(NotFound)，不默空。
    const auto miss = src.bytes("demo/Nope");
    ASSERT_TRUE(miss.is_err());
    EXPECT_EQ(miss.unwrap_err(), consumer::ConsumerError::NotFound);
}

// ---------------------------------------------------------------------------
// SummaryScanner：E3 骨架
// ---------------------------------------------------------------------------

TEST(ConsumerScanTest, SummarySkeletonOfSynthetic)
{
    const std::vector<ca::u8> aBytes = buildA();
    const std::vector<ca::u8> bBytes = buildB();
    ASSERT_FALSE(aBytes.empty());
    ASSERT_FALSE(bBytes.empty());

    const SummaryScanner sc;

    const auto sa = sc.scan(ByteView{aBytes.data(), aBytes.size()});
    ASSERT_TRUE(sa.is_ok());
    const ClassSummary& a = sa.unwrap();
    EXPECT_EQ(a.internalName, "demo/A");
    EXPECT_EQ(a.superClass, "java/lang/Object");
    EXPECT_EQ(a.version, (52u << 16));
    EXPECT_TRUE(a.interfaces.empty());
    ASSERT_EQ(a.methods.size(), static_cast<ca::usize>(2));
    EXPECT_EQ(a.methods[0].name, "call");
    EXPECT_EQ(a.methods[0].desc, "()V");
    EXPECT_TRUE(a.methods[0].hasCode);            // 属性名 Code 命中
    EXPECT_EQ(a.methods[1].name, "witness");
    EXPECT_EQ(a.methods[1].desc, "(Ldemo/B;)V");
    EXPECT_FALSE(a.methods[1].hasCode);
    EXPECT_TRUE(a.fields.empty());

    const auto sb = sc.scan(ByteView{bBytes.data(), bBytes.size()});
    ASSERT_TRUE(sb.is_ok());
    const ClassSummary& b = sb.unwrap();
    EXPECT_EQ(b.internalName, "demo/B");
    ASSERT_EQ(b.methods.size(), static_cast<ca::usize>(1));
    EXPECT_EQ(b.methods[0].name, "nil");
    EXPECT_FALSE(b.methods[0].hasCode);           // 抽象方法无 Code
    ASSERT_EQ(b.fields.size(), static_cast<ca::usize>(1));
    EXPECT_EQ(b.fields[0].name, "v");
    EXPECT_EQ(b.fields[0].desc, "J");

    // 坏字节：Err 只出自 BadClassFile 档。
    std::vector<ca::u8> junk(24, 0xAB);
    const auto bad = sc.scan(ByteView{junk.data(), junk.size()});
    ASSERT_TRUE(bad.is_err());
    EXPECT_EQ(bad.unwrap_err(), consumer::ConsumerError::BadClassFile);
}

TEST(ConsumerScanTest, ScanSourceFailClosed)
{
    const std::vector<ca::u8> aBytes = buildA();
    const std::vector<ca::u8> bBytes = buildB();
    ASSERT_FALSE(aBytes.empty());
    ASSERT_FALSE(bBytes.empty());

    SummaryScanner sc;

    // 正常源：两份骨架，listClasses 序。
    {
        MemoryClassSource src;
        src.add("demo/B", bBytes);
        src.add("demo/A", aBytes);
        const auto all = sc.scanSource(src);
        ASSERT_TRUE(all.is_ok());
        ASSERT_EQ(all.unwrap().size(), static_cast<ca::usize>(2));
        EXPECT_EQ(all.unwrap()[0].internalName, "demo/A");
        EXPECT_EQ(all.unwrap()[1].internalName, "demo/B");
    }

    // fail-closed：任一类坏字节即首错即 Err。
    {
        MemoryClassSource src;
        src.add("demo/A", aBytes);
        src.add("demo/J", std::vector<ca::u8>(16, 0x01));
        const auto all = sc.scanSource(src);
        ASSERT_TRUE(all.is_err());
        EXPECT_EQ(all.unwrap_err(), consumer::ConsumerError::BadClassFile);
    }
}

// ---------------------------------------------------------------------------
// ReferenceScanner：E1 引用事实族（原始 code 域站位）
// ---------------------------------------------------------------------------

TEST(ConsumerRefTest, ReferenceFactsOfSynthetic)
{
    const std::vector<ca::u8> aBytes = buildA();
    const std::vector<ca::u8> bBytes = buildB();
    ASSERT_FALSE(aBytes.empty());
    ASSERT_FALSE(bBytes.empty());

    const ReferenceScanner rs;

    const auto ra = rs.scan(ByteView{aBytes.data(), aBytes.size()});
    ASSERT_TRUE(ra.is_ok());
    const ReferenceSummary& a = ra.unwrap();
    EXPECT_EQ(a.thisClass, "demo/A");
    EXPECT_EQ(a.superClass, "java/lang/Object");
    EXPECT_TRUE(a.interfaces.empty());
    ASSERT_EQ(a.methods.size(), static_cast<ca::usize>(2));

    const consumer::MethodSection& call = a.methods[0];
    EXPECT_EQ(call.name, "call");
    EXPECT_EQ(call.desc, "()V");

    // 站位复核：指令宽度 (ALOAD_0=1, GETFIELD=3, INVOKEVIRTUAL=3,
    // CHECKCAST=3, LDC=2, POP=1, RETURN=1) + 零基 ordinal。
    ASSERT_EQ(call.fieldAccesses.size(), static_cast<ca::usize>(1));
    EXPECT_EQ(call.fieldAccesses[0].owner, "demo/B");
    EXPECT_EQ(call.fieldAccesses[0].name, "v");
    EXPECT_EQ(call.fieldAccesses[0].desc, "J");
    EXPECT_EQ(call.fieldAccesses[0].site.opcode, Op::GETFIELD);
    EXPECT_EQ(call.fieldAccesses[0].site.ordinal, 1u);
    EXPECT_EQ(call.fieldAccesses[0].site.pc, 1u);

    ASSERT_EQ(call.invokes.size(), static_cast<ca::usize>(1));
    EXPECT_EQ(call.invokes[0].owner, "demo/B");
    EXPECT_EQ(call.invokes[0].name, "m");
    EXPECT_EQ(call.invokes[0].desc, "()V");
    EXPECT_EQ(call.invokes[0].site.opcode, Op::INVOKEVIRTUAL);
    EXPECT_EQ(call.invokes[0].site.ordinal, 2u);
    EXPECT_EQ(call.invokes[0].site.pc, 4u);

    ASSERT_EQ(call.typeOps.size(), static_cast<ca::usize>(1));
    EXPECT_EQ(call.typeOps[0].internalName, "demo/B");
    EXPECT_EQ(call.typeOps[0].site.opcode, Op::CHECKCAST);
    EXPECT_EQ(call.typeOps[0].site.ordinal, 3u);
    EXPECT_EQ(call.typeOps[0].site.pc, 7u);

    // 字符串常量按字面量收录（«LB;» 是文本事实，不是引用——rename 的角色
    // 纪律与其对应）。
    ASSERT_EQ(call.strings.size(), static_cast<ca::usize>(1));
    EXPECT_EQ(call.strings[0].text, "LB;");
    EXPECT_EQ(call.strings[0].site.opcode, Op::LDC);
    EXPECT_EQ(call.strings[0].site.ordinal, 4u);
    EXPECT_EQ(call.strings[0].site.pc, 10u);

    EXPECT_TRUE(call.indies.empty());
    EXPECT_TRUE(call.exceptionTypes.empty());

    const consumer::MethodSection& witness = a.methods[1];
    EXPECT_EQ(witness.name, "witness");
    EXPECT_EQ(witness.desc, "(Ldemo/B;)V");
    EXPECT_TRUE(witness.invokes.empty());
    EXPECT_TRUE(witness.fieldAccesses.empty());
    EXPECT_TRUE(witness.typeOps.empty());
    EXPECT_TRUE(witness.strings.empty());

    // 无 Code 方法：全族空段（不可与失败混淆的预期形态）。
    const auto rb = rs.scan(ByteView{bBytes.data(), bBytes.size()});
    ASSERT_TRUE(rb.is_ok());
    const ReferenceSummary& b = rb.unwrap();
    EXPECT_EQ(b.thisClass, "demo/B");
    ASSERT_EQ(b.methods.size(), static_cast<ca::usize>(1));
    EXPECT_EQ(b.methods[0].name, "nil");
    EXPECT_TRUE(b.methods[0].invokes.empty());
    EXPECT_TRUE(b.methods[0].fieldAccesses.empty());
    ASSERT_EQ(b.fields.size(), static_cast<ca::usize>(1));
    EXPECT_EQ(b.fields[0].name, "v");
    EXPECT_EQ(b.fields[0].desc, "J");

    // 坏字节 fail-closed。
    std::vector<ca::u8> junk(24, 0xCD);
    const auto bad = rs.scan(ByteView{junk.data(), junk.size()});
    ASSERT_TRUE(bad.is_err());
    EXPECT_EQ(bad.unwrap_err(), consumer::ConsumerError::BadClassFile);
}

TEST(ConsumerRefTest, ReferenceScanSourceEnumeratesAll)
{
    const std::vector<ca::u8> aBytes = buildA();
    const std::vector<ca::u8> bBytes = buildB();
    ASSERT_FALSE(aBytes.empty());
    ASSERT_FALSE(bBytes.empty());

    MemoryClassSource src;
    src.add("demo/B", bBytes);
    src.add("demo/A", aBytes);

    ReferenceScanner rs;
    const auto all = rs.scanSource(src);
    ASSERT_TRUE(all.is_ok());
    ASSERT_EQ(all.unwrap().size(), static_cast<ca::usize>(2));
    EXPECT_EQ(all.unwrap()[0].thisClass, "demo/A");
    EXPECT_EQ(all.unwrap()[1].thisClass, "demo/B");
}
