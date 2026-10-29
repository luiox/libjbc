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
    // 类型注解触点：RuntimeVisibleTypeAnnotations 的 'c' 值必须随 rename
    // 重写（demo/Expr → demo/Renamed），反射读出的 cls 跟随新名。
    out += " ta=";
    out += className;
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

std::string runMain(const fs::path& dir, std::string_view mainClass = "demo.Main")
{
    const fs::path outFile = fs::path("test") / "libjbc_rename_probe_run.txt";
    std::error_code ec;
    fs::create_directories(outFile.parent_path(), ec);
    const std::string cmd = "java -cp " + dir.string() + " " + std::string(mainClass)
        + " > " + outFile.string() + " 2>&1";
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
    ca::u32 descriptors  = 0;   ///< 描述符 Utf8 重指向数（含 LVT 行描述符列）
    ca::u32 signatures   = 0;   ///< 泛型签名重指向数（Signature 属性 + LVTT 签名列）
    ca::u32 lvttTables   = 0;   ///< 发生行改写的 LocalVariableTypeTable 表数
    ca::u32 taAttrs      = 0;   ///< 发生改写的 Runtime*TypeAnnotations 属性数
};

/// @brief 描述符语境的类名替换：type 层解析验证 + span 全名命中才动。
///
/// 解析失败（不是描述符形态）或没提到目标类，一律不动——这是分角色
/// 的第一道闸；命中 span 按倒序逐个走 jbc::type::replaceClassName 结构化
/// 替换（只动 span 覆盖字节，倒序保持未替换 span 的偏移有效），落位经
/// findUtf8 查重（命中复用不膨胀，逆向 rename 复用回原条目）。
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
    std::vector<jbc::type::ClassNameSpan> hits;
    for (const jbc::type::ClassNameSpan& s : std::move(spans).unwrap()) {
        if (text.substr(s.offset, s.length) == oldName) {
            hits.push_back(s);
        }
    }
    if (hits.empty()) {
        return false;
    }
    std::string cur{text};
    for (auto it = hits.rbegin(); it != hits.rend(); ++it) {
        auto r = jbc::type::replaceClassName(cur, *it, newName);
        if (r.is_err()) {
            return false;
        }
        cur = std::string(std::string_view{std::move(r).unwrap()});
    }
    ca::u32 nu = cp.findUtf8(cur);
    if (nu == 0) {
        nu = cp.addUtf8(cur);
    }
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

    // LVTT descriptor 列：签名宿主第二处，同走 sig_attr 结构化手术
    // （替代更早期的 "L<name>;" 文本 token 替换——已删除）。
    auto lvttR = jbc::ir::renameClassInLocalVariableTypeTables(cf, oldName, newName);
    if (lvttR.is_err()) {
        return Err(std::string("lvtt surgery failed"));
    }
    const jbc::ir::LvttRenameSummary lvttSt = std::move(lvttR).unwrap();
    st.signatures += lvttSt.tablesChanged;
    st.lvttTables = lvttSt.tablesChanged;

    // 类型注解族：type_index / 'c' 元素值里的类描述符（描述符层 span 手术）。
    auto taR = jbc::ir::renameClassInTypeAnnotations(cf, oldName, newName);
    if (taR.is_err()) {
        return Err(std::string("type annotation surgery failed"));
    }
    st.taAttrs = std::move(taR).unwrap().attrsChanged;

    // 局部变量表：LVT 行是擦除后描述符（走描述符层 span 结构化手术）；
    // LVTT 行不在此扫（上面的 sig_attr 全扫入口已覆盖）。
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
                if (!cp.utf8Equals(nested.nameCp, "LocalVariableTable")) {
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
                    if (fixDescriptorCp(cp, row.descCp, oldName, newName)) {
                        changed = true;
                    }
                }
                if (changed) {
                    (void)nested.mutate();
                    ++st.descriptors;
                }
            }
        }
    }

    return Ok(st);
}

