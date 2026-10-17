/// @file        deobf_demo.cpp
/// @brief       闭环 demo：Grunt 混淆产物 jar + Grunt dumpMappings 映射 →
///              libjbc consumer rename 驱动还原类名/成员名 → deobf class
///              目录落盘。
///
/// 真实链路（「库行不行」的最硬证据）：
///   origin.jar --Grunt obf--> obf.jar + mappings.json
///            --libjbc consumer rename--> deobf/*.class
///
/// jar 直读经 libca_zip::ZipFile（JVM ZipFile 语义，条目名按字节精确
/// 匹配）：不用先解包——Windows 下 tar 解包会把仅大小写不同的类条目
/// 塌缩成同一文件（Alphabet 字典大小写混排，实测 775 类塌成 390）。
///
/// 映射格式（grunt-main/.../rename/mapping/NameMapping.kt dump 产物，
/// Gson pretty-print 一层对象）：
///   { "<origin-name>": { "new": "<obf-name>",
///                        "methods": { "<origName><origDesc>": "<obfName>" },
///                        "fields":  { "<origName><origDesc>": "<obfName>" } } }
///
/// 计划下发姿势同语料闸 WholeCorpusResolver 的定型消费方式：**全规则组
/// 广播给每个类**——类名族规则必须到达每个引用方（operand/描述符随行
/// 改写）；与本类无关的规则在类内查命即空 no-op。--members 开成员族：
/// 成员规则命中 BSM 载体实成员（kotlin indy implMethod 等）→
/// Err(BsmTouched) fail-closed，即 v1 已知未竟（rename v2 通道），demo
/// 如实呈现该闸（预期红，不是缺陷）。
///
/// 用法：
///   libjbc_deobf_demo <obf.jar> <mappings.json> <out-dir> [--members]
///
/// 回包（deobf 目录 → jar）由外部 tar 承担（zip 写面不在 libca 包内）：
///   tar -a -cf deobf.jar -C deobf/classes .
///
/// 外部验证口径（demo 不做，留给 harness）：
///   1. deobf 目录类名集合 == origin 类名集合（改名还原不重不漏）；
///   2. JVM defineClass sweep 收 deobf 全语料（语料外依赖按缺失名豁免）。
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "libca/zip/file.hpp"

#include "jbc/consumer/consumer.hpp"
#include "jbc/consumer/rename.hpp"
#include "jbc/consumer/source.hpp"
#include "jbc/ir/units.hpp"

namespace fs = std::filesystem;

using jbc::consumer::ConsumerResult;
using jbc::consumer::MemoryClassSource;
using jbc::consumer::RenamePlan;
using jbc::consumer::renameSource;

// ---------------------------------------------------------------------------
// mini JSON：Gson 输出的定点子集（对象/字符串/数组/数字/真假/空 + 转义）。
// demo 域不需要浮点精度，数字仅跳过。
// ---------------------------------------------------------------------------
namespace mini_json {

struct Value
{
    enum class T { Null, Bool, Num, Str, Arr, Obj };
    T                                           t = T::Null;
    bool                                        b = false;
    std::string                                 s;
    std::vector<Value>                          arr;
    std::vector<std::pair<std::string, Value>>  obj;

    const Value* find(std::string_view key) const
    {
        for (const auto& [k, v] : obj) {
            if (k == key) {
                return &v;
            }
        }
        return nullptr;
    }
};

struct Parser
{
    const char* p = nullptr;
    const char* end = nullptr;
    const char* base = nullptr;

    explicit Parser(std::string_view text)
        : p(text.data()), end(text.data() + text.size()), base(text.data())
    {}

    void skipWs()
    {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
            ++p;
        }
    }

    [[noreturn]] void fail(const char* what) const
    {
        std::cerr << "mappings.json 解析失败（offset " << (p - base) << "）: " << what << '\n';
        std::exit(2);
    }

    void expect(char c)
    {
        if (p >= end || *p != c) {
            fail("期望字符不匹配");
        }
        ++p;
    }

