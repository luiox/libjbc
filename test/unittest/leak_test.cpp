/// @file        leak_test.cpp
/// @brief       内存泄漏检测（Windows CRT 调试堆）。
///
/// 为什么不用 ASan 查泄漏：**MSVC 的 AddressSanitizer 不做泄漏检测**
/// （Linux 的 LSan 才做）。所以两套工具各管一段：
///
/// | 工具 | 查什么 | 怎么开 |
/// |---|---|---|
/// | ASan | 越界、use-after-free、double-free | `xmake f --with_asan=y` |
/// | CRT 调试堆（本文件） | 泄漏 | debug 模式（`_DEBUG`）下自动生效 |
///
/// 做法是在被测代码前后各打一次堆快照并比对差异——比逐个对象记账更可靠，
/// 因为它连「第三方代码顺手泄漏了什么」也算进去。
///
/// 非 MSVC 或非 debug 构建下本文件全部 SKIP：_CrtMemCheckpoint 在 release
/// 里是空实现，查了也是假绿，不如明确跳过。
#include <gtest/gtest.h>

#include "jbc/jbc.hpp"
#include "test_class_bytes.hpp"

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#define JBC_HAS_CRT_LEAK_CHECK 1
#endif

using namespace jbc::core;
using jbc::test::Bytes;
using jbc::test::ClassBuilder;
using jbc::test::makeRichClass;

namespace {

#ifdef JBC_HAS_CRT_LEAK_CHECK

/// @brief 堆快照比对器：构造时打点，@ref diffBytes 时报告这期间的净增量。
class LeakGuard
{
public:
    LeakGuard() { _CrtMemCheckpoint(&before_); }

    /// @brief 返回期间的净分配字节数；有差异时顺带把统计打到调试输出。
    long diffBytes()
    {
        _CrtMemState after{};
        _CrtMemState diff{};
        _CrtMemCheckpoint(&after);
        if (_CrtMemDifference(&diff, &before_, &after) == 0) {
            return 0;
        }
        _CrtMemDumpStatistics(&diff);
        return static_cast<long>(diff.lTotalSize);
    }

private:
    _CrtMemState before_{};
};

/// @brief 跑 warmup 吸收一次性开销，再连跑数遍并断言无净增长。
template <class Op>
void expectNoLeak(Op op, int rounds = 3)
{
    op();  // 预热：吸收 gtest 首次断言、静态初始化等一次性分配
    LeakGuard guard;
    for (int i = 0; i < rounds; ++i) {
        op();
    }
    EXPECT_EQ(guard.diffBytes(), 0) << "net heap growth across " << rounds << " rounds";
}

const Bytes& richBytes()
{
    static const Bytes b = makeRichClass();
    return b;
}

/// @brief 截断到 n 字节的畸形输入（失败路径最容易漏）。
Bytes truncated(ca::usize n)
{
    const Bytes& full = richBytes();
    n = n < full.size() ? n : full.size();
    return Bytes(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(n));
}

#endif  // JBC_HAS_CRT_LEAK_CHECK

}   // namespace

#ifdef JBC_HAS_CRT_LEAK_CHECK

/// 读 → 写 → 析构：整条路径不留残余。
TEST(LeakTest, ParseWriteDestroyLeavesNothingBehind)
{
    expectNoLeak([] {
        auto r = readClass(ByteView{richBytes()});
        if (r.is_ok()) {
            auto rr = std::move(r).unwrap();
            auto w = writeClass(*rr.file);
        }
    });
}

/// 全解码 → 写回 → 析构：解码分配的属性体（含 RawAttr 字节副本）必须回收。
TEST(LeakTest, DecodeAllLeavesNothingBehind)
{
    expectNoLeak([] {
        auto r = readClass(ByteView{richBytes()});
        if (!r.is_ok()) {
            return;
        }
        auto rr = std::move(r).unwrap();
        auto w = writeClass(*rr.file);
    });
}

/// 追加 CP 条目后析构：arena 的 chunk 必须整批回收。
TEST(LeakTest, AppendingToConstantPoolLeavesNothingBehind)
{
    expectNoLeak([] {
        auto r = readClass(ByteView{richBytes()});
        if (!r.is_ok()) {
            return;
        }
        auto rr = std::move(r).unwrap();
        for (int i = 0; i < 20; ++i) {
            rr.file->cp.addUtf8("p/GeneratedName");
        }
        auto w = writeClass(*rr.file);
    });
}

/// 失败路径：截断输入必须干净失败。旧库的泄漏事故（构造中途抛出导致泄漏）
/// 正是这一类的反面教材。
TEST(LeakTest, MalformedInputFailsWithoutLeaking)
{
    const Bytes& full = richBytes();
    expectNoLeak(
        [&full] {
            for (ca::usize n = 0; n < full.size(); n += 7) {
                auto r = readClass(ByteView{truncated(n)});
                if (r.is_ok()) {
                    auto rr = std::move(r).unwrap();
                    auto w  = writeClass(*rr.file);
                }
            }
        },
        2);
}

/// 单类模型反复构造/析构：确认没有随轮次累积的缓慢泄漏。
TEST(LeakTest, RepeatedParseDoesNotAccumulate)
{
    expectNoLeak([] {
        for (int i = 0; i < 8; ++i) {
            auto r = readClass(ByteView{richBytes()});
            if (r.is_ok()) {
                auto rr = std::move(r).unwrap();
                auto w = writeClass(*rr.file);
            }
        }
    });
}

#else

/// 非 Windows debug 构建：CRT 调试堆不可用，明确跳过而不是给个假绿。
TEST(LeakTest, SkippedWithoutMsvcDebugHeap)
{
    GTEST_SKIP() << "leak check needs MSVC + debug (_DEBUG); "
                    "use `xmake f -m debug` on Windows, or ASan on Linux";
}

#endif
