/// @file        no_visitor_gate_test.cpp
/// @brief       visitor 清零门禁（design.md §11 visitor 清零）：src 内不得出现
///              visitor 形状——类型名、visit* 方法、std::function 回调 sink。
///
/// 判定标准不是类名里有没有 Visitor，而是数据「拉回来还是推给你」（design.md §11）；
/// 本测试把它落成三个机械模式。注释与字符串先被掩蔽（替换为空格、保留换行，
/// 行号因此仍然准确），所以「设计上拒绝 visitor」这类正当描述不会误伤。
/// 确需豁免：先改 design.md §11 的条款，再改这里——门禁与文档同源。
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

/// 掩蔽注释与字符串字面量：逐字符替换为空格，换行保留——掩蔽后行号不漂移。
std::string maskNoise(const std::string& src)
{
    std::string out(src.size(), ' ');
    enum class St { Normal, LineComment, BlockComment, Str, Chr } st = St::Normal;
    for (std::size_t i = 0; i < src.size(); ++i) {
        const char c = src[i];
        const char n = (i + 1 < src.size()) ? src[i + 1] : '\0';
        if (st == St::Normal) {
            if (c == '/' && n == '/') {
                out[i] = ' ';
                out[i + 1] = ' ';
                ++i;
                st = St::LineComment;
            } else if (c == '/' && n == '*') {
                out[i] = ' ';
                out[i + 1] = ' ';
                ++i;
                st = St::BlockComment;
            } else if (c == '"') {
                out[i] = ' ';
                st = St::Str;
            } else if (c == '\'') {
                out[i] = ' ';
                st = St::Chr;
            } else {
                out[i] = c;
            }
            continue;
        }
        if (st == St::LineComment) {
            if (c == '\n') {
                out[i] = '\n';
                st = St::Normal;
            }
            continue;
        }
        if (st == St::BlockComment) {
            if (c == '*' && n == '/') {
                out[i] = ' ';
                out[i + 1] = ' ';
                ++i;
                st = St::Normal;
            } else if (c == '\n') {
                out[i] = '\n';
            }
            continue;
        }
        // 字符串 / 字符字面量：内容掩蔽，换行保留（防未闭合时行号塌掉）
        const char quote = (st == St::Str) ? '"' : '\'';
        if (c == '\\' && i + 1 < src.size()) {
            out[i] = (c == '\n') ? '\n' : ' ';
            out[i + 1] = (src[i + 1] == '\n') ? '\n' : ' ';
            ++i;
            continue;
        }
        out[i] = (c == '\n') ? '\n' : ' ';
        if (c == quote) {
            st = St::Normal;
        }
    }
    return out;
}

struct Violation
{
    std::string file;
    int line = 0;
    std::string pattern;
};

/// 定位 src 根：优先运行目录约定（xmake `set_rundir("$(projectdir)")` →
/// 仓根），回退 __FILE__（编译期路径，测试文件在 test/unittest/ 下）。
/// 两处都找不到 = 测试环境错配，返回空由调用方 FAIL——门禁缺席必须可见，
/// 不做 skip。
fs::path srcRoot()
{
    std::error_code ec;
    const fs::path byCwd = fs::path("libs") / "libjbc" / "src";
    if (fs::is_directory(byCwd, ec) && !ec) {
        return fs::absolute(byCwd, ec);
    }
    fs::path self(__FILE__);
    self = self.parent_path().parent_path().parent_path() / "src";
    if (fs::is_directory(self, ec) && !ec) {
        return fs::absolute(self, ec);
    }
    return {};
}

/// 门禁扫描的扩展名集合：src 现用 .hpp/.cpp，另收常见 C++ 头/源扩展。
/// 集合外的文件在扫描循环里显式报出（FAIL）——新扩展静默逃逸门禁与
/// 「门禁缺席必须可见」同一性质，不允许。
constexpr std::array<std::string_view, 10> kScanExt = {
    ".h", ".hpp", ".hh", ".hxx", ".inl",
    ".cpp", ".cc", ".cxx", ".inc", ".ipp"};

}  // namespace

TEST(NoVisitorGate, SrcHasNoVisitorShapes)
{
    const fs::path root = srcRoot();
    ASSERT_FALSE(root.empty())
        << "找不到 src/：请在 xmake set_rundir 的 projectdir 下运行 "
           "(xmake run libjbc_unittest)，门禁不能静默缺席";

    // 三个机械模式（与文档同源；改其一先改另一）
    const std::vector<std::pair<std::regex, std::string>> bans = {
        {std::regex(R"(\b(class|struct)\s+\w*Visitor\b)"), "(class|struct) XxxVisitor"},
        {std::regex(R"(\bvisit\w*\s*\()"), "visitXxx(...)"},
        {std::regex(R"(\bstd::function\b)"), "std::function 回调 sink"},
    };

    std::vector<Violation> hits;
    std::vector<std::string> unclassified;
    std::size_t scanned = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec) || ec) {
            continue;
        }
        const fs::path p = it->path();
        const std::string ext = p.extension().string();
        if (std::find(kScanExt.begin(), kScanExt.end(), ext) == kScanExt.end()) {
            unclassified.push_back(p.lexically_relative(root).string());
            continue;
        }
        std::ifstream in(p, std::ios::binary);
        if (!in) {
            ADD_FAILURE() << "无法读取 " << p.string();
            continue;
        }
        std::ostringstream buf;
        buf << in.rdbuf();
        const std::string masked = maskNoise(buf.str());
        ++scanned;
        for (const auto& [re, name] : bans) {
            for (std::sregex_iterator m(masked.begin(), masked.end(), re), mend; m != mend; ++m) {
                const std::ptrdiff_t off = m->position(0);
                const int line = static_cast<int>(
                    std::count(masked.begin(), masked.begin() + off, '\n') + 1);
                hits.push_back(Violation{p.lexically_relative(root).string(), line, name});
            }
        }
    }
    ASSERT_FALSE(ec) << "目录遍历失败: " << ec.message();

    std::ostringstream msg;
    for (const Violation& v : hits) {
        msg << "\n  " << v.file << ":" << v.line << " 命中 " << v.pattern;
    }
    for (const std::string& f : unclassified) {
        msg << "\n  未扫描 " << f;
    }
    EXPECT_TRUE(unclassified.empty())
        << "src 下存在不在扫描集合里的文件（" << unclassified.size()
        << " 个）——新扩展必须显式加进 kScanExt（并接受门禁扫描）或移出 "
           "src，不允许静默逃逸：" << msg.str();
    EXPECT_TRUE(hits.empty())
        << "visitor 形状残留（" << hits.size() << " 处 / 扫描 " << scanned
        << " 文件）——libjbc 不含 visitor，无论公共 API、codec 内部还是 adapter"
           "（design.md §11 visitor 清零）：" << msg.str();
}
