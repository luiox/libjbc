/// @file        consumer_rename_test.cpp
/// @brief       consumer 批 3 · rename 分族应用（applyClassRename）、
///              BSM 载体 fail-loud 闸、no-op/透传纪律 与跨类驱动
///              （renameSource + IRenameResolver）的合成类直验。
///
/// 合成类与会话从零构造：A 引用 B（member operand + checkcast + ldc
/// 字符串）；G/H 挂 BootstrapMethods 载体（MethodType 参数 + 方法/字段
/// 句柄）。断言覆盖家族计数、字符串角色纪律（«LB;» 字面量不动）、
/// MT 描述符列改写、以及驱动 stage 的原字节透传与新字节替换。
#include <gtest/gtest.h>

#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "jbc/consumer/reference.hpp"
#include "jbc/consumer/rename.hpp"
#include "jbc/consumer/summary.hpp"
#include "jbc/consumer/source.hpp"
#include "jbc/core/attribute.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/ir/code_editor.hpp"
#include "jbc/ir/units.hpp"

using namespace jbc::core;

namespace core       = jbc::core;
namespace ir         = jbc::ir;
namespace consumer   = jbc::consumer;

using consumer::ConsumerError;
using consumer::MemoryClassSource;
using consumer::RenamePlan;
using consumer::ReferenceScanner;
using consumer::ReferenceSummary;
using consumer::SummaryScanner;

namespace {

/// @brief 会话 → writeClass 字节。
std::vector<ca::u8> writeOf(ir::ClassSession& s)
{
    const auto w = writeClass(s.backing());
    if (w.is_err()) {
        ADD_FAILURE() << "writeClass 失败";
        return {};
    }
    return std::move(w).unwrap();
}

/// @brief demo/B：字段 v:J + 抽象方法 m()V。
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
    ms.name   = "m";
    ms.desc   = "()V";
    ms.access = 0x0401;
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
    return writeOf(*ses);
}

/// @brief demo/A：call()V（getfield/invokevirtual/checkcast/ldc 引用 B）
///        + witness(Ldemo/B;)V。
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

/// @brief demo/G：BSM 载体（方法句柄 owner demo/B safe()V + MethodType
///        参数 "()Ldemo/B;"）——类名族改名的合法穿越样本。
std::vector<ca::u8> buildG()
{
    ir::NewClassSeed seed;
    seed.thisName  = "demo/G";
    seed.superName = "java/lang/Object";
    auto s = ir::makeSession(seed);
    if (s.is_err()) {
        ADD_FAILURE() << "makeSession(G) 失败";
        return {};
    }
    const std::shared_ptr<ir::ClassSession> sesHolder = s.unwrap();
    ir::ClassSession& ses = *sesHolder;
    ir::MethodSeed ms;
    ms.name   = "hit";
    ms.desc   = "()V";
    ms.access = 0x0001;
    if (ses.addMethod(ms).is_err()) {
        ADD_FAILURE() << "addMethod 失败";
        return {};
    }
    ir::MethodHandleSpec h;
    h.referenceKind = 5;   // INVOKE_VIRTUAL
    h.owner         = "demo/B";
    h.name          = "safe";
    h.desc          = "()V";
    ir::BsmArg mt;
    mt.kind = ir::BsmArg::Kind::MethodType;
    mt.str  = "()Ldemo/B;";
    if (ses.appendBootstrapMethod(h, {mt}).is_err()) {
        ADD_FAILURE() << "appendBootstrapMethod 失败";
        return {};
    }
    return writeOf(ses);
}

/// @brief demo/H：BSM 载体（字段句柄 kind 1 → demo/B v:J）——成员字段
///        规则的 fail-loud 样本。
std::vector<ca::u8> buildH()
{
    ir::NewClassSeed seed;
    seed.thisName  = "demo/H";
    seed.superName = "java/lang/Object";
    auto s = ir::makeSession(seed);
    if (s.is_err()) {
        ADD_FAILURE() << "makeSession(H) 失败";
        return {};
    }
    const std::shared_ptr<ir::ClassSession> sesHolder = s.unwrap();
    ir::ClassSession& ses = *sesHolder;
    ir::MethodSeed ms;
    ms.name   = "hit";
    ms.desc   = "()V";
    ms.access = 0x0001;
    if (ses.addMethod(ms).is_err()) {
        ADD_FAILURE() << "addMethod 失败";
        return {};
    }
    ir::MethodHandleSpec h;
    h.referenceKind = 1;   // getField
    h.owner         = "demo/B";
    h.name          = "v";
    h.desc          = "J";
    if (ses.appendBootstrapMethod(h, {}).is_err()) {
        ADD_FAILURE() << "appendBootstrapMethod 失败";
        return {};
    }
    return writeOf(ses);
}