    std::string stringLit()
    {
        expect('"');
        std::string out;
        while (p < end && *p != '"') {
            if (*p == '\\') {
                ++p;
                if (p >= end) {
                    fail("坏转义");
                }
                switch (*p) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (end - p < 5) {
                        fail("坏 \\u");
                    }
                    const unsigned code =
                        std::strtoul(std::string(p + 1, 4).c_str(), nullptr, 16);
                    p += 4;
                    // UTF-8 编码（BMP 内；成员名域不会出现 surrogate 对）。
                    if (code < 0x80) {
                        out.push_back(static_cast<char>(code));
                    } else if (code < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    }
                    break;
                }
                default: fail("未知转义");
                }
                ++p;
                continue;
            }
            out.push_back(*p++);
        }
        if (p >= end) {
            fail("字符串未闭合");
        }
        expect('"');
        return out;
    }

    Value value()
    {
        skipWs();
        if (p >= end) {
            fail("值缺失");
        }
        switch (*p) {
        case '{': {
            ++p;
            Value v;
            v.t = Value::T::Obj;
            skipWs();
            if (*p == '}') {
                ++p;
                return v;
            }
            while (true) {
                skipWs();
                v.obj.emplace_back(stringLit(), Value{});
                skipWs();
                expect(':');
                v.obj.back().second = value();
                skipWs();
                if (*p == ',') {
                    ++p;
                    continue;
                }
                expect('}');
                break;
            }
            return v;
        }
        case '[': {
            ++p;
            Value v;
            v.t = Value::T::Arr;
            skipWs();
            if (*p == ']') {
                ++p;
                return v;
            }
            while (true) {
                v.arr.push_back(value());
                skipWs();
                if (*p == ',') {
                    ++p;
                    continue;
                }
                expect(']');
                break;
            }
            return v;
        }
        case '"': {
            Value v;
            v.t = Value::T::Str;
            v.s = stringLit();
            return v;
        }
        case 't':
            if (end - p >= 4 && std::string_view(p, 4) == "true") {
                p += 4;
                Value v;
                v.t = Value::T::Bool;
                v.b = true;
                return v;
            }
            fail("坏字面量");
        case 'f':
            if (end - p >= 5 && std::string_view(p, 5) == "false") {
                p += 5;
                Value v;
                v.t = Value::T::Bool;
                v.b = false;
                return v;
            }
            fail("坏字面量");
        case 'n':
            if (end - p >= 4 && std::string_view(p, 4) == "null") {
                p += 4;
                return Value{};
            }
            fail("坏字面量");
        default: {
            const char* start = p;
            while (p < end
                   && (*p == '-' || (*p >= '0' && *p <= '9') || *p == '.' || *p == 'e'
                       || *p == 'E' || *p == '+')) {
                ++p;
            }
            if (p == start) {
                fail("非法值");
            }
            Value v;
            v.t = Value::T::Num;
            v.s.assign(start, p);
            return v;
        }
        }
    }
};

}   // namespace mini_json

// ---------------------------------------------------------------------------
// Grunt mappings 语义侧
// ---------------------------------------------------------------------------

/// @brief methods/fields 的 key = "<名字><描述符>"。方法名字不含 '('，
///        按首个 '(' 切分；字段 key 无 '('——按「左起最早、其后缀整段
///        为合法字段描述符」的位置切分（JVM 字段名可含 L/B 等字符，故
///        首字符启发式会误切，如字段名 "BR75"）。
struct MemberKey
{
    std::string name;
    std::string desc;
};

/// @brief 整段是否为合法字段描述符（数组维度 + L…;/基本型，须吃满段）。
static bool isFieldDesc(const std::string& s)
{
    if (s.empty()) {
        return false;
    }
    size_t i = 0;
    while (i < s.size() && s[i] == '[') {
        ++i;
    }
    if (i >= s.size()) {
        return false;
    }
    const char c = s[i++];
    switch (c) {
    case 'L': return s.find(';', i) == s.size() - 1;
    case 'Z': case 'C': case 'B': case 'S': case 'I': case 'F': case 'J': case 'D':
        return i == s.size();
    default: return false;
    }
}

