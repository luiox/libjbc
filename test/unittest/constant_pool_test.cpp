/// @file        constant_pool_test.cpp
/// @brief       常量池：零拷贝视图、宽条目双槽、只追加不重排。
#include <gtest/gtest.h>

#include "jbc/jbc.hpp"
#include "test_class_bytes.hpp"

using namespace jbc::core;
using jbc::test::Bytes;
using jbc::test::ClassBuilder;

/// Utf8 条目必须是源字节流的视图，而不是副本——这是「解析不需要为字符串付费」
/// 的直接证据。视图落在源字节范围内即证明没有另分配。
TEST(ConstantPoolTest, Utf8IsZeroCopyViewIntoSource)
{
    ClassBuilder cb;
    const ca::u16 thisIdx  = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 longIdx  = cb.longConst(0x0102030405060708LL);
    (void)longIdx;
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));

    const Bytes bytes = cb.build();
    auto cf = jbc::test::readOk(bytes);
    ASSERT_NE(cf, nullptr);

    const std::string_view sv = cf->utf8(superIdx);
    EXPECT_EQ(sv, "java/lang/Object");

    // 零拷贝判据：视图地址落在模型持有的源字节内部。
    const ca::u8* base   = cf->source().data();
    const ca::u8* limit  = base + cf->source().size();
    const ca::u8* p      = reinterpret_cast<const ca::u8*>(sv.data());
    EXPECT_GE(p, base);
    EXPECT_LT(p, limit);
}

/// Long/Double 占两个槽位，第二个槽位必须不可用（JVMS 4.4.5）。
/// 解析错一个槽，后续所有索引都会整体错位。
TEST(ConstantPoolTest, WideEntriesTakeTwoSlots)
{
    ClassBuilder cb;
    const ca::u16 a = cb.utf8("a");
    const ca::u16 l = cb.longConst(0x0102030405060708LL);
    const ca::u16 d = cb.doubleConst(0x3FF0000000000000ull);
    const ca::u16 b = cb.utf8("bbbb");

    EXPECT_EQ(l, a + 1);
    EXPECT_EQ(d, l + 2);
    EXPECT_EQ(b, d + 2);

    cb.setThis(cb.classRef(cb.utf8("p/T")));
    cb.setSuper(cb.classRef(cb.utf8("java/lang/Object")));

    auto cf = jbc::test::readOk(cb.build());
    ASSERT_NE(cf, nullptr);

    EXPECT_TRUE(cf->cp.valid(l));
    EXPECT_EQ(cf->cp.tagOf(l), CpTag::Long);
    EXPECT_EQ(cf->cp.at(l).lval, 0x0102030405060708LL);

    // 续槽不可寻址：valid() 必须挡住它，否则引用会静默指向错误条目。
    EXPECT_FALSE(cf->cp.valid(l + 1));

    EXPECT_TRUE(cf->cp.valid(d));
    EXPECT_EQ(cf->cp.tagOf(d), CpTag::Double);
    EXPECT_FALSE(cf->cp.valid(d + 1));

    EXPECT_TRUE(cf->cp.valid(b));
    EXPECT_EQ(cf->utf8(b), "bbbb");
}

/// 只追加：新条目一律 append，已有索引及其内容永不改变。
/// 这是「未知属性里的 CP 引用不会失效」的结构性保证。
TEST(ConstantPoolTest, AppendNeverInvalidatesExistingIndex)
{
    ClassBuilder cb;
    const ca::u16 nameIdx = cb.utf8("java/lang/Object");
    const ca::u16 thisIdx = cb.utf8("p/T");
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(nameIdx));

    auto cf = jbc::test::readOk(cb.build());
    ASSERT_NE(cf, nullptr);

    const ca::u32 before      = cf->cp.size();
    const std::string_view sv = cf->utf8(nameIdx);

    const ca::u32 added = cf->cp.addUtf8("p/NewName");
    EXPECT_EQ(added, before);
    EXPECT_EQ(cf->utf8(added), "p/NewName");

    // 旧索引仍指向旧内容，且视图地址未变（append 不触碰源字节）。
    EXPECT_EQ(cf->utf8(nameIdx), "java/lang/Object");
    EXPECT_EQ(cf->utf8(nameIdx).data(), sv.data());

    // 追加的条目标记为脏（没有源 span，写回必须重编码）。
    EXPECT_TRUE(cf->cp.at(added).dirty);
    EXPECT_FALSE(cf->cp.at(nameIdx).dirty);
}

