/// @file        rename_probe_test.cpp
/// @brief       rename 探针：在 libjbc 上跑一个真实跨类改名 pass——javac 编译
///        多类样本 → 改名 → 写回 → JVM 真跑验收。这是「libjbc 能不能担
///        pass」的第一手证据，同时消费 jbc::type 的描述符手术原语。
///
/// 样本故意埋满 rename 触点：instanceof/checkcast/anewarray、throws
/// （Exceptions 属性）、泛型（Signature 属性）、lambda（indy +
/// BootstrapMethods + 合成方法描述符）、局部变量表（-g）、类字面量（ldc
/// Class 常量），以及一个**角色陷阱**：字符串常量 "Ldemo/Expr;"——
/// 天真扫全池的 rename 会把它改坏，pass 必须分角色处理。
#include <gtest/gtest.h>

#include <algorithm>
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

constexpr std::string_view kOldName = "demo/Expr";
constexpr std::string_view kNewName = "demo/Renamed";

#ifdef _WIN32
constexpr const char* kEol = "\r\n";
#else
constexpr const char* kEol = "\n";
#endif

std::string expectedOutput(std::string_view className)
{
    std::string out = "sum=283 box=true cls=demo.Add name=";
    out += className;
    out += " lit=Ldemo/Expr;";
    out += kEol;
    out += "ok";
    out += kEol;
    return out;
}