// ---- debug 剥离（迁移对照：上层产品线 dead-artifact 删除 / 异常表·行号收缩 pass 的
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

// ---- 嵌套类真跑探针（分派面的 JVM 门）------------

constexpr std::string_view kNestOldName = "demo/Nest";
constexpr std::string_view kNestNewName = "demo/NestR";

/// NestProbe 的期望输出：除改名后的外围类名外，两个树完全一致。
/// nestMembers 计数改名前后一致（宿主的成员表条目指向未改名的内部类二进
/// 制名：Token/Bad/匿名/局部四成员 + 宿主自身 = 5）；permits 条目未参与
/// 改名，原样读出。
std::string expectedNestOutput(std::string_view outer)
{
    std::string out = "declaring=";
    out += outer;
    out += kEol;
    out += "anon=true anonOuter=";
    out += outer;
    out += " anonMethod=anonExpr";
    out += kEol;
    out += "local=true localOuter=";
    out += outer;
    out += " localMethod=localExpr";
    out += kEol;
    out += "throws=demo.Fail";
    out += kEol;
    out += "nestHost=";
    out += outer;
    out += kEol;
    out += "nestMembers=5";
    out += kEol;
    out += "permits=demo.Seal$Impl=9";
    out += kEol;
    out += "eval=45";
    out += kEol;
    return out;
}

/// Nest 族结构元数据 typed 面验证统计。
struct StructAttrStats
{
    ca::u32 innerClasses = 0;         ///< 触达的 InnerClasses 属性数
    ca::u32 enclosingMethod = 0;      ///< 触达的 EnclosingMethod 属性数
    ca::u32 exceptions = 0;           ///< 触达的 Exceptions 属性数
    ca::u32 nestHost = 0;             ///< 触达的 NestHost 属性数
    ca::u32 nestMembers = 0;          ///< 触达的 NestMembers 属性数
    ca::u32 permittedSubclasses = 0;  ///< 触达的 PermittedSubclasses 属性数
    ca::u32 nestOuterRows = 0;        ///< outer 槽命中新外围名 NestR 的行数
};

