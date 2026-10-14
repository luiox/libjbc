#include "jbc/type/signature.hpp"

#include <algorithm>

namespace jbc::type {

namespace {

/// @brief 数组维数上限（对齐描述符层与 classfile 的 255）。
constexpr ca::u32 kMaxDims = 255;

/// @brief 类型实参嵌套深度上限（真实 javac 输出 < 20；防对抗输入的栈炸弹）。
constexpr ca::u32 kMaxNesting = 255;

/// @brief 签名词法保留字符（JVMS 4.7.9.1：Identifier 不得含这 7 个 ASCII 字符；
///        其余字节一律按标识符内容放行——roundtrip 保真的前提）。
bool isReserved(char c) noexcept
{
    return c == '.' || c == ';' || c == '[' || c == '/' || c == '<' || c == '>' || c == ':';
}

char baseTypeChar(BaseType b) noexcept
{
    switch (b) {
    case BaseType::Byte: return 'B';
    case BaseType::Char: return 'C';
    case BaseType::Double: return 'D';
    case BaseType::Float: return 'F';
    case BaseType::Int: return 'I';
    case BaseType::Long: return 'J';
    case BaseType::Short: return 'S';
    case BaseType::Boolean: return 'Z';
    }
    return 'I';
}

/// @brief 基类型字符映射；非基类型字符返回 false。
bool baseTypeFromChar(char c, BaseType& out) noexcept
{
    switch (c) {
    case 'B': out = BaseType::Byte; return true;
    case 'C': out = BaseType::Char; return true;
    case 'D': out = BaseType::Double; return true;
    case 'F': out = BaseType::Float; return true;
    case 'I': out = BaseType::Int; return true;
    case 'J': out = BaseType::Long; return true;
    case 'S': out = BaseType::Short; return true;
    case 'Z': out = BaseType::Boolean; return true;
    default: return false;
    }
}

/// @brief 解析游标：位置即错误坐标；嵌套深度经参数逐层下传。
struct SigParser
{
    std::string_view s;
    ca::usize pos = 0;

    bool eof() const noexcept { return pos >= s.size(); }
    char peek() const noexcept { return s[pos]; }

    template <typename T>
    Result<T, SigError> fail(SigErrorCode code) const
    {
        return Err(SigError{code, static_cast<ca::u32>(pos)});
    }

    /// @brief 扫描一个标识符（保留字符前停）；空即 Err(BadIdentifier)。
    Result<std::string_view, SigError> scanIdentifier()
    {
        const ca::usize begin = pos;
        while (pos < s.size() && !isReserved(s[pos])) {
            ++pos;
        }
        if (pos == begin) {
            return Err(SigError{SigErrorCode::BadIdentifier, static_cast<ca::u32>(begin)});
        }
        return Ok(s.substr(begin, pos - begin));
    }

    /// @brief 解析一个类型（JVMS JavaTypeSignature）：前置 '[' 维数 + 叶子。
    /// @param allow_bare_primitive 方法参数/返回位允许裸基元（TypeSignature）；
    ///        纯引用位置（类型实参/界/字段签名）为 false。
    /// @param depth 类型实参嵌套深度（防栈炸弹，超 @ref kMaxNesting 即 Err）。
    Result<SigType, SigError> parseType(bool allow_bare_primitive, ca::u32 depth)
    {
        ca::u32 dims = 0;
        while (!eof() && peek() == '[') {
            ++dims;
            ++pos;
            if (dims > kMaxDims) {
                return fail<SigType>(SigErrorCode::NestingTooDeep);
            }
        }
        if (eof()) {
            return fail<SigType>(SigErrorCode::Truncated);
        }
        SigType t;
        t.array_dims = dims;
        const char c = peek();
        if (c == 'L') {
            auto ct = parseClassType(depth);
            if (ct.is_err()) {
                return Err(std::move(ct).unwrap_err());
            }
            t.kind       = SigType::Kind::Class;
            t.class_type = std::move(ct).unwrap();
            return Ok(std::move(t));
        }
        if (c == 'T') {
            ++pos;
            auto id = scanIdentifier();
            if (id.is_err()) {
                return Err(std::move(id).unwrap_err());
            }
            if (eof() || peek() != ';') {
                return fail<SigType>(SigErrorCode::Truncated);
            }
            ++pos;
            t.kind          = SigType::Kind::TypeVar;
            t.type_var_name = std::move(id).unwrap();
            return Ok(std::move(t));
        }
        BaseType base;
        if (!baseTypeFromChar(c, base)) {
            return fail<SigType>(SigErrorCode::BadBaseType);
        }
        if (dims == 0 && !allow_bare_primitive) {
            return fail<SigType>(SigErrorCode::BadPrimitive);
        }
        ++pos;
        t.kind      = SigType::Kind::Primitive;
        t.primitive = base;
        return Ok(std::move(t));
    }