bool toolPresent(const char* exe)
{
    const std::string cmd = std::string(exe) + " -version > nul 2>&1";
    return std::system(cmd.c_str()) == 0;
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

std::vector<fs::path> collectClassFiles(const fs::path& root)
{
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; it != end && !ec;
         it.increment(ec)) {
        if (!ec && it->is_regular_file() && it->path().extension() == ".class") {
            out.push_back(it->path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool jdkPresent()
{
    return toolPresent("javac") && toolPresent("java");
}

bool compileSample(const fs::path& outDir)
{
    const fs::path srcDir = fs::path("test_resource/libjbc/rename_probe");
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
    const std::string cmd = "javac -g -encoding UTF-8 -d " + outDir.string() + files
        + " > nul 2>&1";
    return std::system(cmd.c_str()) == 0;
}

std::string runMain(const fs::path& dir)
{
    const fs::path outFile = fs::path("test") / "libjbc_rename_probe_run.txt";
    std::error_code ec;
    fs::create_directories(outFile.parent_path(), ec);
    const std::string cmd = "java -cp " + dir.string() + " demo.Main > "
        + outFile.string() + " 2>&1";
    if (std::system(cmd.c_str()) != 0) {
        return {};
    }
    const Bytes out = slurpFile(outFile);
    return std::string(out.begin(), out.end());
}

// ---- rename pass（探针本体）----

struct RenameStats
{
    ca::u32 classEntries = 0;   ///< CP Class 条目重指向数
    ca::u32 descriptors  = 0;   ///< 描述符 Utf8 重指向数
    ca::u32 signatures   = 0;   ///< 泛型签名/局部变量类型重指向数
};

/// @brief 文本级类名替换（描述符与 LVTT 行语境）。
///
/// 描述符/签名词法里 ';' 只出现在类类型结尾，精确 token "L<name>;" 的替换
/// 不会误伤；无命中时 addUtf8 去重返回原 index，天然无操作。
/// （Signature 属性已改走 sig_attr 的结构化手术，不再经过本函数。）
ca::u32 internRenamedInSignature(ConstantPool& cp, std::string_view text,
                                 std::string_view oldName, std::string_view newName)
{
    const std::string tokOld = "L" + std::string(oldName) + ";";
    const std::string tokNew = "L" + std::string(newName) + ";";
    std::string out;
    for (ca::usize i = 0; i < text.size();) {
        if (text.compare(i, tokOld.size(), tokOld) == 0) {
            out += tokNew;
            i += tokOld.size();
        } else {
            out += text[i];
            ++i;
        }
    }
    return cp.addUtf8(out);
}

/// @brief 描述符语境的类名替换：type 层解析验证 + span 命中才动。
///
/// 解析失败（不是描述符形态）或没提到目标类，一律不动——这是分角色
/// 的第一道闸；命中后做全文 token 替换（已验证描述符内 "L<name>;"
/// 只能是类型位置）。
bool fixDescriptorCp(ConstantPool& cp, ca::u32& idx,
                     std::string_view oldName, std::string_view newName)
{
    const std::string_view text = cp.utf8Of(idx);
    if (text.find(oldName) == std::string_view::npos) {
        return false;   // 廉价预筛
    }
    auto spans = jbc::type::classNameSpans(text);
    if (spans.is_err()) {
        return false;
    }
    bool hit = false;
    for (const jbc::type::ClassNameSpan& s : std::move(spans).unwrap()) {
        if (text.substr(s.offset, s.length) == oldName) {
            hit = true;
            break;
        }
    }
    if (!hit) {
        return false;
    }
    const ca::u32 nu = internRenamedInSignature(cp, text, oldName, newName);
    if (nu == idx) {
        return false;
    }
    idx = nu;
    return true;
}

/// @brief 改名一个类（类内自足视角）：CP Class 条目重指向 + 描述符手术
///        + 泛型签名手术。
///
/// 策略是**改条目内容而非改引用**：this/super/接口/指令/Exceptions/
/// StackMapTable/BootstrapMethods 参数全部以索引引用 Class 条目，条目的
/// name Utf8 一换全部自动跟随；描述符走 NameAndType/MethodType/成员头
/// 的重指向；String 条目不碰（角色陷阱防线）。
Result<RenameStats, std::string>
renameClassInPlace(ClassFile& cf, std::string_view oldName, std::string_view newName)
{
    if (!jbc::type::isValidInternalName(oldName)
        || !jbc::type::isValidInternalName(newName)) {
        return Err(std::string("bad internal name"));
    }
    ConstantPool& cp = cf.cp;
    RenameStats st;
    const ca::u32 newUtf8 = cp.addUtf8(newName);

    // Class 条目名匹配：内部名，或其数组形态（"[L<name>;"，数组类常量
    // 由 anewarray/类字面量等产生，名字带维度包装）。
    auto classEntryMatches = [&](std::string_view entryName) -> Result<bool, std::string> {
        if (entryName.empty() || entryName.front() != '[') {
            return Ok(entryName == oldName);
        }
        auto t = jbc::type::parseFieldType(entryName);
        if (t.is_err()) {
            return Err(std::string("bad array class constant: ") + std::string(entryName));
        }
        const jbc::type::TypeDesc d = std::move(t).unwrap();
        return Ok(d.kind == jbc::type::TypeDesc::Kind::Class
                  && d.array_dims > 0 && d.class_name == oldName);
    };

    for (ca::u32 i = 1; i < cp.size(); ++i) {
        ConstantPool::Entry& e = cp.at(i);
        if (e.tag != CpTag::Class) {
            continue;
        }
        auto m = classEntryMatches(cp.utf8Of(e.a));
        if (m.is_err()) {
            return Err(std::move(m).unwrap_err());
        }
        if (!std::move(m).unwrap()) {
            continue;
        }
        const std::string_view n = cp.utf8Of(e.a);
        if (n.front() == '[') {
            // 同维度重建："[" × dims + "L<new>;"
            auto t = jbc::type::parseFieldType(n);
            const ca::u32 dims = std::move(t).unwrap().array_dims;
            std::string repl(static_cast<ca::usize>(dims), '[');
            repl += "L" + std::string(newName) + ";";
            e.a = cp.addUtf8(repl);
        } else {
            e.a = newUtf8;
        }
        e.dirty = true;
        ++st.classEntries;
    }

    for (ca::u32 i = 1; i < cp.size(); ++i) {
        ConstantPool::Entry& e = cp.at(i);
        if (e.tag == CpTag::NameAndType) {
            if (fixDescriptorCp(cp, e.b, oldName, newName)) {
                e.dirty = true;
                ++st.descriptors;
            }
        } else if (e.tag == CpTag::MethodType) {
            if (fixDescriptorCp(cp, e.a, oldName, newName)) {
                e.dirty = true;
                ++st.descriptors;
            }
        }
    }
    for (MemberInfo& m : cf.methods) {
        if (fixDescriptorCp(cp, m.descCp, oldName, newName)) {
            ++st.descriptors;
        }
    }
    for (MemberInfo& f : cf.fields) {
        if (fixDescriptorCp(cp, f.descCp, oldName, newName)) {
            ++st.descriptors;
        }
    }

    // 泛型签名：属性层结构化手术（sig_attr——签名树全名匹配，替代早期
    // 的 "L<name>;" 文本 token 替换与 RawAttr 字节捅）。
    auto sigR = jbc::ir::renameClassInSignatureAttrs(cf, oldName, newName);
    if (sigR.is_err()) {
        return Err(std::string("signature attr surgery failed"));
    }
    st.signatures += std::move(sigR).unwrap().attrsChanged;

    // 局部变量表：LVT 行是描述符（走 type 层验证），LVTT 行是签名（走文本）。
    for (MemberInfo& m : cf.methods) {
        for (AttrSlot& slot : m.attrs) {
            if (!cp.utf8Equals(slot.nameCp, "Code")) {
                continue;
            }
            auto d = cf.decodeAttr(slot);
            if (d.is_err()) {
                return Err(std::string("decode Code attr failed"));
            }
            AttrBody* body = std::move(d).unwrap();
            if (body->kind() != AttrKind::Code) {
                continue;
            }
            auto* code = static_cast<CodeAttr*>(body);
            for (AttrSlot& nested : code->attrs) {
                const bool isLvtt =
                    cp.utf8Equals(nested.nameCp, "LocalVariableTypeTable");
                const bool isLvt = cp.utf8Equals(nested.nameCp, "LocalVariableTable");
                if (!isLvt && !isLvtt) {
                    continue;
                }
                auto ld = cf.decodeAttr(nested);
                if (ld.is_err()) {
                    return Err(std::string("decode LVT attr failed"));
                }
                AttrBody* lbody = std::move(ld).unwrap();
                if (lbody->kind() != AttrKind::LocalVariableTable) {
                    continue;
                }
                auto* lvt = static_cast<LocalVariableTableAttr*>(lbody);
                bool changed = false;
                for (LocalVariableTableAttr::Row& row : lvt->rows) {
                    if (isLvtt) {
                        const ca::u32 nu = internRenamedInSignature(
                            cp, cp.utf8Of(row.descCp), oldName, newName);
                        if (nu != row.descCp) {
                            row.descCp = nu;
                            changed = true;
                        }
                    } else if (fixDescriptorCp(cp, row.descCp, oldName, newName)) {
                        changed = true;
                    }
                }
                if (changed) {
                    (void)nested.mutate();
                    ++st.signatures;
                }
            }
        }
    }

    return Ok(st);
}

// ---- debug 剥离（迁移对照：mjt zkm_dead_artifact_delete / exc_lnt_shrink 的
//      语义最小版）----

ca::u32 stripDebugInfo(ClassFile& cf)
{
    ca::u32 removed = 0;
    auto eraseNamed = [&](std::vector<AttrSlot>& attrs, std::string_view name) {
        const ca::usize before = attrs.size();
        attrs.erase(std::remove_if(attrs.begin(), attrs.end(),
                                   [&](const AttrSlot& s) {
                                       return cf.cp.utf8Equals(s.nameCp, name);
                                   }),
                    attrs.end());
        removed += ca::u32(before - attrs.size());
    };
    eraseNamed(cf.attrs, "SourceFile");
    for (MemberInfo& m : cf.methods) {
        for (AttrSlot& slot : m.attrs) {
            if (!cf.cp.utf8Equals(slot.nameCp, "Code")) {
                continue;
            }
            auto d = cf.decodeAttr(slot);
            if (d.is_err()) {
                continue;
            }
            AttrBody* body = std::move(d).unwrap();
            if (body->kind() != AttrKind::Code) {
                continue;
            }
            auto* code = static_cast<CodeAttr*>(body);
            const ca::usize before = code->attrs.size();
            code->attrs.erase(std::remove_if(code->attrs.begin(), code->attrs.end(),
                                             [&](const AttrSlot& s) {
                                                 return cf.cp.utf8Equals(s.nameCp,
                                                                         "LineNumberTable");
                                             }),
                              code->attrs.end());
            if (code->attrs.size() != before) {
                (void)slot.mutate();
                removed += ca::u32(before - code->attrs.size());
            }
        }
    }
    return removed;
}

ca::usize countSlotsNamed(const ClassFile& cf, const std::vector<AttrSlot>& attrs,
                          std::string_view name)
{
    ca::usize n = 0;
    for (const AttrSlot& s : attrs) {
        if (cf.cp.utf8Equals(s.nameCp, name)) {
            ++n;
        }
    }
    return n;
}

/// 一个类文件跑完 rename + strip 并写出；返回 rename 统计。
/// 定义了被改名类的文件，写出路径换为新类名（JVM 按名定位 class 文件）。
Result<RenameStats, std::string>
renameStripWrite(const fs::path& inPath, const fs::path& outDir,
                 const fs::path& rel, bool withStrip)
{
    const Bytes bytes = slurpFile(inPath);
    if (bytes.empty()) {
        return Err(std::string("slurp failed: ") + inPath.string());
    }
    auto s = jbc::ir::makeSessionFromBytes(ByteView{bytes});
    if (s.is_err()) {
        return Err(std::string("makeSessionFromBytes failed: ") + inPath.string());
    }
    auto session = std::move(s).unwrap();
    ClassFile& cf = session->backing();
    auto r = renameClassInPlace(cf, kOldName, kNewName);
    if (r.is_err()) {
        return Err(std::move(r).unwrap_err());
    }
    const RenameStats st = std::move(r).unwrap();
    if (withStrip) {
        (void)stripDebugInfo(cf);
    }
    fs::path outRel = rel;
    const ConstantPool::Entry& tc = cf.cp.at(cf.thisClass);
    if (cf.cp.utf8Of(tc.a) == kNewName) {
        outRel = fs::path(std::string(kNewName) + ".class");
    }
    auto w = writeClass(cf);
    if (w.is_err()) {
        return Err(std::string("writeClass failed: ") + inPath.string());
    }
    spitFile(outDir / outRel, std::move(w).unwrap());
    return Ok(st);
}

}   // namespace

TEST(RealClassRenameProbe, CrossClassRenameRunsOnJvm)
{
    if (!jdkPresent()) {
        GTEST_SKIP() << "JDK not on PATH";
    }
    const fs::path origDir = fs::path("test") / "libjbc_rename_probe" / "orig";
    const fs::path renamedDir = fs::path("test") / "libjbc_rename_probe" / "renamed";
    if (!compileSample(origDir)) {
        GTEST_SKIP() << "sample compile failed (run from libjbc project root)";
    }
    const std::string origOut = runMain(origDir);
    ASSERT_FALSE(origOut.empty()) << "java run failed on original classes";
    // 原版输出（直接断言，防样本漂移）
    EXPECT_EQ(origOut, expectedOutput("demo.Expr"));

    RenameStats total;
    std::error_code ec;
    fs::remove_all(renamedDir, ec);
    for (const fs::path& p : collectClassFiles(origDir)) {
        const fs::path rel = fs::relative(p, origDir);
        auto r = renameStripWrite(p, renamedDir, rel, /*withStrip=*/false);
        ASSERT_TRUE(r.is_ok()) << std::move(r).unwrap_err();
        const RenameStats st = std::move(r).unwrap();
        total.classEntries += st.classEntries;
        total.descriptors += st.descriptors;
        total.signatures += st.signatures;
    }
    // 触点护栏：三类手术都必须有命中，否则样本没练到位
    EXPECT_GT(total.classEntries, 0u);
    EXPECT_GT(total.descriptors, 0u);
    EXPECT_GT(total.signatures, 0u);

    EXPECT_EQ(runMain(renamedDir), expectedOutput("demo.Renamed"));
}

TEST(RealClassRenameProbe, DebugStripAfterRenameStillRuns)
{
    if (!jdkPresent()) {
        GTEST_SKIP() << "JDK not on PATH";
    }
    const fs::path origDir = fs::path("test") / "libjbc_rename_probe" / "orig";
    const fs::path strippedDir =
        fs::path("test") / "libjbc_rename_probe" / "renamed_stripped";
    if (!fs::exists(origDir) && !compileSample(origDir)) {
        GTEST_SKIP() << "sample compile failed (run from libjbc project root)";
    }

    RenameStats total;
    std::error_code ec;
    fs::remove_all(strippedDir, ec);
    for (const fs::path& p : collectClassFiles(origDir)) {
        const fs::path rel = fs::relative(p, origDir);
        auto r = renameStripWrite(p, strippedDir, rel, /*withStrip=*/true);
        ASSERT_TRUE(r.is_ok()) << std::move(r).unwrap_err();
        const RenameStats st = std::move(r).unwrap();
        total.classEntries += st.classEntries;
        total.descriptors += st.descriptors;
        total.signatures += st.signatures;
    }
    EXPECT_GT(total.classEntries, 0u);

    // 结构自检：剥完的类文件里 SourceFile / LineNumberTable 必须消失
    for (const fs::path& p : collectClassFiles(strippedDir)) {
        const Bytes bytes = slurpFile(p);
        ASSERT_FALSE(bytes.empty()) << p;
        auto rd = readClass(ByteView{bytes});
        ASSERT_TRUE(rd.is_ok()) << p;
        auto file = std::move(rd).unwrap().file;
        EXPECT_EQ(countSlotsNamed(*file, file->attrs, "SourceFile"), 0u) << p;
        for (const MemberInfo& m : file->methods) {
            EXPECT_EQ(countSlotsNamed(*file, m.attrs, "LineNumberTable"), 0u) << p;
        }
    }

    EXPECT_EQ(runMain(strippedDir), expectedOutput("demo.Renamed"));
}