/// @brief Nest 族结构元数据的分派面验证 + 强制重编码。
///
/// 逐槽走 decodeAttr 分派（接线后必须给出 typed 体——Raw 形即接线失效），
/// 槽语义对改名断言：InnerClasses 行 outer 槽与 EnclosingMethod class 槽、
/// NestHost 宿主槽**不得残留旧外围名**（demo/Nest、demo/Expr——残留即改名
/// 没有触达槽面，是本断言钉死的失效形态；lambda 合成面如
/// MethodHandles$Lookup 等未改名外围照常放行），命中新外围名 NestR 的行
/// 计数进 nestOuterRows；匿名行 outer==0 原样保 0。NestMembers/
/// PermittedSubclasses 条目不得精确命中改名掉的旧名（成员表/允许表条目
/// 指向内部类二进制名，前缀命中不算残留）。再 makeNew 置 New 强制走 typed
/// 编码器重编码——JVM 反射面消费的即 typed 编码产物。Exceptions 条目不得
/// 残留改名掉的旧名（demo/Nest、demo/Expr——残留即异常表没有跟随改名），
/// 其余条目（Fail / 嵌套 Bad / JDK 异常）放行。
Result<StructAttrStats, std::string>
verifyStructAttrsTyped(ClassFile& cf)
{
    ConstantPool& cp = cf.cp;
    StructAttrStats st;
    auto walk = [&](std::vector<AttrSlot>& attrs) -> Result<void, std::string> {
        for (AttrSlot& slot : attrs) {
            if (cp.utf8Equals(slot.nameCp, "InnerClasses")) {
                auto d = cf.decodeAttr(slot);
                if (d.is_err()) {
                    return Err(std::string("decode InnerClasses failed"));
                }
                if (d.unwrap()->kind() != AttrKind::InnerClasses) {
                    return Err(std::string("InnerClasses not typed after wiring"));
                }
                const auto& ic = static_cast<const InnerClassesAttr&>(*slot.body);
                for (const InnerClassesAttr::Row& row : ic.rows) {
                    if (row.outerClassInfoCp != 0) {
                        const std::string_view outer = cf.className(row.outerClassInfoCp);
                        if (outer == kNestOldName || outer == kOldName) {
                            return Err(std::string("stale outer after rename: ")
                                       + std::string(outer));
                        }
                        if (outer == kNestNewName) {
                            ++st.nestOuterRows;
                        }
                    }
                }
                AttrSlot fresh = AttrSlot::makeNew(slot.nameCp, std::move(slot.body));
                slot = std::move(fresh);
                ++st.innerClasses;
            }
            else if (cp.utf8Equals(slot.nameCp, "EnclosingMethod")) {
                auto d = cf.decodeAttr(slot);
                if (d.is_err()) {
                    return Err(std::string("decode EnclosingMethod failed"));
                }
                if (d.unwrap()->kind() != AttrKind::EnclosingMethod) {
                    return Err(std::string("EnclosingMethod not typed after wiring"));
                }
                const auto& em = static_cast<const EnclosingMethodAttr&>(*slot.body);
                const std::string_view outer = cf.className(em.classCp);
                if (outer != kNestNewName) {
                    return Err(std::string("stale enclosing class after rename: ")
                               + std::string(outer));
                }
                AttrSlot fresh = AttrSlot::makeNew(slot.nameCp, std::move(slot.body));
                slot = std::move(fresh);
                ++st.enclosingMethod;
            }
            else if (cp.utf8Equals(slot.nameCp, "Exceptions")) {
                auto d = cf.decodeAttr(slot);
                if (d.is_err()) {
                    return Err(std::string("decode Exceptions failed"));
                }
                if (d.unwrap()->kind() != AttrKind::Exceptions) {
                    return Err(std::string("Exceptions not typed after wiring"));
                }
                const auto& exc = static_cast<const ExceptionsAttr&>(*slot.body);
                for (const ca::u32 classCp : exc.exceptionCps) {
                    const std::string_view name = cf.className(classCp);
                    if (name == kNestOldName || name == kOldName) {
                        return Err(std::string("stale throws entry after rename: ")
                                   + std::string(name));
                    }
                }
                AttrSlot fresh = AttrSlot::makeNew(slot.nameCp, std::move(slot.body));
                slot = std::move(fresh);
                ++st.exceptions;
            }
            else if (cp.utf8Equals(slot.nameCp, "NestHost")) {
                auto d = cf.decodeAttr(slot);
                if (d.is_err()) {
                    return Err(std::string("decode NestHost failed"));
                }
                if (d.unwrap()->kind() != AttrKind::NestHost) {
                    return Err(std::string("NestHost not typed after wiring"));
                }
                const auto& nh = static_cast<const NestHostAttr&>(*slot.body);
                const std::string_view host = cf.className(nh.hostClassCp);
                if (host == kNestOldName || host == kOldName) {
                    return Err(std::string("stale nest host after rename: ")
                               + std::string(host));
                }
                AttrSlot fresh = AttrSlot::makeNew(slot.nameCp, std::move(slot.body));
                slot = std::move(fresh);
                ++st.nestHost;
            }
            else if (cp.utf8Equals(slot.nameCp, "NestMembers")) {
                auto d = cf.decodeAttr(slot);
                if (d.is_err()) {
                    return Err(std::string("decode NestMembers failed"));
                }
                if (d.unwrap()->kind() != AttrKind::NestMembers) {
                    return Err(std::string("NestMembers not typed after wiring"));
                }
                const auto& nm = static_cast<const NestMembersAttr&>(*slot.body);
                for (const ca::u32 classCp : nm.memberCps) {
                    const std::string_view name = cf.className(classCp);
                    if (name == kNestOldName || name == kOldName) {
                        return Err(std::string("stale nest member after rename: ")
                                   + std::string(name));
                    }
                }
                AttrSlot fresh = AttrSlot::makeNew(slot.nameCp, std::move(slot.body));
                slot = std::move(fresh);
                ++st.nestMembers;
            }
            else if (cp.utf8Equals(slot.nameCp, "PermittedSubclasses")) {
                auto d = cf.decodeAttr(slot);
                if (d.is_err()) {
                    return Err(std::string("decode PermittedSubclasses failed"));
                }
                if (d.unwrap()->kind() != AttrKind::PermittedSubclasses) {
                    return Err(std::string("PermittedSubclasses not typed after wiring"));
                }
                const auto& ps = static_cast<const PermittedSubclassesAttr&>(*slot.body);
                for (const ca::u32 classCp : ps.permittedCps) {
                    const std::string_view name = cf.className(classCp);
                    if (name == kNestOldName || name == kOldName) {
                        return Err(std::string("stale permitted subclass after rename: ")
                                   + std::string(name));
                    }
                }
                AttrSlot fresh = AttrSlot::makeNew(slot.nameCp, std::move(slot.body));
                slot = std::move(fresh);
                ++st.permittedSubclasses;
            }
        }
        return Ok();
    };
    auto wr = walk(cf.attrs);
    if (wr.is_err()) {
        return Err(std::move(wr).unwrap_err());
    }
    for (MemberInfo& m : cf.methods) {
        auto wm = walk(m.attrs);
        if (wm.is_err()) {
            return Err(std::move(wm).unwrap_err());
        }
    }
    return Ok(st);
}