/// 索引越界与 0 号占位必须被挡住，而不是返回未定义内容。
TEST(ConstantPoolTest, OutOfRangeIndexIsRejected)
{
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/T");
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(cb.utf8("java/lang/Object")));

    auto cf = jbc::test::readOk(cb.build());
    ASSERT_NE(cf, nullptr);

    EXPECT_FALSE(cf->cp.valid(0));
    EXPECT_FALSE(cf->cp.valid(cf->cp.size()));
    EXPECT_FALSE(cf->cp.isUtf8(0));
    EXPECT_TRUE(cf->utf8(0).empty());
    EXPECT_FALSE(cf->cp.utf8Equals(0, "anything"));
}

/// 宽条目的第二槽越出 constant_pool_count 属 CP 结构损坏（design.md §27.3）：
/// 后续条目边界无法确定，且宽容写回会使槽位比输入多 1——reader 阶段即 Err，
/// 不产出降级模型。
TEST(ConstantPoolTest, WideEntryOverflowingCountIsRejected)
{
    ClassBuilder cb;
    const ca::u16 a = cb.utf8("a");
    const ca::u16 l = cb.longConst(0x0102030405060708LL);   // 占 #l、#l+1 两槽
    (void)a;
    cb.setThis(cb.classRef(cb.utf8("p/T")));
    cb.setSuper(cb.classRef(cb.utf8("java/lang/Object")));

    // 声明 count = l+1：Long 的第二槽 #l+1 恰好越出池——畸形输入。
    // 解析在 CP 阶段就该失败，而不是把畸形池读成降级模型再写回多一槽。
    Bytes bytes = cb.build();
    const ca::u16 broken = static_cast<ca::u16>(l + 1);
    bytes[8] = static_cast<ca::u8>(broken >> 8);
    bytes[9] = static_cast<ca::u8>(broken & 0xff);

    const Result<ReadResult, ReadError> r = readClass(ByteView{bytes});
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), ReadError::BadConstantPoolCount);
}

/// utf8Equals 走 memcmp 且不做分配：内容相同即相等，长度不同先否。
TEST(ConstantPoolTest, Utf8EqualsIsByteWise)
{
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/T");
    const ca::u16 objIdx  = cb.utf8("java/lang/Object");
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(objIdx));

    auto cf = jbc::test::readOk(cb.build());
    ASSERT_NE(cf, nullptr);

    EXPECT_TRUE(cf->cp.utf8Equals(objIdx, "java/lang/Object"));
    EXPECT_FALSE(cf->cp.utf8Equals(objIdx, "java/lang/ObjectX"));
    EXPECT_FALSE(cf->cp.utf8Equals(objIdx, "java/lang/Objec"));
    EXPECT_TRUE(cf->cp.utf8StartsWith(objIdx, "java/"));
    EXPECT_FALSE(cf->cp.utf8StartsWith(objIdx, "javax/"));

    // 非 Utf8 条目不得被当作字符串比较（索引指向 Class 条目）。
    EXPECT_FALSE(cf->cp.utf8Equals(cf->thisClass, "p/T"));
}

