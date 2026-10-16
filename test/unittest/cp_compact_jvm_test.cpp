/// @file        cp_compact_jvm_test.cpp
/// @brief       CP 压缩后的 JVM 真跑等价门（A8）：膨胀 → 压缩 → 反射面行为恒等。
///
/// 样本覆盖 Signature（泛型继承 + 泛型方法 + 桥接）、RuntimeVisible Annotations
/// （含 Class 载荷）、Exceptions、InnerClasses、ConstantValue、BootstrapMethods
/// （lambda）、LNT/LVT（-g）。膨胀池（大量未引用 internString）→ 压缩 → 落盘
/// → java 真跑，输出与原类逐字节一致——反射读到的注解/签名/泛型/异常面未损。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "jbc/jbc.hpp"
#include "test_class_bytes.hpp"

namespace fs = std::filesystem;
using namespace jbc::core;
using namespace jbc::ir;
using Bytes = std::vector<ca::u8>;

namespace {

bool toolPresentCp(const char* exe)
{
    const std::string cmd = std::string(exe) + " -version > nul 2>&1";
    return std::system(cmd.c_str()) == 0;
}

Bytes slurpCp(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return Bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void spitCp(const fs::path& p, const Bytes& bytes)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

/// 编译 test_resource/libjbc/cp_compact 样本（-g：带上 LNT/LVT 调试面；
/// --release 8：真跑门样本锚定 v52 形态——Nest 族三件自 v55/v61 才出现，
/// 本门的面在注解/泛型/异常/lambda，nest 面由语料压缩扫与 remapper JVM 门
/// 覆盖）。
bool compileCpSample(const fs::path& outDir)
{
    const fs::path srcDir = fs::path("test_resource/libjbc/cp_compact");
    std::error_code ec;
    if (!fs::exists(srcDir, ec) || ec) {
        return false;
    }
    fs::remove_all(outDir, ec);
    fs::create_directories(outDir, ec);
    std::string files;
    for (fs::recursive_directory_iterator it(srcDir, ec), end; it != end && !ec;
         it.increment(ec)) {
        if (!ec && it->is_regular_file() && it->path().extension() == ".java") {
            files += " ";
            files += it->path().string();
        }
    }
    if (files.empty()) {
        return false;
    }
    const std::string cmd = "javac -g --release 8 -encoding UTF-8 -d " + outDir.string()
        + files + " > nul 2>&1";
    return std::system(cmd.c_str()) == 0;
}

std::string runCpMain(const fs::path& dir, const std::string& mainClass)
{
    const fs::path outFile = fs::path("test") / "libjbc_cp_compact_run.txt";
    std::error_code ec;
    fs::create_directories(outFile.parent_path(), ec);
    const std::string cmd = "java -cp " + dir.string() + " " + mainClass + " > "
        + outFile.string() + " 2>&1";
    if (std::system(cmd.c_str()) != 0) {
        return {};
    }
    const Bytes out = slurpCp(outFile);
    return std::string(out.begin(), out.end());
}

}   // namespace

TEST(CpCompactJvm, ReflectionFaceSurvivesCompaction)
{
    if (!toolPresentCp("javac") || !toolPresentCp("java")) {
        GTEST_SKIP() << "JDK (javac/java) absent";
    }
    const fs::path origDir = fs::path("test") / "libjbc_cp_compact" / "orig";
    ASSERT_TRUE(compileCpSample(origDir)) << "sample compile failed";

    // 膨胀前 oracle：反射面（注解/泛型签名/异常/lambda/内部类）真跑输出。
    const std::string oracle = runCpMain(origDir, "cpprobe.Probe");
    ASSERT_FALSE(oracle.empty()) << "oracle run failed";
    EXPECT_NE(oracle.find("anno=probe/3/Runnable"), std::string::npos) << oracle;
    EXPECT_NE(oracle.find("super=java.lang.String"), std::string::npos) << oracle;
    EXPECT_NE(oracle.find("manno=calc/7/Fold"), std::string::npos) << oracle;
    EXPECT_NE(oracle.find("calc=probe-field/const-tag#42"), std::string::npos) << oracle;

    const Bytes probeBytes = slurpCp(origDir / "cpprobe" / "Probe.class");
    ASSERT_FALSE(probeBytes.empty());

    // 膨胀：300 个未引用 String 进池 → 压缩必须吐掉它们。
    {
        auto sr = makeSessionFromBytes(ByteView{probeBytes});
        ASSERT_TRUE(sr.is_ok());
        auto s = std::move(sr).unwrap();
        for (int i = 0; i < 300; ++i) {
            static_cast<void>(s->internString("unused-pad-" + std::to_string(i)));
        }
        const auto cr = compactConstantPool(*s);
        ASSERT_TRUE(cr.is_ok()) << "compact failed: code=" << int(cr.unwrap_err().code)
                                << " attr=" << cr.unwrap_err().attrName;
        const Bytes out = jbc::test::writeOk(s->backing());
        // 子序映射的最强形态：原类无死条目无重复 → 膨胀全部收回，产物与
        // 原类逐字节一致（BloatAndCompactReturnsToCanonicalShape 的真类版）。
        EXPECT_EQ(out, probeBytes) << "膨胀池压缩后未还原原类";

        // 重读 + 全解码：产物结构自洽。
        auto rr = readClass(ByteView{out});
        ASSERT_TRUE(rr.is_ok());
        auto rd = std::move(rr).unwrap();
        ASSERT_TRUE(decodeAllAttrs(*rd.file).is_ok());

        // 落盘：整包拷贝原类，Probe.class 用压缩产物覆盖。
        const fs::path runDir = fs::path("test") / "libjbc_cp_compact" / "compacted";
        std::error_code ec;
        fs::remove_all(runDir, ec);
        for (fs::recursive_directory_iterator it(origDir, ec), end; it != end && !ec;
             it.increment(ec)) {
            if (!ec && it->is_regular_file() && it->path().extension() == ".class") {
                const fs::path rel = fs::relative(it->path(), origDir, ec);
                spitCp(runDir / rel, slurpCp(it->path()));
            }
        }
        spitCp(runDir / "cpprobe" / "Probe.class", out);
    }

    const std::string after = runCpMain(
        fs::path("test") / "libjbc_cp_compact" / "compacted", "cpprobe.Probe");
    ASSERT_FALSE(after.empty()) << "compacted run failed";
    EXPECT_EQ(after, oracle) << "压缩后 JVM 真跑输出漂移";
}
