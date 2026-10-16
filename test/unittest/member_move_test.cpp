/// @file        member_move_test.cpp
/// @brief       A3/A4/A5 的 JVM 真跑门（JDK 在位时执行，缺席 skip）：
///   1. 跨类搬移——两个 javac 产物类，static 字段 + static 方法从供体搬入
///      宿主（adoptField/adoptMethod + 消费者侧 owner 重映射），宿主单独
///      落盘后 java 真跑：调用搬过去的 static 成员、反射核对字段值，输出
///      与搬移前供体侧 oracle 等价；隔离 classpath（无供体类）证明搬移
///      产物零残留依赖。供体含分支体回归面（if 前向/goto 后向/
///      tableswitch/lookupswitch/异常表/体内私有成员引用）——分支体重放
///      双缺陷（§34.3）在场时 adopt 必败。
///   2. 从零建类——makeSession(NewClassSeed) 空类起步 + addMethod 构建
///      main，写出的类 java 真跑出输出。
#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "jbc/jbc.hpp"
#include "test_class_bytes.hpp"

using namespace jbc::core;
using Bytes = std::vector<ca::u8>;

namespace {

namespace fs = std::filesystem;

#ifdef _WIN32
constexpr const char* kEol = "\r\n";
#else
constexpr const char* kEol = "\n";
#endif

std::string expectedOutput()
{
    std::string out = "grab=7";
    out += kEol;
    out += "stash=7";
    out += kEol;
    out += "classify=62";
    out += kEol;
    out += "pickName=2";
    out += kEol;
    return out;
}

bool toolPresent(const char* exe)
{
    const std::string cmd = std::string(exe) + " -version > nul 2>&1";
    return std::system(cmd.c_str()) == 0;
}

bool jdkPresent()
{
    return toolPresent("javac") && toolPresent("java");
}

Bytes slurpFile(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return Bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void spitFile(const fs::path& p, const Bytes& bytes)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

/// 编译 test_resource/libjbc/member_move 下的样本（--release 8：拼接走
/// StringBuilder，CP 无 indy/condy 条目；无 -g：搬移契约不随迁调试属性，
/// 样本保持最小形态）。
bool compileSample(const fs::path& outDir)
{
    const fs::path srcDir = fs::path("test_resource/libjbc/member_move");
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
    const std::string cmd = "javac -encoding UTF-8 --release 8 -d " + outDir.string()
        + files + " > nul 2>&1";
    return std::system(cmd.c_str()) == 0;
}

std::string runMain(const fs::path& dir, const std::string& mainClass)
{
    const fs::path outFile = fs::path("test") / "libjbc_member_move_run.txt";
    std::error_code ec;
    fs::create_directories(outFile.parent_path(), ec);
    const std::string cmd = "java -cp " + dir.string() + " " + mainClass + " > "
        + outFile.string() + " 2>&1";
    if (std::system(cmd.c_str()) != 0) {
        return {};
    }
    const Bytes out = slurpFile(outFile);
    return std::string(out.begin(), out.end());
}

std::shared_ptr<jbc::ir::ClassSession> sessionOf(const Bytes& bytes)
{
    auto r = jbc::ir::makeSessionFromBytes(ByteView{bytes.data(), bytes.size()});
    return r.is_ok() ? std::move(r).unwrap() : nullptr;
}

}   // namespace

// ===========================================================================
// 跨类搬移：static 字段 + static 方法甲搬乙，JVM 真跑行为等价
// ===========================================================================

TEST(MemberMoveJvm, CrossClassStaticMoveKeepsBehavior)
{
    if (!jdkPresent()) {
        GTEST_SKIP() << "JDK (javac/java) absent";
    }
    const fs::path origDir = fs::path("test") / "libjbc_member_move" / "orig";
    ASSERT_TRUE(compileSample(origDir)) << "sample compile failed";

    // 搬移前 oracle：供体侧反射读 grab/stash。
    EXPECT_EQ(runMain(origDir, "demo.DonorMain"), expectedOutput());

    const Bytes donorBytes = slurpFile(origDir / "demo" / "Donor.class");
    const Bytes hostBytes  = slurpFile(origDir / "demo" / "Host.class");
    ASSERT_FALSE(donorBytes.empty());
    ASSERT_FALSE(hostBytes.empty());
    auto donor = sessionOf(donorBytes);
    auto host  = sessionOf(hostBytes);
    ASSERT_NE(donor, nullptr);
    ASSERT_NE(host, nullptr);

    const jbc::ir::FieldId donorStash = donor->unit().findFieldId("stash");
    const jbc::ir::MethodId donorGrab = donor->unit().findMethodId("grab", "()I");
    const jbc::ir::MethodId donorClassify =
        donor->unit().findMethodId("classify", "(I)I");
    const jbc::ir::MethodId donorPickName =
        donor->unit().findMethodId("pickName", "(Ljava/lang/String;)I");
    const jbc::ir::MethodId donorDive = donor->unit().findMethodId("dive", "(I)I");
    ASSERT_NE(donorStash, jbc::ir::kNoFieldId);
    ASSERT_NE(donorGrab, jbc::ir::kNoMethodId);
    ASSERT_NE(donorClassify, jbc::ir::kNoMethodId);
    ASSERT_NE(donorPickName, jbc::ir::kNoMethodId);
    ASSERT_NE(donorDive, jbc::ir::kNoMethodId);

    // 甲搬乙：字段（元数据）+ 方法（元数据 + IR 重放）。classify/pickName
    // 是分支体回归面（if 前向/goto 后向/tableswitch/lookupswitch/异常表），
    // dive 随 classify 的调用引用一并搬入。
    auto movedField  = host->adoptField(*donor, donorStash);
    auto movedMethod = host->adoptMethod(*donor, donorGrab);
    auto movedClassify = host->adoptMethod(*donor, donorClassify);
    auto movedPickName = host->adoptMethod(*donor, donorPickName);
    auto movedDive     = host->adoptMethod(*donor, donorDive);
    ASSERT_TRUE(movedField.is_ok());
    ASSERT_TRUE(movedMethod.is_ok());
    ASSERT_TRUE(movedClassify.is_ok());
    ASSERT_TRUE(movedPickName.is_ok());
    ASSERT_TRUE(movedDive.is_ok());

    // 消费者侧 owner 重映射（ToolClassCombiner 的核心语义）：搬入体内引用
    // 供体自身的成员改为引用宿主自身。
    const jbc::ir::MethodId hostGrab     = movedMethod.unwrap();
    const jbc::ir::MethodId hostClassify = movedClassify.unwrap();
    const jbc::ir::MethodId hostPickName = movedPickName.unwrap();
    const jbc::ir::MethodId hostDive     = movedDive.unwrap();
    for (const jbc::ir::MethodId mid :
         {hostGrab, hostClassify, hostPickName, hostDive}) {
        auto body = jbc::ir::promoteMethod(*host, mid);
        ASSERT_TRUE(body.is_ok());
        {
            jbc::ir::CodeUnit& cu = *body.unwrap();
            for (const jbc::ir::InsnId iid : cu.dom.order()) {
                auto got = cu.dom.get(iid);
                ASSERT_TRUE(got.is_ok());
                jbc::ir::Insn* in = got.unwrap();
                jbc::ir::ClassSession::MemberSem sem;
                if (host->memberOf(*in, sem)
                    && sem.owner == "demo/Donor") {
                    ASSERT_TRUE(host->setMember(*in, sem.kind, "demo/Host",
                                                sem.name, sem.desc));
                }
            }
            ASSERT_TRUE(jbc::ir::lowerMethod(*host, mid).is_ok());
        }
    }

    // §34.3(4)(b) 消费者义务面：分支体并入 v51+ 目标类必须由消费者再生
    // SMT（libjbc 不自动接线；上层 combiner 消费方经 frame_rewire 收口，有独立
    // JVM 门）。本门聚焦重放保真与零残留依赖，宿主钉 v49 走旧校验器
    // （样本 --release 8 编译，CP 无 indy/condy，降档合法），帧再生职责
    // 不在此重复。
    host->unit().version = (49u << 16);
    ASSERT_TRUE(host->commitClassMetadata().is_ok());

    // 宿主单独落盘：隔离目录里只有 Host.class——跑得起来即证明零残留依赖。
    auto outBytes = jbc::test::writeOk(host->backing());
    ASSERT_FALSE(outBytes.empty());
    const fs::path hostOnlyDir = fs::path("test") / "libjbc_member_move" / "host_only";
    std::error_code ec;
    fs::remove_all(hostOnlyDir, ec);
    spitFile(hostOnlyDir / "demo" / "Host.class", outBytes);

    EXPECT_EQ(runMain(hostOnlyDir, "demo.Host"), expectedOutput());

    // 残留依赖静态复核：宿主全部方法体内不再有指向供体的成员/类引用。
    auto reread = sessionOf(outBytes);
    ASSERT_NE(reread, nullptr);
    for (auto& visit : jbc::ir::collectCodeUnits(*reread)) {
        ASSERT_NE(visit.kind, jbc::ir::CodeVisit::Kind::Failed);
        if (visit.kind != jbc::ir::CodeVisit::Kind::Ok) {
            continue;
        }
        const jbc::ir::CodeUnit& cu = *visit.unit.get();
        for (const jbc::ir::InsnId iid : cu.dom.order()) {
            const jbc::ir::Insn* in = cu.dom.peek(iid);
            ASSERT_NE(in, nullptr);
            jbc::ir::ClassSession::MemberSem sem;
            if (reread->memberOf(*in, sem)) {
                EXPECT_NE(sem.owner, "demo/Donor");
            }
            const std::string_view cn = reread->classOf(*in);
            if (!cn.empty()) {
                EXPECT_NE(cn, "demo/Donor");
            }
        }
    }
}

// ===========================================================================
// 从零建类：空类起步构建 main，JVM 真跑出输出
// ===========================================================================

TEST(MemberMoveJvm, FreshBuiltClassRunsOnJvm)
{
    if (!jdkPresent()) {
        GTEST_SKIP() << "JDK (javac/java) absent";
    }

    jbc::ir::NewClassSeed seed;
    seed.access    = 0x0021;
    seed.thisName  = "freshout/Main";
    seed.superName = "java/lang/Object";
    auto r         = jbc::ir::makeSession(seed);
    ASSERT_TRUE(r.is_ok());
    std::shared_ptr<jbc::ir::ClassSession> s = std::move(r).unwrap();

    jbc::ir::MethodSeed main;
    main.name      = "main";
    main.desc      = "([Ljava/lang/String;)V";
    main.access    = 0x0009;
    main.withCode  = true;
    main.maxStack  = 2;
    main.maxLocals = 1;
    auto added     = s->addMethod(main);
    ASSERT_TRUE(added.is_ok());
    const jbc::ir::MethodId id = added.unwrap();

    auto p = jbc::ir::promoteMethod(*s, id);
    ASSERT_TRUE(p.is_ok());
    {
        jbc::ir::CodeEditor ed(*p.unwrap());
        const ca::u32 outIdx   = s->internFieldRef("java/lang/System", "out",
                                                   "Ljava/io/PrintStream;");
        const ca::u32 msgIdx   = s->internString("fresh-build-ok");
        const ca::u32 printIdx = s->internMethodRef("java/io/PrintStream", "println",
                                                    "(Ljava/lang/String;)V");
        ASSERT_TRUE(ed.append(jbc::ir::CodeEditor::cpInsn(Op::GETSTATIC, outIdx)).is_ok());
        ASSERT_TRUE(ed.append(jbc::ir::CodeEditor::loadConst(msgIdx)).is_ok());
        ASSERT_TRUE(ed.append(jbc::ir::CodeEditor::cpInsn(Op::INVOKEVIRTUAL, printIdx)).is_ok());
        ASSERT_TRUE(ed.append(jbc::ir::CodeEditor::simple(Op::RETURN)).is_ok());
    }
    ASSERT_TRUE(jbc::ir::lowerMethod(*s, id).is_ok());
    ASSERT_TRUE(s->commitClassMetadata().is_ok());

    auto bytes = jbc::test::writeOk(s->backing());
    ASSERT_FALSE(bytes.empty());
    const fs::path freshDir = fs::path("test") / "libjbc_member_move" / "fresh";
    spitFile(freshDir / "freshout" / "Main.class", bytes);

    const std::string out = runMain(freshDir, "freshout.Main");
    std::string want      = "fresh-build-ok";
    want += kEol;
    EXPECT_EQ(out, want);
}
