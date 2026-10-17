/// @file        consumer_corpus_test.cpp
/// @brief       Grunt 实语料验证：boar-main*.jar 上的消费者 pass 编组。
///
/// 语料门（与 corpus_util 同格的「env 注入 + 缺席 skip」纪律，正交于
/// `LIBJBC_REAL_CLASS_DIR` 自产语料线）：
///
/// 1. `LIBJBC_GRUNT_JAR`——Grunt 样例的混淆 jar（boar-main.jar，3065 类、
///    major 61 为主）；`LIBJBC_GRUNT_JAR_ORIGIN`——同名干净基线
///    （boar-main-origin.jar，775 类）。可注其一或都注；缺席 skip，不硬失败。
/// 2. 容器细节不进库（§9）：jar 解包容器面归测试自身承担——委托宿主机
///    `tar`（Win10+ bsdtar / 多数发行版的 bsdtar）解到 test/tmp 下的暂存
///    目录；解压不可用或零 .class 属语料能力缺席，skip。
/// 3. 语料特性断言（引用边总量等）按「真程序 jar」低门槛立——注错语料被
///    红出来是本套件的职责，不是缺陷。
///
/// pass 编组（「对 boar-main*.jar 语料写数个 pass」的验收落地）：
///   - ZeroTouchByteExact：全语料 read→write 逐字节恒等（保真框架批量钉）；
///   - SkeletonSweep：SummaryScanner 全 Ok + internalName == 路径推导名
///     （含 $ 内部类形态）+ 类名全语料唯一；
///   - ReferenceSweep：ReferenceScanner 全 Ok + 跨扫描器形状一致（同一
///     文件的方法段名序与骨架逐项一致）+ 引用边总量低门槛；
///   - RenameClassFamilyWholeCorpus：单条类名规则全语料驱动（典型消费
///     姿势：计划下发每个类）——目标类字节相异、改名后事实族里旧名清零、
///     新名引用文件集 == 旧名引用文件集（目标类新旧名顶替规范化）。
///   - BsmTouchedFailLoud：在混淆语料上实证 fail-loud 闸——定位一条
///     BSM MethodHandle（kind>=5）载体的实成员，全语料逐类应用该成员
///     规则，carrier 类（或跨类 carrier）必现 Err(BsmTouched)；成员名
///     改写位不打断言（BSM 专属成员可能仅经 indy 调用，v1 不改 invoke
///     位），Ok/Err 形状守恒收接。
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "test_helper.hpp"

#include "jbc/core/class_file.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/core/writer.hpp"
#include "jbc/consumer/consumer.hpp"
#include "jbc/consumer/reference.hpp"
#include "jbc/consumer/rename.hpp"
#include "jbc/consumer/source.hpp"
#include "jbc/consumer/summary.hpp"
#include "jbc/type/descriptor.hpp"

namespace fs = std::filesystem;

using Bytes = std::vector<ca::u8>;
using jbc::core::ByteView;
using jbc::core::ClassFile;

namespace {

/// @brief 一个已解包的语料：全 .class 文件（升序内部名 + 原始字节）。
struct Corpus
{
    std::string tag;                                   ///< "main" / "origin"
    std::vector<std::pair<std::string, Bytes>> files;  ///< 内部名（'/' 分隔，无 .class 后缀）→ 字节
};

Bytes slurp(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void listClassFiles(const fs::path& dir, std::vector<fs::path>& out)
{
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec) && it->path().extension() == ".class") {
            out.push_back(it->path());
        }
    }
}

/// @brief 解包一个 jar 到 test/tmp 暂存目录并枚举 .class。容器工具缺席 /
///        零类 → nullopt（语料能力缺席，skip 而非硬失败）。
std::optional<Corpus> loadCorpus(const std::string& tag, const std::string& jarPath)
{
    const fs::path jar(jarPath);
    std::error_code ec;
    if (!fs::exists(jar, ec) || !fs::is_regular_file(jar, ec)) {
        return std::nullopt;
    }
    fs::path staging = test::getTempOutPath("grunt_extract.keep");
    staging          = staging.parent_path() / (std::string("grunt_") + tag);
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    const fs::path log = staging.parent_path() / (std::string("grunt_") + tag + "_tar.log");
    const std::string cmd = "tar -xf \"" + jar.string() + "\" -C \"" + staging.string() + "\"";
    if (std::system((cmd + " > \"" + log.string() + "\" 2>&1").c_str()) != 0) {
        return std::nullopt;
    }
    std::vector<fs::path> paths;
    listClassFiles(staging, paths);
    if (paths.empty()) {
        return std::nullopt;
    }
    std::sort(paths.begin(), paths.end());
    Corpus corpus;
    corpus.tag = tag;
    corpus.files.reserve(paths.size());
    for (const fs::path& p : paths) {
        const fs::path rel = fs::relative(p, staging, ec);
        if (ec) {
            return std::nullopt;
        }
        std::string name = rel.generic_string();
        name             = name.substr(0, name.size() - 6);   // 去 ".class"
        corpus.files.emplace_back(std::move(name), slurp(p));
    }
    return std::optional<Corpus>(std::move(corpus));
}