/// @brief 嵌套类改名写出：Expr→Renamed（既有 pass）+ Nest→NestR（外围名
///        重写触达 InnerClasses/EnclosingMethod 槽），随后三件走分派 typed
///        面验证 + 强制重编码。this_class 命中新名的文件按新名落盘。
Result<StructAttrStats, std::string>
nestRenameWrite(const fs::path& inPath, const fs::path& outDir, const fs::path& rel)
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
    auto r1 = renameClassInPlace(cf, kOldName, kNewName);
    if (r1.is_err()) {
        return Err(std::move(r1).unwrap_err());
    }
    auto r2 = renameClassInPlace(cf, kNestOldName, kNestNewName);
    if (r2.is_err()) {
        return Err(std::move(r2).unwrap_err());
    }
    auto v = verifyStructAttrsTyped(cf);
    if (v.is_err()) {
        return Err(std::move(v).unwrap_err());
    }
    const StructAttrStats st = std::move(v).unwrap();
    fs::path outRel = rel;
    const std::string thisName(cf.className(cf.thisClass));
    if (thisName == kNewName) {
        outRel = fs::path(std::string(kNewName) + ".class");
    }
    else if (thisName == kNestNewName) {
        outRel = fs::path(std::string(kNestNewName) + ".class");
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
    RenameStats mainStats;   ///< demo/Main 的分账：泛型局部变量落 LVTT 的类
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
        total.lvttTables += st.lvttTables;
        total.taAttrs += st.taAttrs;
        if (rel == fs::path("demo") / "Main.class") {
            mainStats = st;
        }
    }
    // 触点护栏：三类手术都必须有命中，否则样本没练到位
    EXPECT_GT(total.classEntries, 0u);
    EXPECT_GT(total.descriptors, 0u);
    EXPECT_GT(total.signatures, 0u);
    // LVTT 签名列护栏：Main 的 Box<Expr>/Function<Integer,Integer> 泛型局部
    // 变量经 -g 落 LVTT，结构化手术必须在真跑样本上命中该列。
    EXPECT_GT(total.lvttTables, 0u);
    EXPECT_GT(mainStats.lvttTables, 0u)
        << "demo/Main must carry LVTT signature rows hitting the rename";
    // 类型注解护栏：Main 的 CACHED 字段挂 @TypeUse(cls = Expr.class) 产出
    // RuntimeVisibleTypeAnnotations，'c' 类描述符必须随 rename 重写。
    EXPECT_GT(total.taAttrs, 0u);
    EXPECT_GT(mainStats.taAttrs, 0u)
        << "demo/Main must carry a RuntimeVisibleTypeAnnotations attr hitting the rename";

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
        total.lvttTables += st.lvttTables;
        total.taAttrs += st.taAttrs;
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

