/// @file        corpus_reloc_test.cpp
/// @brief       布局改动路径的语料扫——relocation contract 的行为级门。
///
/// 每个带码方法开头插 nop（强制 codeChanged），lower 断言：
/// 1. 无 RelocationNotModeled——未建模嵌套属性墙已封（LNT/LVT/LVTT/SMT/
///    TypeAnnotations 全建模，其余已知形态解码层就拒绝）；
/// 2. 无 RelocationBadTarget——语料不含「解码合法但 pc 引用不落指令起点」
///    的畸形属性；非零即语料或重定位实现出问题，须查明而不是放行；
/// 3. 写回 → 重读 → 全解码 → 再写逐字节一致——重定位后的整体保真。
#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "jbc/jbc.hpp"
#include "corpus_util.hpp"
#include "test_class_bytes.hpp"

using namespace jbc::core;
using namespace jbc::ir;
using Bytes = std::vector<ca::u8>;

namespace {

Bytes slurpReloc(const std::string& path, bool& found)
{
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) {
        found = false;
        return {};
    }
    found = true;
    return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}   // namespace

TEST(CorpusReloc, LayoutChangeScanZeroNotModeled)
{
    const auto src = jbc::test::corpus::resolveCorpus();
    if (src.stage == jbc::test::corpus::Stage::None) {
        GTEST_SKIP() << jbc::test::corpus::skipReason(src);
    }
    const auto files = jbc::test::corpus::collectClassFiles(src.dir);
    ASSERT_FALSE(files.empty());

    int filesWalked   = 0;
    int methodsLowered = 0;
    int notModeled    = 0;
    int badTarget     = 0;
    std::map<std::string, int> promoteErrs;

    for (const auto& path : files) {
        bool found = false;
        const Bytes bytes = slurpReloc(path, found);
        if (!found || bytes.empty()) {
            continue;
        }
        auto r = readClass(ByteView{bytes});
        if (r.is_err()) {
            ADD_FAILURE() << path << " read failed: " << jbc::core::to_string(r.unwrap_err());
            continue;
        }
        ++filesWalked;
        auto sessR = makeSession(std::move(r).unwrap().file);
        if (sessR.is_err()) {
            ADD_FAILURE() << path << " session failed";
            continue;
        }
        auto sess = std::move(sessR).unwrap();
        ClassSession& s = *sess;
        const ca::usize mcount = s.unit().methods.size();
        bool touched = false;
        for (ca::usize mi = 0; mi < mcount; ++mi) {
            if (!hasCode(s, mi)) {
                continue;
            }
            auto p = promoteMethod(s, mi);
            if (p.is_err()) {
                promoteErrs[jbc::ir::to_string(p.unwrap_err())]++;
                continue;
            }
            jbc::ir::CodeUnit& cu = *p.unwrap();
            CodeEditor ed(cu);
            if (!ed.insertAtStart(CodeEditor::simple(Op::NOP)).is_ok()) {
                continue;
            }
            auto lr = lowerMethod(s, mi);
            if (lr.is_ok()) {
                ++methodsLowered;
                touched = true;
                continue;
            }
            const jbc::ir::LowerError e = lr.unwrap_err();
            if (e == jbc::ir::LowerError::RelocationNotModeled) {
                ++notModeled;
            }
            else if (e == jbc::ir::LowerError::RelocationBadTarget) {
                ++badTarget;
            }
            else {
                ADD_FAILURE() << path << " method " << mi << " lower: "
                              << jbc::ir::to_string(e);
            }
        }
        if (!touched) {
            continue;
        }
        // 写回 → 重读 → 全解码 → 再写：逐字节一致（重定位后的整体保真）。
        const Bytes out = jbc::test::writeOk(s.backing());
        ASSERT_FALSE(out.empty()) << path;
        auto r2 = readClass(ByteView{out});
        ASSERT_TRUE(r2.is_ok()) << path;
        auto rr2 = std::move(r2).unwrap();
        ASSERT_TRUE(decodeAllAttrs(*rr2.file).is_ok()) << path << " decodeAll failed";
        EXPECT_EQ(jbc::test::writeOk(*rr2.file), out) << path << " reloc roundtrip";
    }

    std::fprintf(stderr,
                 "[corpus-reloc] stage=%d files=%d lowered=%d notModeled=%d badTarget=%d\n",
                 int(src.stage), filesWalked, methodsLowered, notModeled, badTarget);
    for (const auto& kv : promoteErrs) {
        std::fprintf(stderr, "[corpus-reloc] promote_err %s = %d\n",
                     kv.first.c_str(), kv.second);
    }
    EXPECT_EQ(notModeled, 0)
        << "未建模嵌套属性墙未封：布局改动路径仍有 RelocationNotModeled";
    EXPECT_EQ(badTarget, 0)
        << "重定位坏目标：语料存在解码合法但 pc 引用不落指令起点的属性";
}