/// @brief 双语料收集：env 未注入/解包缺席 → 条目不进组（调用方 skip）。
std::vector<Corpus> loadCorpora()
{
    std::vector<Corpus> out;
    if (const char* p = std::getenv("LIBJBC_GRUNT_JAR")) {
        std::optional<Corpus> c = loadCorpus("main", p);
        if (c.has_value()) {
            out.push_back(std::move(*c));
        }
    }
    if (const char* p = std::getenv("LIBJBC_GRUNT_JAR_ORIGIN")) {
        std::optional<Corpus> c = loadCorpus("origin", p);
        if (c.has_value()) {
            out.push_back(std::move(*c));
        }
    }
    return out;
}

Bytes viewBytes(const ByteView& v)
{
    return Bytes(v.data(), v.data() + v.size());
}

/// @brief 语料内是否已有同名类。
bool hasName(const Corpus& c, const std::string& name)
{
    for (const auto& kv : c.files) {
        if (kv.first == name) {
            return true;
        }
    }
    return false;
}

/// @brief 事实族里的类名引用文件集（strings 与描述符列不在 E1 v1 面，不采）。
std::set<std::string> referFiles(const std::vector<jbc::consumer::ReferenceSummary>& facts,
                                 const std::string& name)
{
    std::set<std::string> out;
    for (const jbc::consumer::ReferenceSummary& rs : facts) {
        bool hit = (rs.thisClass == name) || (rs.superClass == name);
        for (const std::string& i : rs.interfaces) {
            hit = hit || (i == name);
        }
        for (const jbc::consumer::MethodSection& m : rs.methods) {
            for (const jbc::consumer::TypeRefFact& t : m.typeOps) {
                hit = hit || (t.internalName == name);
            }
            for (const jbc::consumer::FieldRefFact& f : m.fieldAccesses) {
                hit = hit || (f.owner == name);
            }
            for (const jbc::consumer::MethodRefFact& v : m.invokes) {
                hit = hit || (v.owner == name);
            }
        }
        if (hit) {
            out.insert(rs.thisClass);
        }
    }
    return out;
}

}   // namespace

// ---------------------------------------------------------------------------
// ZeroTouch pass：全语料 read→write 逐字节恒等
// ---------------------------------------------------------------------------

TEST(ConsumerCorpusTest, ZeroTouchByteExact)
{
    const std::vector<Corpus> corpora = loadCorpora();
    if (corpora.empty()) {
        GTEST_SKIP() << "Grunt 语料缺席（注入 LIBJBC_GRUNT_JAR / LIBJBC_GRUNT_JAR_ORIGIN）";
    }
    for (const Corpus& c : corpora) {
        ca::usize checked = 0;
        for (const auto& kv : c.files) {
            const std::string& name  = kv.first;
            const Bytes&       bytes = kv.second;
            auto r = jbc::core::readClass(ByteView{bytes.data(), bytes.size()});
            ASSERT_TRUE(r.is_ok()) << "[" << c.tag << "] read 失败: " << name;
            auto rr = std::move(r).unwrap();
            auto w  = jbc::core::writeClass(*rr.file);
            ASSERT_TRUE(w.is_ok()) << "[" << c.tag << "] write 失败: " << name;
            const auto out = std::move(w).unwrap();
            const ByteView got{out.data(), out.size()};
            const ByteView want{bytes.data(), bytes.size()};
            EXPECT_TRUE(got.equals(want)) << "[" << c.tag << "] 零触碰写回不恒等: " << name;
            ++checked;
        }
        EXPECT_EQ(checked, c.files.size()) << "[" << c.tag << "]";
    }
}

// ---------------------------------------------------------------------------
// Skeleton pass：SummaryScanner 全 Ok + 名实一致 + 语料级唯一
// ---------------------------------------------------------------------------