// ---- 嵌套类真跑门：改名后 getDeclaringClass / isAnonymousClass 反射
//      语义存活（分派面点名的断言）----
//
// 改名 pass 重写 InnerClasses 行 outer 槽与 EnclosingMethod class 槽指向的
// 外围类名（CP Class 条目改名自动跟随），三件随后经分派 typed 面验证 +
// 强制重编码——JVM 反射读到的必须是 typed 编码产物。匿名/局部判定与包围
// 方法名在改名前后必须原样存活；外围类名必须读到新名（旧行为=槽没被改写
// 或 typed 编码 scrambled，都立刻翻红）。

TEST(RealClassRenameProbe, NestedReflectionAfterRename)
{
    if (!jdkPresent()) {
        GTEST_SKIP() << "JDK not on PATH";
    }
    const fs::path origDir = fs::path("test") / "libjbc_rename_probe" / "orig";
    const fs::path nestDir = fs::path("test") / "libjbc_rename_probe" / "nest_renamed";
    if (!compileSample(origDir)) {
        GTEST_SKIP() << "sample compile failed (run from libjbc project root)";
    }

    // 原版基线：探针输出钉死样本形态（匿名/局部判定、包围方法、throws 表）。
    const std::string origOut = runMain(origDir, "demo.NestProbe");
    ASSERT_FALSE(origOut.empty()) << "java run failed on original classes";
    EXPECT_EQ(origOut, expectedNestOutput("demo.Nest"));

    StructAttrStats total;
    std::error_code ec;
    fs::remove_all(nestDir, ec);
    for (const fs::path& p : collectClassFiles(origDir)) {
        const fs::path rel = fs::relative(p, origDir);
        auto r = nestRenameWrite(p, nestDir, rel);
        ASSERT_TRUE(r.is_ok()) << std::move(r).unwrap_err();
        const StructAttrStats st = std::move(r).unwrap();
        total.innerClasses += st.innerClasses;
        total.enclosingMethod += st.enclosingMethod;
        total.exceptions += st.exceptions;
        total.nestHost += st.nestHost;
        total.nestMembers += st.nestMembers;
        total.permittedSubclasses += st.permittedSubclasses;
        total.nestOuterRows += st.nestOuterRows;
    }
    // 触点护栏：结构面 typed 面都必须在真跑样本上被触达，否则样本没练到位
    //（Nest/Token/局部/匿名四文件带 InnerClasses；局部+匿名带
    // EnclosingMethod；Nest.mint 与 Main.drive 带 Exceptions；javac 11+ 在
    // nestmate 族上产出 NestHost+NestMembers 对，Seal 带
    // PermittedSubclasses）；外围名重写必须真实命中 NestR 行，否则改名没有
    // 触达槽面。
    EXPECT_GT(total.innerClasses, 0u);
    EXPECT_GT(total.enclosingMethod, 0u);
    EXPECT_GT(total.exceptions, 0u);
    EXPECT_GT(total.nestHost, 0u)
        << "sample must carry NestHost (javac 11+ nestmate) — got zero";
    EXPECT_GT(total.nestMembers, 0u)
        << "sample must carry NestMembers (javac 11+ nestmate) — got zero";
    EXPECT_GT(total.permittedSubclasses, 0u)
        << "sample must carry PermittedSubclasses (demo.Seal) — got zero";
    EXPECT_GT(total.nestOuterRows, 0u)
        << "rename never reached an InnerClasses outer slot";

    // JVM 真跑：外围类名读新名，匿名/局部语义与 throws 表原样存活；
    // getNestHost 读改名后的宿主、getNestMembers 计数原样、sealed 允许表原样。
    EXPECT_EQ(runMain(nestDir, "demo.NestProbe"), expectedNestOutput("demo.NestR"));
}

