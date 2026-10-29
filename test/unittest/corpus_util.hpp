/// @file        corpus_util.hpp
/// @brief       语料解析：env 注入 > 仓内自产（javac 现场编译）。
///
/// 最强护栏（>1000 方法帧 oracle、语料往返、attr/ir/insn 语料门）基于
/// 「仓内自产、JDK-only 依赖」的语料设施，仓内缺席即 skip：
///
/// 1. `LIBJBC_REAL_CLASS_DIR` 环境变量注入时语义不变（CI/本机注入任意语料目录），
///    注入但目录缺席按缺席处理（不静默回退，保持注入的权威性）。
/// 2. 缺省时用 javac 现场编译仓内样本源（test_resource/java/jbc_corpus）
///    到 test/libjbc_corpus/classes（生成产物目录，gitignore）。
///    javac 缺席时不硬失败；语料缺席时调用方 skip。
/// 3. 样本源与产物之间的陈旧检测：manifest 缺失、编译旗语指纹不符或任一
///    .java 比 manifest 新就重编。manifest 记录类数与旗语指纹，供调用方
///    输出量级信息。
#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace jbc::test::corpus {

namespace fs = std::filesystem;

/// @brief 语料来源阶段（决定依赖哪些门：env 缺席 skip、javac 缺席 skip）。
enum class Stage
{
    None,          ///< 无可用语料（调用方应 GTEST_SKIP）
    Env,           ///< LIBJBC_REAL_CLASS_DIR 注入
    SelfProduced,  ///< 仓内样本源现场编译产物
};

/// @brief Windows/POSIX 通用的命令静默执行（stdout/stderr 重定向到日志文件）。
inline int runQuiet(const std::string& cmd, const fs::path& log)
{
    const std::string full = cmd + " > \"" + log.string() + "\" 2>&1";
    return std::system(full.c_str());
}

inline bool toolPresent(const char* exe)
{
    const std::string cmd = std::string(exe) + " -version > nul 2>&1";
    return std::system(cmd.c_str()) == 0;
}

inline bool javacPresent()
{
    return toolPresent("javac");
}

inline fs::path sourcesDir()
{
    return fs::path("test_resource") / "java" / "jbc_corpus";
}

inline fs::path producedDir()
{
    return fs::path("test") / "libjbc_corpus" / "classes";
}

inline fs::path manifestPath()
{
    return fs::path("test") / "libjbc_corpus" / "corpus_manifest.txt";
}

/// @brief nest 语料根：样本需要 class file v61+（NestHost/NestMembers 自
///        v55、PermittedSubclasses 自 v61），主语料的 --release 8 产物不带
///        这三件，另立小语料现场编译（源同样入仓内 test_resource，产物属生成物）。
inline fs::path nestSourcesDir()
{
    return fs::path("test_resource") / "java" / "jbc_corpus_nest";
}

inline fs::path nestProducedDir()
{
    return fs::path("test") / "libjbc_corpus_nest" / "classes";
}

inline fs::path nestManifestPath()
{
    return fs::path("test") / "libjbc_corpus_nest" / "nest_manifest.txt";
}

/// @brief record 语料根：Record 属性自 class file v61 事实可用（javac
///        --release 16 产物），主语料的 --release 8 产物不带，另立小语料
///        现场编译（源同样入仓内 test_resource，产物属生成物）。
inline fs::path recordSourcesDir()
{
    return fs::path("test_resource") / "java" / "jbc_corpus_record";
}

inline fs::path recordProducedDir()
{
    return fs::path("test") / "libjbc_corpus_record" / "classes";
}

inline fs::path recordManifestPath()
{
    return fs::path("test") / "libjbc_corpus_record" / "record_manifest.txt";
}

/// @brief module 语料根：Module 属性自 class file v53（Java 9）事实可用，
///        module-info 只有 --release 9+ 的 javac 才产；主语料的 --release 8
///        产物不带，另立小语料现场编译（源同样入仓内 test_resource，产物属生成物）。
inline fs::path moduleSourcesDir()
{
    return fs::path("test_resource") / "java" / "jbc_corpus_module";
}