TEST(ConsumerCorpusTest, SkeletonSweep)
{
    const std::vector<Corpus> corpora = loadCorpora();
    if (corpora.empty()) {
        GTEST_SKIP() << "Grunt 语料缺席（注入 LIBJBC_GRUNT_JAR / LIBJBC_GRUNT_JAR_ORIGIN）";
    }
    for (const Corpus& c : corpora) {
        const jbc::consumer::SummaryScanner sc;
        std::set<std::string> seen;
        ca::usize totalMethods = 0;
        for (const auto& kv : c.files) {
            const std::string& name  = kv.first;
            const Bytes&       bytes = kv.second;
            auto r = sc.scan(ByteView{bytes.data(), bytes.size()});
            ASSERT_TRUE(r.is_ok()) << "[" << c.tag << "] 骨架扫描失败: " << name;
            auto rr = std::move(r).unwrap();
            EXPECT_EQ(rr.internalName, name)
                << "[" << c.tag << "] internalName != 路径推导名";
            seen.insert(rr.internalName);
            totalMethods += rr.methods.size();
        }
        EXPECT_EQ(seen.size(), c.files.size()) << "[" << c.tag << "] 类名不唯一";
        EXPECT_GT(totalMethods, ca::usize(0)) << "[" << c.tag << "]";
    }
}

// ---------------------------------------------------------------------------
// Reference pass：ReferenceScanner 全 Ok + 跨扫描器形状一致
// ---------------------------------------------------------------------------

TEST(ConsumerCorpusTest, ReferenceSweep)
{
    const std::vector<Corpus> corpora = loadCorpora();
    if (corpora.empty()) {
        GTEST_SKIP() << "Grunt 语料缺席（注入 LIBJBC_GRUNT_JAR / LIBJBC_GRUNT_JAR_ORIGIN）";
    }
    for (const Corpus& c : corpora) {
        const jbc::consumer::SummaryScanner    sc;
        const jbc::consumer::ReferenceScanner rs;
        ca::usize totalEdges = 0;   ///< invokes + fieldAccesses 汇总
        ca::usize files      = 0;
        for (const auto& kv : c.files) {
            const std::string& name  = kv.first;
            const Bytes&       bytes = kv.second;
            auto skel = sc.scan(ByteView{bytes.data(), bytes.size()});
            ASSERT_TRUE(skel.is_ok()) << "[" << c.tag << "] 骨架失败: " << name;
            const jbc::consumer::ClassSummary sum = std::move(skel).unwrap();

            auto rf = rs.scan(ByteView{bytes.data(), bytes.size()});
            ASSERT_TRUE(rf.is_ok()) << "[" << c.tag << "] 引用扫描失败: " << name;
            const jbc::consumer::ReferenceSummary rsum = std::move(rf).unwrap();

            EXPECT_EQ(rsum.thisClass, sum.internalName)
                << "[" << c.tag << "] thisClass 与骨架不一致: " << name;
            EXPECT_EQ(rsum.methods.size(), sum.methods.size())
                << "[" << c.tag << "] 方法段数与骨架不一致: " << name;
            for (ca::usize i = 0; i < rsum.methods.size(); ++i) {
                EXPECT_EQ(rsum.methods[i].name, sum.methods[i].name) << "[" << c.tag << "] " << name;
                EXPECT_EQ(rsum.methods[i].desc, sum.methods[i].desc) << "[" << c.tag << "] " << name;
            }
            for (const jbc::consumer::MethodSection& m : rsum.methods) {
                totalEdges += m.invokes.size() + m.fieldAccesses.size();
            }
            ++files;
        }
        EXPECT_EQ(files, c.files.size()) << "[" << c.tag << "]";
        EXPECT_GT(totalEdges, files)   // 真程序 jar：成员边总数至少大于文件数
            << "[" << c.tag << "] 引用边总量异常偏少";
    }
}

// ---------------------------------------------------------------------------
// Rename pass：单条类名规则的全语料驱动（origin 优先，缺则 main）
// ---------------------------------------------------------------------------

namespace {

/// @brief 全语料统一下发计划的 resolver——跨类改名的典型消费姿势：
///        类名族规则必须到达每个引用方（operand/描述符列随行改写）。
///        「未计划类透传」语义由合成面（consumer_rename_test 驱动支）
///        钉死，不在语料 pass 重复。
class WholeCorpusResolver final : public jbc::consumer::IRenameResolver
{
public:
    explicit WholeCorpusResolver(const jbc::consumer::RenamePlan& plan) : plan_(plan) {}

    jbc::consumer::ConsumerResult<const jbc::consumer::RenamePlan*>
    planFor(std::string_view) override
    {
        const jbc::consumer::RenamePlan* sel = &plan_;
        return jbc::core::Ok(sel);
    }

private:
    const jbc::consumer::RenamePlan& plan_;
};

}   // namespace