// ---- 布局改动真跑批：全方法插 nop 后 JVM 实跑与调试属性存活（2026-10-14）----

/// javap -v 输出抓取（LVT/LVTT 结构存活的读回面；core 反射读不到调试属性，
/// classfile 层的读回工具就是 javap）。
std::string javapVerbose(const fs::path& cls)
{
    const fs::path outFile = fs::path("test") / "libjbc_reloc_probe" / "javap.txt";
    std::error_code ec;
    fs::create_directories(outFile.parent_path(), ec);
    const std::string cmd = "javap -v -p " + cls.string() + " > " + outFile.string() + " 2>&1";
    if (std::system(cmd.c_str()) != 0) {
        return {};
    }
    const Bytes out = slurpFile(outFile);
    return std::string(out.begin(), out.end());
}

/// 一个类文件全体带码方法开头插 nop 并 lower；写出 outDir/rel。返回动的 方法数。
Result<int, std::string>
nudgeAndWrite(const fs::path& inPath, const fs::path& outDir, const fs::path& rel)
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
    jbc::ir::ClassSession& cs = *session;
    int moved = 0;
    for (ca::usize mi = 0; mi < cs.unit().methods.size(); ++mi) {
        if (!jbc::ir::hasCode(cs, mi)) {
            continue;
        }
        auto pr = jbc::ir::promoteMethod(cs, mi);
        if (pr.is_err()) {
            return Err(std::string("promote failed: ") + inPath.string());
        }
        jbc::ir::CodeUnit& cu = *pr.unwrap();
        jbc::ir::CodeEditor ed(cu);
        if (!ed.insertAtStart(jbc::ir::CodeEditor::simple(Op::NOP)).is_ok()) {
            return Err(std::string("insert failed: ") + inPath.string());
        }
        auto lr = jbc::ir::lowerMethod(cs, mi);
        if (lr.is_err()) {
            return Err(std::string("lower failed: ") + inPath.string()
                       + " method " + std::to_string(mi));
        }
        ++moved;
    }
    // 全抽象类/接口可能一个带码方法都没有：原样写出（JVM 类组要完整）。
    auto w = writeClass(cs.backing());
    if (w.is_err()) {
        return Err(std::string("writeClass failed: ") + inPath.string());
    }
    spitFile(outDir / rel, std::move(w).unwrap());
    return Ok(moved);
}