/// 新增的 Utf8 在 arena 里去重：同名串复用同一份存储，但仍是两个 CP 条目
/// （CP 索引必须唯一，去重只发生在存储层）。
TEST(ConstantPoolTest, AppendedUtf8IsDeduplicatedInArena)
{
    ClassBuilder cb;
    cb.setThis(cb.classRef(cb.utf8("p/T")));
    cb.setSuper(cb.classRef(cb.utf8("java/lang/Object")));

    auto cf = jbc::test::readOk(cb.build());
    ASSERT_NE(cf, nullptr);

    // 解析来的条目一个都不在 arena 里——扫描路径不碰 arena。
    EXPECT_EQ(cf->cp.internedCount(), 0u);
    EXPECT_TRUE(cf->cp.arenaOwnsAllAppended());

    const ca::u32 a = cf->cp.addUtf8("p/Same");
    const ca::u32 b = cf->cp.addUtf8("p/Same");
    const ca::u32 c = cf->cp.addUtf8("p/Other");

    EXPECT_NE(a, b);  // 两个独立条目
    EXPECT_EQ(cf->utf8(a), "p/Same");
    EXPECT_EQ(cf->utf8(b), "p/Same");
    EXPECT_EQ(cf->utf8(c), "p/Other");

    // 去重：三条目只占两份存储，同名的两份指向同一地址。
    EXPECT_EQ(cf->cp.internedCount(), 2u);
    EXPECT_EQ(cf->cp.at(a).utf8, cf->cp.at(b).utf8);
    EXPECT_NE(cf->cp.at(a).utf8, cf->cp.at(c).utf8);

    EXPECT_TRUE(cf->cp.at(a).utf8InArena);
    EXPECT_FALSE(cf->cp.at(1).utf8InArena);  // 解析来的第一个条目
}

/// 归属断言：所有「新增」的 Utf8 视图必须确实落在 arena 的 chunk 内。
/// 这把「新增串的生命周期」从注释约定变成可断言的事实。
TEST(ConstantPoolTest, AppendedUtf8ViewsBelongToArena)
{
    ClassBuilder cb;
    cb.setThis(cb.classRef(cb.utf8("p/T")));
    cb.setSuper(cb.classRef(cb.utf8("java/lang/Object")));

    auto cf = jbc::test::readOk(cb.build());
    ASSERT_NE(cf, nullptr);

    cf->cp.addUtf8("p/A");
    cf->cp.addUtf8("p/B");
    cf->cp.addUtf8("");  // 空串不进 arena，也不得破坏断言
    cf->cp.addUtf8("p/C");

    EXPECT_TRUE(cf->cp.arenaOwnsAllAppended());
    EXPECT_GT(cf->cp.arenaBytes(), 0u);
}

/// modified UTF-8 不是合法 UTF-8：混淆常往里塞任意字节（JVM 把 NUL 编成
/// 0xC0 0x80）。走校验的 intern 会丢数据，intern_raw 必须 1:1 保留字节。
TEST(ConstantPoolTest, ModifiedUtf8BytesSurviveIntern)
{
    ClassBuilder cb;
    cb.setThis(cb.classRef(cb.utf8("p/T")));
    cb.setSuper(cb.classRef(cb.utf8("java/lang/Object")));

    auto cf = jbc::test::readOk(cb.build());
    ASSERT_NE(cf, nullptr);

    const std::string weird{'a', '\xC0', '\x80', '\xFF', 'b'};  // 非法 UTF-8
    const ca::u32 i = cf->cp.addUtf8(weird);

    EXPECT_EQ(cf->utf8(i), weird) << "modified UTF-8 bytes must round-trip unchanged";
    EXPECT_EQ(cf->utf8(i).size(), weird.size());
}

/// 与项目字符串设施对接：同一段字节既给 memcmp 热路径，也能取 libca 的
/// Utf8StringRef 供跨库使用。
TEST(ConstantPoolTest, Utf8RefExposesLibcaStringType)
{
    ClassBuilder cb;
    const ca::u16 objIdx = cb.utf8("java/lang/Object");
    cb.setThis(cb.classRef(cb.utf8("p/T")));
    cb.setSuper(cb.classRef(objIdx));

    auto cf = jbc::test::readOk(cb.build());
    ASSERT_NE(cf, nullptr);

    const ca::str::Utf8StringRef ref = cf->cp.utf8Ref(objIdx);
    ASSERT_FALSE(ref.is_empty());
    EXPECT_EQ(ref.byte_length(), 16u);

    // 两种视图是同一段字节
    EXPECT_EQ(ref.data(), reinterpret_cast<const ca::u8*>(cf->utf8(objIdx).data()));

    // 新增串同样能取到 ref，且落在 arena
    const ca::u32 added = cf->cp.addUtf8("p/Added");
    const ca::str::Utf8StringRef aref = cf->cp.utf8Ref(added);
    EXPECT_EQ(aref.byte_length(), 7u);
    EXPECT_TRUE(cf->cp.arenaOwnsAllAppended());
}

