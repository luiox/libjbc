/// @file        corpus_util.hpp
/// @brief       F1 主门语料解析：env 注入 > 仓内自产（javac 现场编译）。
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

}   // namespace jbc::test::corpus
