/// @file        cp_compact_test.cpp
/// @brief       CP 显式压缩的单元门。
///
/// 覆盖面：稳序映射钉面（新旧索引逐条目断言）、规范去重（Utf8/String/Class
/// 内容复用）、死条目清除、wide 双槽、指令 CP 操作数（LDC/LDC_W/LDC2_W，
/// 形态不收缩）、压缩侧五件（SourceFile/EnclosingMethod/InnerClasses/
/// Exceptions/ConstantValue）、零长透传、Raw 残留显式拒绝（带属性名定位）、
/// 五件畸形拒绝、SMT Object 载荷与 TA 非 CP 字段不误重映射、typed 注解与
/// Signature 面、注解族四件（ParameterAnnotations/AnnotationDefault/
/// MethodParameters/ConstantValue）闭包与重映射去重、会话一致性（升格体同
/// 步、语义 intern 重建、ChangeSet 记账、no-op 不记账）、无变化早退字节恒
/// 等、失败不留半截模型、确定性（两次压缩逐字节一致）、膨胀后压缩回到规范
/// 形、NewClassSeed 无源类适用。
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "jbc/jbc.hpp"
#include "test_class_bytes.hpp"

using namespace jbc::core;
using namespace jbc::ir;
using Bytes = jbc::test::Bytes;


