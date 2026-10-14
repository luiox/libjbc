/// @file        signature_reflect_test.cpp
/// @brief       泛型签名的反射对照 oracle：javac 编译泛型样本 → java 反射
///        dump 全部泛型声明（Type.getTypeName 渲染）→ C++ 侧解析同一批
///        class 的 Signature 属性 → 树渲染为同格式 → 逐行精确对拍。
///
/// 另含规定的手工断言样本：嵌套泛型 Map<String, List<Integer>>、递归类型
/// 边界 <T extends Comparable<T>>、通配符三态/捕获形态、throws 类型变量、
/// 递归实参 + 泛型父类。JDK 缺席时显式 skip。
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "jbc/jbc.hpp"

using namespace jbc::core;
using namespace jbc::type;

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kSrcDir = "test_resource/libjbc/sig_reflect/java";
constexpr std::string_view kOutDir = "test/libjbc_sig_reflect/classes";
constexpr std::string_view kDumpFile = "test/libjbc_sig_reflect/dump.txt";

bool toolPresent(const char* exe)
{
    const std::string cmd = std::string(exe) + " -version > nul 2>&1";
    return std::system(cmd.c_str()) == 0;
}

std::string slurpText(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

std::vector<fs::path> collectJavaSources(const fs::path& root)
{
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; it != end && !ec;
         it.increment(ec)) {
        if (it->is_regular_file() && it->path().extension() == ".java") {
            out.push_back(it->path());
        }
    }
    return out;
}