/// @brief demo/C：极小类（无成员），驱动原字节透传样本。
std::vector<ca::u8> buildC()
{
    ir::NewClassSeed seed;
    seed.thisName  = "demo/C";
    seed.superName = "java/lang/Object";
    auto s = ir::makeSession(seed);
    if (s.is_err()) {
        ADD_FAILURE() << "makeSession(C) 失败";
        return {};
    }
    const std::shared_ptr<ir::ClassSession> ses = s.unwrap();   // 具名持活
    return writeOf(*ses);
}

/// @brief backing 池里全部 MethodType 条目的描述符文本（含空即无 MT 条目）。
std::vector<std::string> methodTypeDescs(const ir::ClassSession& s)
{
    std::vector<std::string> out;
    const core::ConstantPool& cp = s.backing().cp;
    for (ca::u32 i = 1; i < cp.size(); ++i) {
        const core::ConstantPool::Entry& e = cp.at(i);
        if (e.tag == CpTag::MethodType && cp.isUtf8(e.a)) {
            out.emplace_back(cp.utf8Of(e.a));
        }
    }
    return out;
}

const char* findStringText(const ir::ClassSession& s, const std::string_view text)
{
    const core::ConstantPool& cp = s.backing().cp;
    for (ca::u32 i = 1; i < cp.size(); ++i) {
        const core::ConstantPool::Entry& e = cp.at(i);
        if (e.tag != CpTag::String) {
            continue;
        }
        if (cp.isUtf8(e.a) && cp.utf8Of(e.a) == text) {
            return "found";
        }
    }
    return nullptr;
}

}   // namespace

// ---------------------------------------------------------------------------
// applyClassRename：A 上三类规则合流（类名/方法/字段）
// ---------------------------------------------------------------------------

TEST(ConsumerRenameTest, ApplyClassRenameCombinesFamilies)
{
    const std::vector<ca::u8> aBytes = buildA();
    ASSERT_FALSE(aBytes.empty());

    auto sr = ir::makeSessionFromBytes(ByteView{aBytes.data(), aBytes.size()});
    ASSERT_TRUE(sr.is_ok());
    const std::shared_ptr<ir::ClassSession> ses = std::move(sr).unwrap();

    RenamePlan plan;
    plan.renameClass("demo/B", "demo/D");
    plan.renameMethod("demo/B", "m", "()V", "m2");
    plan.renameField("demo/B", "v", "J", "w");

    const auto ar = consumer::applyClassRename(*ses, plan);
    ASSERT_TRUE(ar.is_ok());
    const consumer::RenameSummary& sum = ar.unwrap();
    EXPECT_TRUE(sum.changed);
    EXPECT_EQ(sum.refsRetargeted, 3u);   // getfield + invokevirtual + checkcast
    EXPECT_EQ(sum.classConstants, 1u);   // 唯一的 demo/B Class 条目重指向
    EXPECT_EQ(sum.methodRows, 1u);       // witness(Ldemo/B;)V → (Ldemo/D;)V
    EXPECT_EQ(sum.fieldRows, 0u);        // A 自身字段未被点名
    EXPECT_EQ(sum.descriptorCols, 0u);   // NAT 列 « »V »/«J» 无类名可映射

    // 字符串角色纪律：«LB;» 字面量原样存活（String 条目不碰）。
    EXPECT_NE(findStringText(*ses, "LB;"), nullptr);
    EXPECT_EQ(findStringText(*ses, "LD;"), nullptr);

    // 双层视图一致：写回字节经 E1 重扫，事实族全部落在新名上。
    const auto out = writeClass(ses->backing());
    ASSERT_TRUE(out.is_ok());
    const std::vector<ca::u8> outBytes = std::move(out).unwrap();
    const ReferenceScanner rs;
    const auto r = rs.scan(ByteView{outBytes.data(), outBytes.size()});
    ASSERT_TRUE(r.is_ok());
    const ReferenceSummary& ref = r.unwrap();
    EXPECT_EQ(ref.thisClass, "demo/A");
    ASSERT_EQ(ref.methods.size(), static_cast<ca::usize>(2));
    EXPECT_EQ(ref.methods[0].name, "call");
    ASSERT_EQ(ref.methods[0].fieldAccesses.size(), static_cast<ca::usize>(1));
    EXPECT_EQ(ref.methods[0].fieldAccesses[0].owner, "demo/D");
    EXPECT_EQ(ref.methods[0].fieldAccesses[0].name, "w");
    ASSERT_EQ(ref.methods[0].invokes.size(), static_cast<ca::usize>(1));
    EXPECT_EQ(ref.methods[0].invokes[0].owner, "demo/D");
    EXPECT_EQ(ref.methods[0].invokes[0].name, "m2");
    EXPECT_EQ(ref.methods[0].invokes[0].desc, "()V");
    ASSERT_EQ(ref.methods[0].typeOps.size(), static_cast<ca::usize>(1));
    EXPECT_EQ(ref.methods[0].typeOps[0].internalName, "demo/D");
    ASSERT_EQ(ref.methods[0].strings.size(), static_cast<ca::usize>(1));
    EXPECT_EQ(ref.methods[0].strings[0].text, "LB;");   // 字面量不动
    EXPECT_EQ(ref.methods[1].desc, "(Ldemo/D;)V");
    EXPECT_EQ(ref.methods[1].name, "witness");
}

