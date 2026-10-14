/// @file        signature_corpus_test.cpp
/// @brief       泛型签名语料门：真实 javac 产物的全部 Signature 属性
///        parse→unparse 逐字节恒等（自产 jbcx.corpus / env 注入 private-corpus 双模式），
///        并给出类数/签名数/形态分布统计。
///
/// 语料级手术验证：每条签名里出现的每个去重内部名做正向 rename → 重 parse
/// 恒等 → 逆向 rename 逐字节还原原串（手术后写回→重读→再 parse 恒等的
/// 语料级口径）。
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "jbc/jbc.hpp"
#include "corpus_util.hpp"
#include "test_class_bytes.hpp"

using namespace jbc::core;
using namespace jbc::type;

using Bytes = std::vector<ca::u8>;

namespace {

struct SigStats
{
    ca::u32 classes = 0;
    ca::u32 class_form = 0;
    ca::u32 method_form = 0;
    ca::u32 field_form = 0;
    ca::u32 wildcards = 0;
    ca::u32 type_params = 0;
    ca::u32 throws_clauses = 0;
    ca::u32 inner_chains = 0;   ///< 段数 > 1 的类类型（'.' 后缀链）
    ca::u32 array_types = 0;
    ca::u32 class_names = 0;
    ca::u32 renamed = 0;
};

/// @brief 遍历统计一个类型的形状（通配符/数组/类名/段链）。
void statType(const SigType& t, SigStats& st)
{
    if (t.array_dims > 0) {
        ++st.array_types;
    }
    if (t.kind != SigType::Kind::Class) {
        return;
    }
    if (t.class_type.segments.size() > 1) {
        ++st.inner_chains;
    }
    st.class_names += static_cast<ca::u32>(t.class_type.segments.size());
    for (const SigClassSegment& seg : t.class_type.segments) {
        for (const SigTypeArg& arg : seg.type_args) {
            if (arg.kind == SigTypeArg::Kind::Unbounded) {
                ++st.wildcards;
            }
            if (arg.bound != nullptr) {
                statType(*arg.bound, st);
            }
        }
    }
}

void statParams(const std::vector<SigTypeParam>& tps, SigStats& st)
{
    st.type_params += static_cast<ca::u32>(tps.size());
    for (const SigTypeParam& tp : tps) {
        if (tp.class_bound != nullptr) {
            statType(*tp.class_bound, st);
        }
        for (const SigType& ib : tp.interface_bounds) {
            statType(ib, st);
        }
    }
}

/// @brief 从一个类文件收集全部 Signature 属性（类/方法/字段三级 + 形态）。
void visitSignatures(ClassFile& cf, const std::string& path,
                     const std::function<void(std::string_view, SignatureForm)>& fn)
{
    auto visit = [&](std::vector<AttrSlot>& attrs, SignatureForm form) {
        for (AttrSlot& slot : attrs) {
            if (!cf.cp.utf8Equals(slot.nameCp, "Signature")) {
                continue;
            }
            auto d = cf.decodeAttr(slot);
            if (d.is_err()) {
                ADD_FAILURE() << path << ": Signature attr decode failed";
                continue;
            }
            AttrBody* body = std::move(d).unwrap();
            if (body->kind() != AttrKind::Raw) {
                ADD_FAILURE() << path << ": Signature attr not Raw (unexpected model)";
                continue;
            }
            const std::vector<ca::u8>& bytes = static_cast<RawAttr*>(body)->bytes();
            if (bytes.size() != 2) {
                ADD_FAILURE() << path << ": Signature attr body != 2 bytes";
                continue;
            }
            const ca::u32 idx = (ca::u32(bytes[0]) << 8) | ca::u32(bytes[1]);
            fn(cf.utf8(idx), form);
        }
    };
    visit(cf.attrs, SignatureForm::Class);
    for (MemberInfo& m : cf.methods) {
        visit(m.attrs, SignatureForm::Method);
    }
    for (MemberInfo& f : cf.fields) {
        visit(f.attrs, SignatureForm::Field);
    }
}

Bytes slurp(const std::string& path)
{
    std::ifstream in(path.c_str(), std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}   // namespace

TEST(SignatureCorpus, RoundtripAndRename)
{
    const auto src = jbc::test::corpus::resolveCorpus();
    if (src.stage == jbc::test::corpus::Stage::None) {
        GTEST_SKIP() << jbc::test::corpus::skipReason(src);
    }
    const std::vector<std::string> files = jbc::test::corpus::collectClassFiles(src.dir);
    ASSERT_GT(files.size(), ca::usize{10}) << "corpus too small: " << src.dir;

    SigStats st;
    for (const std::string& path : files) {
        const Bytes bytes = slurp(path);
        ASSERT_FALSE(bytes.empty()) << path;
        auto r = readClass(ByteView{bytes});
        ASSERT_TRUE(r.is_ok()) << path << " read failed";
        auto cf = std::move(r).unwrap().file;

        visitSignatures(*cf, path, [&](std::string_view sig, SignatureForm form) {
            switch (form) {
            case SignatureForm::Class: ++st.class_form; break;
            case SignatureForm::Method: ++st.method_form; break;
            case SignatureForm::Field: ++st.field_form; break;
            }

            // 1) 解析 + roundtrip 逐字节恒等（合法输入零容忍）。
            auto parsed = parseSignature(sig);
            ASSERT_TRUE(parsed.is_ok())
                << path << ": " << sig << " -> "
                << (parsed.is_err() ? to_string(std::move(parsed).unwrap_err().code) : "ok");
            const Signature tree = std::move(parsed).unwrap();
            switch (tree.form) {
            case SignatureForm::Class:
                statParams(tree.class_sig.type_params, st);
                statType(tree.class_sig.superclass, st);
                for (const SigType& itf : tree.class_sig.super_interfaces) {
                    statType(itf, st);
                }
                break;
            case SignatureForm::Method:
                statParams(tree.method_sig.type_params, st);
                for (const SigType& p : tree.method_sig.params) {
                    statType(p, st);
                }
                if (!tree.method_sig.ret_void) {
                    statType(tree.method_sig.ret, st);
                }
                st.throws_clauses += static_cast<ca::u32>(tree.method_sig.throws_list.size());
                for (const SigType& t : tree.method_sig.throws_list) {
                    statType(t, st);
                }
                break;
            case SignatureForm::Field:
                statType(tree.field_sig, st);
                break;
            }
            const ca::str::Utf8String out = unparseSignature(tree);
            ASSERT_EQ(std::string_view(out.c_str(), out.byte_length()), sig)
                << path << ": roundtrip mismatch";

            // 2) 语料级手术：签名里每个去重内部名（首段全名 span 文本）正向
            //    rename → 重 parse 恒等 → 逆向 rename 还原原串。
            std::vector<SigClassNameSpan> spans;
            collectClassNameSpans(tree, sig, spans);
            std::vector<std::string_view> names;
            for (const SigClassNameSpan& span : spans) {
                if (span.kind != SigClassNameSpan::Kind::InternalName) {
                    continue;
                }
                const std::string_view name = sig.substr(span.offset, span.length);
                if (std::find(names.begin(), names.end(), name) == names.end()) {
                    names.push_back(name);
                }
            }
            for (const std::string_view name : names) {
                std::string renamed(name);
                renamed += "_R";
                auto fwd = renameClassInSignature(sig, form, name, renamed);
                ASSERT_TRUE(fwd.is_ok()) << path << ": " << sig << " rename " << name;
                const SigRenameResult res = std::move(fwd).unwrap();
                ASSERT_GE(res.hits, ca::u32{1})
                    << path << ": " << sig << " rename miss " << name;
                ++st.renamed;
                const std::string_view out_sv{
                    res.signature.c_str(), res.signature.byte_length()};
                auto reparsed = parseSignature(out_sv);
                ASSERT_TRUE(reparsed.is_ok())
                    << path << ": renamed signature failed to re-parse";
                auto back = renameClassInSignature(out_sv, form, renamed, name);
                ASSERT_TRUE(back.is_ok()) << path << ": reverse rename failed";
                const SigRenameResult rev = std::move(back).unwrap();
                ASSERT_EQ(std::string_view(rev.signature.c_str(), rev.signature.byte_length()),
                          sig)
                    << path << ": reverse rename mismatch";
            }
        });
        ++st.classes;
    }

    // 统计落盘 + 断言：语料门必须真有泛型，三类形态齐备。
    std::printf(
        "[signature-corpus] stage=%d classes=%u sigs=%u (class=%u method=%u field=%u) "
        "wildcards=%u typeParams=%u throws=%u innerChains=%u arrayTypes=%u "
        "classNames=%u renamed=%u\n",
        static_cast<int>(src.stage), st.classes,
        st.class_form + st.method_form + st.field_form, st.class_form, st.method_form,
        st.field_form, st.wildcards, st.type_params, st.throws_clauses, st.inner_chains,
        st.array_types, st.class_names, st.renamed);

    EXPECT_GT(st.class_form + st.method_form + st.field_form, 0u)
        << "corpus has no Signature attributes";
    EXPECT_GT(st.class_form, 0u);
    EXPECT_GT(st.method_form, 0u);
    EXPECT_GT(st.field_form, 0u);
    EXPECT_GT(st.wildcards, 0u);
    EXPECT_GT(st.type_params, 0u);
    EXPECT_GT(st.class_names, 0u);
    EXPECT_GT(st.renamed, 0u);
}

// ---- 属性级语料门：签名树手术经属性层接线（sig_attr）跑真实语料 ----

namespace {

/// 一个 Signature 属性的坐标与内容快照。
struct AttrSigCoord
{
    int level = 0;        ///< 0=类级 1=方法级 2=字段级
    ca::usize member = 0; ///< 方法/字段下标（level 0 时无意义）
    ca::usize index = 0;  ///< 挂载点属性表内的槽下标
    SignatureForm form = SignatureForm::Field;
    std::string sig;   ///< 原始签名串
};

/// @brief 收集一个类文件全部 Signature 属性（typed 模型读出）。
void collectAttrSigs(ClassFile& cf, std::vector<AttrSigCoord>& out)
{
    auto collect = [&](std::vector<AttrSlot>& attrs, int level, ca::usize member,
                       SignatureForm form) {
        for (ca::usize i = 0; i < attrs.size(); ++i) {
            if (!cf.cp.utf8Equals(attrs[i].nameCp, "Signature")) {
                continue;
            }
            auto d = cf.decodeAttr(attrs[i]);
            if (d.is_err()) {
                ADD_FAILURE() << "Signature attr decode failed";
                continue;
            }
            auto* raw = static_cast<RawAttr*>(std::move(d).unwrap());
            const std::vector<ca::u8>& b = raw->bytes();
            if (b.size() != 2) {
                ADD_FAILURE() << "Signature attr body != 2 bytes";
                continue;
            }
            out.push_back(AttrSigCoord{level, member, i, form,
                                       std::string(cf.utf8((ca::u32(b[0]) << 8) | b[1]))});
        }
    };
    collect(cf.attrs, 0, 0, SignatureForm::Class);
    for (ca::usize m = 0; m < cf.methods.size(); ++m) {
        collect(cf.methods[m].attrs, 1, m, SignatureForm::Method);
    }
    for (ca::usize f = 0; f < cf.fields.size(); ++f) {
        collect(cf.fields[f].attrs, 2, f, SignatureForm::Field);
    }
}

AttrSlot& coordSlot(ClassFile& cf, const AttrSigCoord& c)
{
    switch (c.level) {
    case 0: return cf.attrs[c.index];
    case 1: return cf.methods[c.member].attrs[c.index];
    default: return cf.fields[c.member].attrs[c.index];
    }
}

/// @brief 读出某槽当前指向的签名串（Raw 体 u2 索引一跳）。
std::string_view sigStringOf(ClassFile& cf, AttrSlot& slot)
{
    auto d = cf.decodeAttr(slot);
    EXPECT_TRUE(d.is_ok());
    if (d.is_err()) {
        return {};
    }
    auto* raw = static_cast<RawAttr*>(std::move(d).unwrap());
    const std::vector<ca::u8>& b = raw->bytes();
    EXPECT_EQ(b.size(), ca::usize{2});
    if (b.size() < 2) {
        return {};
    }
    return cf.utf8((ca::u32(b[0]) << 8) | ca::u32(b[1]));
}

}   // namespace

TEST(SignatureCorpus, AttrLevelSurgeryPipeline)
{
    const auto src = jbc::test::corpus::resolveCorpus();
    if (src.stage == jbc::test::corpus::Stage::None) {
        GTEST_SKIP() << jbc::test::corpus::skipReason(src);
    }
    const std::vector<std::string> files = jbc::test::corpus::collectClassFiles(src.dir);
    ASSERT_GT(files.size(), ca::usize{10}) << "corpus too small: " << src.dir;

    ca::u32 filesWithSigs = 0;
    ca::u32 forwardOps = 0;
    ca::u32 attrsChangedTotal = 0;
    ca::u32 hitsTotal = 0;

    for (const std::string& path : files) {
        const Bytes bytes = slurp(path);
        ASSERT_FALSE(bytes.empty()) << path;

        // 硬门禁：不触发手术（含完全不解码）写回逐字节不动。
        {
            auto r = readClass(ByteView{bytes});
            ASSERT_TRUE(r.is_ok()) << path;
            EXPECT_EQ(jbc::test::writeOk(*std::move(r).unwrap().file), bytes) << path;
        }

        // 收集坐标 + 去重内部名。
        {
            auto r = readClass(ByteView{bytes});
            ASSERT_TRUE(r.is_ok()) << path;
            auto cf = std::move(r).unwrap().file;
            std::vector<AttrSigCoord> coords;
            collectAttrSigs(*cf, coords);
            if (coords.empty()) {
                continue;
            }
            ++filesWithSigs;

            std::vector<std::string> names;
            for (const AttrSigCoord& c : coords) {
                auto spans = classNameSpans(c.sig, c.form);
                ASSERT_TRUE(spans.is_ok()) << path << ": " << c.sig;
                for (const SigClassNameSpan& s : std::move(spans).unwrap()) {
                    if (s.kind != SigClassNameSpan::Kind::InternalName) {
                        continue;
                    }
                    const std::string name(c.sig.substr(s.offset, s.length));
                    if (std::find(names.begin(), names.end(), name) == names.end()) {
                        names.push_back(name);
                    }
                }
            }
            ASSERT_FALSE(names.empty()) << path;

            // 未命中门：缺席名全扫零改写，写回逐字节不动。
            {
                auto r2 = readClass(ByteView{bytes});
                ASSERT_TRUE(r2.is_ok()) << path;
                auto cf2 = std::move(r2).unwrap().file;
                auto miss = jbc::ir::renameClassInSignatureAttrs(*cf2, "zzz/Absent_R", "zzz/Moved_R");
                ASSERT_TRUE(miss.is_ok()) << path;
                EXPECT_EQ(std::move(miss).unwrap().attrsChanged, 0u) << path;
                EXPECT_EQ(jbc::test::writeOk(*cf2), bytes) << path;
            }

            // 逐名正向手术 → 写回 → 重读逐属性比对 → 逆向手术还原逐属性比对。
            for (const std::string& name : names) {
                const std::string renamed = name + "_R";
                auto rf = readClass(ByteView{bytes});
                ASSERT_TRUE(rf.is_ok()) << path;
                auto cff = std::move(rf).unwrap().file;

                auto fwd = jbc::ir::renameClassInSignatureAttrs(*cff, name, renamed);
                ASSERT_TRUE(fwd.is_ok())
                    << path << ": forward surgery failed for " << name;
                jbc::ir::SigAttrRenameSummary st = std::move(fwd).unwrap();
                EXPECT_GT(st.attrsChanged, 0u) << path << ": " << name;
                EXPECT_GE(st.hits, st.attrsChanged) << path << ": " << name;
                ++forwardOps;
                attrsChangedTotal += st.attrsChanged;
                hitsTotal += st.hits;

                // 期望串：纯函数面按坐标逐条算出（与属性层入口互为 oracle）。
                const Bytes outBytes = jbc::test::writeOk(*cff);
                auto rr = readClass(ByteView{outBytes});
                ASSERT_TRUE(rr.is_ok()) << path;
                auto cfr = std::move(rr).unwrap().file;
                std::vector<AttrSigCoord> rcoords;
                collectAttrSigs(*cfr, rcoords);
                ASSERT_EQ(rcoords.size(), coords.size()) << path;
                for (ca::usize i = 0; i < coords.size(); ++i) {
                    auto want = renameClassInSignature(coords[i].sig, coords[i].form,
                                                       name, renamed);
                    ASSERT_TRUE(want.is_ok()) << path << ": " << coords[i].sig;
                    const SigRenameResult w = std::move(want).unwrap();
                    EXPECT_EQ(sigStringOf(*cfr, coordSlot(*cfr, rcoords[i])),
                              std::string_view(w.signature.c_str(), w.signature.byte_length()))
                        << path << ": attr " << i << " forward mismatch";
                }

                // 逆向手术：属性层入口还原每个原始签名串。
                auto back = jbc::ir::renameClassInSignatureAttrs(*cff, renamed, name);
                ASSERT_TRUE(back.is_ok()) << path << ": reverse surgery failed";
                EXPECT_EQ(std::move(back).unwrap().attrsChanged, st.attrsChanged)
                    << path << ": " << name;
                const Bytes backBytes = jbc::test::writeOk(*cff);
                auto rb = readClass(ByteView{backBytes});
                ASSERT_TRUE(rb.is_ok()) << path;
                auto cfb = std::move(rb).unwrap().file;
                std::vector<AttrSigCoord> bcoords;
                collectAttrSigs(*cfb, bcoords);
                ASSERT_EQ(bcoords.size(), coords.size()) << path;
                for (ca::usize i = 0; i < coords.size(); ++i) {
                    EXPECT_EQ(sigStringOf(*cfb, coordSlot(*cfb, bcoords[i])), coords[i].sig)
                        << path << ": attr " << i << " reverse mismatch";
                }
            }
        }
    }

    std::printf("[signature-attr-corpus] stage=%d filesWithSigs=%u forwardOps=%u "
                "attrsChanged=%u hits=%u\n",
                static_cast<int>(src.stage), filesWithSigs, forwardOps, attrsChangedTotal,
                hitsTotal);
    EXPECT_GT(filesWithSigs, 0u);
    EXPECT_GT(forwardOps, 0u);
    EXPECT_GT(attrsChangedTotal, 0u);
    EXPECT_GT(hitsTotal, 0u);
}