TEST(ConsumerCorpusTest, RenameClassFamilyWholeCorpus)
{
    const std::vector<Corpus> corpora = loadCorpora();
    if (corpora.empty()) {
        GTEST_SKIP() << "Grunt 语料缺席（注入 LIBJBC_GRUNT_JAR / LIBJBC_GRUNT_JAR_ORIGIN）";
    }
    // 改名 pass 落在单语料上：origin 优先（干净基线语义更强），缺则 main。
    const Corpus* picked = nullptr;
    for (const Corpus& c : corpora) {
        if (c.tag == "origin") {
            picked = &c;
        }
    }
    if (picked == nullptr) {
        picked = &corpora.front();
    }
    const Corpus& c = *picked;

    ASSERT_GE(c.files.size(), ca::usize(2)) << "[" << c.tag << "] 语料过小，改名面无从谈起";
    const std::string oldName = c.files.front().first;
    const std::string newName = oldName + "X0";
    ASSERT_TRUE(jbc::type::isValidInternalName(newName)) << "改名后缀产出非法内部名";
    ASSERT_FALSE(hasName(c, newName)) << "新名与语料既有类冲突";

    // 改名前：旧名的引用文件集（事实族口径，含目标类自身的 thisClass）。
    const jbc::consumer::ReferenceScanner rs;
    std::vector<jbc::consumer::ReferenceSummary> preFacts;
    preFacts.reserve(c.files.size());
    for (const auto& kv : c.files) {
        const Bytes& bytes = kv.second;
        auto r = rs.scan(ByteView{bytes.data(), bytes.size()});
        ASSERT_TRUE(r.is_ok()) << "[" << c.tag << "] 预扫失败: " << kv.first;
        preFacts.push_back(std::move(r).unwrap());
    }
    const std::set<std::string> referrers = referFiles(preFacts, oldName);
    ASSERT_FALSE(referrers.empty()) << "目标类自身未入引用集？（thisClass 应命中）";

    jbc::consumer::RenamePlan plan;
    plan.renameClass(oldName, newName);
    WholeCorpusResolver resolver(plan);

    jbc::consumer::MemoryClassSource in;
    for (const auto& kv : c.files) {
        in.add(kv.first, kv.second);
    }
    auto r = jbc::consumer::renameSource(in, resolver);
    ASSERT_TRUE(r.is_ok()) << "[" << c.tag << "] 驱动失败";
    auto stage = std::move(r).unwrap();

    EXPECT_EQ(stage.classCount(), in.classCount()) << "[" << c.tag << "] stage 尺寸不守恒";

    // ① 目标类字节相异（其余类是否被触碰由其引用面决定，非透传语义）。
    {
        auto sb = stage.bytes(oldName);
        ASSERT_TRUE(sb.is_ok()) << "[" << c.tag << "] stage 缺目标键";
        auto sbv = std::move(sb).unwrap();
        EXPECT_FALSE(sbv.equals(ByteView{c.files.front().second.data(), c.files.front().second.size()}))
            << "[" << c.tag << "] 目标类字节未动";
    }

    // ② 改名后事实族：旧名清零；新名引用文件集 == 改名前旧名引用集。
    std::vector<jbc::consumer::ReferenceSummary> postFacts;
    postFacts.reserve(c.files.size());
    for (const auto& kv : c.files) {
        auto sb = stage.bytes(kv.first);
        ASSERT_TRUE(sb.is_ok());
        auto sbv = std::move(sb).unwrap();
        auto pf  = rs.scan(sbv);
        ASSERT_TRUE(pf.is_ok()) << "[" << c.tag << "] 改名后重扫失败: " << kv.first;
        postFacts.push_back(std::move(pf).unwrap());
    }
    EXPECT_EQ(referFiles(postFacts, oldName).size(), ca::usize(0))
        << "[" << c.tag << "] 改名后事实族仍见旧名";
    const std::set<std::string> newReferrers = referFiles(postFacts, newName);
    // 目标类的 post thisClass 已是新名（stage 键不变，事实记录用 this）——
    // 期望集按「oldName → newName 顶替」规范化后比较。
    std::set<std::string> expectedNew = referrers;
    if (expectedNew.erase(oldName) > 0) {
        expectedNew.insert(newName);
    }
    EXPECT_EQ(newReferrers, expectedNew)
        << "[" << c.tag << "] 新名引用文件集 != 旧名引用文件集（顶替后）";
}

// ---------------------------------------------------------------------------
// BSM 闸 pass：混淆语料上的 Err(BsmTouched) 实证
// ---------------------------------------------------------------------------