// ---------------------------------------------------------------------------
// applyClassRename：BSM 载体两侧（类名族穿越 + 成员规则 fail-loud）
// ---------------------------------------------------------------------------

TEST(ConsumerRenameTest, BsmCarrierCrossedForClassRulesOnly)
{
    const std::vector<ca::u8> gBytes = buildG();
    ASSERT_FALSE(gBytes.empty());

    auto sr = ir::makeSessionFromBytes(ByteView{gBytes.data(), gBytes.size()});
    ASSERT_TRUE(sr.is_ok());
    const std::shared_ptr<ir::ClassSession> ses = std::move(sr).unwrap();

    RenamePlan plan;
    plan.renameClass("demo/B", "demo/D");
    const auto ar = consumer::applyClassRename(*ses, plan);
    ASSERT_TRUE(ar.is_ok());
    const consumer::RenameSummary& sum = ar.unwrap();
    EXPECT_TRUE(sum.changed);
    EXPECT_EQ(sum.classConstants, 1u);   // demo/B Class 条目（句柄 owner 走的地方）
    ASSERT_EQ(sum.descriptorCols, 1u);   // MethodType 参数列 ()Ldemo/B; → ()Ldemo/D;

    // MT 条目文本落新名。
    const std::vector<std::string> mts = methodTypeDescs(*ses);
    ASSERT_EQ(mts.size(), static_cast<ca::usize>(1));
    EXPECT_EQ(mts[0], "()Ldemo/D;");

    // BSM 属性本身保持一条（载荷结构不动）。
    AttrSlot* bsm = ses->backing().findClassAttr("BootstrapMethods");
    ASSERT_NE(bsm, nullptr);
    const auto d = ses->backing().decodeAttr(*bsm);
    ASSERT_TRUE(d.is_ok());
    ASSERT_EQ(d.unwrap()->kind(), AttrKind::BootstrapMethods);
    const auto* bm = static_cast<const BootstrapMethodsAttr*>(bsm->body.get());
    ASSERT_EQ(bm->entries.size(), static_cast<ca::usize>(1));
}

TEST(ConsumerRenameTest, MethodRuleOnBsmCarrierFailsLoud)
{
    const std::vector<ca::u8> gBytes = buildG();
    ASSERT_FALSE(gBytes.empty());

    auto sr = ir::makeSessionFromBytes(ByteView{gBytes.data(), gBytes.size()});
    ASSERT_TRUE(sr.is_ok());
    const std::shared_ptr<ir::ClassSession> ses = std::move(sr).unwrap();

    RenamePlan plan;
    plan.renameMethod("demo/B", "safe", "()V", "safe2");   // 载体目标
    const auto ar = consumer::applyClassRename(*ses, plan);
    ASSERT_TRUE(ar.is_err());
    EXPECT_EQ(ar.unwrap_err().kind, consumer::RenameError::Kind::BsmTouched);
}

TEST(ConsumerRenameTest, FieldRuleOnBsmCarrierFailsLoud)
{
    const std::vector<ca::u8> hBytes = buildH();
    ASSERT_FALSE(hBytes.empty());

    auto sr = ir::makeSessionFromBytes(ByteView{hBytes.data(), hBytes.size()});
    ASSERT_TRUE(sr.is_ok());
    const std::shared_ptr<ir::ClassSession> ses = std::move(sr).unwrap();

    RenamePlan plan;
    plan.renameField("demo/B", "v", "J", "w");
    const auto ar = consumer::applyClassRename(*ses, plan);
    ASSERT_TRUE(ar.is_err());
    EXPECT_EQ(ar.unwrap_err().kind, consumer::RenameError::Kind::BsmTouched);
}