static bool splitMemberKey(const std::string& key, bool isMethod, MemberKey& out)
{
    if (isMethod) {
        const size_t lp = key.find('(');
        if (lp == std::string::npos || lp == 0) {
            return false;
        }
        out.name = key.substr(0, lp);
        out.desc = key.substr(lp);
        return true;
    }
    for (size_t i = 1; i < key.size(); ++i) {
        if (isFieldDesc(key.substr(i))) {
            out.name = key.substr(0, i);
            out.desc = key.substr(i);
            return true;
        }
    }
    return false;
}

/// @brief 描述符列类名 token 原样替换（origin→obf）：仅替换完整 "L名;"
///        token，数组维度保持形态；未命中 token（java.lang.* 等）原样回。
static std::string mapDescClasses(const std::string& desc,
                                  const std::map<std::string, std::string>& origToObf)
{
    std::string out = desc;
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] != 'L') {
            continue;
        }
        const size_t semi = out.find(';', i);
        if (semi == std::string::npos) {
            break;
        }
        const std::string n  = out.substr(i + 1, semi - i - 1);
        const auto        it = origToObf.find(n);
        if (it != origToObf.end()) {
            out.replace(i + 1, semi - i - 1, it->second);
            i += it->second.size();
        } else {
            i = semi;
        }
    }
    return out;
}

struct GruntMapping
{
    std::map<std::string, std::string> obfToOrig;   ///< obf 类名 → origin 类名
    std::map<std::string, std::string> origToObf;   ///< origin 类名 → obf 类名
    // obfOwner → "<origin 名><origin desc>" → obf 成员名（origin desc 在
    // 组装规则时按 origToObf 重写成 obf 侧匹配形态）。
    std::map<std::string, std::map<std::string, std::string>> obfMethods;
    std::map<std::string, std::map<std::string, std::string>> obfFields;

    ca::usize classCount() const noexcept { return obfToOrig.size(); }
    bool      className(const std::string& obf, std::string& out) const
    {
        auto it = obfToOrig.find(obf);
        if (it == obfToOrig.end()) {
            return false;
        }
        out = it->second;
        return true;
    }
};

static GruntMapping parseMappings(std::string_view text)
{
    mini_json::Parser      parser(text);
    const mini_json::Value root = parser.value();
    if (root.t != mini_json::Value::T::Obj) {
        std::cerr << "mappings.json 顶层非对象\n";
        std::exit(2);
    }
    GruntMapping g;
    for (const auto& [originName, entry] : root.obj) {
        const mini_json::Value* nw = entry.find("new");
        if (nw == nullptr || nw->t != mini_json::Value::T::Str || nw->s.empty()) {
            std::cerr << "[map] 条目缺 'new': " << originName << '\n';
            std::exit(2);
        }
        g.origToObf[originName] = nw->s;
        g.obfToOrig[nw->s]      = originName;

        if (const auto* methods = entry.find("methods"); methods != nullptr
            && methods->t == mini_json::Value::T::Obj) {
            for (const auto& [key, val] : methods->obj) {
                MemberKey mk;
                if (val.t != mini_json::Value::T::Str || !splitMemberKey(key, true, mk)) {
                    std::cerr << "[map] 方法键形病: " << originName << "::" << key << '\n';
                    std::exit(2);
                }
                g.obfMethods[nw->s][key] = val.s;
            }
        }
        if (const auto* fields = entry.find("fields"); fields != nullptr
            && fields->t == mini_json::Value::T::Obj) {
            for (const auto& [key, val] : fields->obj) {
                MemberKey mk;
                if (val.t != mini_json::Value::T::Str || !splitMemberKey(key, false, mk)) {
                    std::cerr << "[map] 字段键形病: " << originName << "::" << key << '\n';
                    std::exit(2);
                }
                g.obfFields[nw->s][key] = val.s;
            }
        }
    }
    return g;
}

// ---------------------------------------------------------------------------
// demo 主体
// ---------------------------------------------------------------------------

namespace {

/// @brief 广播 resolver：全规则组下发每个类（语料闸定型消费姿势——类名
///        族规则必须到达每个引用方；无关规则在类内查命即空 no-op）。
class BroadcastResolver final : public jbc::consumer::IRenameResolver
{
public:
    explicit BroadcastResolver(const RenamePlan& plan) : plan_(plan) {}