TEST(RealClassRelocProbe, LayoutChangeJvmRealRunAndDebugAttrsSurvive)
{
    if (!jdkPresent()) {
        GTEST_SKIP() << "JDK not on PATH";
    }
    const fs::path origDir = fs::path("test") / "libjbc_rename_probe" / "orig";
    const fs::path relocDir = fs::path("test") / "libjbc_reloc_probe" / "reloc";
    if (!fs::exists(origDir) && !compileSample(origDir)) {
        GTEST_SKIP() << "sample compile failed (run from libjbc project root)";
    }
    const std::string origOut = runMain(origDir);
    ASSERT_FALSE(origOut.empty()) << "java run failed on original classes";

    // 全类全带码方法开头插 nop：codeChanged → relocation contract 全链
    // （LNT/LVT/LVTT/SMT/TA 全部要重定向，漏一类即显式 Err）。
    std::error_code ec;
    fs::remove_all(relocDir, ec);
    int movedTotal = 0;
    for (const fs::path& p : collectClassFiles(origDir)) {
        const fs::path rel = fs::relative(p, origDir);
        auto r = nudgeAndWrite(p, relocDir, rel);
        ASSERT_TRUE(r.is_ok()) << std::move(r).unwrap_err();
        movedTotal += std::move(r).unwrap();
    }
    EXPECT_GT(movedTotal, 0);

    // 1) JVM 真跑：改布局后的全类组行为不变——栈帧（SMT 重算）、分支、
    //    TA 反射（ta=，Field::getAnnotatedType 路径）全过验证器。
    EXPECT_EQ(runMain(relocDir), origOut);

    // 2) LVTT/LVT 存活：javap 读回改布局后的 demo/Main——泛型局部变量表
    //    （Signature 列）在布局改动后仍在（core 反射读不到调试属性，
    //    classfile 层读回工具即 javap）。javap 不在 PATH 时跳过本读回
    //    （行级结构已有 crafted 测试与语料门兜底），不视为失败。
    const std::string v = javapVerbose(relocDir / fs::path("demo") / "Main.class");
    if (v.empty()) {
        std::fprintf(stderr,
                     "[reloc-probe] javap not on PATH; LVT/LVTT read-back check skipped\n");
    }
    else {
        EXPECT_NE(v.find("LocalVariableTable"), std::string::npos)
            << "LVT missing after relocation";
        EXPECT_NE(v.find("LocalVariableTypeTable"), std::string::npos)
            << "LVTT missing after relocation";
    }

    // 3) CRT 携带类 JVM 照单全收：给 demo/Add 的 Code 注入一条合成
    //    CharacterRangeTable（New 槽，1 行）——现代 javac 不产出 CRT，
    //    「JVM 忽略但接受携带 CRT 的类」由此补位。
    const fs::path crtDir = fs::path("test") / "libjbc_reloc_probe" / "crt";
    fs::remove_all(crtDir, ec);
    fs::copy(relocDir, crtDir, fs::copy_options::recursive, ec);
    ASSERT_FALSE(ec) << "copy reloc -> crt failed";
    {
        const fs::path addRel = fs::path("demo") / "Add.class";
        const Bytes bytes = slurpFile(crtDir / addRel);
        ASSERT_FALSE(bytes.empty());
        auto s = jbc::ir::makeSessionFromBytes(ByteView{bytes});
        ASSERT_TRUE(s.is_ok());
        auto session = std::move(s).unwrap();
        ClassFile& cf = session->backing();
        bool injected = false;
        for (MemberInfo& m : cf.methods) {
            for (AttrSlot& slot : m.attrs) {
                auto d = cf.decodeAttr(slot);
                ASSERT_TRUE(d.is_ok());
                if (d.unwrap()->kind() != AttrKind::Code) {
                    continue;
                }
                auto& code = static_cast<CodeAttr&>(*slot.body);
                auto crt = std::make_unique<CharacterRangeTableAttr>();
                CharacterRangeTableAttr::Row row;
                row.startPc   = 0;
                row.endPc     = 1;
                row.charStart = 0x0c09;
                row.charEnd   = 0x0c17;
                row.flags     = 1;
                crt->rows.push_back(row);
                AttrSlot nested;
                nested.nameCp = cf.cp.addUtf8("CharacterRangeTable");
                nested.state  = AttrSlot::State::New;
                nested.body   = std::move(crt);
                code.attrs.push_back(std::move(nested));
                (void)slot.mutate();
                injected = true;
                break;
            }
            if (injected) {
                break;
            }
        }
        ASSERT_TRUE(injected) << "no Code attr in demo/Add";
        auto w = writeClass(cf);
        ASSERT_TRUE(w.is_ok());
        spitFile(crtDir / addRel, std::move(w).unwrap());
    }
    EXPECT_EQ(runMain(crtDir), origOut);
}