TEST(ConsumerRenameTest, ClassRuleCrossesFieldCarrier)
{
    // 对照组：同一字段句柄载体，只改类名——合法穿越（CP Class 条目重指向）。
    const std::vector<ca::u8> hBytes = buildH();
    ASSERT_FALSE(hBytes.empty());

    auto sr = ir::makeSessionFromBytes(ByteView{hBytes.data(), hBytes.size()});
    ASSERT_TRUE(sr.is_ok());
    const std::shared_ptr<ir::ClassSession> ses = std::move(sr).unwrap();

    RenamePlan plan;
    plan.renameClass("demo/B", "demo/D");
    const auto ar = consumer::applyClassRename(*ses, plan);
    ASSERT_TRUE(ar.is_ok());
    const consumer::RenameSummary& sum2 = ar.unwrap();
    EXPECT_TRUE(sum2.changed);
    EXPECT_EQ(sum2.classConstants, 1u);
    EXPECT_EQ(sum2.descriptorCols, 0u);   // NAT 列 «J» 无类名
}

// ---------------------------------------------------------------------------
// applyClassRename：计划纪律
// ---------------------------------------------------------------------------

TEST(ConsumerRenameTest, BadRuleRejectedBeforeTouch)
{
    const std::vector<ca::u8> bBytes = buildB();
    ASSERT_FALSE(bBytes.empty());

    auto sr = ir::makeSessionFromBytes(ByteView{bBytes.data(), bBytes.size()});
    ASSERT_TRUE(sr.is_ok());
    const std::shared_ptr<ir::ClassSession> ses = std::move(sr).unwrap();

    RenamePlan plan;
    plan.renameClass("demo/B", "bad;name");   // 分号：非法内部名（isValidInternalName 拦 '.['与';'）
    const auto ar = consumer::applyClassRename(*ses, plan);
    ASSERT_TRUE(ar.is_err());
    EXPECT_EQ(ar.unwrap_err().kind, consumer::RenameError::Kind::BadRule);
}

TEST(ConsumerRenameTest, EmptyPlanIsNoOp)
{
    const std::vector<ca::u8> bBytes = buildB();
    ASSERT_FALSE(bBytes.empty());

    auto sr = ir::makeSessionFromBytes(ByteView{bBytes.data(), bBytes.size()});
    ASSERT_TRUE(sr.is_ok());
    const std::shared_ptr<ir::ClassSession> ses = std::move(sr).unwrap();

    const auto ar = consumer::applyClassRename(*ses, RenamePlan{});
    ASSERT_TRUE(ar.is_ok());
    const consumer::RenameSummary& sum = ar.unwrap();
    EXPECT_FALSE(sum.changed);
    EXPECT_EQ(sum.classConstants, 0u);
}

// ---------------------------------------------------------------------------
// renameSource：跨类驱动
// ---------------------------------------------------------------------------

namespace {

class TestResolver final : public consumer::IRenameResolver
{
public:
    explicit TestResolver(std::map<std::string, const RenamePlan*> plans,
                          const bool fail = false)
        : plans_(std::move(plans)), fail_(fail)
    {
    }

    consumer::ConsumerResult<const RenamePlan*> planFor(std::string_view className) override
    {
        if (fail_) {
            return Err(ConsumerError::RenameRule);
        }
        const auto it = plans_.find(std::string(className));
        if (it == plans_.end()) {
            const RenamePlan* miss = nullptr;   // 缺计划 = 原字节透传
            return Ok(miss);
        }
        return Ok(it->second);
    }

private:
    std::map<std::string, const RenamePlan*> plans_;
    bool fail_;
};

}   // namespace