/// @brief 读一个 class 文件；读失败返回空指针（调用方 ADD_FAILURE）。
std::unique_ptr<ClassFile> readClassFile(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    const std::vector<ca::u8> bytes((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    auto r = readClass(ByteView{bytes});
    if (r.is_err()) {
        return nullptr;
    }
    return std::move(r).unwrap().file;
}

/// @brief 取属性集合里第一个 Signature 属性指向的 Utf8；无则空视图。
std::string_view signatureOf(ClassFile& cf, std::vector<AttrSlot>& attrs)
{
    for (AttrSlot& slot : attrs) {
        if (!cf.cp.utf8Equals(slot.nameCp, "Signature")) {
            continue;
        }
        auto d = cf.decodeAttr(slot);
        if (d.is_err()) {
            return {};
        }
        AttrBody* body = std::move(d).unwrap();
        if (body->kind() != AttrKind::Raw || static_cast<RawAttr*>(body)->bytes().size() != 2) {
            return {};
        }
        const std::vector<ca::u8>& b = static_cast<RawAttr*>(body)->bytes();
        return cf.utf8((ca::u32(b[0]) << 8) | ca::u32(b[1]));
    }
    return {};
}

// ---- 树 → java.lang.reflect.Type.getTypeName 同格式渲染 ----

const char* primitiveName(BaseType b)
{
    switch (b) {
    case BaseType::Byte: return "byte";
    case BaseType::Char: return "char";
    case BaseType::Double: return "double";
    case BaseType::Float: return "float";
    case BaseType::Int: return "int";
    case BaseType::Long: return "long";
    case BaseType::Short: return "short";
    case BaseType::Boolean: return "boolean";
    }
    return "int";
}

std::string renderClassType(const SigClassType& ct)
{
    // 二进制名：包 '.' + 首段，后缀段 '$' 连接（= Class.getName()）。
    std::string out;
    for (const char c : ct.package_prefix) {
        out += (c == '/') ? '.' : c;
    }
    out += std::string(ct.segments[0].name);
    for (ca::usize i = 1; i < ct.segments.size(); ++i) {
        out += '$';
        out += std::string(ct.segments[i].name);
    }
    return out;
}

/// @brief 参数化类的实参渲染（取全部段的实参，按序 join ", "）。
bool collectArgs(const SigClassType& ct, std::vector<const SigTypeArg*>& args)
{
    for (const SigClassSegment& seg : ct.segments) {
        for (const SigTypeArg& arg : seg.type_args) {
            args.push_back(&arg);
        }
    }
    return !args.empty();
}

std::string renderArgs(const std::vector<const SigTypeArg*>& args);

std::string renderType(const SigType& t)
{
    std::string out;
    switch (t.kind) {
    case SigType::Kind::Primitive:
        out += primitiveName(t.primitive);
        break;
    case SigType::Kind::TypeVar:
        out += std::string(t.type_var_name);
        break;
    case SigType::Kind::Class: {
        std::vector<const SigTypeArg*> args;
        const bool has_args = collectArgs(t.class_type, args);
        out += renderClassType(t.class_type);
        if (has_args) {
            out += '<';
            out += renderArgs(args);
            out += '>';
        }
        break;
    }
    }
    for (ca::u32 i = 0; i < t.array_dims; ++i) {
        out += "[]";
    }
    return out;
}

std::string renderArg(const SigTypeArg& arg)
{
    switch (arg.kind) {
    case SigTypeArg::Kind::Unbounded:
        return "?";
    case SigTypeArg::Kind::UpperBound:
        return "? extends " + renderType(*arg.bound);
    case SigTypeArg::Kind::LowerBound:
        return "? super " + renderType(*arg.bound);
    case SigTypeArg::Kind::Invariant:
    default:
        return renderType(*arg.bound);
    }
}

std::string renderArgs(const std::vector<const SigTypeArg*>& args)
{
    std::string out;
    for (ca::usize i = 0; i < args.size(); ++i) {
        if (i > 0) {
            out += ", ";
        }
        out += renderArg(*args[i]);
    }
    return out;
}

void renderParams(const std::vector<SigTypeParam>& tps, std::string& out)
{
    for (const SigTypeParam& tp : tps) {
        out += "|P:";
        out += std::string(tp.name);
        if (tp.class_bound != nullptr) {
            out += "|PB:";
            out += renderType(*tp.class_bound);
        }
        for (const SigType& ib : tp.interface_bounds) {
            out += "|PB:";
            out += renderType(ib);
        }
    }
}

/// @brief 渲染 dump 行（形态特定；field 形态单独走 renderType）。
std::string renderMethodLine(const std::string& owner, const std::string& name,
                             const SigMethodSignature& ms)
{
    std::string out = "method|" + owner + "|" + name + "|";
    out += ms.ret_void ? "void" : renderType(ms.ret);
    renderParams(ms.type_params, out);
    for (const SigType& p : ms.params) {
        out += '|';
        out += renderType(p);
    }
    for (const SigType& t : ms.throws_list) {
        out += "|E:";
        out += renderType(t);
    }
    return out;
}

std::string renderClassLine(const std::string& owner, const SigClassSignature& cs)
{
    std::string out = "class|" + owner + "|" + renderType(cs.superclass);
    for (const SigType& itf : cs.super_interfaces) {
        out += "|I:";
        out += renderType(itf);
    }
    renderParams(cs.type_params, out);
    return out;
}

std::vector<std::string> splitLines(const std::string& text)
{
    std::vector<std::string> out;
    ca::usize begin = 0;
    while (begin <= text.size()) {
        const ca::usize nl = text.find('\n', begin);
        if (nl == std::string::npos) {
            if (begin < text.size()) {
                out.push_back(text.substr(begin));
            }
            break;
        }
        std::string line = text.substr(begin, nl - begin);
        while (!line.empty() && (line.back() == '\r')) {
            line.pop_back();
        }
        if (!line.empty()) {
            out.push_back(std::move(line));
        }
        begin = nl + 1;
    }
    return out;
}

}   // namespace

TEST(SignatureReflect, DumpMatchesTreeRendering)
{
    ASSERT_TRUE(toolPresent("javac") && toolPresent("java"))
        << "JDK not on PATH";
    const fs::path src_dir(kSrcDir);
    const fs::path out_dir(kOutDir);
    const fs::path dump_path(kDumpFile);
    ASSERT_TRUE(fs::exists(src_dir)) << src_dir.string();

    std::error_code ec;
    fs::remove_all(out_dir, ec);
    fs::create_directories(out_dir, ec);
    std::string files;
    for (const fs::path& src : collectJavaSources(src_dir)) {
        files += " \"";
        files += src.string();
        files += "\"";
    }
    const std::string javac = "javac --release 8 -encoding UTF-8 -d \""
        + out_dir.string() + "\"" + files + " > nul 2>&1";
    ASSERT_EQ(std::system(javac.c_str()), 0) << "sample compile failed";
    const std::string java = "java -cp \"" + out_dir.string()
        + "\" jbcx.sigref.DumpSigRef > \"" + dump_path.string() + "\" 2>&1";
    ASSERT_EQ(std::system(java.c_str()), 0) << "reflection dump failed";
    const std::string dump_text = slurpText(dump_path);
    ASSERT_FALSE(dump_text.empty());
    const std::vector<std::string> lines = splitLines(dump_text);
    ASSERT_GE(lines.size(), ca::usize{16}) << "dump too small: " << dump_text;

    // 读入全部样本 class（owner 二进制名 → 模型）。
    std::map<std::string, std::unique_ptr<ClassFile>> classes;
    for (fs::recursive_directory_iterator it(out_dir, ec), end; it != end && !ec;
         it.increment(ec)) {
        if (!it->is_regular_file() || it->path().extension() != ".class") {
            continue;
        }
        auto cf = readClassFile(it->path());
        ASSERT_NE(cf, nullptr) << it->path();
        const std::string binary =
            std::string(cf->thisName().data(), cf->thisName().size());
        std::string dotted;
        for (const char c : binary) {
            dotted += (c == '/') ? '.' : c;
        }
        classes.emplace(std::move(dotted), std::move(cf));
    }
    ASSERT_GE(classes.size(), ca::usize{6});

    ca::u32 compared_class = 0;
    ca::u32 compared_field = 0;
    ca::u32 compared_method = 0;
    ca::u32 skipped_erased = 0;   // 无 Signature 属性的成员行（桥方法/非泛型）

    for (const std::string& line : lines) {
        const ca::usize f1 = line.find('|');
        ASSERT_NE(f1, std::string::npos) << line;
        const std::string kind = line.substr(0, f1);

        if (kind == "class") {
            const ca::usize f2 = line.find('|', f1 + 1);
            const std::string owner = line.substr(f1 + 1, f2 - f1 - 1);
            auto it = classes.find(owner);
            ASSERT_NE(it, classes.end()) << line;
            const std::string_view sig = signatureOf(*it->second, it->second->attrs);
            if (sig.empty()) {
                // 非泛型类无 Signature 属性：dump 行必须确属擦除形态（无接口、
                // 无类型参数）才放行，否则样本/对照失配。
                ASSERT_EQ(line.find("|I:"), std::string::npos) << line;
                ASSERT_EQ(line.find("|P:"), std::string::npos) << line;
                ++skipped_erased;
                continue;
            }
            auto parsed = parseClassSignature(sig);
            ASSERT_TRUE(parsed.is_ok()) << sig;
            EXPECT_EQ(renderClassLine(owner, std::move(parsed).unwrap()), line);
            ++compared_class;
            continue;
        }

        if (kind == "field" || kind == "method") {
            const ca::usize f2 = line.find('|', f1 + 1);
            const ca::usize f3 = line.find('|', f2 + 1);
            const std::string owner = line.substr(f1 + 1, f2 - f1 - 1);
            const std::string member = line.substr(f2 + 1, f3 - f2 - 1);
            auto it = classes.find(owner);
            ASSERT_NE(it, classes.end()) << line;
            ClassFile& cf = *it->second;

            bool matched = false;
            bool have_erased_candidate = false;
            if (kind == "field") {
                for (MemberInfo& f : cf.fields) {
                    if (cf.nameOf(f) != member) {
                        continue;
                    }
                    const std::string_view sig = signatureOf(cf, f.attrs);
                    if (sig.empty()) {
                        have_erased_candidate = true;
                        continue;
                    }
                    auto parsed = parseFieldSignature(sig);
                    ASSERT_TRUE(parsed.is_ok()) << sig;
                    if (renderType(std::move(parsed).unwrap()) == line.substr(f3 + 1)) {
                        matched = true;
                    }
                }
                if (matched) {
                    ++compared_field;
                }
            }
            else {
                for (MemberInfo& m : cf.methods) {
                    if (cf.nameOf(m) != member) {
                        continue;
                    }
                    const std::string_view sig = signatureOf(cf, m.attrs);
                    if (sig.empty()) {
                        have_erased_candidate = true;   // 桥方法/非泛型方法
                        continue;
                    }
                    auto parsed = parseMethodSignature(sig);
                    ASSERT_TRUE(parsed.is_ok()) << sig;
                    if (renderMethodLine(owner, member, std::move(parsed).unwrap())
                        == line) {
                        matched = true;
                    }
                }
                if (matched) {
                    ++compared_method;
                }
            }
            if (!matched) {
                // 未命中仅当该行确属无签名的擦除成员（桥/非泛型）。
                ASSERT_TRUE(have_erased_candidate)
                    << "no signature-bearing member matches dump line: " << line;
                ++skipped_erased;
            }
            continue;
        }

        FAIL() << "unknown dump line kind: " << line;
    }

    // 对照覆盖：6 类中 4 类带 Signature（Wild/Hold 非泛型按擦除跳过），
    // 4 个泛型字段、4 个泛型方法全比；擦除行 = compareTo 桥×2 + 非泛型类×2。
    EXPECT_EQ(compared_class, 4u);
    EXPECT_EQ(compared_field, 4u);
    EXPECT_EQ(compared_method, 4u);
    EXPECT_EQ(skipped_erased, 4u);
}

TEST(SignatureReflect, HandAssertedShapes)
{
    ASSERT_TRUE(toolPresent("javac")) << "JDK not on PATH";
    const fs::path out_dir(kOutDir);
    if (!fs::exists(out_dir)) {
        fs::create_directories(out_dir);
        const std::string javac = "javac --release 8 -encoding UTF-8 -d \""
            + out_dir.string() + "\" \"" + std::string(kSrcDir)
            + "/jbcx/sigref/SigRefSamples.java\" > nul 2>&1";
        ASSERT_EQ(std::system(javac.c_str()), 0) << "sample compile failed";
    }

    auto load = [&](const std::string& simple) -> ClassFile& {
        static std::map<std::string, std::unique_ptr<ClassFile>> cache;
        auto it = cache.find(simple);
        if (it != cache.end()) {
            return *it->second;
        }
        auto cf = readClassFile(out_dir / "jbcx/sigref" / (simple + ".class"));
        EXPECT_NE(cf, nullptr) << simple;
        auto* raw = cf.get();
        cache.emplace(simple, std::move(cf));
        return *raw;
    };

    // 样本 1：嵌套泛型 Map<String, List<Integer>>
    {
        ClassFile& cf = load("SigRefSamples");
        bool found = false;
        for (MemberInfo& f : cf.fields) {
            if (cf.nameOf(f) != "nested") {
                continue;
            }
            found = true;
            const std::string_view sig = signatureOf(cf, f.attrs);
            EXPECT_EQ(sig,
                      "Ljava/util/Map<Ljava/lang/String;Ljava/util/List<Ljava/lang/"
                      "Integer;>;>;");
            auto r = parseFieldSignature(sig);
            ASSERT_TRUE(r.is_ok());
            const SigType t = std::move(r).unwrap();
            ASSERT_EQ(t.class_type.segments.size(), 1u);
            EXPECT_EQ(t.class_type.segments[0].name, "Map");
            const auto& args = t.class_type.segments[0].type_args;
            ASSERT_EQ(args.size(), 2u);
            EXPECT_EQ(args[0].bound->class_type.segments[0].name, "String");
            const SigType& list = *args[1].bound;
            ASSERT_EQ(list.class_type.segments.size(), 1u);
            EXPECT_EQ(list.class_type.segments[0].name, "List");
            ASSERT_EQ(list.class_type.segments[0].type_args.size(), 1u);
            EXPECT_EQ(list.class_type.segments[0].type_args[0].bound->class_type.segments[0]
                          .name,
                      "Integer");
        }
        ASSERT_TRUE(found) << "nested field not found";
    }

    // 样本 2：递归类型边界 <T extends Comparable<T>>。本机 javac 对"只有接口
    // 界"的参数产出类界缺席的显式形态 `T::`（<E::…> 同款），解析树
    // class_bound==nullptr、接口界持递归实参。
    {
        ClassFile& cf = load("SigRefSamples$Store");
        const std::string_view sig = signatureOf(cf, cf.attrs);
        EXPECT_EQ(sig, "<T::Ljava/lang/Comparable<TT;>;>Ljava/lang/Object;");
        auto r = parseClassSignature(sig);
        ASSERT_TRUE(r.is_ok());
        const SigClassSignature cs = std::move(r).unwrap();
        ASSERT_EQ(cs.type_params.size(), 1u);
        EXPECT_EQ(cs.type_params[0].name, "T");
        EXPECT_EQ(cs.type_params[0].class_bound, nullptr);
        ASSERT_EQ(cs.type_params[0].interface_bounds.size(), 1u);
        const SigType& bound = cs.type_params[0].interface_bounds[0];
        EXPECT_EQ(bound.class_type.segments[0].name, "Comparable");
        ASSERT_EQ(bound.class_type.segments[0].type_args.size(), 1u);
        const SigType& arg = *bound.class_type.segments[0].type_args[0].bound;
        EXPECT_EQ(arg.kind, SigType::Kind::TypeVar);
        EXPECT_EQ(arg.type_var_name, "T");
    }

    // 样本 3：通配符三态 + 捕获形态（<? super Integer> 作类型变量界）
    {
        ClassFile& wild = load("SigRefSamples$Wild");
        for (MemberInfo& f : wild.fields) {
            const std::string_view sig = signatureOf(wild, f.attrs);
            ASSERT_FALSE(sig.empty()) << "field " << wild.nameOf(f) << " lacks signature";
            if (wild.nameOf(f) == "upper") {
                EXPECT_EQ(sig, "Ljava/util/List<+Ljava/lang/Number;>;");
                auto r = parseFieldSignature(sig);
                ASSERT_TRUE(r.is_ok());
                // 具名值绑定：树节点只移不拷，引用不得指向 unwrap() 的临时。
                const SigType t = std::move(r).unwrap();
                const auto& args = t.class_type.segments[0].type_args;
                ASSERT_EQ(args.size(), 1u);
                EXPECT_EQ(args[0].kind, SigTypeArg::Kind::UpperBound);
                EXPECT_EQ(args[0].bound->class_type.segments[0].name, "Number");
            }
            else if (wild.nameOf(f) == "lower") {
                EXPECT_EQ(sig, "Ljava/util/Map<-Ljava/lang/Integer;*>;");
                auto r = parseFieldSignature(sig);
                ASSERT_TRUE(r.is_ok());
                const SigType t = std::move(r).unwrap();
                const auto& args = t.class_type.segments[0].type_args;
                ASSERT_EQ(args.size(), 2u);
                EXPECT_EQ(args[0].kind, SigTypeArg::Kind::LowerBound);
                EXPECT_EQ(args[0].bound->class_type.segments[0].name, "Integer");
                EXPECT_EQ(args[1].kind, SigTypeArg::Kind::Unbounded);
                EXPECT_FALSE(args[1].hasBound());
            }
        }
        for (MemberInfo& m : wild.methods) {
            if (wild.nameOf(m) != "capture") {
                continue;
            }
            const std::string_view sig = signatureOf(wild, m.attrs);
            EXPECT_EQ(sig, "<C::Ljava/util/List<-Ljava/lang/Integer;>;>(TC;)TC;");
            auto r = parseMethodSignature(sig);
            ASSERT_TRUE(r.is_ok());
            const SigMethodSignature ms = std::move(r).unwrap();
            ASSERT_EQ(ms.type_params.size(), 1u);
            EXPECT_EQ(ms.type_params[0].class_bound, nullptr);
            ASSERT_EQ(ms.type_params[0].interface_bounds.size(), 1u);
            const SigType& list = ms.type_params[0].interface_bounds[0];
            EXPECT_EQ(list.class_type.segments[0].name, "List");
            ASSERT_EQ(list.class_type.segments[0].type_args.size(), 1u);
            EXPECT_EQ(list.class_type.segments[0].type_args[0].kind,
                      SigTypeArg::Kind::LowerBound);
            EXPECT_EQ(ms.ret.kind, SigType::Kind::TypeVar);
            EXPECT_EQ(ms.ret.type_var_name, "C");
            ASSERT_EQ(ms.params.size(), 1u);
            EXPECT_EQ(ms.params[0].type_var_name, "C");
        }
    }

    // 样本 4：throws 类型变量 + 具体类
    {
        ClassFile& cf = load("SigRefSamples$Thrower");
        for (MemberInfo& m : cf.methods) {
            if (cf.nameOf(m) != "blast") {
                continue;
            }
            const std::string_view sig = signatureOf(cf, m.attrs);
            EXPECT_EQ(sig, "()V^TT;^Ljava/lang/IllegalStateException;");
            auto r = parseMethodSignature(sig);
            ASSERT_TRUE(r.is_ok());
            const SigMethodSignature ms = std::move(r).unwrap();
            ASSERT_EQ(ms.throws_list.size(), 2u);
            EXPECT_EQ(ms.throws_list[0].kind, SigType::Kind::TypeVar);
            EXPECT_EQ(ms.throws_list[0].type_var_name, "T");
            EXPECT_EQ(ms.throws_list[1].kind, SigType::Kind::Class);
            EXPECT_EQ(ms.throws_list[1].class_type.segments[0].name,
                      "IllegalStateException");
        }
    }

    // 样本 5：递归实参 + 泛型父类（Pair<K, V extends List<K>>）
    {
        ClassFile& cf = load("SigRefSamples$Pair");
        const std::string_view sig = signatureOf(cf, cf.attrs);
        EXPECT_EQ(sig,
                  "<K:Ljava/lang/Object;V::Ljava/util/List<TK;>;>Ljava/util/HashMap<TK;TV;>;");
        auto r = parseClassSignature(sig);
        ASSERT_TRUE(r.is_ok());
        const SigClassSignature cs = std::move(r).unwrap();
        ASSERT_EQ(cs.type_params.size(), 2u);
        EXPECT_EQ(cs.type_params[0].name, "K");
        EXPECT_EQ(cs.type_params[1].name, "V");
        EXPECT_EQ(cs.type_params[1].class_bound, nullptr);
        ASSERT_EQ(cs.type_params[1].interface_bounds.size(), 1u);
        const SigType& v_bound = cs.type_params[1].interface_bounds[0];
        ASSERT_EQ(v_bound.class_type.segments[0].type_args.size(), 1u);
        EXPECT_EQ(v_bound.class_type.segments[0].type_args[0].bound->type_var_name, "K");
        ASSERT_EQ(cs.superclass.class_type.segments[0].type_args.size(), 2u);
        EXPECT_EQ(cs.superclass.class_type.segments[0].name, "HashMap");
    }

    // 样本 6：数组 × 泛型交叠（ArrayList<Hold[]>）
    {
        ClassFile& cf = load("SigRefSamples$Hold");
        for (MemberInfo& f : cf.fields) {
            if (cf.nameOf(f) != "grid") {
                continue;
            }
            const std::string_view sig = signatureOf(cf, f.attrs);
            EXPECT_EQ(sig, "Ljava/util/ArrayList<[Ljbcx/sigref/SigRefSamples$Hold;>;");
            auto r = parseFieldSignature(sig);
            ASSERT_TRUE(r.is_ok());
            const SigType t = std::move(r).unwrap();
            const auto& args = t.class_type.segments[0].type_args;
            ASSERT_EQ(args.size(), 1u);
            EXPECT_EQ(args[0].bound->kind, SigType::Kind::Class);
            EXPECT_EQ(args[0].bound->array_dims, 1u);
            EXPECT_EQ(args[0].bound->class_type.segments[0].name,
                      "SigRefSamples$Hold");
        }
    }
}
