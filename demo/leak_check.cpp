/// @file        leak_check.cpp
/// @brief       轻量泄漏检测：CRT 调试堆快照比对。
///
/// 为什么单独做一个而不塞进 gtest：`libjbc_unittest` 依赖 test_helper + gtest +
/// boost，debug 模式下整条链重编代价很大；本程序只依赖 `libjbc` 与 libca，
/// 能在 debug 下快速编出来。gtest 版见 `test/unittest/leak_test.cpp`。
///
/// 用法（**必须是 debug 构建**，否则 _CrtMemCheckpoint 是空操作）：
/// ```sh
/// xmake f -m debug && xmake build libjbc_leak_check
/// xmake run libjbc_leak_check -- <class 目录> [上限]
/// ```
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "jbc/jbc.hpp"

#ifdef _MSC_VER
#include <crtdbg.h>
#endif

namespace fs = std::filesystem;

namespace {

using Bytes = std::vector<ca::u8>;

// ---------------------------------------------------------------------------
// 动态分配计数（release 下亦可用）
//
// CRT 调试堆只活在 _DEBUG 里，而 debug 全链重编代价很大。这里补一条随时能跑
// 的路子：接管全局 operator new/delete，统计「活着的对象数」。跑完一段工作
// 负载后它应当回到起点——回不去就是有东西没释放。
//
// 实现一律走 malloc/free，与 MSVC 默认 operator new/delete 的底层一致，避免
// 跨堆释放。C++17 的 aligned 版本也必须接管，否则计数会漏。
// ---------------------------------------------------------------------------
long g_liveAllocs = 0;
long g_totalAllocs = 0;

}   // namespace

void* operator new(std::size_t n)
{
    void* p = std::malloc(n != 0 ? n : 1);
    if (p != nullptr) {
        ++g_liveAllocs;
        ++g_totalAllocs;
    }
    return p;
}

void* operator new[](std::size_t n)
{
    void* p = std::malloc(n != 0 ? n : 1);
    if (p != nullptr) {
        ++g_liveAllocs;
        ++g_totalAllocs;
    }
    return p;
}

void operator delete(void* p) noexcept
{
    if (p != nullptr) {
        --g_liveAllocs;
        std::free(p);
    }
}

void operator delete[](void* p) noexcept
{
    if (p != nullptr) {
        --g_liveAllocs;
        std::free(p);
    }
}

void operator delete(void* p, std::size_t) noexcept
{
    if (p != nullptr) {
        --g_liveAllocs;
        std::free(p);
    }
}

void operator delete[](void* p, std::size_t) noexcept
{
    if (p != nullptr) {
        --g_liveAllocs;
        std::free(p);
    }
}

namespace {

Bytes slurp(const fs::path& p)
{
    std::error_code ec;
    const auto sz = fs::file_size(p, ec);
    if (ec || sz == 0) {
        return {};
    }
    FILE* f = nullptr;
#ifdef _MSC_VER
    _wfopen_s(&f, p.c_str(), L"rb");
#else
    f = std::fopen(p.c_str(), "rb");
#endif
    if (f == nullptr) {
        return {};
    }
    Bytes buf(sz);
    const std::size_t got = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    return got == buf.size() ? buf : Bytes{};
}

std::vector<fs::path> collect(const fs::path& root, std::size_t limit)
{
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; it != end && out.size() < limit;
         it.increment(ec)) {
        if (!ec && it->is_regular_file() && it->path().extension() == ".class") {
            out.push_back(it->path());
        }
    }
    return out;
}

std::size_t oneTransformRound(const Bytes& bytes);

/// @brief 一轮「读 → 全解码 → 写回 → 析构」。返回处理成功的类数。
std::size_t oneRound(const std::vector<fs::path>& files)
{
    using namespace jbc::core;

    std::size_t ok = 0;
    for (const fs::path& p : files) {
        const Bytes bytes = slurp(p);
        if (bytes.empty()) {
            continue;
        }
        auto r = readClass(ByteView{bytes.data(), bytes.size()});
        if (r.is_err()) {
            continue;
        }
        auto rr = std::move(r).unwrap();
        ClassFile& cf = *rr.file;

        // 全解码：把每个属性体都物化出来（含 RawAttr 的字节副本）
        if (decodeAllAttrs(cf).is_err()) {
            continue;
        }

        // 顺手追加几个 CP 条目，把 arena 也拉进来一起测
        cf.cp.addUtf8("p/LeakProbeName");
        cf.cp.addUtf8("p/LeakProbeName");  // 同名，应被去重

        auto w = writeClass(cf);
        ++ok;

        ok += oneTransformRound(bytes);
    }
    return ok;
}

