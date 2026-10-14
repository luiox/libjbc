/// @file        class_file.cpp
/// @brief       单类 DOM 的查询与 lazy 解码入口。
///
/// 本文件只做模型侧的事（名字解析、属性查找、解码分派）。「怎么从字节解出
/// 一个属性」属于 codec，实现在 attr_codec.cpp。
#include "jbc/core/class_file.hpp"

#include "jbc/core/attr_codec.hpp"

namespace jbc::core {

std::string_view ClassFile::className(ca::u32 classCp) const noexcept
{
    if (classCp == 0 || !cp.valid(classCp) || cp.tagOf(classCp) != CpTag::Class) {
        return std::string_view{};
    }
    return cp.utf8Of(cp.at(classCp).a);
}

std::string_view ClassFile::thisName() const noexcept
{
    return className(thisClass);
}

std::string_view ClassFile::superName() const noexcept
{
    return className(superClass);
}

AttrSlot* ClassFile::findAttrByCp(std::vector<AttrSlot>& v, ca::u32 nameCp) noexcept
{
    for (AttrSlot& s : v) {
        if (s.nameCp == nameCp) {
            return &s;
        }
    }
    return nullptr;
}

AttrSlot* ClassFile::findClassAttr(std::string_view name) noexcept
{
    for (AttrSlot& s : attrs) {
        if (cp.utf8Equals(s.nameCp, name)) {
            return &s;
        }
    }
    return nullptr;
}

Result<AttrBody*, ReadError> ClassFile::decodeAttr(AttrSlot& slot)
{
    if (slot.body != nullptr) {
        return Ok(slot.body.get());
    }
    if (!hasSource() && slot.data.length != 0) {
        return Err(ReadError::Truncated);
    }
    return decodeAttrBody(*this, slot);
}

Result<CodeAttr*, ReadError> ClassFile::decodeCode(MemberInfo& m)
{
    for (AttrSlot& s : m.attrs) {
        if (!cp.utf8Equals(s.nameCp, "Code")) {
            continue;
        }
        const Result<AttrBody*, ReadError> r = decodeAttr(s);
        if (r.is_err()) {
            return Err(r.unwrap_err());
        }
        AttrBody* body = r.unwrap();
        if (body == nullptr || body->getType() != static_cast<int>(AttrKind::Code)) {
            return Ok(static_cast<CodeAttr*>(nullptr));
        }
        return Ok(static_cast<CodeAttr*>(body));
    }
    return Ok(static_cast<CodeAttr*>(nullptr));
}

}   // namespace jbc::core