TEST(ConsumerCorpusTest, BsmTouchedFailLoud)
{
    const std::vector<Corpus> corpora = loadCorpora();
    const Corpus* main = nullptr;
    for (const Corpus& c : corpora) {
        if (c.tag == "main") {
            main = &c;
        }
    }
    if (main == nullptr) {
        GTEST_SKIP() << "混淆语料缺席（注入 LIBJBC_GRUNT_JAR）";
    }
    const Corpus& c = *main;

    // 定位：BSM 条目 MethodHandle（kind>=5）载体指向语料内实成员。
    std::string owner, name, desc;
    for (const auto& kv : c.files) {
        const Bytes& bytes = kv.second;
        auto r = jbc::core::readClass(ByteView{bytes.data(), bytes.size()});
        ASSERT_TRUE(r.is_ok()) << "[" << c.tag << "] 读失败: " << kv.first;
        auto rr = std::move(r).unwrap();
        ClassFile& cf = *rr.file;
        if (auto* bsmSlot = cf.findClassAttr("BootstrapMethods"); bsmSlot != nullptr) {
            auto d = cf.decodeAttr(*bsmSlot);
            ASSERT_TRUE(d.is_ok()) << "[" << c.tag << "] BSM 解码失败: " << kv.first;
            auto bsm = std::move(d).unwrap();
            if (bsm->kind() == jbc::core::AttrKind::BootstrapMethods) {
                const auto& bm =
                    *static_cast<const jbc::core::BootstrapMethodsAttr*>(bsm);
                const jbc::core::ConstantPool& cp = cf.cp;
                for (const jbc::core::BootstrapMethodsAttr::Entry& e : bm.entries) {
                    if (!cp.valid(e.methodRef)
                        || cp.tagOf(e.methodRef) != jbc::core::CpTag::MethodHandle) {
                        continue;
                    }
                    const jbc::core::ConstantPool::Entry& mh = cp.at(e.methodRef);
                    if (mh.a < 5 || !cp.valid(mh.b)) {
                        continue;
                    }
                    const jbc::core::CpTag rt = cp.tagOf(mh.b);
                    if (rt != jbc::core::CpTag::Methodref
                        && rt != jbc::core::CpTag::InterfaceMethodref) {
                        continue;
                    }
                    const jbc::core::ConstantPool::Entry& ref = cp.at(mh.b);
                    if (!cp.valid(ref.a) || !cp.valid(ref.b)
                        || cp.tagOf(ref.b) != jbc::core::CpTag::NameAndType) {
                        continue;
                    }
                    const jbc::core::ConstantPool::Entry& nat = cp.at(ref.b);
                    if (!cp.isUtf8(nat.a) || !cp.isUtf8(nat.b)) {
                        continue;
                    }
                    owner = cf.className(ref.a);
                    name  = cp.utf8Of(nat.a);
                    desc  = cp.utf8Of(nat.b);
                    break;
                }
            }
        }
        if (!owner.empty()) {
            break;
        }
    }
    if (owner.empty()) {
        GTEST_SKIP() << "语料无方法形态 BSM 载体（kind>=5），闸面无从实证";
    }

    // 全语料逐类应用成员规则：carrier 必现 BsmTouched。（BSM 专属
    // synthetic 成员只经 indy 调用，v1 不改写 invoke 位——retargets
    // 计数在此不立断言，形状守恒由 Ok+Err == 全文件数叮嘱。）
    jbc::consumer::RenamePlan plan;
    plan.renameMethod(owner, name, desc, name + "X0");
    ca::usize touched   = 0;
    ca::usize okCount   = 0;
    for (const auto& kv : c.files) {
        const Bytes& bytes = kv.second;
        auto sr = jbc::ir::makeSessionFromBytes(ByteView{bytes.data(), bytes.size()});
        ASSERT_TRUE(sr.is_ok()) << "[" << c.tag << "] 会话失败: " << kv.first;
        auto sess  = std::move(sr).unwrap();
        const auto ar = jbc::consumer::applyClassRename(*sess, plan);
        if (ar.is_err()) {
            EXPECT_EQ(ar.unwrap_err().kind, jbc::consumer::RenameError::Kind::BsmTouched)
                << "[" << c.tag << "] 非 BsmTouched 失败: " << kv.first;
            ++touched;
        } else {
            ++okCount;
        }
    }
    EXPECT_GE(touched, ca::usize(1)) << "[" << c.tag << "] 载体类必现 BsmTouched，未见";
    EXPECT_EQ(touched + okCount, c.files.size()) << "[" << c.tag << "] 应用形状不守恒";
}