namespace {

/// @brief 建会话；失败即断言。
std::shared_ptr<ClassSession> sessionOf(const Bytes& bytes)
{
    auto r = makeSessionFromBytes(ByteView{bytes.data(), bytes.size()});
    if (r.is_err()) {
        ADD_FAILURE() << "makeSessionFromBytes failed";
        return nullptr;
    }
    return std::move(r).unwrap();
}

std::string utf8At(const ClassFile& cf, ca::u32 idx)
{
    return std::string(cf.utf8(idx));
}

/// @brief 取 DOM 指令（句柄簿记自洽，直接解包）。
const Insn* domInsn(CodeUnit& cu, InsnId id)
{
    const Result<Insn*, EditError> g = cu.dom.get(id);
    return g.unwrap();
}

TEST(CpCompact, StableMappingPinnedFace)
{
    // 池：1:"p/T" 2:"java/lang/Object" 3:"Code" 4:"<init>" 5:"()V"
    //     6:Class(1)=this 7:Class(2)=super 8:"A"(死) 9:"p/T"(死重复) 10:Integer(死)
    jbc::test::ClassBuilder cb;
    const ca::u16 u1 = cb.utf8("p/T");
    const ca::u16 u2 = cb.utf8("java/lang/Object");
    const ca::u16 u3 = cb.utf8("Code");
    const ca::u16 u4 = cb.utf8("<init>");
    const ca::u16 u5 = cb.utf8("()V");
    const ca::u16 c1 = cb.classRef(u1);
    const ca::u16 c2 = cb.classRef(u2);
    static_cast<void>(cb.utf8("A"));          // 死条目
    static_cast<void>(cb.utf8("p/T"));        // 死重复
    static_cast<void>(cb.integer(7));         // 死条目
    cb.setThis(c1).setSuper(c2);
    const Bytes code = {0xB1};                // return
    cb.addMethod(0x0001, u4, u5,
                 {jbc::test::AttrSpec{u3, jbc::test::codeContent(1, 1, code)}});
    const Bytes bytes = cb.build();

    auto sess = sessionOf(bytes);
    ASSERT_NE(sess, nullptr);
    ASSERT_TRUE(compactConstantPool(*sess).is_ok());

    // 子序映射：活条目按老索引升序原位保留，死条目 8/9/10 不落入
    // 新池——本例活面无重复，映射逐条恒等：1→1 … 7→7。
    const ClassFile& cf = sess->backing();
    ASSERT_EQ(cf.cp.size(), 8u);
    EXPECT_EQ(utf8At(cf, 1), "p/T");
    EXPECT_EQ(utf8At(cf, 2), "java/lang/Object");
    EXPECT_EQ(utf8At(cf, 3), "Code");
    EXPECT_EQ(utf8At(cf, 4), "<init>");
    EXPECT_EQ(utf8At(cf, 5), "()V");
    ASSERT_TRUE(cf.cp.valid(6));
    EXPECT_EQ(cf.cp.tagOf(6), CpTag::Class);
    EXPECT_EQ(cf.cp.at(6).a, 1u);
    ASSERT_TRUE(cf.cp.valid(7));
    EXPECT_EQ(cf.cp.tagOf(7), CpTag::Class);
    EXPECT_EQ(cf.cp.at(7).a, 2u);
    EXPECT_FALSE(cf.cp.valid(8u));   // 死条目全部消失
    EXPECT_FALSE(cf.cp.valid(9u));
    EXPECT_FALSE(cf.cp.valid(10u));
    EXPECT_EQ(cf.thisClass, 6u);
    EXPECT_EQ(cf.superClass, 7u);
    ASSERT_EQ(cf.methods.size(), 1u);
    EXPECT_EQ(cf.methods[0].nameCp, 4u);
    EXPECT_EQ(cf.methods[0].descCp, 5u);
    ASSERT_EQ(cf.methods[0].attrs.size(), 1u);
    EXPECT_EQ(cf.methods[0].attrs[0].nameCp, 3u);
}

TEST(CpCompact, DeterminismAndNoopEarlyExit)
{
    jbc::test::ClassBuilder cb;
    const ca::u16 u1 = cb.utf8("p/T");
    const ca::u16 u2 = cb.utf8("java/lang/Object");
    const ca::u16 u3 = cb.utf8("Code");
    const ca::u16 u4 = cb.utf8("<init>");
    const ca::u16 u5 = cb.utf8("()V");
    const ca::u16 c1 = cb.classRef(u1);
    const ca::u16 c2 = cb.classRef(u2);
    static_cast<void>(cb.utf8("dead-pad"));   // 死条目：让首压是实改而非 no-op
    cb.setThis(c1).setSuper(c2);
    const Bytes code = {0xB1};
    cb.addMethod(0x0001, u4, u5,
                 {jbc::test::AttrSpec{u3, jbc::test::codeContent(1, 1, code)}});
    const Bytes bytes = cb.build();

    auto sess = sessionOf(bytes);
    ASSERT_NE(sess, nullptr);
    const ca::usize changesBefore = sess->changeSet().size();
    ASSERT_TRUE(compactConstantPool(*sess).is_ok());
    const Bytes first = jbc::test::writeOk(sess->backing());
    ASSERT_FALSE(first.empty());

    // 首次压缩记了 ClassMetadata（类级实改）。
    EXPECT_GT(sess->changeSet().size(), changesBefore);
    EXPECT_EQ(sess->changeSet().classChange(), ClassChange::Metadata);

    // 第二次压缩：池已紧凑 → no-op 早退，不记账、字节不动。
    const ca::usize changesMid = sess->changeSet().size();
    ASSERT_TRUE(compactConstantPool(*sess).is_ok());
    EXPECT_EQ(sess->changeSet().size(), changesMid);
    const Bytes second = jbc::test::writeOk(sess->backing());
    EXPECT_EQ(second, first);

    // 独立会话压缩同一输入：产物逐字节一致（确定性）。
    auto sess2 = sessionOf(bytes);
    ASSERT_NE(sess2, nullptr);
    ASSERT_TRUE(compactConstantPool(*sess2).is_ok());
    EXPECT_EQ(jbc::test::writeOk(sess2->backing()), first);

    // 已紧凑的类再压缩：无变化早退（不记账、写回与输入一致）。
    auto sess3 = sessionOf(first);
    ASSERT_NE(sess3, nullptr);
    ASSERT_TRUE(compactConstantPool(*sess3).is_ok());
    EXPECT_EQ(sess3->changeSet().size(), 0u);
    EXPECT_EQ(jbc::test::writeOk(sess3->backing()), first);
}

TEST(CpCompact, DedupMergesDuplicatesFormsPreserved)
{
    // Utf8/String 内容重复复合去重；LDC/LDC_W/LDC2_W 形态不收缩，只换操作数。
    jbc::test::ClassBuilder cb;
    const ca::u16 u1 = cb.utf8("p/T");
    const ca::u16 u2 = cb.utf8("java/lang/Object");
    const ca::u16 u3 = cb.utf8("Code");
    const ca::u16 u4 = cb.utf8("run");
    const ca::u16 u5 = cb.utf8("()V");
    const ca::u16 c1 = cb.classRef(u1);
    const ca::u16 c2 = cb.classRef(u2);
    const ca::u16 s1 = cb.utf8("S");
    const ca::u16 st1 = cb.stringConst(s1);
    static_cast<void>(cb.utf8("S"));              // 内容重复（死重复）
    const ca::u16 st2 = cb.stringConst(cb.utf8("S"));  // → 与 st1 复合去重
    static_cast<void>(cb.integer(77));
    const ca::u16 lo1 = cb.longConst(999888777);  // 占两槽
    static_cast<void>(cb.integer(77));            // 死重复
    const ca::u16 cc1 = cb.classRef(u1);          // 与 c1 复合去重
    cb.setThis(c1).setSuper(c2);
    const Bytes code = {0x12, static_cast<ca::u8>(st1),                    // ldc st1
                        0x13, static_cast<ca::u8>(st2 >> 8), static_cast<ca::u8>(st2 & 0xFF),
                        0x12, static_cast<ca::u8>(st1),                    // （多余一条 ldc 便于计数）
                        0x14, static_cast<ca::u8>(lo1 >> 8), static_cast<ca::u8>(lo1 & 0xFF),
                        0x58,                                              // pop2
                        0x12, static_cast<ca::u8>(cc1),                    // ldc class
                        0x57,                                              // pop
                        0xB1};
    cb.addMethod(0x0009, u4, u5,
                 {jbc::test::AttrSpec{u3, jbc::test::codeContent(4, 1, code)}});
    const Bytes bytes = cb.build();

    auto sess = sessionOf(bytes);
    ASSERT_NE(sess, nullptr);
    const ca::usize before = sess->backing().cp.size();
    ASSERT_TRUE(compactConstantPool(*sess).is_ok());
    const ClassFile& cf = sess->backing();
    EXPECT_LT(cf.cp.size(), before);              // 去重生效：条目数下降
    EXPECT_EQ(cf.cp.findUtf8("S"), 8u);              // 首触序稳定位置
    EXPECT_EQ(cf.cp.findUtf8("p/T"), 1u);

    // 重读：语义面恒等 + 指令形态不收缩。
    const Bytes out = jbc::test::writeOk(cf);
    auto rr = readClass(ByteView{out});
    ASSERT_TRUE(rr.is_ok());
    auto sr = makeSession(std::move(rr).unwrap().file);
    ASSERT_TRUE(sr.is_ok());
    auto s2 = sr.unwrap();
    const ca::usize mi = s2->unit().findMethod("run");
    ASSERT_NE(mi, kNoMethod);
    auto p = promoteMethod(*s2, mi);
    ASSERT_TRUE(p.is_ok());
    CodeUnit& cu = *p.unwrap();
    ASSERT_EQ(cu.dom.size(), 8u);
    const std::vector<InsnId> order = cu.dom.order();

    const Insn* i0 = domInsn(cu, order[0]);
    EXPECT_EQ(i0->op, Op::LDC);                   // LDC 保持 LDC
    EXPECT_EQ(i0->cp, 9u);
    const Insn* i1 = domInsn(cu, order[1]);
    EXPECT_EQ(i1->op, Op::LDC_W);                 // 形态不收缩：ldc_w 不降格
    EXPECT_EQ(i1->cp, 9u);                        // 去重后与 ldc 同一条 String
    const Insn* i3 = domInsn(cu, order[3]);
    EXPECT_EQ(i3->op, Op::LDC2_W);
    EXPECT_EQ(i3->cp, 10u);                       // Long 主槽新索引
    EXPECT_FALSE(cf.cp.valid(11u));               // wide 续槽保持占位
    const Insn* i5 = domInsn(cu, order[5]);
    EXPECT_EQ(i5->op, Op::LDC);
    EXPECT_EQ(i5->cp, 6u);                        // 与 thisClass 复合去重（代表=老 6）

    ClassSession::ConstSem c0;
    ASSERT_TRUE(s2->constOf(*i0, c0));
    EXPECT_EQ(c0.kind, ClassSession::ConstSem::Kind::String);
    EXPECT_EQ(c0.str, "S");
    ClassSession::ConstSem c3;
    ASSERT_TRUE(s2->constOf(*i3, c3));
    EXPECT_EQ(c3.kind, ClassSession::ConstSem::Kind::Long);
    EXPECT_EQ(c3.num, 999888777);
    ClassSession::ConstSem c5;
    ASSERT_TRUE(s2->constOf(*i5, c5));
    EXPECT_EQ(c5.kind, ClassSession::ConstSem::Kind::Class);
    EXPECT_EQ(c5.str, "p/T");
}

TEST(CpCompact, MethodHandleKeyUsesRefNotKindColumn)
{
    // MH/Dyn/Indy 的 a 列（reference_kind / bootstrap 下标）不是 CP 引用：
    // 等价类键必须取引用列（b）的类号——同 kind 异引用不得并类，真重复才并。
    jbc::test::ClassBuilder cb;
    const ca::u16 uT = cb.utf8("p/T");
    const ca::u16 uO = cb.utf8("java/lang/Object");
    const ca::u16 uCode = cb.utf8("Code");
    const ca::u16 uInit = cb.utf8("<init>");
    const ca::u16 uVoid = cb.utf8("()V");
    const ca::u16 uBM = cb.utf8("BootstrapMethods");
    const ca::u16 uA = cb.utf8("a1");
    const ca::u16 uB = cb.utf8("b2");
    const ca::u16 c1 = cb.classRef(uT);
    const ca::u16 c2 = cb.classRef(uO);
    const ca::u16 m1 = cb.methodref(cb.classRef(cb.utf8("p/A")), cb.nameAndType(cb.utf8("m1"), uVoid));
    const ca::u16 m2 = cb.methodref(cb.classRef(cb.utf8("p/B")), cb.nameAndType(cb.utf8("m2"), uVoid));
    const ca::u16 mh1 = cb.methodHandle(6, m1);       // kind 6 → m1
    const ca::u16 mh2 = cb.methodHandle(6, m2);       // 同 kind → m2：不得与 mh1 并类
    const ca::u16 mh3 = cb.methodHandle(6, m1);       // 真重复：与 mh1 并类
    cb.setThis(c1).setSuper(c2);
    const Bytes code = {0xB1};
    cb.addMethod(0x0001, uInit, uVoid,
                 {jbc::test::AttrSpec{uCode, jbc::test::codeContent(1, 1, code)}});
    // BootstrapMethods：三行，行 0/2 指向同一内容（并类），行 1 独立。
    Bytes bm;
    jbc::test::detail::putU16(bm, 3);
    for (const ca::u16 mh : {mh1, mh2, mh3}) {
        jbc::test::detail::putU16(bm, mh);
        jbc::test::detail::putU16(bm, 0);             // 无参数
    }
    cb.addClassAttr(uBM, bm);
    const Bytes bytes = cb.build();

    auto sess = sessionOf(bytes);
    ASSERT_NE(sess, nullptr);
    ASSERT_TRUE(compactConstantPool(*sess).is_ok());
    ClassFile& cf = sess->backing();
    // 计划相已解码：直接读模型。mh1 与 mh3 同槽；mh2 独立存在。
    AttrSlot* bmSlot = cf.findClassAttr("BootstrapMethods");
    ASSERT_NE(bmSlot, nullptr);
    ASSERT_NE(bmSlot->body, nullptr);
    auto& b = static_cast<BootstrapMethodsAttr&>(*bmSlot->body);
    ASSERT_EQ(b.entries.size(), 3u);
    EXPECT_EQ(b.entries[0].methodRef, b.entries[2].methodRef);
    EXPECT_NE(b.entries[0].methodRef, b.entries[1].methodRef);
    // 行 1 的目标确实还是 m2（kind 列误入键时会错并成 m1/metafactory 形态）。
    const ca::u32 mhIdx  = b.entries[1].methodRef;
    const ca::u32 mrefIx = cf.cp.at(mhIdx).b;
    const ca::u32 natIx  = cf.cp.at(mrefIx).b;
    EXPECT_EQ(cf.utf8(cf.cp.at(natIx).a), "m2");
}

TEST(CpCompact, CompactClosedSetSideAttrsRemapped)
{
    jbc::test::ClassBuilder cb;
    const ca::u16 uT = cb.utf8("p/T");
    const ca::u16 uO = cb.utf8("java/lang/Object");
    const ca::u16 uCode = cb.utf8("Code");
    const ca::u16 uInit = cb.utf8("<init>");
    const ca::u16 uVoid = cb.utf8("()V");
    const ca::u16 uSF = cb.utf8("SourceFile");
    const ca::u16 uSFv = cb.utf8("p/T.java");
    const ca::u16 uIC = cb.utf8("InnerClasses");
    const ca::u16 uIn = cb.utf8("In");
    const ca::u16 uInT = cb.utf8("p/T$In");
    const ca::u16 uEM = cb.utf8("EnclosingMethod");
    const ca::u16 uRun = cb.utf8("run");
    const ca::u16 uEmV = cb.utf8("()V");          // 与 uVoid 内容重复 → 去重
    const ca::u16 cT = cb.classRef(uT);
    const ca::u16 cO = cb.classRef(uO);
    const ca::u16 cIn = cb.classRef(uInT);
    const ca::u16 cT2 = cb.classRef(uT);          // 外层类：与 cT 复合去重
    const ca::u16 emNat = cb.nameAndType(uRun, uEmV);
    const ca::u16 uEx = cb.utf8("Exceptions");
    const ca::u16 uThr = cb.utf8("java/lang/Throwable");
    const ca::u16 cThr = cb.classRef(uThr);
    const ca::u16 uCV = cb.utf8("ConstantValue");
    const ca::u16 uVal = cb.utf8("value");
    const ca::u16 uI = cb.utf8("I");
    const ca::u16 int42 = cb.integer(42);
    const ca::u16 uDep = cb.utf8("Deprecated");   // 零长透传面
    const ca::u16 uNH = cb.utf8("NestHost");
    const ca::u16 uNM = cb.utf8("NestMembers");
    const ca::u16 uPS = cb.utf8("PermittedSubclasses");
    const ca::u16 uRec = cb.utf8("Record");
    const ca::u16 uSigName = cb.utf8("Signature");
    const ca::u16 uRecName = cb.utf8("value");              // 与 uVal 内容重复 → 压缩去重
    const ca::u16 uRTDesc = cb.utf8("Ljava/lang/Throwable;");
    const ca::u16 uRTSig  = cb.utf8("Ljava/lang/Throwable;"); // 与上一条重复 → 压缩去重
    cb.setThis(cT).setSuper(cO);

    Bytes cvBody;
    jbc::test::detail::putU16(cvBody, int42);
    cb.addField(0x0018, uVal, uI, {jbc::test::AttrSpec{uCV, cvBody}});

    Bytes excBody;
    jbc::test::detail::putU16(excBody, 1);
    jbc::test::detail::putU16(excBody, cThr);
    const Bytes initCode = {0xB1};
    cb.addMethod(0x0001, uInit, uVoid,
                 {jbc::test::AttrSpec{uCode, jbc::test::codeContent(1, 1, initCode)},
                  jbc::test::AttrSpec{uEx, excBody}});

    Bytes sfBody, icBody, emBody, nhBody, nmBody, psBody, recBody;
    jbc::test::detail::putU16(sfBody, uSFv);
    jbc::test::detail::putU16(icBody, 1);
    jbc::test::detail::putU16(icBody, cIn);
    jbc::test::detail::putU16(icBody, cT2);
    jbc::test::detail::putU16(icBody, uIn);
    jbc::test::detail::putU16(icBody, 0);
    jbc::test::detail::putU16(emBody, cT);
    jbc::test::detail::putU16(emBody, emNat);
    // Nest 族三件条目复用既有 Class 条目（super/内部类/Throwable/自身），
    // 同时压一遍去重面：cT2 是 p/T 的复合去重代表。
    jbc::test::detail::putU16(nhBody, cO);
    jbc::test::detail::putU16(nmBody, 2);
    jbc::test::detail::putU16(nmBody, cIn);
    jbc::test::detail::putU16(nmBody, cThr);
    jbc::test::detail::putU16(psBody, 1);
    jbc::test::detail::putU16(psBody, cT2);
    // Record：一组件（name 与字段名 "value" 重复），带嵌套 Signature——组件
    // 两槽 Utf8 与嵌套属性载荷三处都过压缩通道（两两内容重复 → 去重同条目）。
    jbc::test::detail::putU16(recBody, 1);
    jbc::test::detail::putU16(recBody, uRecName);
    jbc::test::detail::putU16(recBody, uRTDesc);
    jbc::test::detail::putU16(recBody, 1);
    jbc::test::detail::putU16(recBody, uSigName);
    jbc::test::detail::putU32(recBody, 2);
    jbc::test::detail::putU16(recBody, uRTSig);
    cb.addClassAttr(uSF, sfBody);
    cb.addClassAttr(uIC, icBody);
    cb.addClassAttr(uEM, emBody);
    cb.addClassAttr(uNH, nhBody);
    cb.addClassAttr(uNM, nmBody);
    cb.addClassAttr(uPS, psBody);
    cb.addClassAttr(uRec, recBody);
    cb.addClassAttr(uDep, {});                    // Deprecated：零长体
    const Bytes bytes = cb.build();

    auto sess = sessionOf(bytes);
    ASSERT_NE(sess, nullptr);
    ASSERT_TRUE(compactConstantPool(*sess).is_ok());

    const Bytes out = jbc::test::writeOk(sess->backing());
    auto rr = readClass(ByteView{out});
    ASSERT_TRUE(rr.is_ok());
    auto rd = std::move(rr).unwrap();
    auto& cf = *rd.file;

    // SourceFile：语义恒等。分派接线后为 typed 体（Raw 形不可达），
    // 直接按 typed 模型读索引。
    const AttrSlot* sf = cf.findClassAttr("SourceFile");
    ASSERT_NE(sf, nullptr);
    ASSERT_TRUE(cf.decodeAttr(const_cast<AttrSlot&>(*sf)).is_ok());
    const auto& sfAttr = static_cast<const SourceFileAttr&>(*sf->body);
    EXPECT_EQ(sfAttr.trailing.size(), 0u);
    const ca::u32 sfIdx = sfAttr.sourceFileCp;
    EXPECT_EQ(cf.utf8(sfIdx), "p/T.java");

    // InnerClasses：四列中前三列重映射、access 列不动。分派接线后为
    // typed 体（Raw 形不可达），直接按 typed 模型读三槽。
    const AttrSlot* ic = cf.findClassAttr("InnerClasses");
    ASSERT_NE(ic, nullptr);
    ASSERT_TRUE(cf.decodeAttr(const_cast<AttrSlot&>(*ic)).is_ok());
    const auto& icAttr = static_cast<const InnerClassesAttr&>(*ic->body);
    ASSERT_EQ(icAttr.rows.size(), 1u);
    EXPECT_EQ(cf.className(icAttr.rows[0].innerClassInfoCp), "p/T$In");
    EXPECT_EQ(cf.className(icAttr.rows[0].outerClassInfoCp), "p/T");
    EXPECT_EQ(cf.utf8(icAttr.rows[0].innerNameCp), "In");
    EXPECT_EQ(icAttr.rows[0].accessFlags, 0u);

    // EnclosingMethod：类 + NAT（描述符与 uVoid 去重成同一条目）。
    const AttrSlot* em = cf.findClassAttr("EnclosingMethod");
    ASSERT_NE(em, nullptr);
    ASSERT_TRUE(cf.decodeAttr(const_cast<AttrSlot&>(*em)).is_ok());
    const auto& emAttr = static_cast<const EnclosingMethodAttr&>(*em->body);
    EXPECT_EQ(cf.className(emAttr.classCp), "p/T");
    ASSERT_TRUE(cf.cp.valid(emAttr.methodNatCp));
    ASSERT_EQ(cf.cp.tagOf(emAttr.methodNatCp), CpTag::NameAndType);
    EXPECT_EQ(cf.utf8(cf.cp.at(emAttr.methodNatCp).a), "run");
    EXPECT_EQ(cf.utf8(cf.cp.at(emAttr.methodNatCp).b), "()V");
    // 去重面：EnclosingMethod 的 NAT 描述符与方法描述符是同一条目。
    ASSERT_EQ(cf.methods.size(), 1u);
    EXPECT_EQ(cf.cp.at(emAttr.methodNatCp).b, cf.methods[0].descCp);

    // Exceptions：方法上的一条 Class。
    const AttrSlot* ex = ClassFile::findAttrByCp(cf.methods[0].attrs,
                                                 cf.methods[0].attrs[1].nameCp);
    ASSERT_NE(ex, nullptr);
    ASSERT_TRUE(cf.decodeAttr(const_cast<AttrSlot&>(*ex)).is_ok());
    const auto& exAttr = static_cast<const ExceptionsAttr&>(*ex->body);
    ASSERT_EQ(exAttr.exceptionCps.size(), 1u);
    EXPECT_EQ(cf.className(exAttr.exceptionCps[0]), "java/lang/Throwable");

    // NestHost：单 Class 槽（super 类）。
    const AttrSlot* nh = cf.findClassAttr("NestHost");
    ASSERT_NE(nh, nullptr);
    ASSERT_TRUE(cf.decodeAttr(const_cast<AttrSlot&>(*nh)).is_ok());
    const auto& nhAttr = static_cast<const NestHostAttr&>(*nh->body);
    EXPECT_EQ(cf.className(nhAttr.hostClassCp), "java/lang/Object");

    // NestMembers：两条 Class 槽（内部类 + Throwable）。
    const AttrSlot* nm = cf.findClassAttr("NestMembers");
    ASSERT_NE(nm, nullptr);
    ASSERT_TRUE(cf.decodeAttr(const_cast<AttrSlot&>(*nm)).is_ok());
    const auto& nmAttr = static_cast<const NestMembersAttr&>(*nm->body);
    ASSERT_EQ(nmAttr.memberCps.size(), 2u);
    EXPECT_EQ(cf.className(nmAttr.memberCps[0]), "p/T$In");
    EXPECT_EQ(cf.className(nmAttr.memberCps[1]), "java/lang/Throwable");

    // PermittedSubclasses：一条 Class 槽（p/T 自身，走复合去重代表 cT2）。
    const AttrSlot* ps = cf.findClassAttr("PermittedSubclasses");
    ASSERT_NE(ps, nullptr);
    ASSERT_TRUE(cf.decodeAttr(const_cast<AttrSlot&>(*ps)).is_ok());
    const auto& psAttr = static_cast<const PermittedSubclassesAttr&>(*ps->body);
    ASSERT_EQ(psAttr.permittedCps.size(), 1u);
    EXPECT_EQ(cf.className(psAttr.permittedCps[0]), "p/T");

    // Record：组件两槽 Utf8 重映射 + 嵌套 Signature 递归重映射；组件名与
    // 字段名、组件描述符与签名串两两去重同条目。
    const AttrSlot* rec = cf.findClassAttr("Record");
    ASSERT_NE(rec, nullptr);
    ASSERT_TRUE(cf.decodeAttr(const_cast<AttrSlot&>(*rec)).is_ok());
    const auto& recAttr = static_cast<const RecordAttr&>(*rec->body);
    ASSERT_EQ(recAttr.components.size(), 1u);
    EXPECT_EQ(cf.utf8(recAttr.components[0].nameCp), "value");
    EXPECT_EQ(cf.utf8(recAttr.components[0].descriptorCp), "Ljava/lang/Throwable;");
    ASSERT_EQ(recAttr.components[0].attrs.size(), 1u);
    ASSERT_TRUE(cf.decodeAttr(const_cast<AttrSlot&>(recAttr.components[0].attrs[0])).is_ok());
    const auto& recSig = static_cast<const SignatureAttr&>(*recAttr.components[0].attrs[0].body);
    EXPECT_EQ(cf.utf8(recSig.signatureCp), "Ljava/lang/Throwable;");
    // 去重面：组件描述符与嵌套签名串压缩后同条目。
    EXPECT_EQ(recAttr.components[0].descriptorCp, recSig.signatureCp);

    // ConstantValue：字段上的 Integer 42。ConstantValue 接 typed 分派后为
    // typed 体（Raw 形不可达），直接按 typed 模型读索引。
    ASSERT_EQ(cf.fields.size(), 1u);
    const AttrSlot* cv = ClassFile::findAttrByCp(cf.fields[0].attrs,
                                                 cf.fields[0].attrs[0].nameCp);
    ASSERT_NE(cv, nullptr);
    ASSERT_TRUE(cf.decodeAttr(const_cast<AttrSlot&>(*cv)).is_ok());
    const auto& cvAttr = static_cast<const ConstantValueAttr&>(*cv->body);
    ASSERT_TRUE(cf.cp.valid(cvAttr.constantValueCp));
    EXPECT_EQ(cf.cp.tagOf(cvAttr.constantValueCp), CpTag::Integer);
    EXPECT_EQ(cf.cp.at(cvAttr.constantValueCp).ival, 42);

    // Deprecated：零长体透传，属性保留。
    EXPECT_NE(cf.findClassAttr("Deprecated"), nullptr);
}

TEST(CpCompact, RawResidueRejectedWithAttrName)
{
    jbc::test::ClassBuilder cb;
    const ca::u16 u1 = cb.utf8("p/T");
    const ca::u16 u2 = cb.utf8("java/lang/Object");
    const ca::u16 u3 = cb.utf8("Code");
    const ca::u16 u4 = cb.utf8("<init>");
    const ca::u16 u5 = cb.utf8("()V");
    const ca::u16 c1 = cb.classRef(u1);
    const ca::u16 c2 = cb.classRef(u2);
    const ca::u16 uW = cb.utf8("WeirdCustomAttr");
    cb.setThis(c1).setSuper(c2);
    const Bytes code = {0xB1};
    cb.addMethod(0x0001, u4, u5,
                 {jbc::test::AttrSpec{u3, jbc::test::codeContent(1, 1, code)}});
    cb.addClassAttr(uW, {0x00, 0x01, 0x02});      // 非零长未知属性
    const Bytes bytes = cb.build();

    auto sess = sessionOf(bytes);
    ASSERT_NE(sess, nullptr);
    const auto r = compactConstantPool(*sess);
    ASSERT_TRUE(r.is_err());
    const CompactFailure f = r.unwrap_err();
    EXPECT_EQ(f.code, CompactError::AttrNotModeled);
    EXPECT_EQ(f.attrName, "WeirdCustomAttr");

    // 失败不留半截模型：写回字节与进入前一致，且不记账。
    EXPECT_EQ(jbc::test::writeOk(sess->backing()), bytes);
    EXPECT_EQ(sess->changeSet().size(), 0u);
}

TEST(CpCompact, MalformedKnownRawRejectedWithAttrName)
{
    // SourceFile 体只有 1 字节：五件是定长形状，畸形显式拒绝。
    jbc::test::ClassBuilder cb;
    const ca::u16 u1 = cb.utf8("p/T");
    const ca::u16 u2 = cb.utf8("java/lang/Object");
    const ca::u16 u4 = cb.utf8("<init>");
    const ca::u16 u5 = cb.utf8("()V");
    const ca::u16 c1 = cb.classRef(u1);
    const ca::u16 c2 = cb.classRef(u2);
    const ca::u16 uSF = cb.utf8("SourceFile");
    cb.setThis(c1).setSuper(c2);
    cb.addMethod(0x0001, u4, u5, {});
    cb.addClassAttr(uSF, {0x00});                 // 半截 u2
    const Bytes bytes = cb.build();
    auto sess = sessionOf(bytes);
    ASSERT_NE(sess, nullptr);
    const auto r = compactConstantPool(*sess);
    ASSERT_TRUE(r.is_err());
    const CompactFailure f = r.unwrap_err();
    EXPECT_EQ(f.code, CompactError::MalformedAttr);
    EXPECT_EQ(f.attrName, "SourceFile");

    // InnerClasses 行数声明 2 但只有 1 行：eof 显式拒绝。
    jbc::test::ClassBuilder cb2;
    const ca::u16 v1 = cb2.utf8("p/T");
    const ca::u16 v2 = cb2.utf8("java/lang/Object");
    const ca::u16 v4 = cb2.utf8("<init>");
    const ca::u16 v5 = cb2.utf8("()V");
    const ca::u16 w1 = cb2.classRef(v1);
    const ca::u16 w2 = cb2.classRef(v2);
    const ca::u16 vIC = cb2.utf8("InnerClasses");
    const ca::u16 vIn = cb2.utf8("p/T$In");
    const ca::u16 wIn = cb2.classRef(vIn);
    cb2.setThis(w1).setSuper(w2);
    cb2.addMethod(0x0001, v4, v5, {});
    Bytes ic;
    jbc::test::detail::putU16(ic, 2);             // 声明 2 行
    jbc::test::detail::putU16(ic, wIn);
    jbc::test::detail::putU16(ic, w1);
    jbc::test::detail::putU16(ic, vIn);
    jbc::test::detail::putU16(ic, 0);
    cb2.addClassAttr(vIC, ic);
    const Bytes bytes2 = cb2.build();
    auto sess2 = sessionOf(bytes2);
    ASSERT_NE(sess2, nullptr);
    const auto r2 = compactConstantPool(*sess2);
    ASSERT_TRUE(r2.is_err());
    const CompactFailure f2 = r2.unwrap_err();
    EXPECT_EQ(f2.code, CompactError::MalformedAttr);
    EXPECT_EQ(f2.attrName, "InnerClasses");
}

TEST(CpCompact, SmtObjectPayloadAndTaNonCpFieldsUntouched)
{
    jbc::test::ClassBuilder cb;
    const ca::u16 u1 = cb.utf8("p/T");
    const ca::u16 u2 = cb.utf8("java/lang/Object");
    const ca::u16 u3 = cb.utf8("Code");
    const ca::u16 u4 = cb.utf8("<init>");
    const ca::u16 u5 = cb.utf8("()V");
    const ca::u16 uSmt = cb.utf8("StackMapTable");
    const ca::u16 uTa = cb.utf8("RuntimeVisibleTypeAnnotations");
    const ca::u16 uAnno = cb.utf8("Lp/Anno;");
    const ca::u16 c1 = cb.classRef(u1);
    const ca::u16 c2 = cb.classRef(u2);
    cb.setThis(c1).setSuper(c2).setVersion(52);
    // SMT：1 帧 full_frame，stack = [Object(thisClass)]；Object 载荷须重映射。
    Bytes smt;
    jbc::test::detail::putU16(smt, 1);
    smt.push_back(0xFF);                          // full_frame
    jbc::test::detail::putU16(smt, 0);            // offset
    jbc::test::detail::putU16(smt, 0);            // locals
    jbc::test::detail::putU16(smt, 1);            // stack
    smt.push_back(0x07);                          // Object
    jbc::test::detail::putU16(smt, c1);           // 载荷 = CP Class
    // TA：throws 目标（Exceptions 表行下标，非 CP）= 3；type_index 是 CP。
    Bytes ta;
    jbc::test::detail::putU16(ta, 1);
    ta.push_back(0x17);                           // target_type = throws
    jbc::test::detail::putU16(ta, 3);             // throws_type_index（非 CP！）
    ta.push_back(0x00);                           // type_path 长度
    jbc::test::detail::putU16(ta, uAnno);         // type_index（CP）
    jbc::test::detail::putU16(ta, 0);             // 无元素值
    const Bytes code = {0xB1};
    cb.addMethod(0x0001, u4, u5,
                 {jbc::test::AttrSpec{u3, jbc::test::codeContent(1, 1, code)},
                  jbc::test::AttrSpec{uSmt, smt}});
    cb.addClassAttr(uTa, ta);
    const Bytes bytes = cb.build();

    auto sess = sessionOf(bytes);
    ASSERT_NE(sess, nullptr);
    ASSERT_TRUE(compactConstantPool(*sess).is_ok());
    const Bytes out = jbc::test::writeOk(sess->backing());
    auto rr = readClass(ByteView{out});
    ASSERT_TRUE(rr.is_ok());
    auto rd = std::move(rr).unwrap();
    auto& cf = *rd.file;

    // SMT Object 载荷落到 thisClass 的新索引（SMT 挂方法级，非 Code 嵌套）。
    ASSERT_EQ(cf.methods.size(), 1u);
    ASSERT_EQ(cf.methods[0].attrs.size(), 2u);
    AttrSlot& smtSlot = cf.methods[0].attrs[1];
    ASSERT_TRUE(cf.decodeAttr(smtSlot).is_ok());
    auto& smtAttr = static_cast<StackMapTableAttr&>(*smtSlot.body);
    ASSERT_EQ(smtAttr.frames.size(), 1u);
    ASSERT_EQ(smtAttr.frames[0].stack.size(), 1u);
    EXPECT_EQ(smtAttr.frames[0].stack[0].tag, StackMapTableAttr::VType::kObject);
    EXPECT_EQ(smtAttr.frames[0].stack[0].cp, cf.thisClass);
    EXPECT_EQ(cf.className(cf.thisClass), "p/T");

    // TA 的 throws_type_index 保持 3（非 CP 字段不误重映射），type_index 已重映射。
    const AttrSlot* taSlot = cf.findClassAttr("RuntimeVisibleTypeAnnotations");
    ASSERT_NE(taSlot, nullptr);
    ASSERT_TRUE(cf.decodeAttr(const_cast<AttrSlot&>(*taSlot)).is_ok());
    auto& taAttr = static_cast<TypeAnnotationsAttr&>(*taSlot->body);
    ASSERT_EQ(taAttr.annotations.size(), 1u);
    EXPECT_EQ(taAttr.annotations[0].throwsIndex, 3u);
    EXPECT_EQ(utf8At(cf, taAttr.annotations[0].typeIndexCp), "Lp/Anno;");
}

TEST(CpCompact, TypedAnnotationAndSignatureFacesRemapped)
{
    // 类级 RVA（'@' 嵌套 + 'I'/'e'/'s' 元素值）+ 方法 Signature typed 体。
    jbc::test::ClassBuilder cb;
    const ca::u16 u1 = cb.utf8("p/T");
    const ca::u16 u2 = cb.utf8("java/lang/Object");
    const ca::u16 u3 = cb.utf8("Code");
    const ca::u16 u4 = cb.utf8("<init>");
    const ca::u16 u5 = cb.utf8("()V");
    const ca::u16 uRva = cb.utf8("RuntimeVisibleAnnotations");
    const ca::u16 uAnnoD = cb.utf8("Lp/Anno;");
    const ca::u16 uSig = cb.utf8("Signature");
    const ca::u16 uSigV = cb.utf8("<T:Ljava/lang/Object;>Ljava/lang/Object;");
    const ca::u16 uS = cb.utf8("X");
    const ca::u16 uEnumT = cb.utf8("Lp/Color;");
    const ca::u16 uEnumV = cb.utf8("RED");
    const ca::u16 uNV = cb.utf8("value");
    const ca::u16 uNN = cb.utf8("nested");
    const ca::u16 uNE = cb.utf8("color");
    const ca::u16 uNC = cb.utf8("name");
    const ca::u16 int9 = cb.integer(9);
    const ca::u16 strX = cb.stringConst(uS);
    const ca::u16 c1 = cb.classRef(u1);
    const ca::u16 c2 = cb.classRef(u2);
    cb.setThis(c1).setSuper(c2);

    Bytes rva;
    jbc::test::detail::putU16(rva, 1);            // 1 条注解
    jbc::test::detail::putU16(rva, uAnnoD);       // type
    jbc::test::detail::putU16(rva, 3);            // 3 个元素值
    jbc::test::detail::putU16(rva, uNV);
    rva.push_back(static_cast<ca::u8>('I'));
    jbc::test::detail::putU16(rva, int9);
    jbc::test::detail::putU16(rva, uNE);
    rva.push_back(static_cast<ca::u8>('e'));
    jbc::test::detail::putU16(rva, uEnumT);
    jbc::test::detail::putU16(rva, uEnumV);
    jbc::test::detail::putU16(rva, uNN);
    rva.push_back(static_cast<ca::u8>('@'));
    jbc::test::detail::putU16(rva, uAnnoD);
    jbc::test::detail::putU16(rva, 1);
    jbc::test::detail::putU16(rva, uNC);
    rva.push_back(static_cast<ca::u8>('s'));
    jbc::test::detail::putU16(rva, strX);
    const Bytes code = {0xB1};
    Bytes sigBody;
    jbc::test::detail::putU16(sigBody, uSigV);
    cb.addMethod(0x0001, u4, u5,
                 {jbc::test::AttrSpec{u3, jbc::test::codeContent(1, 1, code)},
                  jbc::test::AttrSpec{uSig, sigBody}});
    cb.addClassAttr(uRva, rva);
    const Bytes bytes = cb.build();

    auto sess = sessionOf(bytes);
    ASSERT_NE(sess, nullptr);
    ASSERT_TRUE(compactConstantPool(*sess).is_ok());
    const Bytes out = jbc::test::writeOk(sess->backing());
    auto rr = readClass(ByteView{out});
    ASSERT_TRUE(rr.is_ok());
    auto rd = std::move(rr).unwrap();
    auto& cf = *rd.file;

    // Signature typed 体：索引重映射后语义恒等。
    ASSERT_EQ(cf.methods.size(), 1u);
    const AttrSlot* sig = ClassFile::findAttrByCp(cf.methods[0].attrs,
                                                  cf.methods[0].attrs[1].nameCp);
    ASSERT_NE(sig, nullptr);
    ASSERT_TRUE(cf.decodeAttr(const_cast<AttrSlot&>(*sig)).is_ok());
    auto& sigAttr = static_cast<SignatureAttr&>(*sig->body);
    EXPECT_EQ(utf8At(cf, sigAttr.signatureCp),
              "<T:Ljava/lang/Object;>Ljava/lang/Object;");

    // RVA：注解体逐元素语义恒等（type/枚举/嵌套/字符串值）。
    const AttrSlot* rvaSlot = cf.findClassAttr("RuntimeVisibleAnnotations");
    ASSERT_NE(rvaSlot, nullptr);
    ASSERT_TRUE(cf.decodeAttr(const_cast<AttrSlot&>(*rvaSlot)).is_ok());
    auto& rvaAttr = static_cast<AnnotationsAttr&>(*rvaSlot->body);
    ASSERT_EQ(rvaAttr.annos.size(), 1u);
    const auto& anno = rvaAttr.annos[0];
    EXPECT_EQ(utf8At(cf, anno.typeCp), "Lp/Anno;");
    ASSERT_EQ(anno.values.size(), 3u);
    EXPECT_EQ(utf8At(cf, anno.values[0].first), "value");
    EXPECT_EQ(anno.values[0].second.tag, 'I');
    EXPECT_EQ(cf.cp.at(anno.values[0].second.cp).ival, 9);
    EXPECT_EQ(utf8At(cf, anno.values[1].second.typeNameCp), "Lp/Color;");
    EXPECT_EQ(utf8At(cf, anno.values[1].second.constNameCp), "RED");
    ASSERT_TRUE(anno.values[2].second.nested != nullptr);
    EXPECT_EQ(utf8At(cf, anno.values[2].second.nested->typeCp), "Lp/Anno;");
    ASSERT_EQ(anno.values[2].second.nested->values.size(), 1u);
    EXPECT_EQ(utf8At(cf, anno.values[2].second.nested->values[0].first), "name");
    const ca::u32 sIdx = anno.values[2].second.nested->values[0].second.cp;
    ASSERT_TRUE(cf.cp.valid(sIdx));
    EXPECT_EQ(cf.cp.tagOf(sIdx), CpTag::String);
    EXPECT_EQ(cf.utf8(cf.cp.at(sIdx).a), "X");
}

TEST(CpCompact, AnnotFamilyCompactFacesRemappedAndDeduped)
{
    // 注解族四件进封闭集：类级 RVA 与方法级 RVPA/AnnotationDefault/
    // MethodParameters/ConstantValue 的 CP 槽全走闭包标记 + 重映射；跨条目
    // 等价内容（同名描述符/元素名/参数名/共享 Integer）按 (tag, content)
    // 并类（与 Module 族先例同口径）。死条目与重复内容驱动索引移动。
    jbc::test::ClassBuilder cb;
    const ca::u16 u1 = cb.utf8("p/T");
    const ca::u16 u2 = cb.utf8("java/lang/Object");
    const ca::u16 uRva = cb.utf8("RuntimeVisibleAnnotations");
    const ca::u16 uRvpa = cb.utf8("RuntimeVisibleParameterAnnotations");
    const ca::u16 uAD = cb.utf8("AnnotationDefault");
    const ca::u16 uMP = cb.utf8("MethodParameters");
    const ca::u16 uCV = cb.utf8("ConstantValue");
    const ca::u16 uAnno  = cb.utf8("Lp/Anno;");
    const ca::u16 uAnnoDup = cb.utf8("Lp/Anno;");       // 内容重复 → 压缩去重
    const ca::u16 uName  = cb.utf8("value");
    const ca::u16 uNameDup = cb.utf8("value");          // 内容重复 → 压缩去重
    const ca::u16 uS = cb.utf8("x");
    const ca::u16 strX = cb.stringConst(uS);
    const ca::u16 uEnumT = cb.utf8("Lp/Color;");
    const ca::u16 uEnumV = cb.utf8("RED");
    const ca::u16 uRun = cb.utf8("run");
    const ca::u16 uRunDup = cb.utf8("run");             // 内容重复 → 压缩去重
    const ca::u16 int42 = cb.integer(42);
    const ca::u16 uM = cb.utf8("m");
    const ca::u16 uMdesc = cb.utf8("(II)I");
    const ca::u16 uV = cb.utf8("v");
    const ca::u16 uVdesc = cb.utf8("I");
    static_cast<void>(cb.utf8("dead-pad"));             // 死条目：驱动索引移动
    static_cast<void>(cb.integer(99));                  // 死条目
    const ca::u16 c1 = cb.classRef(u1);
    const ca::u16 c2 = cb.classRef(u2);
    cb.setThis(c1).setSuper(c2);

    // 字段：ConstantValue → Integer 42。
    Bytes cvBody;
    jbc::test::detail::putU16(cvBody, int42);
    cb.addField(0x0018, uV, uVdesc, {jbc::test::AttrSpec{uCV, cvBody}});

    // 方法：RVPA（参数 0 一条注解 's' 值，参数 1 空组）+ MethodParameters
    // （两行参数名，内容跨条目重复）+ AnnotationDefault（'e' 枚举）。
    Bytes rvpa;
    rvpa.push_back(0x02);                               // num_parameters = 2（u1）
    jbc::test::detail::putU16(rvpa, 1);                 // 参数 0：1 条注解
    jbc::test::detail::putU16(rvpa, uAnnoDup);          // type（与 uAnno 去重面）
    jbc::test::detail::putU16(rvpa, 1);                 // 1 个元素值
    jbc::test::detail::putU16(rvpa, uNameDup);          // 元素名（与 uName 去重面）
    rvpa.push_back(static_cast<ca::u8>('s'));
    jbc::test::detail::putU16(rvpa, strX);
    jbc::test::detail::putU16(rvpa, 0);                 // 参数 1：空组
    Bytes mp;
    mp.push_back(2);                                    // parameters_count = 2（u1）
    jbc::test::detail::putU16(mp, uRun);
    jbc::test::detail::putU16(mp, 0x0010);              // ACC_FINAL
    jbc::test::detail::putU16(mp, uRunDup);
    jbc::test::detail::putU16(mp, 0x0000);
    Bytes ad;
    ad.push_back(static_cast<ca::u8>('e'));
    jbc::test::detail::putU16(ad, uEnumT);
    jbc::test::detail::putU16(ad, uEnumV);
    cb.addMethod(0x0001, uM, uMdesc,
                 {jbc::test::AttrSpec{uRvpa, rvpa},
                  jbc::test::AttrSpec{uMP, mp},
                  jbc::test::AttrSpec{uAD, ad}});

    // 类级 RVA：type = uAnno（与 RVPA 的 uAnnoDup 去重面）、'I' 值 =
    // int42（与 ConstantValue 共享同一 Integer 条目 → 去重面）。
    Bytes rva;
    jbc::test::detail::putU16(rva, 1);
    jbc::test::detail::putU16(rva, uAnno);
    jbc::test::detail::putU16(rva, 1);
    jbc::test::detail::putU16(rva, uName);
    rva.push_back(static_cast<ca::u8>('I'));
    jbc::test::detail::putU16(rva, int42);
    cb.addClassAttr(uRva, rva);
    const Bytes bytes = cb.build();

    auto sess = sessionOf(bytes);
    ASSERT_NE(sess, nullptr);
    const ca::usize before = sess->backing().cp.size();
    ASSERT_TRUE(compactConstantPool(*sess).is_ok());
    const ClassFile& cf = sess->backing();
    EXPECT_LT(cf.cp.size(), before);   // 死条目清除 + 重复并类：池收缩

    const Bytes out = jbc::test::writeOk(cf);
    auto rr = readClass(ByteView{out});
    ASSERT_TRUE(rr.is_ok());
    auto rd = std::move(rr).unwrap();
    auto& cf2 = *rd.file;

    // RVA：type 与 'I' 值重映射后语义恒等；'I' 值与字段 ConstantValue 同槽
    // （跨条目去重）。
    const AttrSlot* rvaSlot = cf2.findClassAttr("RuntimeVisibleAnnotations");
    ASSERT_NE(rvaSlot, nullptr);
    ASSERT_TRUE(cf2.decodeAttr(const_cast<AttrSlot&>(*rvaSlot)).is_ok());
    const auto& rvaAttr = static_cast<const AnnotationsAttr&>(*rvaSlot->body);
    ASSERT_EQ(rvaAttr.annos.size(), 1u);
    EXPECT_EQ(cf2.utf8(rvaAttr.annos[0].typeCp), "Lp/Anno;");
    ASSERT_EQ(rvaAttr.annos[0].values.size(), 1u);
    EXPECT_EQ(cf2.utf8(rvaAttr.annos[0].values[0].first), "value");
    EXPECT_EQ(rvaAttr.annos[0].values[0].second.tag, 'I');

    // 字段 ConstantValue：Integer 42 语义恒等，且与 RVA 的 'I' 值同槽。
    ASSERT_EQ(cf2.fields.size(), 1u);
    const AttrSlot* cvSlot = ClassFile::findAttrByCp(cf2.fields[0].attrs,
                                                     cf2.fields[0].attrs[0].nameCp);
    ASSERT_NE(cvSlot, nullptr);
    ASSERT_TRUE(cf2.decodeAttr(const_cast<AttrSlot&>(*cvSlot)).is_ok());
    const auto& cvAttr = static_cast<const ConstantValueAttr&>(*cvSlot->body);
    EXPECT_EQ(cf2.cp.tagOf(cvAttr.constantValueCp), CpTag::Integer);
    EXPECT_EQ(cf2.cp.at(cvAttr.constantValueCp).ival, 42);
    EXPECT_EQ(rvaAttr.annos[0].values[0].second.cp, cvAttr.constantValueCp);

    // 方法 RVPA：注解 type 与类级 RVA 同槽（uAnno/uAnnoDup 并类）、元素名与
    // RVA 同槽（uName/uNameDup 并类）、's' 值语义恒等、空组保留。
    ASSERT_EQ(cf2.methods.size(), 1u);
    const AttrSlot* rvpaSlot = ClassFile::findAttrByCp(cf2.methods[0].attrs,
                                                       cf2.methods[0].attrs[0].nameCp);
    ASSERT_NE(rvpaSlot, nullptr);
    ASSERT_TRUE(cf2.decodeAttr(const_cast<AttrSlot&>(*rvpaSlot)).is_ok());
    const auto& rvpaAttr = static_cast<const ParameterAnnotationsAttr&>(*rvpaSlot->body);
    ASSERT_EQ(rvpaAttr.parameters.size(), 2u);
    ASSERT_EQ(rvpaAttr.parameters[0].size(), 1u);
    EXPECT_EQ(rvpaAttr.parameters[0][0].typeCp, rvaAttr.annos[0].typeCp);
    ASSERT_EQ(rvpaAttr.parameters[0][0].values.size(), 1u);
    EXPECT_EQ(rvpaAttr.parameters[0][0].values[0].first, rvaAttr.annos[0].values[0].first);
    EXPECT_EQ(rvpaAttr.parameters[0][0].values[0].second.tag, 's');
    const ca::u32 sIdx = rvpaAttr.parameters[0][0].values[0].second.cp;
    ASSERT_TRUE(cf2.cp.valid(sIdx));
    EXPECT_EQ(cf2.cp.tagOf(sIdx), CpTag::String);
    EXPECT_EQ(cf2.utf8(cf2.cp.at(sIdx).a), "x");
    EXPECT_EQ(rvpaAttr.parameters[1].size(), 0u);

    // MethodParameters：两行参数名去重同槽、旗语位原样保留。
    const AttrSlot* mpSlot = ClassFile::findAttrByCp(cf2.methods[0].attrs,
                                                     cf2.methods[0].attrs[1].nameCp);
    ASSERT_NE(mpSlot, nullptr);
    ASSERT_TRUE(cf2.decodeAttr(const_cast<AttrSlot&>(*mpSlot)).is_ok());
    const auto& mpAttr = static_cast<const MethodParametersAttr&>(*mpSlot->body);
    ASSERT_EQ(mpAttr.rows.size(), 2u);
    EXPECT_EQ(cf2.utf8(mpAttr.rows[0].nameCp), "run");
    EXPECT_EQ(mpAttr.rows[0].accessFlags, 0x0010u);
    EXPECT_EQ(cf2.utf8(mpAttr.rows[1].nameCp), "run");
    EXPECT_EQ(mpAttr.rows[1].accessFlags, 0x0000u);
    EXPECT_EQ(mpAttr.rows[0].nameCp, mpAttr.rows[1].nameCp) << "重复参数名应并类";

    // AnnotationDefault：枚举双槽语义恒等。
    const AttrSlot* adSlot = ClassFile::findAttrByCp(cf2.methods[0].attrs,
                                                     cf2.methods[0].attrs[2].nameCp);
    ASSERT_NE(adSlot, nullptr);
    ASSERT_TRUE(cf2.decodeAttr(const_cast<AttrSlot&>(*adSlot)).is_ok());
    const auto& adAttr = static_cast<const AnnotationDefaultAttr&>(*adSlot->body);
    EXPECT_EQ(adAttr.defaultValue.tag, 'e');
    EXPECT_EQ(cf2.utf8(adAttr.defaultValue.typeNameCp), "Lp/Color;");
    EXPECT_EQ(cf2.utf8(adAttr.defaultValue.constNameCp), "RED");

    // 保真：产物重读 → 全解码 → 再写逐字节一致。
    ASSERT_TRUE(decodeAllAttrs(*rd.file).is_ok()) << "decodeAll failed";
    EXPECT_EQ(jbc::test::writeOk(*rd.file), out) << "roundtrip";
}

TEST(CpCompact, PromotedCodeUnitStaysInSync)
{
    // 升格后压缩：CodeUnit 的 cp 与异常表 catchCp 同步重写，lower 写回语义恒等。
    jbc::test::ClassBuilder cb;
    const ca::u16 u1 = cb.utf8("p/T");
    const ca::u16 u2 = cb.utf8("java/lang/Object");
    const ca::u16 u3 = cb.utf8("Code");
    const ca::u16 u4 = cb.utf8("run");
    const ca::u16 u5 = cb.utf8("()V");
    const ca::u16 uThr = cb.utf8("java/lang/Throwable");
    const ca::u16 uExc = cb.utf8("Exceptions");   // 占位（异常表用 catchType，不用该属性）
    const ca::u16 s1 = cb.utf8("pad-content");
    const ca::u16 st1 = cb.stringConst(s1);
    const ca::u16 c1 = cb.classRef(u1);
    const ca::u16 c2 = cb.classRef(u2);
    const ca::u16 cThr = cb.classRef(uThr);
    static_cast<void>(uExc);
    cb.setThis(c1).setSuper(c2);
    // ldc st1 → athrow（catch_type = cThr 的异常表覆盖）→ return...
    // 简化体：ldc + pop + return；异常表 [0,3)->handler=3, catch=cThr。
    const Bytes code = {0x12, static_cast<ca::u8>(st1), 0x57, 0xB1};
    jbc::test::ExcSpec exc{0, 3, 0, cThr};
    cb.addMethod(0x0001, u4, u5,
                 {jbc::test::AttrSpec{u3, jbc::test::codeContent(2, 1, code, {exc})}});
    const Bytes bytes = cb.build();

    auto sess = sessionOf(bytes);
    ASSERT_NE(sess, nullptr);
    auto p = promoteMethod(*sess, 0);
    ASSERT_TRUE(p.is_ok());
    CodeUnit& cu = *p.unwrap();
    ASSERT_EQ(cu.exc.size(), 1u);
    EXPECT_EQ(cu.exc[0].catchCp, cThr);

    // 先在 IR 面追加一个新 String 引用（setString 走语义 intern）。
    {
        const Result<Insn*, EditError> g = cu.dom.get(cu.dom.order()[0]);
        ASSERT_TRUE(g.is_ok());
        ASSERT_TRUE(sess->setString(*g.unwrap(), "moved-content"));
    }

    ASSERT_TRUE(compactConstantPool(*sess).is_ok());

    // 升格体的 cp 已随映射更新（子序映射：活条目 1..6 恒等，7/8/9=三个 Class，
    // DOM 新引用 10:"moved-content" 11:String 落在活面子序尾部；陈旧 backing
    // 面的 "pad-content" 死在压缩里）；异常表 catchCp 同步。
    const Insn* i0 = domInsn(cu, cu.dom.order()[0]);
    ASSERT_EQ(i0->op, Op::LDC);
    EXPECT_EQ(i0->cp, 11u);
    ClassSession::ConstSem c;
    ASSERT_TRUE(sess->constOf(*i0, c));
    EXPECT_EQ(c.kind, ClassSession::ConstSem::Kind::String);
    EXPECT_EQ(c.str, "moved-content");
    EXPECT_EQ(cu.exc[0].catchCp, 9u);
    EXPECT_EQ(sess->backing().className(cu.exc[0].catchCp), "java/lang/Throwable");
    EXPECT_EQ(sess->backing().cp.findUtf8("pad-content"), 0u);   // 旧串已死

    // lower → 写出 → 重读：异常表 catch 类名恒等。
    ASSERT_TRUE(lowerMethod(*sess, 0).is_ok());
    const Bytes out = jbc::test::writeOk(sess->backing());
    auto rr = readClass(ByteView{out});
    ASSERT_TRUE(rr.is_ok());
    auto rd = std::move(rr).unwrap();
    auto& cf = *rd.file;
    ASSERT_EQ(cf.methods.size(), 1u);
    AttrSlot& codeSlot = cf.methods[0].attrs[0];
    ASSERT_TRUE(cf.decodeAttr(codeSlot).is_ok());
    auto& codeAttr = static_cast<CodeAttr&>(*codeSlot.body);
    ASSERT_EQ(codeAttr.excTable.size(), 1u);
    const ca::u32 catchIdx = codeAttr.excTable[0].catchType;
    EXPECT_EQ(cf.className(catchIdx), "java/lang/Throwable");
    // 指令语义：ldc 装载的仍是 "moved-content"。
    auto sess2r = makeSessionFromBytes(ByteView{out});
    ASSERT_TRUE(sess2r.is_ok());
    auto s2 = sess2r.unwrap();
    auto p2 = promoteMethod(*s2, 0);
    ASSERT_TRUE(p2.is_ok());
    const Insn* j0 = domInsn(*p2.unwrap(), p2.unwrap()->dom.order()[0]);
    ClassSession::ConstSem c2sem;
    ASSERT_TRUE(s2->constOf(*j0, c2sem));
    EXPECT_EQ(c2sem.str, "moved-content");
}

TEST(CpCompact, NewClassSeedSessionCompactAndWrite)
{
    // 无源类：全合成模型压缩后照常无源写出（writer 从零分支）。
    NewClassSeed seed;
    seed.thisName = "p/Fresh";
    seed.superName = "java/lang/Object";
    auto r = makeSession(seed);
    ASSERT_TRUE(r.is_ok());
    auto s = std::move(r).unwrap();
    MethodSeed ms;
    ms.name = "go";
    ms.desc = "()V";
    ms.access = 0x0009;
    ms.withCode = true;
    auto am = s->addMethod(ms);
    ASSERT_TRUE(am.is_ok());
    auto p = promoteMethod(*s, 0);
    ASSERT_TRUE(p.is_ok());
    CodeUnit& cu = *p.unwrap();
    Insn ret;
    ret.op = Op::RETURN;
    ASSERT_TRUE(cu.dom.append(ret).is_ok());
    ASSERT_TRUE(lowerMethod(*s, 0).is_ok());
    // 制造膨胀：追加但不引用（压缩应清死）。
    for (int i = 0; i < 64; ++i) {
        static_cast<void>(s->internString("pad-" + std::to_string(i)));
    }
    const ca::usize bloated = s->backing().cp.size();
    EXPECT_GE(bloated, 70u);
    ASSERT_TRUE(compactConstantPool(*s).is_ok());
    EXPECT_LT(s->backing().cp.size(), bloated);

    const Bytes out = jbc::test::writeOk(s->backing());   // 无源写出
    ASSERT_FALSE(out.empty());
    auto rr = readClass(ByteView{out});
    ASSERT_TRUE(rr.is_ok());
    auto rd = std::move(rr).unwrap();
    auto& cf = *rd.file;
    EXPECT_EQ(cf.thisName(), "p/Fresh");
    ASSERT_EQ(cf.methods.size(), 1u);
    EXPECT_EQ(cf.nameOf(cf.methods[0]), "go");
    EXPECT_EQ(cf.cp.findUtf8("pad-0"), 0u);                  // 死条目已清
}

TEST(CpCompact, BloatAndCompactReturnsToCanonicalShape)
{
    // 膨胀（大量 internString 不引用）→ 压缩：产物与直接压缩原类逐字节一致。
    jbc::test::ClassBuilder cb;
    const ca::u16 u1 = cb.utf8("p/T");
    const ca::u16 u2 = cb.utf8("java/lang/Object");
    const ca::u16 u3 = cb.utf8("Code");
    const ca::u16 u4 = cb.utf8("<init>");
    const ca::u16 u5 = cb.utf8("()V");
    const ca::u16 c1 = cb.classRef(u1);
    const ca::u16 c2 = cb.classRef(u2);
    cb.setThis(c1).setSuper(c2);
    const Bytes code = {0xB1};
    cb.addMethod(0x0001, u4, u5,
                 {jbc::test::AttrSpec{u3, jbc::test::codeContent(1, 1, code)}});
    const Bytes bytes = cb.build();

    auto plain = sessionOf(bytes);
    ASSERT_NE(plain, nullptr);
    ASSERT_TRUE(compactConstantPool(*plain).is_ok());
    const Bytes canonical = jbc::test::writeOk(plain->backing());

    auto bloated = sessionOf(bytes);
    ASSERT_NE(bloated, nullptr);
    for (int i = 0; i < 300; ++i) {
        static_cast<void>(bloated->internString("unused-" + std::to_string(i)));
    }
    const ca::usize bloatedSize = bloated->backing().cp.size();
    EXPECT_GE(bloatedSize, 300u);
    ASSERT_TRUE(compactConstantPool(*bloated).is_ok());
    EXPECT_EQ(jbc::test::writeOk(bloated->backing()), canonical);
    EXPECT_LT(bloated->backing().cp.size(), bloatedSize);
}

TEST(CpCompact, SemanticInternWorksAfterCompact)
{
    // 换池后语义 intern 的去重索引已重建：命中既有池不膨胀、miss 追加。
    jbc::test::ClassBuilder cb;
    const ca::u16 u1 = cb.utf8("p/T");
    const ca::u16 u2 = cb.utf8("java/lang/Object");
    const ca::u16 u3 = cb.utf8("Code");
    const ca::u16 u4 = cb.utf8("<init>");
    const ca::u16 u5 = cb.utf8("()V");
    const ca::u16 c1 = cb.classRef(u1);
    const ca::u16 c2 = cb.classRef(u2);
    const ca::u16 clsTarget = cb.utf8("p/Other");
    const ca::u16 clsTargetC = cb.classRef(clsTarget);
    static_cast<void>(clsTargetC);   // 只留 Utf8（无 Class 条目）
    cb.setThis(c1).setSuper(c2);
    const Bytes code = {0xB1};
    cb.addMethod(0x0001, u4, u5,
                 {jbc::test::AttrSpec{u3, jbc::test::codeContent(1, 1, code)}});
    const Bytes bytes = cb.build();

    auto sess = sessionOf(bytes);
    ASSERT_NE(sess, nullptr);
    ASSERT_TRUE(compactConstantPool(*sess).is_ok());
    const ca::usize sizeAfterCompact = sess->backing().cp.size();

    // 命中既有 Utf8：不膨胀。
    const ca::u32 hit = sess->internClass("p/T");
    EXPECT_EQ(sess->backing().cp.size(), sizeAfterCompact);
    EXPECT_TRUE(sess->backing().cp.utf8Equals(sess->backing().cp.at(hit).a, "p/T"));
    // miss：追加。
    const ca::u32 fresh = sess->internClass("p/Other");
    EXPECT_GT(sess->backing().cp.size(), sizeAfterCompact);
    EXPECT_TRUE(sess->backing().cp.utf8Equals(sess->backing().cp.at(fresh).a, "p/Other"));
}

}   // namespace