/// 未知 CP tag 必须报错而不是猜——猜错一个 tag 会让整个 CP 的对齐错位。
TEST(ConstantPoolTest, UnknownTagIsRejected)
{
    ClassBuilder cb;
    cb.utf8("p/T");
    const Bytes bytes = cb.build();

    // 把第一条 Utf8 的 tag（偏移 10，即 CP 内容起点）改成未定义的 2。
    Bytes broken = bytes;
    broken[10]   = 2;

    const Result<ReadResult, ReadError> r = readClass(ByteView{broken});
    EXPECT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err(), ReadError::BadConstantPoolTag);
}

// ---------------------------------------------------------------------------
// mutateAt：语义改写的置脏单入口（Tier A，与 AttrSlot::mutate 对称）
// ---------------------------------------------------------------------------

/// 经 mutateAt 改写的条目：置脏 → 写回重编码 → 重读语义一致。
TEST(ConstantPoolTest, MutateAtMarksDirtyAndReencodes)
{
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 thisCls = cb.classRef(thisIdx);
    const ca::u16 superCls = cb.classRef(superIdx);
    cb.setThis(thisCls);
    cb.setSuper(superCls);
    const Bytes bytes = cb.build();

    auto cf = jbc::test::readOk(bytes);
    ASSERT_NE(cf, nullptr);
    EXPECT_FALSE(cf->cp.at(superCls).dirty);

    // 父类改名：新增 Utf8 + mutateAt 把 Class 条目重指向（rename 探针的手工
    // 三连收敛为一个置脏入口）。
    const ca::u32 newName = cf->cp.addUtf8("p/Base");
    cf->cp.mutateAt(superCls).a = newName;
    EXPECT_TRUE(cf->cp.at(superCls).dirty);

    const Bytes out = jbc::test::writeOk(*cf);
    EXPECT_NE(out, bytes);   // 置脏确实触发重编码

    auto cf2 = jbc::test::readOk(out);
    ASSERT_NE(cf2, nullptr);
    EXPECT_EQ(cf2->superName(), "p/Base");   // 重读语义一致
}

/// 对照组：经 at() 改写而不置脏 → 写回按源 span 复制，改写被静默还原。
/// 这是文档化的既定语义（at() 不置脏），本用例把「陷阱是契约」钉死——
/// 若有人让 at() 自动置脏，此用例红，倒逼其面对全量重编码的语义变化。
TEST(ConstantPoolTest, AtWithoutDirtyRestoresOriginalOnWrite)
{
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 superCls = cb.classRef(superIdx);
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(superCls);
    const Bytes bytes = cb.build();

    auto cf = jbc::test::readOk(bytes);
    ASSERT_NE(cf, nullptr);
    const ca::u32 newName = cf->cp.addUtf8("p/Base");
    cf->cp.at(superCls).a = newName;   // 不置脏
    EXPECT_FALSE(cf->cp.at(superCls).dirty);

    // 追加的 Utf8 本身置脏 → 写出走重编码路径；未脏的 super 条目仍按源
    // span 复制 → at() 的改写被静默还原（输出不是原字节，因为新条目在场）。
    const Bytes out = jbc::test::writeOk(*cf);
    EXPECT_NE(out, bytes);
    auto cf2 = jbc::test::readOk(out);
    ASSERT_NE(cf2, nullptr);
    EXPECT_EQ(cf2->superName(), "java/lang/Object");
}

/// Entry& 不得跨 append 存活（边界卫生）：跨追加持 u32 索引、用前重 at()。
TEST(ConstantPoolTest, IndexRemainsStableAcrossAppends)
{
    ClassBuilder cb;
    const ca::u16 thisIdx = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    const Bytes bytes = cb.build();

    auto cf = jbc::test::readOk(bytes);
    ASSERT_NE(cf, nullptr);

    // 记索引，不持引用；append 触发扩容搬迁后索引语义不变。
    const ca::u32 held = superIdx;
    for (int i = 0; i < 64; ++i) {
        (void)cf->cp.addUtf8("filler-" + std::to_string(i));
    }
    EXPECT_TRUE(cf->cp.utf8Equals(held, "java/lang/Object"));
    EXPECT_EQ(cf->cp.tagOf(held), CpTag::Utf8);
}