    /// @brief 解析类类型（调用方保证 peek()=='L'）：包前缀 + 段链 + ';'。
    Result<SigClassType, SigError> parseClassType(ca::u32 depth)
    {
        ++pos;   // 越过 'L'
        SigClassType ct;
        const ca::usize prefix_begin = pos;
        std::string_view first_name;
        for (;;) {
            if (eof()) {
                return fail<SigClassType>(SigErrorCode::Truncated);
            }
            auto id = scanIdentifier();
            if (id.is_err()) {
                return Err(std::move(id).unwrap_err());
            }
            if (!eof() && peek() == '/') {
                ++pos;   // 包段分隔，继续包前缀扫描
                continue;
            }
            first_name = std::move(id).unwrap();
            break;
        }
        ct.package_prefix = s.substr(prefix_begin, pos - first_name.size() - prefix_begin);

        SigClassSegment seg0;
        seg0.name = first_name;
        if (!eof() && peek() == '<') {
            auto args = parseTypeArgs(depth);
            if (args.is_err()) {
                return Err(std::move(args).unwrap_err());
            }
            seg0.type_args = std::move(args).unwrap();
        }
        ct.segments.push_back(std::move(seg0));

        while (!eof() && peek() == '.') {
            ++pos;
            SigClassSegment seg;
            auto id = scanIdentifier();
            if (id.is_err()) {
                return Err(std::move(id).unwrap_err());
            }
            seg.name = std::move(id).unwrap();
            if (!eof() && peek() == '<') {
                auto args = parseTypeArgs(depth);
                if (args.is_err()) {
                    return Err(std::move(args).unwrap_err());
                }
                seg.type_args = std::move(args).unwrap();
            }
            ct.segments.push_back(std::move(seg));
        }
        if (eof()) {
            return fail<SigClassType>(SigErrorCode::Truncated);
        }
        if (peek() != ';') {
            return fail<SigClassType>(SigErrorCode::BadClassType);
        }
        ++pos;
        return Ok(std::move(ct));
    }

    /// @brief 解析类型实参表（调用方保证 peek()=='<'）。
    Result<std::vector<SigTypeArg>, SigError> parseTypeArgs(ca::u32 depth)
    {
        if (depth >= kMaxNesting) {
            return fail<std::vector<SigTypeArg>>(SigErrorCode::NestingTooDeep);
        }
        const ca::u32 inner = depth + 1;
        ++pos;   // 越过 '<'
        std::vector<SigTypeArg> args;
        for (;;) {
            if (eof()) {
                return fail<std::vector<SigTypeArg>>(SigErrorCode::Truncated);
            }
            if (peek() == '>') {
                return fail<std::vector<SigTypeArg>>(SigErrorCode::BadTypeArg);   // 空表非法
            }
            SigTypeArg arg;
            const char c = peek();
            if (c == '*') {
                ++pos;
                arg.kind = SigTypeArg::Kind::Unbounded;
            }
            else {
                if (c == '+') {
                    arg.kind = SigTypeArg::Kind::UpperBound;
                    ++pos;
                }
                else if (c == '-') {
                    arg.kind = SigTypeArg::Kind::LowerBound;
                    ++pos;
                }
                else {
                    arg.kind = SigTypeArg::Kind::Invariant;
                }
                auto bound = parseType(/*allow_bare_primitive=*/false, inner);
                if (bound.is_err()) {
                    return Err(std::move(bound).unwrap_err());
                }
                arg.bound = std::make_unique<SigType>(std::move(bound).unwrap());
            }
            args.push_back(std::move(arg));
            if (eof()) {
                return fail<std::vector<SigTypeArg>>(SigErrorCode::Truncated);
            }
            if (peek() == '>') {
                ++pos;
                break;
            }
        }
        return Ok(std::move(args));
    }