inline fs::path moduleProducedDir()
{
    return fs::path("test") / "libjbc_corpus_module" / "classes";
}

inline fs::path moduleManifestPath()
{
    return fs::path("test") / "libjbc_corpus_module" / "module_manifest.txt";
}

/// @brief 编译旗语指纹（manifest 标记行）：旗语升级即整批重编。
///
/// `-g` 产出 LVT/LVTT 调试属性——LVTT 签名列是泛型签名的第二处宿主，
/// 语料门需要它在自产模式下可复现（env 注入模式的分布另计）。
constexpr const char* kCompileFlagsMark = "flags=-g";

inline std::vector<fs::path> collectSources()
{
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::directory_iterator it(sourcesDir(), ec), end; it != end && !ec; it.increment(ec)) {
        if (it->is_regular_file() && it->path().extension() == ".java") {
            out.push_back(it->path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// @brief 产物是否缺失或比样本源旧（源有更新即重编）。
inline bool buildNeeded()
{
    std::error_code ec;
    if (!fs::exists(manifestPath(), ec) || ec) {
        return true;
    }
    // 编译旗语指纹：manifest 缺当前旗语即重编（旗语升级如补 -g 产出 LVT/LVTT）。
    {
        std::ifstream in(manifestPath());
        std::string line;
        bool hasVarsDebug = false;
        while (std::getline(in, line)) {
            if (line == kCompileFlagsMark) {
                hasVarsDebug = true;
            }
        }
        if (!hasVarsDebug) {
            return true;
        }
    }
    const auto marker = fs::last_write_time(manifestPath(), ec);
    if (ec) {
        return true;
    }
    for (const fs::path& src : collectSources()) {
        const auto t = fs::last_write_time(src, ec);
        if (!ec && t > marker) {
            return true;
        }
    }
    return false;
}

/// @brief 现场编译仓内样本源。javac 缺席返回 false（调用方降级，不失败）；
///        源存在且编译失败是仓内缺陷，直接 ADD_FAILURE（红）而不是静默 skip。
inline bool produceSelfCorpus()
{
    const std::vector<fs::path> sources = collectSources();
    if (sources.empty()) {
        return false;   // 不在仓库根运行（cwd 不对）或样本被删：按缺席降级
    }
    if (!javacPresent()) {
        return false;
    }
    std::error_code ec;
    fs::remove_all(producedDir(), ec);
    fs::create_directories(producedDir(), ec);
    std::string files;
    for (const fs::path& src : sources) {
        files += " \"";
        files += src.string();
        files += "\"";
    }
    const std::string cmd = "javac --release 8 -g -encoding UTF-8 -d \""
        + producedDir().string() + "\"" + files;
    const fs::path log = fs::path("test") / "libjbc_corpus" / "javac.log";
    fs::create_directories(log.parent_path(), ec);
    const int rc = runQuiet(cmd, log);
    if (rc != 0) {
        ADD_FAILURE() << "corpus javac failed (rc=" << rc << "), log: " << log.string();
        return false;
    }
    // manifest：类数记录 + 陈旧检测的时间戳基准。
    ca::usize classes = 0;
    for (fs::recursive_directory_iterator it(producedDir(), ec), end; it != end && !ec;
         it.increment(ec)) {
        if (!ec && it->is_regular_file() && it->path().extension() == ".class") {
            ++classes;
        }
    }
    std::ofstream manifest(manifestPath(), std::ios::binary | std::ios::trunc);
    manifest << "classes=" << classes << "\n" << kCompileFlagsMark << "\n";
    return classes > 0;
}

inline bool dirHasClassFiles(const fs::path& dir)
{
    std::error_code ec;
    if (!fs::exists(dir, ec) || ec) {
        return false;
    }
    for (fs::recursive_directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
        if (!ec && it->is_regular_file() && it->path().extension() == ".class") {
            return true;
        }
    }
    return false;
}

/// @brief nest 语料编译旗语指纹（manifest 标记行）：旗语升级即整批重编。
constexpr const char* kNestCompileFlagsMark = "flags=-g --release 17";

inline std::vector<fs::path> collectNestSources()
{
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::directory_iterator it(nestSourcesDir(), ec), end; it != end && !ec;
         it.increment(ec)) {
        if (it->is_regular_file() && it->path().extension() == ".java") {
            out.push_back(it->path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// @brief javac 是否具备 --release 17 能力（JDK 9+ 才有 --release；值高于
///        本体版本会报错）。能力缺席属环境问题，调用方按语料缺席降级。
inline bool javacSupportsRelease17()
{
    if (!javacPresent()) {
        return false;
    }
    const fs::path log = fs::path("test") / "libjbc_corpus_nest" / "javac_probe.log";
    std::error_code ec;
    fs::create_directories(log.parent_path(), ec);
    return runQuiet("javac --release 17 -help", log) == 0;
}

/// @brief nest 语料是否缺失或比样本源旧（语义同 buildNeeded）。
inline bool nestBuildNeeded()
{
    std::error_code ec;
    if (!fs::exists(nestManifestPath(), ec) || ec) {
        return true;
    }
    {
        std::ifstream in(nestManifestPath());
        std::string line;
        bool hasMark = false;
        while (std::getline(in, line)) {
            if (line == kNestCompileFlagsMark) {
                hasMark = true;
            }
        }
        if (!hasMark) {
            return true;
        }
    }
    const auto marker = fs::last_write_time(nestManifestPath(), ec);
    if (ec) {
        return true;
    }
    for (const fs::path& src : collectNestSources()) {
        const auto t = fs::last_write_time(src, ec);
        if (!ec && t > marker) {
            return true;
        }
    }
    return false;
}

/// @brief 现场编译 nest 语料。返回 false = 语料缺席（能力或源缺失，调用方
///        降级）；javac 能力具备但样本源编译失败是仓内缺陷，直接 ADD_FAILURE。
inline bool produceNestCorpus()
{
    const std::vector<fs::path> sources = collectNestSources();
    if (sources.empty()) {
        return false;   // 不在仓库根运行或样本被删：按缺席降级
    }
    if (!javacSupportsRelease17()) {
        return false;   // JDK 能力不足（<17）：环境问题，降级不硬红
    }
    std::error_code ec;
    fs::remove_all(nestProducedDir(), ec);
    fs::create_directories(nestProducedDir(), ec);
    std::string files;
    for (const fs::path& src : sources) {
        files += " \"";
        files += src.string();
        files += "\"";
    }
    const std::string cmd = "javac --release 17 -g -encoding UTF-8 -d \""
        + nestProducedDir().string() + "\"" + files;
    const fs::path log = fs::path("test") / "libjbc_corpus_nest" / "javac.log";
    fs::create_directories(log.parent_path(), ec);
    const int rc = runQuiet(cmd, log);
    if (rc != 0) {
        ADD_FAILURE() << "nest corpus javac failed (rc=" << rc << "), log: "
                      << log.string();
        return false;
    }
    ca::usize classes = 0;
    for (fs::recursive_directory_iterator it(nestProducedDir(), ec), end; it != end && !ec;
         it.increment(ec)) {
        if (!ec && it->is_regular_file() && it->path().extension() == ".class") {
            ++classes;
        }
    }
    std::ofstream manifest(nestManifestPath(), std::ios::binary | std::ios::trunc);
    manifest << "classes=" << classes << "\n" << kNestCompileFlagsMark << "\n";
    return classes > 0;
}

/// @brief 解析结果：类文件根目录 + 来源阶段。stage == None 时调用方 skip。
struct CorpusSource
{
    std::string dir;
    Stage stage = Stage::None;
};

/// @brief 解析序：env 注入 > 仓内自产。
///
/// env 注入的语义与批前一致：环境变量指向的目录存在即用，不存在则整体缺席
/// （注入是权威指令，不静默改道）。仓内自产在产物缺席/陈旧且 javac 在位时
/// 现场编译；javac 缺席则调用方 skip。
inline CorpusSource resolveCorpus()
{
    CorpusSource out;
    if (const char* env = std::getenv("LIBJBC_REAL_CLASS_DIR")) {
        const std::string dir = std::string(env);
        if (dirHasClassFiles(fs::path(dir))) {
            out.dir = dir;
            out.stage = Stage::Env;
        }
        return out;
    }
    const bool needBuild = buildNeeded();
    const bool haveOutput = dirHasClassFiles(producedDir());
    if (!haveOutput || needBuild) {
        produceSelfCorpus();
    }
    if (dirHasClassFiles(producedDir())) {
        out.dir = producedDir().string();
        out.stage = Stage::SelfProduced;
        return out;
    }
    return out;
}

/// @brief 递归收集 .class 文件（语料门通用）。
inline std::vector<std::string> collectClassFiles(const std::string& root)
{
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(fs::path(root), ec), end; it != end && !ec;
         it.increment(ec)) {
        if (!ec && it->is_regular_file() && it->path().extension() == ".class") {
            out.push_back(it->path().string());
        }
    }
    return out;
}

/// @brief 语料门统一跳过话术（skip 计数的归因可读性）。
inline std::string skipReason(const CorpusSource& s)
{
    switch (s.stage) {
    case Stage::None: return "corpus unavailable (javac/JDK absent and no local corpus)";
    case Stage::Env: return "env corpus empty";
    case Stage::SelfProduced: return "self-produced corpus empty";
    }
    return "corpus unavailable";
}

/// @brief nest 语料解析：命中 = 自产模式产物根（Stage::SelfProduced）；缺席
///        返回 stage == None（调用方按需降级）。env 注入主语料已覆盖 nest
///        三件的面（注入语料可能自带），本根只服务自产侧分布硬断言与字节门。
inline CorpusSource resolveNestCorpus()
{
    CorpusSource out;
    if (nestBuildNeeded() || !dirHasClassFiles(nestProducedDir())) {
        produceNestCorpus();
    }
    if (dirHasClassFiles(nestProducedDir())) {
        out.dir   = nestProducedDir().string();
        out.stage = Stage::SelfProduced;
    }
    return out;
}

/// @brief record 语料编译旗语指纹（manifest 标记行）：旗语升级即整批重编。
constexpr const char* kRecordCompileFlagsMark = "flags=-g --release 16";

inline std::vector<fs::path> collectRecordSources()
{
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::directory_iterator it(recordSourcesDir(), ec), end; it != end && !ec;
         it.increment(ec)) {
        if (it->is_regular_file() && it->path().extension() == ".java") {
            out.push_back(it->path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// @brief javac 是否具备 --release 16 能力（record 形态的事实下界；JDK 16+
///        才能编译，能力缺席属环境问题，调用方按语料缺席降级）。
inline bool javacSupportsRelease16()
{
    if (!javacPresent()) {
        return false;
    }
    const fs::path log = fs::path("test") / "libjbc_corpus_record" / "javac_probe.log";
    std::error_code ec;
    fs::create_directories(log.parent_path(), ec);
    return runQuiet("javac --release 16 -help", log) == 0;
}

/// @brief record 语料是否缺失或比样本源旧（语义同 buildNeeded）。
inline bool recordBuildNeeded()
{
    std::error_code ec;
    if (!fs::exists(recordManifestPath(), ec) || ec) {
        return true;
    }
    {
        std::ifstream in(recordManifestPath());
        std::string line;
        bool hasMark = false;
        while (std::getline(in, line)) {
            if (line == kRecordCompileFlagsMark) {
                hasMark = true;
            }
        }
        if (!hasMark) {
            return true;
        }
    }
    const auto marker = fs::last_write_time(recordManifestPath(), ec);
    if (ec) {
        return true;
    }
    for (const fs::path& src : collectRecordSources()) {
        const auto t = fs::last_write_time(src, ec);
        if (!ec && t > marker) {
            return true;
        }
    }
    return false;
}

/// @brief 现场编译 record 语料。返回 false = 语料缺席（能力或源缺失，调用方
///        降级）；javac 能力具备但样本源编译失败是仓内缺陷，直接 ADD_FAILURE。
inline bool produceRecordCorpus()
{
    const std::vector<fs::path> sources = collectRecordSources();
    if (sources.empty()) {
        return false;   // 不在仓库根运行或样本被删：按缺席降级
    }
    if (!javacSupportsRelease16()) {
        return false;   // JDK 能力不足（<16）：环境问题，降级不硬红
    }
    std::error_code ec;
    fs::remove_all(recordProducedDir(), ec);
    fs::create_directories(recordProducedDir(), ec);
    std::string files;
    for (const fs::path& src : sources) {
        files += " \"";
        files += src.string();
        files += "\"";
    }
    const std::string cmd = "javac --release 16 -g -encoding UTF-8 -d \""
        + recordProducedDir().string() + "\"" + files;
    const fs::path log = fs::path("test") / "libjbc_corpus_record" / "javac.log";
    fs::create_directories(log.parent_path(), ec);
    const int rc = runQuiet(cmd, log);
    if (rc != 0) {
        ADD_FAILURE() << "record corpus javac failed (rc=" << rc << "), log: "
                      << log.string();
        return false;
    }
    ca::usize classes = 0;
    for (fs::recursive_directory_iterator it(recordProducedDir(), ec), end; it != end && !ec;
         it.increment(ec)) {
        if (!ec && it->is_regular_file() && it->path().extension() == ".class") {
            ++classes;
        }
    }
    std::ofstream manifest(recordManifestPath(), std::ios::binary | std::ios::trunc);
    manifest << "classes=" << classes << "\n" << kRecordCompileFlagsMark << "\n";
    return classes > 0;
}

/// @brief record 语料解析：命中 = 自产模式产物根（Stage::SelfProduced）；缺席
///        返回 stage == None（调用方按需降级）。env 注入主语料的 Record 分布
///        是显式事实（注入语料可能自带，只记录不硬断言），本根服务自产侧分布
///        硬断言（组件嵌套 Signature/注解面由样本保证）与字节门。
inline CorpusSource resolveRecordCorpus()
{
    CorpusSource out;
    if (recordBuildNeeded() || !dirHasClassFiles(recordProducedDir())) {
        produceRecordCorpus();
    }
    if (dirHasClassFiles(recordProducedDir())) {
        out.dir   = recordProducedDir().string();
        out.stage = Stage::SelfProduced;
    }
    return out;
}

/// @brief module 语料编译旗语指纹（manifest 标记行）：旗语升级即整批重编。
///        与 nest 语料同为 --release 17（module-info 指令形态的事实下界远低
///        于 17，取与 nest 同一能力探针，不再单设 release 探测）。
constexpr const char* kModuleCompileFlagsMark = "flags=-g --release 17";

inline std::vector<fs::path> collectModuleSources()
{
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::directory_iterator it(moduleSourcesDir(), ec), end; it != end && !ec;
         it.increment(ec)) {
        if (it->is_regular_file() && it->path().extension() == ".java") {
            out.push_back(it->path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// @brief module 语料是否缺失或比样本源旧（语义同 buildNeeded）。
inline bool moduleBuildNeeded()
{
    std::error_code ec;
    if (!fs::exists(moduleManifestPath(), ec) || ec) {
        return true;
    }
    {
        std::ifstream in(moduleManifestPath());
        std::string line;
        bool hasMark = false;
        while (std::getline(in, line)) {
            if (line == kModuleCompileFlagsMark) {
                hasMark = true;
            }
        }
        if (!hasMark) {
            return true;
        }
    }
    const auto marker = fs::last_write_time(moduleManifestPath(), ec);
    if (ec) {
        return true;
    }
    for (const fs::path& src : collectModuleSources()) {
        const auto t = fs::last_write_time(src, ec);
        if (!ec && t > marker) {
            return true;
        }
    }
    return false;
}

/// @brief 现场编译 module 语料。样本全部平铺在语料根（包名写在源内，产物
///        布局由 -d 展开），module-info.java 与全部类源同一次 javac 调用编
///        译——module 指令只对同调用的类生效。返回 false = 语料缺席（能力
///        或源缺失，调用方降级）；javac 能力具备但样本源编译失败是仓内缺
///        陷，直接 ADD_FAILURE。
inline bool produceModuleCorpus()
{
    const std::vector<fs::path> sources = collectModuleSources();
    if (sources.empty()) {
        return false;   // 不在仓库根运行或样本被删：按缺席降级
    }
    if (!javacSupportsRelease17()) {
        return false;   // JDK 能力不足（<17）：环境问题，降级不硬红
    }
    std::error_code ec;
    fs::remove_all(moduleProducedDir(), ec);
    fs::create_directories(moduleProducedDir(), ec);
    std::string files;
    for (const fs::path& src : sources) {
        files += " \"";
        files += src.string();
        files += "\"";
    }
    const std::string cmd = "javac --release 17 -g -encoding UTF-8 -d \""
        + moduleProducedDir().string() + "\"" + files;
    const fs::path log = fs::path("test") / "libjbc_corpus_module" / "javac.log";
    fs::create_directories(log.parent_path(), ec);
    const int rc = runQuiet(cmd, log);
    if (rc != 0) {
        ADD_FAILURE() << "module corpus javac failed (rc=" << rc << "), log: "
                      << log.string();
        return false;
    }
    ca::usize classes = 0;
    for (fs::recursive_directory_iterator it(moduleProducedDir(), ec), end;
         it != end && !ec; it.increment(ec)) {
        if (!ec && it->is_regular_file() && it->path().extension() == ".class") {
            ++classes;
        }
    }
    std::ofstream manifest(moduleManifestPath(), std::ios::binary | std::ios::trunc);
    manifest << "classes=" << classes << "\n" << kModuleCompileFlagsMark << "\n";
    return classes > 0;
}

/// @brief module 语料解析：命中 = 自产模式产物根（Stage::SelfProduced）；缺席
///        返回 stage == None（调用方按需降级）。module-info 类不在 env 注入
///        主语料的分布内（注入语料无模块面），本根只服务自产侧。
inline CorpusSource resolveModuleCorpus()
{
    CorpusSource out;
    if (moduleBuildNeeded() || !dirHasClassFiles(moduleProducedDir())) {
        produceModuleCorpus();
    }
    if (dirHasClassFiles(moduleProducedDir())) {
        out.dir   = moduleProducedDir().string();
        out.stage = Stage::SelfProduced;
    }
    return out;
}

/// @brief annot 语料根：注解族属性（AnnotationDefault/参数注解双保留）+
///        MethodParameters（javac -parameters 旗语产物；主语料的 --release 8
///        基础旗语不产参数名表），另立小语料现场编译（源同样入仓内
///        test_resource，产物属生成物）。
inline fs::path annotSourcesDir()
{
    return fs::path("test_resource") / "java" / "jbc_corpus_annot";
}

inline fs::path annotProducedDir()
{
    return fs::path("test") / "libjbc_corpus_annot" / "classes";
}

inline fs::path annotManifestPath()
{
    return fs::path("test") / "libjbc_corpus_annot" / "annot_manifest.txt";
}

/// @brief annot 语料编译旗语指纹（manifest 标记行）：旗语升级即整批重编。
///        -parameters 产出 MethodParameters——注解族语料门需要它在自产模式
///        下可复现（env 注入模式的分布另计）。
constexpr const char* kAnnotCompileFlagsMark = "flags=-g -parameters --release 8";

inline std::vector<fs::path> collectAnnotSources()
{
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::directory_iterator it(annotSourcesDir(), ec), end; it != end && !ec;
         it.increment(ec)) {
        if (it->is_regular_file() && it->path().extension() == ".java") {
            out.push_back(it->path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// @brief annot 语料是否缺失或比样本源旧（语义同 buildNeeded）。
inline bool annotBuildNeeded()
{
    std::error_code ec;
    if (!fs::exists(annotManifestPath(), ec) || ec) {
        return true;
    }
    {
        std::ifstream in(annotManifestPath());
        std::string line;
        bool hasMark = false;
        while (std::getline(in, line)) {
            if (line == kAnnotCompileFlagsMark) {
                hasMark = true;
            }
        }
        if (!hasMark) {
            return true;
        }
    }
    const auto marker = fs::last_write_time(annotManifestPath(), ec);
    if (ec) {
        return true;
    }
    for (const fs::path& src : collectAnnotSources()) {
        const auto t = fs::last_write_time(src, ec);
        if (!ec && t > marker) {
            return true;
        }
    }
    return false;
}

/// @brief 现场编译 annot 语料。--release 8 与主语料同一能力下界（javac 在位
///        即可编，不单设 release 探测）；javac 缺席返回 false（调用方降级，
///        不失败）；源存在且编译失败是仓内缺陷，直接 ADD_FAILURE（红）。
inline bool produceAnnotCorpus()
{
    const std::vector<fs::path> sources = collectAnnotSources();
    if (sources.empty()) {
        return false;   // 不在仓库根运行或样本被删：按缺席降级
    }
    if (!javacPresent()) {
        return false;
    }
    std::error_code ec;
    fs::remove_all(annotProducedDir(), ec);
    fs::create_directories(annotProducedDir(), ec);
    std::string files;
    for (const fs::path& src : sources) {
        files += " \"";
        files += src.string();
        files += "\"";
    }
    const std::string cmd = "javac --release 8 -g -parameters -encoding UTF-8 -d \""
        + annotProducedDir().string() + "\"" + files;
    const fs::path log = fs::path("test") / "libjbc_corpus_annot" / "javac.log";
    fs::create_directories(log.parent_path(), ec);
    const int rc = runQuiet(cmd, log);
    if (rc != 0) {
        ADD_FAILURE() << "annot corpus javac failed (rc=" << rc << "), log: "
                      << log.string();
        return false;
    }
    ca::usize classes = 0;
    for (fs::recursive_directory_iterator it(annotProducedDir(), ec), end; it != end && !ec;
         it.increment(ec)) {
        if (!ec && it->is_regular_file() && it->path().extension() == ".class") {
            ++classes;
        }
    }
    std::ofstream manifest(annotManifestPath(), std::ios::binary | std::ios::trunc);
    manifest << "classes=" << classes << "\n" << kAnnotCompileFlagsMark << "\n";
    return classes > 0;
}

/// @brief annot 语料解析：命中 = 自产模式产物根（Stage::SelfProduced）；缺席
///        返回 stage == None（调用方按需降级）。本根不消费 env 注入（参数名
///        表/注解默认值的分布由注入语料自带，自产侧另设硬断言）。
inline CorpusSource resolveAnnotCorpus()
{
    CorpusSource out;
    if (annotBuildNeeded() || !dirHasClassFiles(annotProducedDir())) {
        produceAnnotCorpus();
    }
    if (dirHasClassFiles(annotProducedDir())) {
        out.dir   = annotProducedDir().string();
        out.stage = Stage::SelfProduced;
    }
    return out;
}

}   // namespace jbc::test::corpus
