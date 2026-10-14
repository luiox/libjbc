#include "jbc/type/descriptor.hpp"

namespace jbc::type {

namespace {

/// @brief 一个类名在描述符串中的出现（扫描中间产物，offset 相对整串）。
struct RawSpan
{
    ca::u32 offset;
    ca::u32 length;
};

/// @brief 从 pos 起扫描一个类型，推进 pos 越过它；spans 非空则收集类名。
///        dims 累计前置 '['；返回 Err 时 pos 状态不承诺。
Result<TypeDesc, DescError> scanType(std::string_view s, ca::usize& pos,
                                     std::vector<RawSpan>* spans)
{
    ca::u32 dims = 0;
    while (pos < s.size() && s[pos] == '[') {
        ++dims;
        ++pos;
    }
    if (dims > 255) {
        return Err(DescError::NestingTooDeep);
    }
    if (pos >= s.size()) {
        return Err(DescError::Truncated);
    }

    TypeDesc t;
    t.array_dims = dims;
    const char c = s[pos];
    if (c == 'L') {
        const ca::usize name_begin = ++pos;
        while (pos < s.size() && s[pos] != ';') {
            ++pos;
        }
        if (pos >= s.size()) {
            return Err(DescError::Truncated);
        }
        const std::string_view name = s.substr(name_begin, pos - name_begin);
        if (!isValidInternalName(name)) {
            return Err(DescError::BadClassName);
        }
        ++pos;   // 越过 ';'
        t.kind       = TypeDesc::Kind::Class;
        t.class_name = name;
        if (spans != nullptr) {
            spans->push_back(RawSpan{static_cast<ca::u32>(name_begin),
                                     static_cast<ca::u32>(name.size())});
        }
        return Ok(std::move(t));
    }

    switch (c) {
    case 'B': t.primitive = BaseType::Byte; break;
    case 'C': t.primitive = BaseType::Char; break;
    case 'D': t.primitive = BaseType::Double; break;
    case 'F': t.primitive = BaseType::Float; break;
    case 'I': t.primitive = BaseType::Int; break;
    case 'J': t.primitive = BaseType::Long; break;
    case 'S': t.primitive = BaseType::Short; break;
    case 'Z': t.primitive = BaseType::Boolean; break;
    default:
        // V 与一切未知名在这里都非法；V 的合法位置只有方法返回位，由
        // parseMethodDescriptor 单独处理。
        return Err(DescError::BadBaseType);
    }
    ++pos;
    t.kind = TypeDesc::Kind::Primitive;
    return Ok(std::move(t));
}

/// @brief 解析方法体：从 pos 起扫描到 ')' 为止的全部参数类型。
Result<std::vector<TypeDesc>, DescError>
scanParams(std::string_view s, ca::usize& pos, std::vector<RawSpan>* spans)
{
    std::vector<TypeDesc> params;
    while (pos < s.size() && s[pos] != ')') {
        auto r = scanType(s, pos, spans);
        if (r.is_err()) {
            return Err(std::move(r).unwrap_err());
        }
        params.push_back(std::move(r).unwrap());
    }
    if (pos >= s.size()) {
        return Err(DescError::Truncated);
    }
    ++pos;   // 越过 ')'
    return Ok(std::move(params));
}

/// @brief 对整个描述符做完整语法扫描（方法/字段自动分派），顺路收类名。
Result<std::vector<RawSpan>, DescError> scanDescriptor(std::string_view s)
{
    std::vector<RawSpan> spans;
    if (s.empty()) {
        return Err(DescError::Empty);
    }
    ca::usize pos = 0;
    if (s[0] == '(') {
        ++pos;
        auto params = scanParams(s, pos, &spans);
        if (params.is_err()) {
            return Err(std::move(params).unwrap_err());
        }
        if (pos < s.size() && s[pos] == 'V') {
            ++pos;
        }
        else {
            auto ret = scanType(s, pos, &spans);
            if (ret.is_err()) {
                return Err(std::move(ret).unwrap_err());
            }
        }
    }
    else {
        auto only = scanType(s, pos, &spans);
        if (only.is_err()) {
            return Err(std::move(only).unwrap_err());
        }
    }
    if (pos != s.size()) {
        return Err(DescError::TrailingChars);
    }
    return Ok(std::move(spans));
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

Result<ca::str::Utf8String, DescError> replaceAt(std::string_view s, ca::u32 offset,
                                                 ca::u32 length, std::string_view repl)
{
    if (offset > s.size() || length > s.size() - offset) {
        return Err(DescError::Truncated);
    }
    ca::str::Utf8StringBuilder b;
    b.reserve(s.size() + repl.size());
    b.append(s.data(), offset);
    b.append(repl.data(), repl.size());
    b.append(s.data() + offset + length, s.size() - offset - length);
    return Ok(b.build());
}

}   // namespace

const char* to_string(DescError e) noexcept
{
    switch (e) {
    case DescError::Ok: return "ok";
    case DescError::Empty: return "empty input";
    case DescError::Truncated: return "truncated descriptor";
    case DescError::TrailingChars: return "trailing characters after descriptor";
    case DescError::BadBaseType: return "bad base type char";
    case DescError::BadClassName: return "bad class internal name";
    case DescError::BadMethodShape: return "method descriptor must start with '('";
    case DescError::NestingTooDeep: return "array nesting over 255";
    }
    return "unknown";
}

Result<TypeDesc, DescError> parseFieldType(std::string_view s) noexcept
{
    if (s.empty()) {
        return Err(DescError::Empty);
    }
    ca::usize pos = 0;
    auto r = scanType(s, pos, nullptr);
    if (r.is_err()) {
        return Err(std::move(r).unwrap_err());
    }
    if (pos != s.size()) {
        return Err(DescError::TrailingChars);
    }
    return r;
}

Result<MethodDesc, DescError> parseMethodDescriptor(std::string_view s)
{
    if (s.empty()) {
        return Err(DescError::Empty);
    }
    if (s[0] != '(') {
        return Err(DescError::BadMethodShape);
    }
    ca::usize pos = 1;   // 已消费 '('
    MethodDesc m;
    auto params = scanParams(s, pos, nullptr);
    if (params.is_err()) {
        return Err(std::move(params).unwrap_err());
    }
    m.params = std::move(params).unwrap();
    if (pos < s.size() && s[pos] == 'V') {
        m.ret_void = true;
        ++pos;
    }
    else {
        auto ret = scanType(s, pos, nullptr);
        if (ret.is_err()) {
            return Err(std::move(ret).unwrap_err());
        }
        m.ret = std::move(ret).unwrap();
    }
    if (pos != s.size()) {
        return Err(DescError::TrailingChars);
    }
    return Ok(std::move(m));
}

Result<std::vector<ClassNameSpan>, DescError>
classNameSpans(std::string_view descriptor)
{
    auto raw = scanDescriptor(descriptor);
    if (raw.is_err()) {
        return Err(std::move(raw).unwrap_err());
    }
    std::vector<ClassNameSpan> out;
    for (const RawSpan& r : std::move(raw).unwrap()) {
        out.push_back(ClassNameSpan{r.offset, r.length});
    }
    return Ok(std::move(out));
}

Result<ca::str::Utf8String, DescError>
replaceClassName(std::string_view descriptor, const ClassNameSpan& span,
                 std::string_view new_name)
{
    if (!isValidInternalName(new_name)) {
        return Err(DescError::BadClassName);
    }
    return replaceAt(descriptor, span.offset, span.length, new_name);
}

ca::u32 slotWidth(const TypeDesc& t) noexcept
{
    if (t.array_dims > 0) {
        return 1;   // 数组是引用，一槽
    }
    if (t.kind == TypeDesc::Kind::Primitive
        && (t.primitive == BaseType::Long || t.primitive == BaseType::Double)) {
        return 2;
    }
    return 1;
}

ca::u32 paramSlotCount(const MethodDesc& m) noexcept
{
    ca::u32 total = 0;
    for (const TypeDesc& p : m.params) {
        total += slotWidth(p);
    }
    return total;
}

bool isValidInternalName(std::string_view name) noexcept
{
    if (name.empty()) {
        return false;
    }
    for (const char c : name) {
        if (c == '.' || c == '[' || c == ';') {
            return false;
        }
    }
    return true;
}

ca::str::Utf8String internalToBinary(std::string_view internal_name)
{
    ca::str::Utf8StringBuilder b;
    b.reserve(internal_name.size());
    for (const char c : internal_name) {
        const char mapped = (c == '/') ? '.' : c;
        b.append(&mapped, 1);
    }
    return b.build();
}

ca::str::Utf8String binaryToInternal(std::string_view binary_name)
{
    ca::str::Utf8StringBuilder b;
    b.reserve(binary_name.size());
    for (const char c : binary_name) {
        const char mapped = (c == '.') ? '/' : c;
        b.append(&mapped, 1);
    }
    return b.build();
}

}   // namespace jbc::type
