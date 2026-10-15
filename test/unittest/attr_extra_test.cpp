/// @file        attr_extra_test.cpp
/// @brief       v0.2 属性建模扩展测试：BootstrapMethods / LineNumberTable /
///        StackMapTable / 注解 的解码形状与重编码精确性。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "jbc/jbc.hpp"
#include "corpus_util.hpp"
#include "test_class_bytes.hpp"

using namespace jbc::core;
using Bytes = std::vector<ca::u8>;
using jbc::test::AttrSpec;
using jbc::test::ClassBuilder;

namespace {

/// 最小 class：一个 <init>()V（code 只有 return），顶层与方法 Code 内各挂一份
/// 指定属性的副本。
Bytes buildWithAttr(const std::string& name, const Bytes& body)
{
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 codeIdx = cb.utf8("Code");
    const ca::u16 attrIdx = cb.utf8(name);
    const ca::u16 initIdx = cb.utf8("<init>");
    const ca::u16 voidIdx = cb.utf8("()V");

    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setAccess(0x0021);
    cb.setVersion(52);

    const Bytes code = {0xb1};   // return（字节内容对 reader 不透明）
    std::vector<AttrSpec> methodAttrs;
    methodAttrs.push_back(AttrSpec{codeIdx, jbc::test::codeContent(0, 0, code, {}, {{attrIdx, body}})});
    cb.addMethod(0x0001, initIdx, voidIdx, std::move(methodAttrs));
    cb.addClassAttr(attrIdx, body);
    return cb.build();
}

std::vector<std::string> collectClassFiles(const std::string& mainRoot)
{
    namespace fs = std::filesystem;
    // 主语料 = 语料解析序产物（env > 仓内自产，F1）；
    // 固定语料 = test_resource 里的注解样本（cfr 提取，含类/方法级注解与
    // lambda 的 BootstrapMethods），保证注解路径不依赖构建产物存在。
    std::vector<fs::path> roots{fs::path(mainRoot), fs::path("test_resource/libjbc/annotated")};
    std::vector<std::string> out;
    std::error_code ec;
    for (const fs::path& root : roots) {
        for (fs::recursive_directory_iterator it(root, ec), end; it != end && !ec;
             it.increment(ec)) {
            if (!ec && it->is_regular_file() && it->path().extension() == ".class") {
                out.push_back(it->path().string());
            }
        }
        ec.clear();
    }
    return out;
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

TEST(AttrExtraTest, BootstrapMethodsDecodeAndReencode)
{
    // 1 条目 methodRef=#5，2 参数(#6,#7)
    const Bytes body = {0x00, 0x01, 0x00, 0x05, 0x00, 0x02, 0x00, 0x06, 0x00, 0x07};
    const Bytes cls  = buildWithAttr("BootstrapMethods", body);

    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    auto d = cf->decodeAttr(cf->attrs[0]);
    ASSERT_TRUE(d.is_ok());
    const auto& bs = static_cast<const BootstrapMethodsAttr&>(*d.unwrap());
    ASSERT_EQ(bs.entries.size(), 1u);
    EXPECT_EQ(bs.entries[0].methodRef, 5u);
    EXPECT_EQ(bs.entries[0].args, (std::vector<ca::u32>{6, 7}));

    (void)cf->attrs[0].mutate();
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

/// B6 冒烟：BootstrapMethods 参数改写 → 写出 → 重读一致（checklist B6）。
///
/// javac lambda 形态的最小载体：BSM 条目（methodRef = MethodHandle，
/// args = [String 常量]）+ invokedynamic 指令（CP InvokeDynamic 条目）。
/// 改写走既有通道：session intern 新 String 常量（CP 只追加）→
/// AttrSlot::mutate 改 BSM 条目参数（唯一置脏入口）→ writeClass 重编码 →
/// 重读断言新参数落位、methodRef 原样、indy 调用点语义不受牵连。
TEST(AttrExtraTest, BootstrapMethodsArgRewriteRoundTrips)
{
    ClassBuilder cb;
    const ca::u16 thisIdx       = cb.utf8("p/L");
    const ca::u16 superIdx      = cb.utf8("java/lang/Object");
    const ca::u16 codeIdx       = cb.utf8("Code");
    const ca::u16 mIdx          = cb.utf8("m");
    const ca::u16 voidIdx       = cb.utf8("()V");
    const ca::u16 bsmNameIdx    = cb.utf8("BootstrapMethods");
    const ca::u16 applyNameIdx  = cb.utf8("apply");
    const ca::u16 csDescIdx     = cb.utf8("()Ljava/lang/invoke/CallSite;");
    const ca::u16 bsmDescIdx    = cb.utf8("(Ljava/lang/invoke/MethodHandles$Lookup;"
                                          "Ljava/lang/String;Ljava/lang/invoke/MethodType;)"
                                          "Ljava/lang/invoke/CallSite;");
    const ca::u16 bsmRef        = cb.methodref(cb.classRef(cb.utf8("p/B")),
                                               cb.nameAndType(cb.utf8("bootstrap"), bsmDescIdx));
    const ca::u16 mh            = cb.methodHandle(6, bsmRef);   // REF_invokeStatic
    const ca::u16 oldStr        = cb.stringConst(cb.utf8("oldTarget"));
    const ca::u16 indy          = cb.invokeDynamic(0, cb.nameAndType(applyNameIdx, csDescIdx));

    // BSM 属性体：1 条目 {methodRef=MH, args=[oldStr]}。
    Bytes bsmBody;
    jbc::test::detail::putU16(bsmBody, 1);
    jbc::test::detail::putU16(bsmBody, mh);
    jbc::test::detail::putU16(bsmBody, 1);
    jbc::test::detail::putU16(bsmBody, oldStr);

    // 0 invokedynamic #indy / 5 pop / 6 return。
    Bytes code;
    code.push_back(0xBA);
    jbc::test::detail::putU16(code, indy);
    code.push_back(0x00);
    code.push_back(0x00);
    code.push_back(0x57);
    code.push_back(0xb1);

    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setAccess(0x0021);
    cb.setVersion(52);
    std::vector<AttrSpec> ma;
    ma.push_back(AttrSpec{codeIdx, jbc::test::codeContent(1, 0, code)});
    cb.addMethod(0x0009, mIdx, voidIdx, std::move(ma));
    cb.addClassAttr(bsmNameIdx, std::move(bsmBody));
    const Bytes cls = cb.build();

    auto r = jbc::ir::makeSessionFromBytes(ByteView{cls.data(), cls.size()});
    ASSERT_TRUE(r.is_ok());
    auto s = std::move(r).unwrap();

    // 读侧前提（材料面）：indy 调用点 memberOf 命中 InvokeDynamic 族。
    {
        auto p = jbc::ir::promoteMethod(*s, 0);
        ASSERT_TRUE(p.is_ok());
        ASSERT_GT(p.unwrap()->dom.size(), 0u);
        const jbc::ir::Insn* in = p.unwrap()->dom.peek(p.unwrap()->dom.order()[0]);
        ASSERT_NE(in, nullptr);
        EXPECT_EQ(in->op, Op::INVOKEDYNAMIC);
        jbc::ir::ClassSession::MemberSem sem;
        ASSERT_TRUE(s->memberOf(*in, sem));
        EXPECT_EQ(sem.kind, jbc::ir::ClassSession::MemberSem::Kind::InvokeDynamic);
        EXPECT_EQ(sem.name, "apply");
    }

    // 改写：args[0] 支到新 intern 的 String 常量（CP 只追加，索引稳定）。
    const ca::u32 newConst = s->internString("newTarget");
    ASSERT_NE(newConst, 0u);
    AttrSlot* bsmSlot = nullptr;
    for (AttrSlot& slot : s->backing().attrs) {
        auto d = s->backing().decodeAttr(slot);
        ASSERT_TRUE(d.is_ok());
        if (slot.peek() != nullptr && slot.peek()->kind() == AttrKind::BootstrapMethods) {
            bsmSlot = &slot;
        }
    }
    ASSERT_NE(bsmSlot, nullptr);
    BootstrapMethodsAttr* bs = static_cast<BootstrapMethodsAttr*>(bsmSlot->mutate());
    ASSERT_EQ(bs->entries.size(), 1u);
    ASSERT_EQ(bs->entries[0].args.size(), 1u);
    EXPECT_EQ(bs->entries[0].methodRef, mh);
    bs->entries[0].args[0] = newConst;

    const Bytes out = jbc::test::writeOk(s->backing());
    ASSERT_FALSE(out.empty());

    // 重读：新参数落位，methodRef 与调用点语义原样。
    auto r2 = jbc::ir::makeSessionFromBytes(ByteView{out.data(), out.size()});
    ASSERT_TRUE(r2.is_ok());
    auto s2 = std::move(r2).unwrap();
    const BootstrapMethodsAttr* bs2 = nullptr;
    for (AttrSlot& slot : s2->backing().attrs) {
        auto d = s2->backing().decodeAttr(slot);
        ASSERT_TRUE(d.is_ok());
        if (slot.peek() != nullptr && slot.peek()->kind() == AttrKind::BootstrapMethods) {
            bs2 = static_cast<const BootstrapMethodsAttr*>(slot.peek());
        }
    }
    ASSERT_NE(bs2, nullptr);
    ASSERT_EQ(bs2->entries.size(), 1u);
    EXPECT_EQ(bs2->entries[0].methodRef, mh);
    ASSERT_EQ(bs2->entries[0].args.size(), 1u);
    EXPECT_EQ(bs2->entries[0].args[0], newConst);
    // 新参数确为新 String 常量（内容校验，防「索引对但指错条目」）。
    EXPECT_EQ(s2->backing().cp.tagOf(newConst), CpTag::String);
    const ca::u16 utf8OfNew = static_cast<ca::u16>(s2->backing().cp.at(newConst).a);
    EXPECT_EQ(s2->backing().cp.utf8Of(utf8OfNew), "newTarget");

    // indy 调用点在改写后的类上语义不变。
    {
        auto p2 = jbc::ir::promoteMethod(*s2, 0);
        ASSERT_TRUE(p2.is_ok());
        const jbc::ir::Insn* in2 = p2.unwrap()->dom.peek(p2.unwrap()->dom.order()[0]);
        ASSERT_NE(in2, nullptr);
        jbc::ir::ClassSession::MemberSem sem2;
        ASSERT_TRUE(s2->memberOf(*in2, sem2));
        EXPECT_EQ(sem2.kind, jbc::ir::ClassSession::MemberSem::Kind::InvokeDynamic);
        EXPECT_EQ(sem2.name, "apply");
    }
}

TEST(AttrExtraTest, LineNumberTableDecodeAndReencode)
{
    const Bytes body = {0x00, 0x02, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x05, 0x00, 0x14};
    const Bytes cls  = buildWithAttr("LineNumberTable", body);

    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    auto dc = cf->decodeAttr(cf->methods[0].attrs[0]);
    ASSERT_TRUE(dc.is_ok());
    auto& code = static_cast<CodeAttr&>(*dc.unwrap());
    AttrSlot& lnt = code.attrs[0];
    auto d = cf->decodeAttr(lnt);
    ASSERT_TRUE(d.is_ok());
    const auto& t = static_cast<const LineNumberTableAttr&>(*d.unwrap());
    ASSERT_EQ(t.rows.size(), 2u);
    EXPECT_EQ(t.rows[0].startPc, 0u);
    EXPECT_EQ(t.rows[0].line, 10u);
    EXPECT_EQ(t.rows[1].startPc, 5u);
    EXPECT_EQ(t.rows[1].line, 20u);

    (void)lnt.mutate();
    (void)cf->methods[0].attrs[0].mutate();
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

TEST(AttrExtraTest, StackMapTableFrameShapes)
{
    // 覆盖：same(≤63) / same_locals_1_stack(64..127) / chop(248) /
    // same_ext(251) / append(252) / full(255)
    const Bytes body = {
        0x00, 0x06,                               // number_of_entries（u2）= 6
        0x05,                                     // same_frame offset=5
        0x41, 0x01,                               // s_l_1_stack: offset=1, Top
        0xf8, 0x00, 0x07,                         // chop_3: delta=7
        0xfb, 0x00, 0x09,                         // same_frame_extended: delta=9
        0xfc, 0x00, 0x0a, 0x07, 0x00, 0x20,       // append_1: delta=10, Object#32
        0xff, 0x00, 0x0c, 0x00, 0x02, 0x01, 0x06, // full: offset=12, locals={Int,Null}
        0x00, 0x01, 0x08, 0x00, 0x1f,             //   stack={Uninit offset=31}
    };
    const Bytes cls = buildWithAttr("StackMapTable", body);

    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    auto dc = cf->decodeAttr(cf->methods[0].attrs[0]);
    ASSERT_TRUE(dc.is_ok());
    auto& code = static_cast<CodeAttr&>(*dc.unwrap());
    AttrSlot& sm = code.attrs[0];
    auto d = cf->decodeAttr(sm);
    ASSERT_TRUE(d.is_ok());
    const auto& t = static_cast<const StackMapTableAttr&>(*d.unwrap());
    ASSERT_EQ(t.frames.size(), 6u);

    EXPECT_EQ(t.frames[0].type, 0x05);
    EXPECT_EQ(t.frames[1].type, 0x41);
    ASSERT_EQ(t.frames[1].stack.size(), 1u);
    EXPECT_EQ(t.frames[1].stack[0].tag, 0x01);
    EXPECT_EQ(t.frames[2].type, 0xf8);
    EXPECT_EQ(t.frames[2].offset, 7u);
    EXPECT_EQ(t.frames[3].offset, 9u);
    ASSERT_EQ(t.frames[4].locals.size(), 1u);
    EXPECT_EQ(t.frames[4].locals[0].tag, 0x07);
    EXPECT_EQ(t.frames[4].locals[0].cp, 0x20u);
    ASSERT_EQ(t.frames[5].locals.size(), 2u);
    ASSERT_EQ(t.frames[5].stack.size(), 1u);
    EXPECT_EQ(t.frames[5].stack[0].tag, 0x08);
    EXPECT_EQ(t.frames[5].stack[0].cp, 0x1fu);

    (void)sm.mutate();
    (void)cf->methods[0].attrs[0].mutate();
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

TEST(AttrExtraTest, AnnotationsNestedShapes)
{
    // @A(name="x", val=enum E.X, sub=@B, arr=[1, 2])
    const Bytes body = {
        0x00, 0x01,                          // 1 个注解
        0x00, 0x10,                          // typeCp=#16
        0x00, 0x04,                          // 4 个值
        0x00, 0x20, 's', 0x00, 0x30,         // name → "s" 常量 #48
        0x00, 0x21, 'e', 0x00, 0x11, 0x00, 0x12,
        0x00, 0x22, '@', 0x00, 0x13, 0x00, 0x00,
        0x00, 0x23, '[', 0x00, 0x02, 'I', 0x00, 0x31, 'I', 0x00, 0x32,
    };
    const Bytes cls = buildWithAttr("RuntimeVisibleAnnotations", body);

    auto cf = jbc::test::readOk(cls);
    ASSERT_NE(cf, nullptr);
    auto d = cf->decodeAttr(cf->attrs[0]);
    ASSERT_TRUE(d.is_ok());
    const auto& a = static_cast<const AnnotationsAttr&>(*d.unwrap());
    ASSERT_EQ(a.annos.size(), 1u);
    ASSERT_EQ(a.annos[0].values.size(), 4u);

    EXPECT_EQ(a.annos[0].values[0].second.tag, 's');
    EXPECT_EQ(a.annos[0].values[0].second.cp, 0x30u);
    EXPECT_EQ(a.annos[0].values[1].second.tag, 'e');
    EXPECT_EQ(a.annos[0].values[1].second.typeNameCp, 0x11u);
    EXPECT_EQ(a.annos[0].values[2].second.tag, '@');
    ASSERT_NE(a.annos[0].values[2].second.nested, nullptr);
    EXPECT_EQ(a.annos[0].values[2].second.nested->typeCp, 0x13u);
    EXPECT_EQ(a.annos[0].values[3].second.tag, '[');
    ASSERT_EQ(a.annos[0].values[3].second.items.size(), 2u);

    (void)cf->attrs[0].mutate();
    EXPECT_EQ(jbc::test::writeOk(*cf), cls);
}

namespace {

/// 构造带指定属性（类级 + Code 嵌套）的类并解码类级槽；失败返回空。
std::unique_ptr<ClassFile> classWithDecodedAttr(const std::string& name, const Bytes& body)
{
    auto cf = jbc::test::readOk(buildWithAttr(name, body));
    if (cf == nullptr) {
        return nullptr;
    }
    if (cf->decodeAttr(cf->attrs[0]).is_err()) {
        return nullptr;
    }
    return cf;
}

}   // namespace

/// 属性体 trailing（design.md §27.1）：声明结构读完后的多余字节在「未改
/// 往返」与「置脏重编码」两条路径下都必须原样保留——声明 length 比结构
/// 多 4 字节，静默丢尾即保真违约。五个已知属性 decoder 同治。
TEST(AttrExtraTest, AttrBodyTrailingSurvivesReencode)
{
    struct Case
    {
        const char* name;
        Bytes body;
    };
    // 各属性的最小合法声明结构（1 条目/1 行/1 frame/1 注解）。
    const std::vector<Case> cases = {
        {"BootstrapMethods", {0x00, 0x01, 0x00, 0x05, 0x00, 0x00}},
        {"LineNumberTable", {0x00, 0x01, 0x00, 0x00, 0x00, 0x0a}},
        {"LocalVariableTable",
         {0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x10, 0x00, 0x20, 0x00, 0x00}},
        {"StackMapTable", {0x00, 0x01, 0x05}},
        {"RuntimeVisibleAnnotations", {0x00, 0x01, 0x00, 0x10, 0x00, 0x00}},
    };
    const Bytes tail = {0xDE, 0xAD, 0xBE, 0xEF};

    for (const Case& c : cases) {
        Bytes body = c.body;
        body.insert(body.end(), tail.begin(), tail.end());   // 声明 length 多 4 字节
        const Bytes cls = buildWithAttr(c.name, body);

        auto cf = classWithDecodedAttr(c.name, body);
        ASSERT_NE(cf, nullptr) << c.name;

        // 未修改往返（Decoded 态走 copy）：字节一致。
        EXPECT_EQ(jbc::test::writeOk(*cf), cls) << c.name;

        // 置脏触发重编码：结构输出后 trailing 原样追加，字节仍一致。
        ASSERT_NE(cf->attrs[0].mutate(), nullptr) << c.name;
        EXPECT_EQ(jbc::test::writeOk(*cf), cls) << c.name << " re-encode dropped trailing";
    }
}

/// 编码闸门（design.md §27.2）：计数/索引超 u16 必须 Err(Unencodable)，
/// 不得 static_cast 静默截断——截断产出计数错位 / 引用错条目的 class。
TEST(AttrExtraTest, EncodersRejectOutOfRangeFields)
{
    auto expectUnencodable = [](std::unique_ptr<ClassFile> cf) {
        ASSERT_NE(cf, nullptr);
        const Result<Bytes, WriteError> r = writeClass(*cf);
        ASSERT_TRUE(r.is_err());
        EXPECT_EQ(r.unwrap_err(), WriteError::Unencodable);
    };

    // LNT：行字段超 u16。
    {
        auto cf = classWithDecodedAttr("LineNumberTable", {0x00, 0x01, 0x00, 0x00, 0x00, 0x0a});
        ASSERT_NE(cf, nullptr);
        static_cast<LineNumberTableAttr*>(cf->attrs[0].mutate())->rows[0].startPc = 0x10000;
        expectUnencodable(std::move(cf));
    }
    // BootstrapMethods：methodRef 超 u16。
    {
        auto cf = classWithDecodedAttr("BootstrapMethods", {0x00, 0x01, 0x00, 0x05, 0x00, 0x00});
        ASSERT_NE(cf, nullptr);
        static_cast<BootstrapMethodsAttr*>(cf->attrs[0].mutate())->entries[0].methodRef = 0x10000;
        expectUnencodable(std::move(cf));
    }
    // LVT：行字段超 u16。
    {
        auto cf = classWithDecodedAttr(
            "LocalVariableTable",
            {0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x10, 0x00, 0x20, 0x00, 0x00});
        ASSERT_NE(cf, nullptr);
        static_cast<LocalVariableTableAttr*>(cf->attrs[0].mutate())->rows[0].descCp = 0x10000;
        expectUnencodable(std::move(cf));
    }
    // SMT：extended/chop/full 的 offset 超 u16。
    {
        auto cf = classWithDecodedAttr("StackMapTable", {0x00, 0x01, 0xf8, 0x00, 0x07});
        ASSERT_NE(cf, nullptr);
        static_cast<StackMapTableAttr*>(cf->attrs[0].mutate())->frames[0].offset = 0x10000;
        expectUnencodable(std::move(cf));
    }
    // 注解：element 值对数超 u16。
    {
        auto cf = classWithDecodedAttr("RuntimeVisibleAnnotations",
                                       {0x00, 0x01, 0x00, 0x10, 0x00, 0x00});
        ASSERT_NE(cf, nullptr);
        static_cast<AnnotationsAttr*>(cf->attrs[0].mutate())->annos[0].values.resize(0x10000);
        expectUnencodable(std::move(cf));
    }
}

/// 真实类：全量解码（含新属性）→ 逐槽置脏 → 写回与源字节一致。
/// 「编码器是解码的精确逆」的全语料验收。
TEST(AttrExtraRealClassTest, DecodeAllMutateAllRewriteIdentical)
{
    const jbc::test::corpus::CorpusSource src = jbc::test::corpus::resolveCorpus();
    if (src.stage == jbc::test::corpus::Stage::None) {
        GTEST_SKIP() << jbc::test::corpus::skipReason(src);
    }
    const std::vector<std::string> files = collectClassFiles(src.dir);
    if (files.empty()) {
        GTEST_SKIP() << jbc::test::corpus::skipReason(src);
    }

    ca::usize seen[16] = {};  // 按 AttrKind 值计数（容量随新建模属性增长；B3 四件后最大 kind=13）
    auto dirtyAll = [&](std::vector<AttrSlot>& slots) {
        for (AttrSlot& s : slots) {
            if (s.decoded()) {
                ++seen[static_cast<int>(s.peek()->kind())];
                (void)s.mutate();
            }
        }
    };
    for (const std::string& path : files) {
        const Bytes bytes = slurpFile(path);
        ASSERT_FALSE(bytes.empty()) << path;

        auto r = readClass(ByteView{bytes});
        ASSERT_TRUE(r.is_ok()) << path;
        auto cf = std::move(r).unwrap().file;

        ASSERT_TRUE(decodeAllAttrs(*cf).is_ok()) << path;
        dirtyAll(cf->attrs);
        for (MemberInfo& m : cf->fields) {
            dirtyAll(m.attrs);
        }
        for (MemberInfo& m : cf->methods) {
            dirtyAll(m.attrs);
            for (AttrSlot& s : m.attrs) {
                if (s.decoded() && s.peek()->kind() == AttrKind::Code) {
                    dirtyAll(static_cast<CodeAttr&>(*s.body).attrs);
                }
            }
        }

        EXPECT_EQ(jbc::test::writeOk(*cf), bytes) << path << " re-encode mismatch";
    }
    // 真实语料必须真的覆盖到新属性（否则本测试在测空气）。
    EXPECT_GT(seen[static_cast<int>(AttrKind::LineNumberTable)], 0u);
    EXPECT_GT(seen[static_cast<int>(AttrKind::StackMapTable)], 0u);
    EXPECT_GT(seen[static_cast<int>(AttrKind::Annotations)], 0u);
    EXPECT_GT(seen[static_cast<int>(AttrKind::LocalVariableTable)], 0u);
}