    ConsumerResult<const RenamePlan*> planFor(std::string_view) override
    {
        const RenamePlan* sel = &plan_;
        return jbc::core::Ok(sel);
    }

private:
    const RenamePlan& plan_;
};

static std::string slurp(const fs::path& p)
{
    std::ifstream     f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}   // namespace

int main(int argc, char** argv)
{
    const std::vector<std::string> args(argv, argv + argc);
    const bool members =
        std::find(args.begin(), args.end(), std::string("--members")) != args.end();
    if (args.size() < 4) {
        std::cerr << "用法: libjbc_deobf_demo <obf.jar> <mappings.json> "
                     "<out-dir> [--members]\n";
        return 1;
    }
    const fs::path jarPath     = args[1];
    const fs::path mappingPath = args[2];
    const fs::path outDir      = args[3];

    const GruntMapping g = parseMappings(slurp(mappingPath));

    // 装料：obf jar → 内存源（条目名按 CEN 字节序精确枚举，Windows 文件
    // 系统大小写塌缩不发生）。
    ca::zip::ZipFile jar;
    if (jar.open(jarPath.generic_string()).is_err()) {
        std::cerr << "obf jar 打开失败: " << jarPath.generic_string() << '\n';
        return 2;
    }
    MemoryClassSource in;
    ca::usize         entriesTotal = 0;
    for (const std::string& entry : jar.entries()) {
        ++entriesTotal;
        if (entry.size() <= 6 || entry.compare(entry.size() - 6, 6, ".class") != 0) {
            continue;
        }
        std::string name = entry.substr(0, entry.size() - 6);
        auto        rr   = jar.read(entry);
        if (rr.is_err()) {
            std::cerr << "条目读取失败: " << entry << '\n';
            return 2;
        }
        in.add(std::move(name), std::move(rr).unwrap());
    }
    std::cout << "[demo] jar entries: " << entriesTotal << ", obf classes: "
              << in.classCount() << "（mapping kinds: " << g.classCount() << "）\n";

    // 计划 formation（消费方域）：类名族全量 + 可选成员族
    RenamePlan plan;
    for (const auto& [obf, orig] : g.obfToOrig) {
        plan.renameClass(obf, orig);
    }
    if (members) {
        for (const auto& [owner, table] : g.obfMethods) {
            for (const auto& [key, obfName] : table) {
                MemberKey mk;
                if (!splitMemberKey(key, true, mk)) {
                    std::cerr << "[plan] 方法键反切失败: " << owner << "::" << key << '\n';
                    return 2;
                }
                plan.renameMethod(owner, obfName, mapDescClasses(mk.desc, g.origToObf), mk.name);
            }
        }
        for (const auto& [owner, table] : g.obfFields) {
            for (const auto& [key, obfName] : table) {
                MemberKey mk;
                if (!splitMemberKey(key, false, mk)) {
                    std::cerr << "[plan] 字段键反切失败: " << owner << "::" << key << '\n';
                    return 2;
                }
                plan.renameField(owner, obfName, mapDescClasses(mk.desc, g.origToObf), mk.name);
            }
        }
        std::cout << "[demo] members=full：成员规则随行下发（BSM 载体实成员按 v1"
                     " 语义 fail-closed 预期）\n";
    }

    // 跨类驱动（fail-closed：首错即 Err，不产 stage）
    BroadcastResolver resolver(plan);
    auto              r = renameSource(in, resolver);
    if (r.is_err()) {
        std::cerr << "[demo] renameSource 失败: "
                  << jbc::consumer::to_string(r.unwrap_err()) << "（逐类诊断…）\n";
        // 诊断通道：fail-closed 驱动不吐出事类（首错即停）；逐类复刻驱动
        // 的「建会话 + 单类应用」序，定位首个失败类 + 失败族。
        std::vector<std::string_view> diagNames;
        in.listClasses(diagNames);
        bool located = false;
        for (const std::string_view nm : diagNames) {
            const auto bytes = in.bytes(nm).unwrap();
            auto       sr    = jbc::ir::makeSessionFromBytes(bytes);
            if (sr.is_err()) {
                const auto se = sr.unwrap_err();
                std::cerr << "[diag] 会话构造失败: " << nm << "  stage="
                          << (se.stage == jbc::ir::SessionError::Stage::Read ? "read"
                                                                              : "session")
                          << '\n';
                located = true;
                break;
            }
            const auto ses = std::move(sr).unwrap();
            auto       ap  = jbc::consumer::applyClassRename(*ses, plan);
            if (ap.is_err()) {
                const auto re = ap.unwrap_err();
                static constexpr const char* kKinds[] = {"Lower", "Signature",
                                                         "TypeAnnotation", "Decode",
                                                         "BsmTouched", "BadRule"};
                std::cerr << "[diag] 首个失败类: " << nm << "  kind=" << kKinds[int(re.kind)];
                if (re.kind == jbc::consumer::RenameError::Kind::Signature) {
                    std::cerr << "  sigCode=" << jbc::type::to_string(re.sig.sig.code)
                              << "  offset=" << re.sig.sig.offset << '\n';
                    // 规则级二分：单规则复刻本类应用，锁定肇事规则。
                    for (const auto& cr : plan.classes()) {
                        jbc::consumer::RenamePlan solo;
                        solo.renameClass(cr.oldName, cr.newName);
                        auto sr2 = jbc::ir::makeSessionFromBytes(bytes);
                        if (sr2.is_err()) {
                            break;
                        }
                        const auto ses2 = std::move(sr2).unwrap();
                        auto     ap2    = jbc::consumer::applyClassRename(*ses2, solo);
                        if (ap2.is_err()) {
                            const auto e2 = ap2.unwrap_err();
                            std::cerr << "[diag] 肇事规则: " << cr.oldName << " -> "
                                      << cr.newName << "  kind=" << kKinds[int(e2.kind)];
                            if (e2.kind == jbc::consumer::RenameError::Kind::Signature) {
                                std::cerr << "  sigCode="
                                          << jbc::type::to_string(e2.sig.sig.code)
                                          << "  offset=" << e2.sig.sig.offset;
                            }
                            std::cerr << '\n';
                            break;
                        }
                    }
                }
                if (re.kind != jbc::consumer::RenameError::Kind::Signature) {
                    std::cerr << '\n';
                }
                located = true;
                break;
            }
        }
        if (!located) {
            std::cerr << "[diag] 逐类复刻未复现失败（疑驱动编排态）\n";
        }
        return 2;
    }
    auto stage = std::move(r).unwrap();
    std::cout << "[demo] stage classes: " << stage.classCount() << '\n';

    // 落盘：stage 键 = obf 内部名（库契语义：stage 键不随 this_name 改名
    // 移动），文件名按映射还原翻译；未映射键即透传原样。
    std::error_code ec;
    fs::remove_all(outDir, ec);
    fs::create_directories(outDir, ec);
    std::vector<std::string_view> names;
    stage.listClasses(names);
    ca::usize renamed = 0;
    for (std::string_view name : names) {
        const std::string obfName(name);
        std::string       finalName = obfName;
        if (g.className(obfName, finalName)) {
            ++renamed;
        }
        const auto         bytes   = stage.bytes(obfName).unwrap();
        const fs::path     outPath = outDir / (finalName + ".class");
        fs::create_directories(outPath.parent_path(), ec);
        std::ofstream      o(outPath, std::ios::binary);
        o.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
        if (!o) {
            std::cerr << "[demo] 写盘失败: " << finalName << '\n';
            return 2;
        }
    }
    std::cout << "[demo] renamed classes: " << renamed << " / " << stage.classCount()
              << (members ? "  （含成员族规则命中侧写；BSM 载体实成员 fail-closed 预期红）"
                          : "  （passthrough = 0 期望）")
              << '\n';
    std::cout << "[demo] out: " << outDir.generic_string() << '\n';
    std::cout << (members ? "[demo] mode=members-full done" : "[demo] mode=class-only done")
              << '\n';
    return 0;
}