TEST(ConsumerRenameTest, DriverStagesRenamesAndPassesThrough)
{
    const std::vector<ca::u8> aBytes = buildA();
    const std::vector<ca::u8> bBytes = buildB();
    const std::vector<ca::u8> cBytes = buildC();
    ASSERT_FALSE(aBytes.empty());
    ASSERT_FALSE(bBytes.empty());
    ASSERT_FALSE(cBytes.empty());

    MemoryClassSource src;
    src.add("demo/B", bBytes);
    src.add("demo/A", aBytes);
    src.add("demo/C", cBytes);

    RenamePlan planA;
    planA.renameClass("demo/B", "demo/D");
    planA.renameMethod("demo/B", "m", "()V", "m2");
    planA.renameField("demo/B", "v", "J", "w");

    RenamePlan planB;
    planB.renameClass("demo/B", "demo/D");
    planB.renameMethod("demo/B", "m", "()V", "m2");
    planB.renameField("demo/B", "v", "J", "w");

    TestResolver resolver{{{"demo/A", &planA}, {"demo/B", &planB}}};
    auto staged = consumer::renameSource(src, resolver);
    ASSERT_TRUE(staged.is_ok());
    consumer::MemoryClassSource stage = std::move(staged).unwrap();

    // 键 = 源枚举名（改名的类文件按原枚举名落位，舞台键不重排）。
    EXPECT_EQ(stage.classCount(), static_cast<ca::usize>(3));
    std::vector<std::string_view> names;
    stage.listClasses(names);
    ASSERT_EQ(names.size(), static_cast<ca::usize>(3));
    EXPECT_EQ(std::string(names[0]), "demo/A");
    EXPECT_EQ(std::string(names[1]), "demo/B");
    EXPECT_EQ(std::string(names[2]), "demo/C");

    // 无计划类：字节精确透传。
    const auto rc = stage.bytes("demo/C");
    ASSERT_TRUE(rc.is_ok());
    EXPECT_EQ(rc.unwrap().size(), cBytes.size());
    EXPECT_EQ(0, std::memcmp(rc.unwrap().data(), cBytes.data(), cBytes.size()));

    // A 阶段产物：引用事实族落新名；字符串字面量角色纪律保留。
    const auto ra = stage.bytes("demo/A");
    ASSERT_TRUE(ra.is_ok());
    const ReferenceScanner rs;
    const auto refA = rs.scan(ByteView{ra.unwrap().data(), ra.unwrap().size()});
    ASSERT_TRUE(refA.is_ok());
    EXPECT_EQ(refA.unwrap().methods[0].invokes[0].owner, "demo/D");
    EXPECT_EQ(refA.unwrap().methods[0].invokes[0].name, "m2");
    EXPECT_EQ(refA.unwrap().methods[0].fieldAccesses[0].name, "w");
    EXPECT_EQ(refA.unwrap().methods[0].strings[0].text, "LB;");
    EXPECT_EQ(refA.unwrap().methods[0].typeOps[0].internalName, "demo/D");

    // B 阶段产物：自身改名为 D，成员行落新名。
    const auto rb = stage.bytes("demo/B");
    ASSERT_TRUE(rb.is_ok());
    const SummaryScanner ss;
    const auto sumB = ss.scan(ByteView{rb.unwrap().data(), rb.unwrap().size()});
    ASSERT_TRUE(sumB.is_ok());
    EXPECT_EQ(sumB.unwrap().internalName, "demo/D");
    ASSERT_EQ(sumB.unwrap().fields.size(), static_cast<ca::usize>(1));
    EXPECT_EQ(sumB.unwrap().fields[0].name, "w");
    ASSERT_EQ(sumB.unwrap().methods.size(), static_cast<ca::usize>(1));
    EXPECT_EQ(sumB.unwrap().methods[0].name, "m2");
}

TEST(ConsumerRenameTest, DriverFailClosedOnResolverError)
{
    const std::vector<ca::u8> bBytes = buildB();
    ASSERT_FALSE(bBytes.empty());

    MemoryClassSource src;
    src.add("demo/B", bBytes);
    TestResolver resolver{{}, true};   // resolver 直接失败
    const auto staged = consumer::renameSource(src, resolver);
    ASSERT_TRUE(staged.is_err());
    EXPECT_EQ(staged.unwrap_err(), ConsumerError::RenameRule);
}

TEST(ConsumerRenameTest, DriverFailClosedOnBadClassFile)
{
    const std::vector<ca::u8> junk(32, 0x33);
    MemoryClassSource src;
    src.add("demo/J", junk);

    RenamePlan planJ;
    planJ.renameClass("demo/J", "demo/K");
    TestResolver resolver{{{"demo/J", &planJ}}};
    const auto staged = consumer::renameSource(src, resolver);
    ASSERT_TRUE(staged.is_err());
    EXPECT_EQ(staged.unwrap_err(), ConsumerError::BadClassFile);
}

TEST(ConsumerRenameTest, DriverWritesBsmTouchedAsRenameFailed)
{
    const std::vector<ca::u8> hBytes = buildH();
    ASSERT_FALSE(hBytes.empty());

    MemoryClassSource src;
    src.add("demo/H", hBytes);

    RenamePlan planH;
    planH.renameField("demo/B", "v", "J", "w");   // 载体触达 → gate Err
    TestResolver resolver{{{"demo/H", &planH}}};
    const auto staged = consumer::renameSource(src, resolver);
    ASSERT_TRUE(staged.is_err());
    EXPECT_EQ(staged.unwrap_err(), ConsumerError::RenameFailed);
}
