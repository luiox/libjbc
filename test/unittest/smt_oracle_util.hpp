/// @file        smt_oracle_util.hpp
/// @brief       SMT oracle 测试设施：javac 样本语料 + SMT 解码展开比对。
///
/// 由 strict_frame_test 与 smt_build_test 共用：同一份
/// javac（--release 8）语料、同一套「SMT 帧链 → 全帧展开」机制、同一套
/// 「公共前缀逐槽一致」比对判据与陈述口径。语料覆盖
/// loop 与 switch 两样本（回边与 switch 目标帧）。
#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "jbc/jbc.hpp"
#include "test_class_bytes.hpp"

namespace jbc::test {

using jbc::analysis::ITypeResolver;
using jbc::analysis::MethodSig;
using jbc::analysis::TypeMergeError;
using jbc::analysis::VKind;
using jbc::analysis::VType;

using Bytes = std::vector<ca::u8>;
namespace fs = std::filesystem;

/// javac 缺席探测（Windows 下重定向到 nul）。
inline bool oracleToolPresent(const char* exe)
{
    const std::string cmd = std::string(exe) + " -version > nul 2>&1";
    return std::system(cmd.c_str()) == 0;
}

inline bool jdkPresent()
{
    return oracleToolPresent("javac") && oracleToolPresent("java");
}

inline void spitFile(const fs::path& p, const std::string& text)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

inline Bytes slurpFile(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return Bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// javac oracle 语料：new+\<init\> 翻转、跨类合并（resolver 父链）、checkcast
/// 精确化、数组描述符、构造器 UninitThis、异常 handler（多 catch/嵌套/
/// finally/synchronized）、回边循环（cat2 局部）、switch（table+lookup）。
inline bool writeOracleSamples(const fs::path& srcDir)
{
    spitFile(srcDir / "demo" / "NewInitFlip.java",
             "package demo;\n"
             "public class NewInitFlip {\n"
             "    static String make(boolean b) {\n"
             "        StringBuilder sb = new StringBuilder();\n"
             "        if (b) { sb.append('x'); }\n"
             "        return sb.toString();\n"
             "    }\n"
             "}\n");
    spitFile(srcDir / "demo" / "BranchLca.java",
             "package demo;\n"
             "public class BranchLca {\n"
             "    static Object pick(boolean b) {\n"
             "        Object o = b ? \"x\" : new Object();\n"
             "        return o;\n"
             "    }\n"
             "    static String[] pick2(boolean b, String[] arr) {\n"
             "        String[] o = b ? arr : new String[2];\n"
             "        return o;\n"
             "    }\n"
             "    static Object pick3(boolean b, int[] arr) {\n"
             "        Object o = b ? arr : new String[2];\n"
             "        return o;\n"
             "    }\n"
             "    static String pick4(boolean b, Object x) {\n"
             "        String o = b ? (String) x : \"y\";\n"
             "        return o;\n"
             "    }\n"
             "}\n");
    spitFile(srcDir / "demo" / "CtorBase.java",
             "package demo;\n"
             "public class CtorBase {\n"
             "    CtorBase(int v) { }\n"
             "}\n");
    spitFile(srcDir / "demo" / "CtorUninit.java",
             "package demo;\n"
             "public class CtorUninit extends CtorBase {\n"
             "    int field;\n"
             "    CtorUninit(boolean b) {\n"
             "        super(b ? 1 : 2);\n"
             "        if (b) { field = 1; }\n"
             "    }\n"
             "}\n");
    spitFile(srcDir / "demo" / "ArrayMerge.java",
             "package demo;\n"
             "public class ArrayMerge {\n"
             "    static Object elem(boolean b, String[][] m) {\n"
             "        String[] e = m[0];\n"
             "        return b ? e : \"s\";\n"
             "    }\n"
             "    static int prim(boolean b, int[] a) {\n"
             "        int[] c = b ? a : new int[2];\n"
             "        return c[0];\n"
             "    }\n"
             "}\n");
    spitFile(srcDir / "demo" / "TryCatch.java",
             "package demo;\n"
             "public class TryCatch {\n"
             "    static int grab(String[] a) {\n"
             "        try { return a[0].length(); }\n"
             "        catch (RuntimeException e) { return -1; }\n"
             "    }\n"
             "}\n");
    spitFile(srcDir / "demo" / "TryShapes.java",
             "package demo;\n"
             "public class TryShapes {\n"
             "    static int multiCatch(String s) {\n"
             "        try { return s.length() + Integer.parseInt(s); }\n"
             "        catch (IllegalStateException | NumberFormatException e) { return 0; }\n"
             "    }\n"
             "    static int nested(String[] a) {\n"
             "        try {\n"
             "            try { return a[0].length(); }\n"
             "            catch (NullPointerException e) { return -2; }\n"
             "        } catch (RuntimeException e) { return -1; }\n"
             "    }\n"
             "    static int rethrow(String[] a) {\n"
             "        try { return a[0].length(); }\n"
             "        catch (RuntimeException e) {\n"
             "            try { return a[1].length(); }\n"
             "            finally { System.out.println(\"f\"); }\n"
             "        }\n"
             "    }\n"
             "    static int rethrowInCatch(String[] a) {\n"
             "        try {\n"
             "            return Integer.parseInt(a[0]);\n"
             "        } catch (RuntimeException e) {\n"
             "            try { throw new IllegalStateException(e); }\n"
             "            catch (IllegalStateException s) { return -3; }\n"
             "        }\n"
             "    }\n"
             "    static int div(int a, int b) {\n"
             "        try { return a / b; }\n"
             "        catch (ArithmeticException e) { return -1; }\n"
             "    }\n"
             "    static void sync(Object o) {\n"
             "        try {\n"
             "            synchronized (o) { o.hashCode(); }\n"
             "        } catch (RuntimeException e) { }\n"
             "    }\n"
             "    static int withLocals(boolean b, String[] a) {\n"
             "        String s = \"x\";\n"
             "        try {\n"
             "            if (b) { s = a[0]; }\n"
             "            return s.length();\n"
             "        } catch (RuntimeException e) { return -1; }\n"
             "    }\n"
             "}\n");
    spitFile(srcDir / "demo" / "LoopSum.java",
             "package demo;\n"
             "public class LoopSum {\n"
             "    static int sum(int n) {\n"
             "        int s = 0;\n"
             "        for (int i = 0; i < n; ++i) { s += i; }\n"
             "        return s;\n"
             "    }\n"
             "    static long mask(long base, int shift) {\n"
             "        long m = base;\n"
             "        while (shift > 0) { m = (m << 1) | 1L; --shift; }\n"
             "        return m;\n"
             "    }\n"
             "}\n");
    spitFile(srcDir / "demo" / "SwitchShapes.java",
             "package demo;\n"
             "public class SwitchShapes {\n"
             "    static int table(int v) {\n"
             "        switch (v) {\n"
             "        case 1: return 10;\n"
             "        case 2: return 20;\n"
             "        case 5: return 50;\n"
             "        default: return -1;\n"
             "        }\n"
             "    }\n"
             "    static int lookup(String s) {\n"
             "        if (s == null) { return 0; }\n"
             "        switch (s) {\n"
             "        case \"aa\": return 1;\n"
             "        case \"bb\": return 2;\n"
             "        case \"cc\": return 3;\n"
             "        default: return -1;\n"
             "        }\n"
             "    }\n"
             "}\n");
    return true;
}

/// 现场编译语料（--release 8，与帧 oracle 同一口径）。
inline bool compileOracleSamples(const fs::path& root)
{
    const fs::path srcDir = root / "src";
    writeOracleSamples(srcDir);
    const fs::path outDir = root / "classes";
    std::error_code ec;
    fs::remove_all(outDir, ec);
    fs::create_directories(outDir, ec);
    std::string files;
    for (fs::directory_iterator it(srcDir / "demo"), end; it != end; ++it) {
        if (it->is_regular_file() && it->path().extension() == ".java") {
            files += " ";
            files += it->path().string();
        }
    }
    const std::string cmd = "javac --release 8 -encoding UTF-8 -d " + outDir.string()
        + files + " > nul 2>&1";
    return std::system(cmd.c_str()) == 0;
}

/// 最小 stub resolver（真实现归上层消费方，测试内只配最小层）：
/// 同类返回本类；跨类沿父链求首个公共祖先（缺省父 = Object）；
/// 空名参与跨类合并视为不可解（防止「无名 Ref 被静默兜底」掩盖缺陷）。
/// 求解结果经内部驻留表交付——VType.name 是借用视图，不得指向临时串。
class StubResolver final : public ITypeResolver
{
public:
    std::map<std::string, std::string> parent;   ///< 子 → 父（可缺省）
    bool failCrossClass = false;
    int calls = 0;

    ca::core::Result<VType, TypeMergeError> merge(VType a, VType b) override
    {
        ++calls;
        if (!a.name.empty() && a.name == b.name) {
            return ca::core::Ok(VType::ref(a.name));
        }
        if (failCrossClass || a.name.empty() || b.name.empty()) {
            return ca::core::Err(TypeMergeError::Unresolvable);
        }
        return ca::core::Ok(
            VType::ref(*interned_.insert(lca(std::string(a.name), std::string(b.name)))
                           .first));
    }

    /// JDK 异常族父链（multi-catch 的 lub 求解走 super-chain）。
    void seedJdkParents()
    {
        parent["java/lang/RuntimeException"] = "java/lang/Exception";
        parent["java/lang/Exception"] = "java/lang/Throwable";
        parent["java/lang/Error"] = "java/lang/Throwable";
        parent["java/lang/IllegalStateException"] = "java/lang/RuntimeException";
        parent["java/lang/NumberFormatException"] = "java/lang/IllegalArgumentException";
        parent["java/lang/IllegalArgumentException"] = "java/lang/RuntimeException";
        parent["java/lang/ArithmeticException"] = "java/lang/RuntimeException";
        parent["java/lang/NullPointerException"] = "java/lang/RuntimeException";
    }

private:
    std::set<std::string> interned_;   ///< 求解结果驻留（节点地址稳定）

    std::string parentOf(const std::string& n) const
    {
        const auto it = parent.find(n);
        return it != parent.end() ? it->second : std::string("java/lang/Object");
    }

    std::string lca(const std::string& a, const std::string& b) const
    {
        std::map<std::string, bool> seen;
        for (std::string cur = a;; cur = parentOf(cur)) {
            seen[cur] = true;
            if (cur == "java/lang/Object") {
                break;
            }
        }
        for (std::string cur = b;; cur = parentOf(cur)) {
            if (seen.count(cur) != 0) {
                return cur;
            }
            if (cur == "java/lang/Object") {
                break;
            }
        }
        return "java/lang/Object";
    }
};

/// 字节 → 会话（失败返回空——调用方 ASSERT_NE）。
inline std::shared_ptr<jbc::ir::ClassSession> sessionOf(const Bytes& bytes)
{
    auto cf = readOk(bytes);
    if (cf == nullptr) {
        return nullptr;
    }
    auto s = jbc::ir::makeSession(std::move(cf));
    return s.is_ok() ? std::move(s).unwrap() : nullptr;
}

/// 方法 Code 嵌套 StackMapTableAttr 的定位（语料 oracle 共用）：惰性解码
/// 触发缓存（非语义修改）；无 Code / 无 SMT / 解码失败返回 nullptr。
inline const core::StackMapTableAttr* findMethodSmt(jbc::ir::ClassSession& s,
                                                    ca::usize methodIdx)
{
    core::ClassFile& cf = s.backing();
    if (methodIdx >= cf.methods.size()) {
        return nullptr;
    }
    const core::StackMapTableAttr* out = nullptr;
    for (core::AttrSlot& slot : cf.methods[methodIdx].attrs) {
        auto b = cf.decodeAttr(slot);
        if (b.is_err() || slot.peek() == nullptr ||
            slot.peek()->kind() != core::AttrKind::Code) {
            continue;
        }
        auto& codeAttr = static_cast<core::CodeAttr&>(*slot.body);
        for (core::AttrSlot& nested : codeAttr.attrs) {
            auto nb = cf.decodeAttr(nested);
            if (nb.is_err() || nested.peek() == nullptr ||
                nested.peek()->kind() != core::AttrKind::StackMapTable) {
                continue;
            }
            out = static_cast<const core::StackMapTableAttr*>(nested.peek());
        }
    }
    return out;
}

/// order 下标 ←→ 帧 下标对齐；按 srcPc 定位指令在 order 中的位置。
inline ca::usize indexAtPc(const jbc::ir::CodeUnit& u, ca::u32 pc)
{
    const auto& order = u.dom.order();
    for (ca::usize i = 0; i < order.size(); ++i) {
        const jbc::ir::Insn* in = u.dom.peek(order[i]);
        if (in != nullptr && in->srcPc == pc) {
            return i;
        }
    }
    return static_cast<ca::usize>(-1);
}

/// SMT verification_type_info → VType（tag 语义 JVMS 4.7.4）。
inline VType fromSmtTag(const core::StackMapTableAttr::VType& v, const core::ConstantPool& cp)
{
    using SmtV = core::StackMapTableAttr::VType;
    switch (v.tag) {
    case SmtV::kTop: return VType{VKind::Top, 0, {}};
    case SmtV::kInteger: return VType{VKind::Int, 0, {}};
    case SmtV::kFloat: return VType{VKind::Float, 0, {}};
    case SmtV::kDouble: return VType{VKind::Double, 0, {}};
    case SmtV::kLong: return VType{VKind::Long, 0, {}};
    case SmtV::kNull: return VType{VKind::Null, 0, {}};
    case SmtV::kUninitializedThis: return VType{VKind::UninitThis, 0, {}};
    case SmtV::kObject: {
        if (!cp.valid(v.cp) || cp.tagOf(v.cp) != core::CpTag::Class ||
            !cp.isUtf8(cp.at(v.cp).a)) {
            return VType{VKind::Bottom, 0, {}};
        }
        return VType::ref(cp.utf8Of(cp.at(v.cp).a));
    }
    case SmtV::kUninitialized: return VType::uninitNew(v.cp);   // 载荷即 new 指令 pc
    default: return VType{VKind::Bottom, 0, {}};
    }
}

/// 展开后的全帧：offset + 槽位对齐的 locals/stack（cat2 展开为值+垫底两槽）。
struct FullFrame
{
    ca::u32 offset = 0;
    std::vector<VType> locals;
    std::vector<VType> stack;
};

/// SMT 帧链 → 全帧序列（起始状态 = 入参 implicit frame 的 locals；
/// chop 按 verification_type 条目计数，cat2 条目展开为两槽）。
inline std::vector<FullFrame> expandSmt(const core::StackMapTableAttr& smt,
                                        const std::vector<VType>& entryLocals,
                                        const core::ConstantPool& cp)
{
    using SmtAttr = core::StackMapTableAttr;
    struct Entry
    {
        VType v;
        bool cat2 = false;
    };
    const auto toEntry = [](const VType& v) {
        return Entry{v, v.k == VKind::Long || v.k == VKind::Double};
    };
    const auto materialize = [](const std::vector<Entry>& es) {
        std::vector<VType> slots;
        for (const Entry& e : es) {
            slots.push_back(e.v);
            if (e.cat2) {
                slots.push_back(VType{VKind::Top, 0, {}});   // cat2 第二槽垫底
            }
        }
        return slots;
    };

    std::vector<Entry> locals;
    for (const VType& v : entryLocals) {
        locals.push_back(toEntry(v));
    }

    std::vector<FullFrame> out;
    ca::u32 offset = 0;
    for (ca::usize i = 0; i < smt.frames.size(); ++i) {
        const SmtAttr::Frame& f = smt.frames[i];
        offset = i == 0 ? f.offset : core::smtAbsPc(offset, f.offset);
        const ca::u8 t = f.type;
        if (t >= SmtAttr::Frame::kChopMax && t <= SmtAttr::Frame::kChopMin) {
            // chop：去掉最后 k 条（k = 251 - t）
            const ca::usize k = static_cast<ca::usize>(251 - t);
            locals.resize(locals.size() > k ? locals.size() - k : 0);
        } else if (t >= SmtAttr::Frame::kAppendMin && t <= SmtAttr::Frame::kAppendMax) {
            // append：追加 k = t - 251 条
            for (const SmtAttr::VType& v : f.locals) {
                locals.push_back(toEntry(fromSmtTag(v, cp)));
            }
        } else if (t == SmtAttr::Frame::kFull) {
            // full：整体替换
            locals.clear();
            for (const SmtAttr::VType& v : f.locals) {
                locals.push_back(toEntry(fromSmtTag(v, cp)));
            }
        }
        // same 家族栈空；same_locals_1_stack_item 家族栈 1 项（载荷在 f.stack）。
        std::vector<VType> stack;
        for (const SmtAttr::VType& v : f.stack) {
            const VType sv = fromSmtTag(v, cp);
            stack.push_back(sv);
            if (sv.k == VKind::Long || sv.k == VKind::Double) {
                stack.push_back(VType{VKind::Top, 0, {}});
            }
        }
        FullFrame ff;
        ff.offset = offset;
        ff.locals = materialize(locals);
        ff.stack = std::move(stack);
        out.push_back(std::move(ff));
    }
    return out;
}

/// 入参 implicit frame 的 locals（条目口径：cat2 一参一条，expandSmt
/// 的 materialize 统一垫底展开；槽级填充在此处重复会移位布局）。
inline std::vector<VType> entryLocalsOf(const MethodSig& sig)
{
    std::vector<VType> out;
    if (!sig.isStatic) {
        out.push_back(sig.name == "<init>" && !sig.thisClass.empty()
                          ? VType{VKind::UninitThis, 0, {}}
                          : VType::ref(sig.thisClass));
    }
    const std::string_view d = sig.desc;
    if (!d.empty() && d[0] == '(') {
        ca::usize pos = 1;
        while (pos < d.size() && d[pos] != ')') {
            const std::string_view t = analysis::detail::oneType(d, pos);
            if (t.empty()) {
                break;
            }
            const VKind k = analysis::detail::kindOfType(t);
            out.push_back(k == VKind::Ref ? VType::ref(analysis::detail::refNameOfType(t))
                                          : VType{k, 0, {}});
            pos += t.size();
        }
    }
    return out;
}

inline std::string vtypeText(const VType& v)
{
    switch (v.k) {
    case VKind::Top: return "top";
    case VKind::Int: return "int";
    case VKind::Float: return "float";
    case VKind::Long: return "long";
    case VKind::Double: return "double";
    case VKind::Ref: return "ref(" + std::string(v.name) + ")";
    case VKind::Null: return "null";
    case VKind::UninitThis: return "uninitThis";
    case VKind::Uninitialized: return "uninit@" + std::to_string(v.uninitOffset());
    case VKind::ReturnAddress: return "retAddr@" + std::to_string(v.jsrPc());
    case VKind::Bottom: return "bottom";
    }
    return "?";
}

inline std::string slotsText(const std::vector<VType>& slots)
{
    std::string s = "[";
    for (ca::usize i = 0; i < slots.size(); ++i) {
        if (i != 0) {
            s += ", ";
        }
        s += vtypeText(slots[i]);
    }
    return s + "]";
}

/// 尾部 Top 填充不计（引擎按触达的最大槽位交付，javac 按活局部交付）。
inline ca::usize trimmedSize(const std::vector<VType>& slots)
{
    ca::usize n = slots.size();
    while (n > 0 && slots[n - 1].k == VKind::Top) {
        --n;
    }
    return n;
}

inline bool sameSlot(const VType& a, const VType& b)
{
    return a.k == b.k && a.cls == b.cls && a.name == b.name;
}

inline std::vector<VType> headOf(const std::vector<VType>& slots, ca::usize n)
{
    return std::vector<VType>(slots.begin(),
                              slots.begin() + static_cast<std::ptrdiff_t>(n));
}

}   // namespace jbc::test