/// @brief v0.3+ 变换链路一轮：session/promote/编辑（插桩+语义 setter）/
/// erase 双策略/帧引擎/lower/写回/重读——全部新建路径进泄漏口径。
std::size_t oneTransformRound(const Bytes& bytes)
{
    using namespace jbc::core;
    namespace ir = jbc::ir;
    namespace an = jbc::analysis;

    auto r = readClass(ByteView{bytes.data(), bytes.size()});
    if (r.is_err()) {
        return 0;
    }
    auto ses = ir::makeSession(std::move(std::move(r).unwrap().file));
    if (ses.is_err()) {
        return 0;
    }
    auto s = std::move(ses).unwrap();
    const ca::u32 marker = s->internString("leak-probe-marker");
    ca::usize touched = 0;
    for (ca::usize i = 0; i < s->unit().methods.size(); ++i) {
        auto p = ir::promoteMethod(*s, i);
        if (p.is_err()) {
            continue;
        }
        ir::CodeUnit& cu = *p.unwrap();
        ir::CodeEditor ed(cu);
        (void)ed.insertAtStart(ir::CodeEditor::loadConst(marker));
        (void)ed.insertAfter(cu.dom.first(), ir::CodeEditor::simple(Op::POP));

        bool sawMember = false;
        bool sawString = false;
        for (const ir::InsnId& id : cu.dom.order()) {
            auto g = cu.dom.get(id);
            if (g.is_err()) {
                continue;
            }
            ir::Insn* in = g.unwrap();
            ir::ClassSession::MemberSem m;
            if (!sawMember && s->memberOf(*in, m) && !m.owner.empty()) {
                (void)s->setMember(*in, m.kind, m.owner, m.name, m.desc);
                sawMember = true;
            }
            ir::ClassSession::ConstSem cv;
            if (!sawString && s->constOf(*in, cv) &&
                cv.kind == ir::ClassSession::ConstSem::Kind::String) {
                (void)s->setString(*in, "leak-probe-string");
                sawString = true;
            }
        }
        // erase 双策略（失败也走分配路径）
        (void)ed.erase(cu.dom.last(), ir::CodeEditor::ErasePolicy::kRejectIfUsed);
        (void)ed.erase(cu.dom.last(),
                       ir::CodeEditor::ErasePolicy::kRetargetToFallthrough);

        an::ShapeResolver res;
        an::FrameEngine fe(s->backing().cp, res);
        (void)fe.compute(cu);

        (void)ir::lowerMethod(*s, i);
        ++touched;
    }
    if (touched == 0) {
        return 0;
    }
    auto w = writeClass(s->backing());
    if (w.is_err()) {
        return 0;
    }
    // 重读一轮（读路径分配也计入）。
    auto r2 = readClass(ByteView{w.unwrap()});
    return r2.is_ok() ? 1 : 0;
}

/// @brief ASan 自检：故意做一次堆越界写，验证插桩确实生效。
///
/// 「ASan 没报错」有两种可能——代码干净，或者 ASan 根本没开。这个开关用来
/// 排除后者：设了环境变量就必须报错，不报错说明构建配置有问题。
/// 默认关闭，常规运行不触发。
/// ```sh
/// JBC_ASAN_SELFTEST=1 xmake run libjbc_leak_check
/// ```
void asanSelfTest()
{
    const char* flag = std::getenv("JBC_ASAN_SELFTEST");
    if (flag == nullptr || flag[0] == '\0' || flag[0] == '0') {
        return;
    }
    std::printf("asan self-test: triggering a deliberate heap overflow...\n");
    std::fflush(stdout);
    volatile ca::u8* p = new ca::u8[4];
    p[8] = 1;  // 越界写：ASan 应当在此中断并打印报告
    delete[] p;
    std::printf("asan self-test FAILED: no report, sanitizer is NOT active\n");
}

}   // namespace

int main(int argc, char** argv)
{
    asanSelfTest();

    const char* envRoot = std::getenv("LIBJBC_REAL_CLASS_DIR");
    const fs::path root = argc > 1 ? argv[1] : (envRoot ? envRoot : "./corpus");
    const std::size_t limit = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 300;

    const std::vector<fs::path> files = collect(root, limit);
    if (files.empty()) {
        std::fprintf(stderr, "no .class under %s\n", root.string().c_str());
        return 2;
    }

#if !defined(_MSC_VER) || !defined(_DEBUG)
    // CRT 调试堆不可用（release / 非 MSVC）：改用全局分配计数。工作负载照跑，
    // 因此在 ASan 构建下本程序同样有价值——ASan 盯越界/UAF，计数盯平衡。
    std::printf("NOTE: CRT debug heap needs MSVC + _DEBUG (xmake f -m debug);\n");
    std::printf("      falling back to new/delete balance counting.\n");

    oneRound(files);  // 预热
    const long liveBefore = g_liveAllocs;
    const std::size_t n1 = oneRound(files);
    const std::size_t n2 = oneRound(files);
    const long liveAfter = g_liveAllocs;

    std::printf("parses=%zu  total-allocs=%ld  live-before=%ld  live-after=%ld\n",
                n1 + n2, g_totalAllocs, liveBefore, liveAfter);
    if (liveAfter != liveBefore) {
        std::printf("LEAK: %ld allocations still live\n", liveAfter - liveBefore);
        return 1;
    }
    std::printf("OK: new/delete balanced across %zu parses\n", n1 + n2);
    return 0;
#else
    std::printf("leak check over %zu classes\n", files.size());

    oneRound(files);  // 预热：吸收首次分配的一次性开销

    _CrtMemState before{};
    _CrtMemState after{};
    _CrtMemState diff{};
    _CrtMemCheckpoint(&before);

    const std::size_t done = oneRound(files);
    const std::size_t done2 = oneRound(files);

    _CrtMemCheckpoint(&after);
    if (_CrtMemDifference(&diff, &before, &after) == 0) {
        std::printf("OK: no heap growth across %zu parses (2 rounds)\n", done + done2);
        return 0;
    }

    std::printf("LEAK: net growth after %zu parses:\n", done + done2);
    _CrtMemDumpStatistics(&diff);
    return 1;
#endif
}