    /// @brief 解析类型参数表（调用方保证 peek()=='<'）。
    Result<std::vector<SigTypeParam>, SigError> parseTypeParams()
    {
        ++pos;   // 越过 '<'
        std::vector<SigTypeParam> params;
        for (;;) {
            if (eof()) {
                return fail<std::vector<SigTypeParam>>(SigErrorCode::Truncated);
            }
            if (peek() == '>') {
                return fail<std::vector<SigTypeParam>>(SigErrorCode::BadTypeParams);   // 空表非法
            }
            SigTypeParam tp;
            auto id = scanIdentifier();
            if (id.is_err()) {
                return Err(std::move(id).unwrap_err());
            }
            tp.name = std::move(id).unwrap();
            if (eof() || peek() != ':') {
                return fail<std::vector<SigTypeParam>>(SigErrorCode::BadTypeParams);
            }
            ++pos;   // 越过类界冒号
            if (!eof() && (peek() == ':' || peek() == '>')) {
                // 类界缺席（JVMS ClassBound 的引用类型可省）：javac 的 `E::`
                // 形态；`<T:>` 则是类界与接口界全省。
                tp.class_bound = nullptr;
            }
            else if (!eof() && (peek() == 'L' || peek() == 'T' || peek() == '[')) {
                auto bound = parseType(/*allow_bare_primitive=*/false, 0);
                if (bound.is_err()) {
                    return Err(std::move(bound).unwrap_err());
                }
                tp.class_bound = std::make_unique<SigType>(std::move(bound).unwrap());
            }
            else {
                return fail<std::vector<SigTypeParam>>(SigErrorCode::BadBound);
            }
            while (!eof() && peek() == ':') {
                ++pos;   // 越过接口界冒号
                if (eof() || (peek() != 'L' && peek() != 'T' && peek() != '[')) {
                    return fail<std::vector<SigTypeParam>>(SigErrorCode::BadBound);
                }
                auto bound = parseType(/*allow_bare_primitive=*/false, 0);
                if (bound.is_err()) {
                    return Err(std::move(bound).unwrap_err());
                }
                tp.interface_bounds.push_back(std::move(bound).unwrap());
            }
            params.push_back(std::move(tp));
            if (eof()) {
                return fail<std::vector<SigTypeParam>>(SigErrorCode::Truncated);
            }
            if (peek() == '>') {
                ++pos;
                break;
            }
        }
        return Ok(std::move(params));
    }
};

/// @brief 消费可选的类型参数表；存在但非法即 Err。
Result<std::vector<SigTypeParam>, SigError> parseOptTypeParams(SigParser& p)
{
    if (!p.eof() && p.peek() == '<') {
        return p.parseTypeParams();
    }
    return Ok(std::vector<SigTypeParam>{});
}

/// @brief 消费一个父类/接口位置的类类型（语法限定 Class 叶子）；非法即 Err。
Result<SigType, SigError> parseSuperEntry(SigParser& p)
{
    if (p.eof() || p.peek() != 'L') {
        return p.fail<SigType>(SigErrorCode::BadClassType);
    }
    return p.parseType(/*allow_bare_primitive=*/false, 0);
}

void appendTypeParams(ca::str::Utf8StringBuilder& b, const std::vector<SigTypeParam>& tps)
{
    if (tps.empty()) {
        return;
    }
    const char lt = '<';
    const char gt = '>';
    const char colon = ':';
    b.append(&lt, 1);
    for (const SigTypeParam& tp : tps) {
        b.append(tp.name.data(), tp.name.size());
        b.append(&colon, 1);
        if (tp.class_bound != nullptr) {
            appendSigType(b, *tp.class_bound);
        }
        for (const SigType& ib : tp.interface_bounds) {
            b.append(&colon, 1);
            appendSigType(b, ib);
        }
    }
    b.append(&gt, 1);
}

void appendTypeArgs(ca::str::Utf8StringBuilder& b, const std::vector<SigTypeArg>& args)
{
    if (args.empty()) {
        return;
    }
    const char lt = '<';
    const char gt = '>';
    b.append(&lt, 1);
    for (const SigTypeArg& arg : args) {
        if (arg.kind == SigTypeArg::Kind::Unbounded) {
            const char star = '*';
            b.append(&star, 1);
            continue;
        }
        if (arg.kind == SigTypeArg::Kind::UpperBound) {
            const char plus = '+';
            b.append(&plus, 1);
        }
        else if (arg.kind == SigTypeArg::Kind::LowerBound) {
            const char minus = '-';
            b.append(&minus, 1);
        }
        appendSigType(b, *arg.bound);
    }
    b.append(&gt, 1);
}

/// @brief 按 '$' 切内部名为段；段为空或字符集非法即 Err(BadIdentifier)。
Result<std::vector<std::string_view>, SigError>
splitInternalName(std::string_view name)
{
    std::vector<std::string_view> parts;
    ca::usize begin = 0;
    for (ca::usize i = 0; i <= name.size(); ++i) {
        if (i == name.size() || name[i] == '$') {
            const std::string_view part = name.substr(begin, i - begin);
            const bool head = parts.empty();
            if (head ? !isValidSigInternalName(part) : !isValidSigSimpleName(part)) {
                return Err(SigError{SigErrorCode::BadIdentifier, static_cast<ca::u32>(begin)});
            }
            parts.push_back(part);
            begin = i + 1;
        }
    }
    return Ok(std::move(parts));
}

/// @brief 一个匹配段的改写坐标（升序应用，前向拼装以保其余字节原序）。
struct RenamePatch
{
    ca::u32 offset;
    ca::u32 length;
    ca::str::Utf8String replacement;   ///< 段级替换串（多部件时按 '$' 合成）
};

/// @brief 段名里的 '$' 个数（内部类嵌套层次；决定该段对应几个 '$' 部件）。
ca::usize dollarCount(std::string_view s) noexcept
{
    ca::usize n = 0;
    for (const char c : s) {
        if (c == '$') {
            ++n;
        }
    }
    return n;
}

/// @brief 段名是否等于部件序列按 '$' 连接的结果（零分配逐段比对）。
/// @param head 首部件（seg0 时已剥离包前缀；后缀段即 old_parts[part]）。
bool segmentNameMatches(std::string_view seg_name, std::string_view head,
                        const std::vector<std::string_view>& parts, ca::usize part,
                        ca::usize need)
{
    for (ca::usize k = 0; k < need; ++k) {
        const std::string_view piece = (k == 0) ? head : parts[part + k];
        if (seg_name.size() < piece.size()
            || seg_name.substr(0, piece.size()) != piece) {
            return false;
        }
        seg_name.remove_prefix(piece.size());
        if (k + 1 < need) {
            if (seg_name.empty() || seg_name.front() != '$') {
                return false;
            }
            seg_name.remove_prefix(1);
        }
    }
    return seg_name.empty();
}

/// @brief 类类型全名是否与 old 段部件吻合；命中时把各段改写坐标填入 patches。
///
/// 内部名在签名字节里有两种等价形态——'.' 段链（`Outer<T>.Inner`）与段名内
/// 嵌 '$'（`Outer$Inner`，javac 常规产物）——统一按全名相等匹配：段名内的
/// 每个 '$' 使该段多消费一个 old 部件，替换时新部件按同结构 '$' 合成，段链
/// 结构原样保留。old 部件数多于段链可消费量不命中。
bool matchClassType(const SigClassType& ct, std::string_view input,
                    const std::vector<std::string_view>& old_parts,
                    const std::vector<std::string_view>& new_parts,
                    std::vector<RenamePatch>& patches)
{
    ca::usize part = 0;
    for (ca::usize i = 0; i < ct.segments.size(); ++i) {
        const SigClassSegment& seg = ct.segments[i];
        const ca::usize need = dollarCount(seg.name) + 1;
        if (part + need > old_parts.size()) {
            return false;
        }
        std::string_view head;
        if (i == 0) {
            // 首段：部件带包前缀，先对齐前缀字节（前缀与段名在输入中连续）。
            if (ct.package_prefix.size() > old_parts[part].size()) {
                return false;
            }
            if (ct.package_prefix
                != old_parts[part].substr(0, ct.package_prefix.size())) {
                return false;
            }
            head = old_parts[part].substr(ct.package_prefix.size());
        }
        else {
            head = old_parts[part];
        }
        if (!segmentNameMatches(seg.name, head, old_parts, part, need)) {
            return false;
        }

        // 段级替换串：对应新部件按同结构 '$' 合成（分配进 patch 持有）。
        ca::str::Utf8StringBuilder rb;
        for (ca::usize k = 0; k < need; ++k) {
            if (k > 0) {
                const char dollar = '$';
                rb.append(&dollar, 1);
            }
            const std::string_view np = new_parts[part + k];
            rb.append(np.data(), np.size());
        }
        ca::str::Utf8String replacement = rb.build();

        const ca::u32 base =
            static_cast<ca::u32>(ct.package_prefix.data() - input.data());
        if (i == 0) {
            patches.push_back(RenamePatch{
                base,
                static_cast<ca::u32>(ct.package_prefix.size() + seg.name.size()),
                std::move(replacement)});
        }
        else {
            const ca::u32 off = static_cast<ca::u32>(seg.name.data() - input.data());
            patches.push_back(
                RenamePatch{off, static_cast<ca::u32>(seg.name.size()),
                            std::move(replacement)});
        }
        part += need;
    }
    return part == old_parts.size();
}

}   // namespace

const char* to_string(SigErrorCode e) noexcept
{
    switch (e) {
    case SigErrorCode::Ok: return "ok";
    case SigErrorCode::Empty: return "empty signature";
    case SigErrorCode::Truncated: return "truncated signature";
    case SigErrorCode::TrailingChars: return "trailing characters after signature";
    case SigErrorCode::BadSignatureShape: return "not a class/method/field signature";
    case SigErrorCode::BadIdentifier: return "bad identifier";
    case SigErrorCode::BadTypeParams: return "bad type parameter list";
    case SigErrorCode::BadBound: return "bad type parameter bound";
    case SigErrorCode::BadTypeArg: return "bad type argument list";
    case SigErrorCode::BadClassType: return "bad class type signature";
    case SigErrorCode::BadMethodShape: return "method signature must start with '('";
    case SigErrorCode::BadThrows: return "bad throws clause";
    case SigErrorCode::BadPrimitive: return "bare primitive in reference-type position";
    case SigErrorCode::BadBaseType: return "bad base type char";
    case SigErrorCode::NestingTooDeep: return "nesting over 255";
    case SigErrorCode::RenameShapeMismatch:
        return "rename old/new internal name $-shape mismatch";
    }
    return "unknown";
}

Result<SigClassSignature, SigError> parseClassSignature(std::string_view s)
{
    if (s.empty()) {
        return Err(SigError{SigErrorCode::Empty, 0});
    }
    SigParser p{s};
    SigClassSignature cs;
    auto tps = parseOptTypeParams(p);
    if (tps.is_err()) {
        return Err(std::move(tps).unwrap_err());
    }
    cs.type_params = std::move(tps).unwrap();
    auto sup = parseSuperEntry(p);
    if (sup.is_err()) {
        return Err(std::move(sup).unwrap_err());
    }
    cs.superclass = std::move(sup).unwrap();
    while (!p.eof() && p.peek() == 'L') {
        auto itf = p.parseType(/*allow_bare_primitive=*/false, 0);
        if (itf.is_err()) {
            return Err(std::move(itf).unwrap_err());
        }
        cs.super_interfaces.push_back(std::move(itf).unwrap());
    }
    if (!p.eof()) {
        return p.fail<SigClassSignature>(SigErrorCode::TrailingChars);
    }
    return Ok(std::move(cs));
}

Result<SigMethodSignature, SigError> parseMethodSignature(std::string_view s)
{
    if (s.empty()) {
        return Err(SigError{SigErrorCode::Empty, 0});
    }
    SigParser p{s};
    SigMethodSignature ms;
    auto tps = parseOptTypeParams(p);
    if (tps.is_err()) {
        return Err(std::move(tps).unwrap_err());
    }
    ms.type_params = std::move(tps).unwrap();
    if (p.eof() || p.peek() != '(') {
        return p.fail<SigMethodSignature>(SigErrorCode::BadMethodShape);
    }
    ++p.pos;   // 越过 '('
    while (!p.eof() && p.peek() != ')') {
        auto param = p.parseType(/*allow_bare_primitive=*/true, 0);
        if (param.is_err()) {
            return Err(std::move(param).unwrap_err());
        }
        ms.params.push_back(std::move(param).unwrap());
    }
    if (p.eof()) {
        return p.fail<SigMethodSignature>(SigErrorCode::Truncated);
    }
    ++p.pos;   // 越过 ')'
    if (p.eof()) {
        return p.fail<SigMethodSignature>(SigErrorCode::Truncated);
    }
    if (p.peek() == 'V') {
        ms.ret_void = true;
        ++p.pos;
    }
    else {
        auto ret = p.parseType(/*allow_bare_primitive=*/true, 0);
        if (ret.is_err()) {
            return Err(std::move(ret).unwrap_err());
        }
        ms.ret = std::move(ret).unwrap();
    }
    while (!p.eof() && p.peek() == '^') {
        ++p.pos;
        if (p.eof() || (p.peek() != 'L' && p.peek() != 'T')) {
            return p.fail<SigMethodSignature>(SigErrorCode::BadThrows);
        }
        auto thrown = p.parseType(/*allow_bare_primitive=*/false, 0);
        if (thrown.is_err()) {
            return Err(std::move(thrown).unwrap_err());
        }
        ms.throws_list.push_back(std::move(thrown).unwrap());
    }
    if (!p.eof()) {
        return p.fail<SigMethodSignature>(SigErrorCode::TrailingChars);
    }
    return Ok(std::move(ms));
}

Result<SigType, SigError> parseFieldSignature(std::string_view s)
{
    if (s.empty()) {
        return Err(SigError{SigErrorCode::Empty, 0});
    }
    SigParser p{s};
    auto t = p.parseType(/*allow_bare_primitive=*/false, 0);
    if (t.is_err()) {
        return Err(std::move(t).unwrap_err());
    }
    if (!p.eof()) {
        return p.fail<SigType>(SigErrorCode::TrailingChars);
    }
    return t;
}

Result<Signature, SigError> parseSignature(std::string_view s)
{
    if (s.empty()) {
        return Err(SigError{SigErrorCode::Empty, 0});
    }
    Signature sig;
    switch (s[0]) {
    case '(': {
        auto m = parseMethodSignature(s);
        if (m.is_err()) {
            return Err(std::move(m).unwrap_err());
        }
        sig.form       = SignatureForm::Method;
        sig.method_sig = std::move(m).unwrap();
        return Ok(std::move(sig));
    }
    case '[':
    case 'T': {
        auto f = parseFieldSignature(s);
        if (f.is_err()) {
            return Err(std::move(f).unwrap_err());
        }
        sig.form      = SignatureForm::Field;
        sig.field_sig = std::move(f).unwrap();
        return Ok(std::move(sig));
    }
    case 'L': {
        // 类形态是字段形态的语法超集：完整消费即类形态，否则回退字段形态；
        // 两者皆败报告类形态错误（第一次尝试）。
        auto c = parseClassSignature(s);
        if (c.is_ok()) {
            sig.form      = SignatureForm::Class;
            sig.class_sig = std::move(c).unwrap();
            return Ok(std::move(sig));
        }
        const SigError class_err = std::move(c).unwrap_err();
        auto f = parseFieldSignature(s);
        if (f.is_ok()) {
            sig.form      = SignatureForm::Field;
            sig.field_sig = std::move(f).unwrap();
            return Ok(std::move(sig));
        }
        return Err(class_err);
    }
    case '<': {
        // 带类型参数：先按类形态试（<..> 后应为父类 L），失败回退方法形态。
        auto c = parseClassSignature(s);
        if (c.is_ok()) {
            sig.form      = SignatureForm::Class;
            sig.class_sig = std::move(c).unwrap();
            return Ok(std::move(sig));
        }
        const SigError class_err = std::move(c).unwrap_err();
        auto m = parseMethodSignature(s);
        if (m.is_ok()) {
            sig.form       = SignatureForm::Method;
            sig.method_sig = std::move(m).unwrap();
            return Ok(std::move(sig));
        }
        return Err(class_err);
    }
    default:
        return Err(SigError{SigErrorCode::BadSignatureShape, 0});
    }
}

void appendClassType(ca::str::Utf8StringBuilder& b, const SigClassType& ct)
{
    const char l = 'L';
    const char dot = '.';
    const char semi = ';';
    b.append(&l, 1);
    b.append(ct.package_prefix.data(), ct.package_prefix.size());
    for (ca::usize i = 0; i < ct.segments.size(); ++i) {
        if (i > 0) {
            b.append(&dot, 1);
        }
        b.append(ct.segments[i].name.data(), ct.segments[i].name.size());
        appendTypeArgs(b, ct.segments[i].type_args);
    }
    b.append(&semi, 1);
}

void appendSigType(ca::str::Utf8StringBuilder& b, const SigType& t)
{
    for (ca::u32 i = 0; i < t.array_dims; ++i) {
        const char lb = '[';
        b.append(&lb, 1);
    }
    switch (t.kind) {
    case SigType::Kind::Primitive: {
        const char c = baseTypeChar(t.primitive);
        b.append(&c, 1);
        break;
    }
    case SigType::Kind::Class:
        appendClassType(b, t.class_type);
        break;
    case SigType::Kind::TypeVar: {
        const char tv = 'T';
        const char semi = ';';
        b.append(&tv, 1);
        b.append(t.type_var_name.data(), t.type_var_name.size());
        b.append(&semi, 1);
        break;
    }
    }
}

void appendSignature(ca::str::Utf8StringBuilder& b, const Signature& sig)
{
    switch (sig.form) {
    case SignatureForm::Class: {
        appendTypeParams(b, sig.class_sig.type_params);
        appendSigType(b, sig.class_sig.superclass);
        for (const SigType& itf : sig.class_sig.super_interfaces) {
            appendSigType(b, itf);
        }
        break;
    }
    case SignatureForm::Method: {
        appendTypeParams(b, sig.method_sig.type_params);
        const char lp = '(';
        const char rp = ')';
        const char v = 'V';
        const char hat = '^';
        b.append(&lp, 1);
        for (const SigType& param : sig.method_sig.params) {
            appendSigType(b, param);
        }
        b.append(&rp, 1);
        if (sig.method_sig.ret_void) {
            b.append(&v, 1);
        }
        else {
            appendSigType(b, sig.method_sig.ret);
        }
        for (const SigType& thrown : sig.method_sig.throws_list) {
            b.append(&hat, 1);
            appendSigType(b, thrown);
        }
        break;
    }
    case SignatureForm::Field:
        appendSigType(b, sig.field_sig);
        break;
    }
}

ca::str::Utf8String unparseClassSignature(const SigClassSignature& cs)
{
    ca::str::Utf8StringBuilder b;
    b.reserve(64);
    appendTypeParams(b, cs.type_params);
    appendSigType(b, cs.superclass);
    for (const SigType& itf : cs.super_interfaces) {
        appendSigType(b, itf);
    }
    return b.build();
}

ca::str::Utf8String unparseMethodSignature(const SigMethodSignature& ms)
{
    ca::str::Utf8StringBuilder b;
    b.reserve(64);
    appendTypeParams(b, ms.type_params);
    const char lp = '(';
    const char rp = ')';
    const char v = 'V';
    const char hat = '^';
    b.append(&lp, 1);
    for (const SigType& param : ms.params) {
        appendSigType(b, param);
    }
    b.append(&rp, 1);
    if (ms.ret_void) {
        b.append(&v, 1);
    }
    else {
        appendSigType(b, ms.ret);
    }
    for (const SigType& thrown : ms.throws_list) {
        b.append(&hat, 1);
        appendSigType(b, thrown);
    }
    return b.build();
}

ca::str::Utf8String unparseFieldSignature(const SigType& t)
{
    ca::str::Utf8StringBuilder b;
    b.reserve(64);
    appendSigType(b, t);
    return b.build();
}

ca::str::Utf8String unparseSignature(const Signature& sig)
{
    ca::str::Utf8StringBuilder b;
    b.reserve(64);
    appendSignature(b, sig);
    return b.build();
}

void collectClassNameSpans(const Signature& sig, std::string_view input,
                           std::vector<SigClassNameSpan>& out)
{
    const char* base = input.data();

    auto walkSigType = [&](const SigType& t, auto&& self) -> void {
        if (t.kind != SigType::Kind::Class) {
            return;
        }
        const SigClassType& ct = t.class_type;
        for (ca::usize i = 0; i < ct.segments.size(); ++i) {
            SigClassNameSpan span;
            if (i == 0) {
                // 首段全名：包前缀 + 段名在输入中字节连续，合一个 span。
                span.kind   = SigClassNameSpan::Kind::InternalName;
                span.offset = static_cast<ca::u32>(ct.package_prefix.data() - base);
                span.length = static_cast<ca::u32>(ct.package_prefix.size()
                                                   + ct.segments[0].name.size());
            }
            else {
                span.kind   = SigClassNameSpan::Kind::SimpleName;
                span.offset = static_cast<ca::u32>(ct.segments[i].name.data() - base);
                span.length = static_cast<ca::u32>(ct.segments[i].name.size());
            }
            out.push_back(span);
            // 实参紧跟段名之后：span 序保持字节序。
            for (const SigTypeArg& arg : ct.segments[i].type_args) {
                if (arg.bound != nullptr) {
                    self(*arg.bound, self);
                }
            }
        }
    };

    auto walkParams = [&](const std::vector<SigTypeParam>& tps) {
        for (const SigTypeParam& tp : tps) {
            if (tp.class_bound != nullptr) {
                walkSigType(*tp.class_bound, walkSigType);
            }
            for (const SigType& ib : tp.interface_bounds) {
                walkSigType(ib, walkSigType);
            }
        }
    };

    switch (sig.form) {
    case SignatureForm::Class:
        walkParams(sig.class_sig.type_params);
        walkSigType(sig.class_sig.superclass, walkSigType);
        for (const SigType& itf : sig.class_sig.super_interfaces) {
            walkSigType(itf, walkSigType);
        }
        break;
    case SignatureForm::Method:
        walkParams(sig.method_sig.type_params);
        for (const SigType& param : sig.method_sig.params) {
            walkSigType(param, walkSigType);
        }
        if (!sig.method_sig.ret_void) {
            walkSigType(sig.method_sig.ret, walkSigType);
        }
        for (const SigType& thrown : sig.method_sig.throws_list) {
            walkSigType(thrown, walkSigType);
        }
        break;
    case SignatureForm::Field:
        walkSigType(sig.field_sig, walkSigType);
        break;
    }
}

Result<std::vector<SigClassNameSpan>, SigError>
classNameSpans(std::string_view signature, SignatureForm form)
{
    std::vector<SigClassNameSpan> out;
    Signature sig;
    switch (form) {
    case SignatureForm::Class: {
        auto r = parseClassSignature(signature);
        if (r.is_err()) {
            return Err(std::move(r).unwrap_err());
        }
        sig.form      = SignatureForm::Class;
        sig.class_sig = std::move(r).unwrap();
        break;
    }
    case SignatureForm::Method: {
        auto r = parseMethodSignature(signature);
        if (r.is_err()) {
            return Err(std::move(r).unwrap_err());
        }
        sig.form       = SignatureForm::Method;
        sig.method_sig = std::move(r).unwrap();
        break;
    }
    case SignatureForm::Field: {
        auto r = parseFieldSignature(signature);
        if (r.is_err()) {
            return Err(std::move(r).unwrap_err());
        }
        sig.form      = SignatureForm::Field;
        sig.field_sig = std::move(r).unwrap();
        break;
    }
    }
    collectClassNameSpans(sig, signature, out);
    return Ok(std::move(out));
}

Result<ca::str::Utf8String, SigError>
replaceClassNameSpan(std::string_view signature, const SigClassNameSpan& span,
                     std::string_view new_name)
{
    const bool valid = (span.kind == SigClassNameSpan::Kind::InternalName)
                           ? isValidSigInternalName(new_name)
                           : isValidSigSimpleName(new_name);
    if (!valid) {
        return Err(SigError{SigErrorCode::BadIdentifier, span.offset});
    }
    if (span.offset > signature.size() || span.length > signature.size() - span.offset) {
        return Err(SigError{SigErrorCode::Truncated, span.offset});
    }
    ca::str::Utf8StringBuilder b;
    b.reserve(signature.size() + new_name.size());
    b.append(signature.data(), span.offset);
    b.append(new_name.data(), new_name.size());
    b.append(signature.data() + span.offset + span.length,
             signature.size() - span.offset - span.length);
    return Ok(b.build());
}

Result<SigRenameResult, SigError>
renameClassInSignature(std::string_view signature, SignatureForm form,
                       std::string_view old_internal, std::string_view new_internal)
{
    auto old_parts_r = splitInternalName(old_internal);
    if (old_parts_r.is_err()) {
        return Err(std::move(old_parts_r).unwrap_err());
    }
    auto new_parts_r = splitInternalName(new_internal);
    if (new_parts_r.is_err()) {
        return Err(std::move(new_parts_r).unwrap_err());
    }
    const auto old_parts = std::move(old_parts_r).unwrap();
    const auto new_parts = std::move(new_parts_r).unwrap();
    if (old_parts.size() != new_parts.size()) {
        // rename 面不做结构重排（内部类嵌套在字节码层面不可变）。
        return Err(SigError{SigErrorCode::RenameShapeMismatch, 0});
    }

    Signature sig;
    switch (form) {
    case SignatureForm::Class: {
        auto r = parseClassSignature(signature);
        if (r.is_err()) {
            return Err(std::move(r).unwrap_err());
        }
        sig.form      = SignatureForm::Class;
        sig.class_sig = std::move(r).unwrap();
        break;
    }
    case SignatureForm::Method: {
        auto r = parseMethodSignature(signature);
        if (r.is_err()) {
            return Err(std::move(r).unwrap_err());
        }
        sig.form       = SignatureForm::Method;
        sig.method_sig = std::move(r).unwrap();
        break;
    }
    case SignatureForm::Field: {
        auto r = parseFieldSignature(signature);
        if (r.is_err()) {
            return Err(std::move(r).unwrap_err());
        }
        sig.form      = SignatureForm::Field;
        sig.field_sig = std::move(r).unwrap();
        break;
    }
    }

    std::vector<RenamePatch> patches;
    ca::u32 matches = 0;
    auto matchWalk = [&](const SigType& t, auto&& self) -> void {
        if (t.kind != SigType::Kind::Class) {
            return;
        }
        if (matchClassType(t.class_type, signature, old_parts, new_parts, patches)) {
            ++matches;
        }
        for (const SigClassSegment& seg : t.class_type.segments) {
            for (const SigTypeArg& arg : seg.type_args) {
                if (arg.bound != nullptr) {
                    self(*arg.bound, self);
                }
            }
        }
    };

    switch (form) {
    case SignatureForm::Class: {
        for (const SigTypeParam& tp : sig.class_sig.type_params) {
            if (tp.class_bound != nullptr) {
                matchWalk(*tp.class_bound, matchWalk);
            }
            for (const SigType& ib : tp.interface_bounds) {
                matchWalk(ib, matchWalk);
            }
        }
        matchWalk(sig.class_sig.superclass, matchWalk);
        for (const SigType& itf : sig.class_sig.super_interfaces) {
            matchWalk(itf, matchWalk);
        }
        break;
    }
    case SignatureForm::Method: {
        for (const SigTypeParam& tp : sig.method_sig.type_params) {
            if (tp.class_bound != nullptr) {
                matchWalk(*tp.class_bound, matchWalk);
            }
            for (const SigType& ib : tp.interface_bounds) {
                matchWalk(ib, matchWalk);
            }
        }
        for (const SigType& param : sig.method_sig.params) {
            matchWalk(param, matchWalk);
        }
        if (!sig.method_sig.ret_void) {
            matchWalk(sig.method_sig.ret, matchWalk);
        }
        for (const SigType& thrown : sig.method_sig.throws_list) {
            matchWalk(thrown, matchWalk);
        }
        break;
    }
    case SignatureForm::Field:
        matchWalk(sig.field_sig, matchWalk);
        break;
    }

    SigRenameResult result;
    if (patches.empty()) {
        // 未命中：原串原样返回（消费者据此跳过 CP intern）。
        ca::str::Utf8StringBuilder b;
        b.append(signature.data(), signature.size());
        result.signature = b.build();
        return Ok(std::move(result));
    }
    result.hits = matches;
    std::sort(patches.begin(), patches.end(),
              [](const RenamePatch& a, const RenamePatch& c) {
                  return a.offset < c.offset;
              });
    ca::str::Utf8StringBuilder b;
    b.reserve(signature.size());
    ca::usize copied = 0;   // 已拼装前缀的终点
    for (const RenamePatch& patch : patches) {
        b.append(signature.data() + copied, patch.offset - copied);
        b.append(patch.replacement.data(), patch.replacement.byte_length());
        copied = patch.offset + patch.length;
    }
    b.append(signature.data() + copied, signature.size() - copied);
    result.signature = b.build();
    return Ok(std::move(result));
}

ca::str::Utf8String internalNameOf(const SigClassType& ct)
{
    ca::str::Utf8StringBuilder b;
    b.reserve(ct.package_prefix.size() + 16);
    b.append(ct.package_prefix.data(), ct.package_prefix.size());
    for (ca::usize i = 0; i < ct.segments.size(); ++i) {
        if (i > 0) {
            const char dollar = '$';
            b.append(&dollar, 1);
        }
        b.append(ct.segments[i].name.data(), ct.segments[i].name.size());
    }
    return b.build();
}

bool isValidSigIdentifier(std::string_view name) noexcept
{
    if (name.empty()) {
        return false;
    }
    for (const char c : name) {
        if (isReserved(c)) {
            return false;
        }
    }
    return true;
}

bool isValidSigInternalName(std::string_view name) noexcept
{
    if (name.empty()) {
        return false;
    }
    for (const char c : name) {
        if (c != '/' && isReserved(c)) {
            return false;
        }
    }
    return true;
}

bool isValidSigSimpleName(std::string_view name) noexcept
{
    return isValidSigIdentifier(name) && name.find('/') == std::string_view::npos;
}

}   // namespace jbc::type
