/// @file        error.cpp
/// @brief       错误码与解析状态的文本映射。
#include "jbc/core/error.hpp"

namespace jbc::core {

const char* to_string(ReadError e) noexcept
{
    switch (e) {
        case ReadError::Ok: return "ok";
        case ReadError::Truncated: return "truncated";
        case ReadError::BadMagic: return "bad_magic";
        case ReadError::UnsupportedVersion: return "unsupported_version";
        case ReadError::BadConstantPoolCount: return "bad_constant_pool_count";
        case ReadError::BadConstantPoolTag: return "bad_constant_pool_tag";
        case ReadError::BadConstantPoolRef: return "bad_constant_pool_ref";
        case ReadError::BadUtf8: return "bad_utf8";
        case ReadError::BadAttributeLength: return "bad_attribute_length";
        case ReadError::BadMemberCount: return "bad_member_count";
        case ReadError::BadCodeLength: return "bad_code_length";
        case ReadError::UnsupportedFeature: return "unsupported_feature";
    }
    return "unknown";
}

const char* to_string(WriteError e) noexcept
{
    switch (e) {
        case WriteError::Ok: return "ok";
        case WriteError::MissingSource: return "missing_source";
        case WriteError::BadSpan: return "bad_span";
        case WriteError::Unencodable: return "unencodable";
        case WriteError::NotModeled: return "not_modeled";
    }
    return "unknown";
}

const char* to_string(ParseState s) noexcept
{
    switch (s) {
        case ParseState::Validated: return "validated";
        case ParseState::Recovered: return "recovered";
    }
    return "unknown";
}

}   // namespace jbc::core
