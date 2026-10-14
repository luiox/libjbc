/// @file        test_class_bytes.hpp
/// @brief       测试用具：手工拼装 classfile 字节。
///
/// 刻意不依赖任何真实 .class 样本：测试要能精确控制每一个字节（包括畸形
/// 输入），样本文件做不到这一点。
#pragma once

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "libca/core/datatype.hpp"
#include "jbc/jbc.hpp"

namespace jbc::test {

using Bytes = std::vector<ca::u8>;

/// @brief 读取成功则返回模型，失败则记一条 gtest 失败并返回空。
inline std::unique_ptr<jbc::core::ClassFile> readOk(const Bytes& bytes,
                                                    const jbc::core::ReadOptions& opt = {})
{
    using jbc::core::ByteView;
    using jbc::core::readClass;
    using jbc::core::to_string;

    auto r = readClass(ByteView{bytes}, opt);
    if (r.is_err()) {
        ADD_FAILURE() << "readClass failed: " << to_string(r.unwrap_err());
        return nullptr;
    }
    return std::move(r).unwrap().file;
}

/// @brief 写回成功则返回字节，失败则记一条 gtest 失败并返回空。
inline Bytes writeOk(const jbc::core::ClassFile& cf)
{
    using jbc::core::to_string;
    using jbc::core::writeClass;

    auto r = writeClass(cf);
    if (r.is_err()) {
        ADD_FAILURE() << "writeClass failed: " << to_string(r.unwrap_err());
        return {};
    }
    return std::move(r).unwrap();
}

/// @brief 属性规格：名字索引 + 内容字节。
struct AttrSpec
{
    ca::u16 nameIdx = 0;
    Bytes content;
};

/// @brief Code 属性的异常表条目。
struct ExcSpec
{
    ca::u16 startPc = 0;
    ca::u16 endPc = 0;
    ca::u16 handlerPc = 0;
    ca::u16 catchType = 0;
};

namespace detail {

inline void putU8(Bytes& b, ca::u8 v) { b.push_back(v); }

inline void putU16(Bytes& b, ca::u16 v)
{
    b.push_back(static_cast<ca::u8>(v >> 8));
    b.push_back(static_cast<ca::u8>(v));
}

inline void putU32(Bytes& b, ca::u32 v)
{
    b.push_back(static_cast<ca::u8>(v >> 24));
    b.push_back(static_cast<ca::u8>(v >> 16));
    b.push_back(static_cast<ca::u8>(v >> 8));
    b.push_back(static_cast<ca::u8>(v));
}

inline void putU64(Bytes& b, ca::u64 v)
{
    putU32(b, static_cast<ca::u32>(v >> 32));
    putU32(b, static_cast<ca::u32>(v & 0xFFFFFFFFu));
}

/// @brief 属性列表序列化（attributes_count + 各 attribute_info）。
inline void putAttrs(Bytes& b, const std::vector<AttrSpec>& attrs)
{
    putU16(b, static_cast<ca::u16>(attrs.size()));
    for (const AttrSpec& a : attrs) {
        putU16(b, a.nameIdx);
        putU32(b, static_cast<ca::u32>(a.content.size()));
        b.insert(b.end(), a.content.begin(), a.content.end());
    }
}

}   // namespace detail

/// @brief classfile 字节构造器：CP 追加式分配，其余部分自由组装。
class ClassBuilder
{
public:
    // ---- 常量池（索引一经分配不再改变）----

    ca::u16 utf8(const std::string& s)
    {
        const ca::u16 i = next_;
        next_ += 1;
        detail::putU8(cp_, 1);
        detail::putU16(cp_, static_cast<ca::u16>(s.size()));
        for (const char c : s) {
            cp_.push_back(static_cast<ca::u8>(c));
        }
        return i;
    }

    ca::u16 classRef(ca::u16 nameIdx)
    {
        const ca::u16 i = next_;
        next_ += 1;
        detail::putU8(cp_, 7);
        detail::putU16(cp_, nameIdx);
        return i;
    }

    ca::u16 nameAndType(ca::u16 nameIdx, ca::u16 descIdx)
    {
        const ca::u16 i = next_;
        next_ += 1;
        detail::putU8(cp_, 12);
        detail::putU16(cp_, nameIdx);
        detail::putU16(cp_, descIdx);
        return i;
    }

    ca::u16 integer(ca::i32 v)
    {
        const ca::u16 i = next_;
        next_ += 1;
        detail::putU8(cp_, 3);
        detail::putU32(cp_, static_cast<ca::u32>(v));
        return i;
    }

    /// @brief Long 常量：占两个 CP 槽位（JVMS 4.4.5）。
    ca::u16 longConst(ca::i64 v)
    {
        const ca::u16 i = next_;
        next_ += 2;
        detail::putU8(cp_, 5);
        detail::putU64(cp_, static_cast<ca::u64>(v));
        return i;
    }

    /// @brief Double 常量：占两个 CP 槽位。
    ca::u16 doubleConst(ca::u64 bits)
    {
        const ca::u16 i = next_;
        next_ += 2;
        detail::putU8(cp_, 6);
        detail::putU64(cp_, bits);
        return i;
    }

    ca::u16 fieldref(ca::u16 cls, ca::u16 nt)
    {
        const ca::u16 i = next_;
        next_ += 1;
        detail::putU8(cp_, 9);
        detail::putU16(cp_, cls);
        detail::putU16(cp_, nt);
        return i;
    }

    ca::u16 stringConst(ca::u16 utf8Idx)
    {
        const ca::u16 i = next_;
        next_ += 1;
        detail::putU8(cp_, 8);
        detail::putU16(cp_, utf8Idx);
        return i;
    }

    ca::u16 methodref(ca::u16 cls, ca::u16 nt)
    {
        const ca::u16 i = next_;
        next_ += 1;
        detail::putU8(cp_, 10);
        detail::putU16(cp_, cls);
        detail::putU16(cp_, nt);
        return i;
    }

    /// @brief InterfaceMethodref（JVMS 4.4.2，tag 11）：invokeinterface 指向的形态。
    ca::u16 interfaceMethodref(ca::u16 cls, ca::u16 nt)
    {
        const ca::u16 i = next_;
        next_ += 1;
        detail::putU8(cp_, 11);
        detail::putU16(cp_, cls);
        detail::putU16(cp_, nt);
        return i;
    }

    /// @brief MethodHandle（JVMS 4.4.8，tag 15）：BootstrapMethods 条目引用形态。
    /// @param kind reference_kind（1..9；6 = REF_invokeStatic）
    ca::u16 methodHandle(ca::u8 kind, ca::u16 refIdx)
    {
        const ca::u16 i = next_;
        next_ += 1;
        detail::putU8(cp_, 15);
        detail::putU8(cp_, kind);
        detail::putU16(cp_, refIdx);
        return i;
    }

    /// @brief InvokeDynamic（JVMS 4.4.10，tag 18）：invokedynamic 指令的 CP 形态。
    /// @param bsmIndex BootstrapMethods 属性的条目下标（0 起）
    ca::u16 invokeDynamic(ca::u16 bsmIndex, ca::u16 natIdx)
    {
        const ca::u16 i = next_;
        next_ += 1;
        detail::putU8(cp_, 18);
        detail::putU16(cp_, bsmIndex);
        detail::putU16(cp_, natIdx);
        return i;
    }

    /// @brief 当前 CP 的 constant_pool_count（= 下一待分配索引）。
    ca::u16 cpCount() const { return next_; }

    // ---- 类结构 ----

    ClassBuilder& setVersion(ca::u16 major)
    {
        major_ = major;
        return *this;
    }
    ClassBuilder& setAccess(ca::u16 access)
    {
        access_ = access;
        return *this;
    }
    ClassBuilder& setThis(ca::u16 c)
    {
        thisClass_ = c;
        return *this;
    }
    ClassBuilder& setSuper(ca::u16 c)
    {
        superClass_ = c;
        return *this;
    }
    ClassBuilder& addInterface(ca::u16 c)
    {
        interfaces_.push_back(c);
        return *this;
    }

    ClassBuilder& addField(ca::u16 access,
                           ca::u16 nameIdx,
                           ca::u16 descIdx,
                           std::vector<AttrSpec> attrs = {})
    {
        fields_.push_back(Member{access, nameIdx, descIdx, std::move(attrs)});
        return *this;
    }

    ClassBuilder& addMethod(ca::u16 access,
                            ca::u16 nameIdx,
                            ca::u16 descIdx,
                            std::vector<AttrSpec> attrs = {})
    {
        methods_.push_back(Member{access, nameIdx, descIdx, std::move(attrs)});
        return *this;
    }

    ClassBuilder& addClassAttr(ca::u16 nameIdx, Bytes content)
    {
        classAttrs_.push_back(AttrSpec{nameIdx, std::move(content)});
        return *this;
    }

    /// @brief 组装完整 classfile 字节。
    Bytes build() const
    {
        Bytes b;
        detail::putU32(b, 0xCAFEBABEu);
        detail::putU16(b, 0);          // minor
        detail::putU16(b, major_);
        detail::putU16(b, next_);      // constant_pool_count
        b.insert(b.end(), cp_.begin(), cp_.end());
        detail::putU16(b, access_);
        detail::putU16(b, thisClass_);
        detail::putU16(b, superClass_);
        detail::putU16(b, static_cast<ca::u16>(interfaces_.size()));
        for (const ca::u16 i : interfaces_) {
            detail::putU16(b, i);
        }
        putMembers(b, fields_);
        putMembers(b, methods_);
        detail::putAttrs(b, classAttrs_);
        return b;
    }

private:
    struct Member
    {
        ca::u16 access;
        ca::u16 nameIdx;
        ca::u16 descIdx;
        std::vector<AttrSpec> attrs;
    };

    static void putMembers(Bytes& b, const std::vector<Member>& ms)
    {
        detail::putU16(b, static_cast<ca::u16>(ms.size()));
        for (const Member& m : ms) {
            detail::putU16(b, m.access);
            detail::putU16(b, m.nameIdx);
            detail::putU16(b, m.descIdx);
            detail::putAttrs(b, m.attrs);
        }
    }

    Bytes cp_;
    ca::u16 next_ = 1;
    ca::u16 major_ = 52;
    ca::u16 access_ = 0x0021;  // ACC_PUBLIC | ACC_SUPER
    ca::u16 thisClass_ = 0;
    ca::u16 superClass_ = 0;
    std::vector<ca::u16> interfaces_;
    std::vector<Member> fields_;
    std::vector<Member> methods_;
    std::vector<AttrSpec> classAttrs_;
};

/// @brief 组装 Code 属性内容（不含 name_index 与 length）。
inline Bytes codeContent(ca::u16 maxStack,
                         ca::u16 maxLocals,
                         const Bytes& code,
                         const std::vector<ExcSpec>& exc = {},
                         const std::vector<AttrSpec>& nested = {})
{
    Bytes b;
    detail::putU16(b, maxStack);
    detail::putU16(b, maxLocals);
    detail::putU32(b, static_cast<ca::u32>(code.size()));
    b.insert(b.end(), code.begin(), code.end());
    detail::putU16(b, static_cast<ca::u16>(exc.size()));
    for (const ExcSpec& e : exc) {
        detail::putU16(b, e.startPc);
        detail::putU16(b, e.endPc);
        detail::putU16(b, e.handlerPc);
        detail::putU16(b, e.catchType);
    }
    detail::putAttrs(b, nested);
    return b;
}

/// @brief 一个有字段、方法、Code 属性（含异常表与嵌套未知属性）和类级未知属性的类。
///
/// 刻意包含一个 libjbc 不认识的 `WeirdCustomAttr`——它正是保真写回的主要照顾对象。
inline Bytes makeRichClass()
{
    ClassBuilder cb;
    const ca::u16 thisIdx  = cb.utf8("p/T");
    const ca::u16 superIdx = cb.utf8("java/lang/Object");
    const ca::u16 codeIdx  = cb.utf8("Code");
    const ca::u16 weirdIdx = cb.utf8("WeirdCustomAttr");
    const ca::u16 initIdx  = cb.utf8("<init>");
    const ca::u16 voidIdx  = cb.utf8("()V");
    const ca::u16 valueIdx = cb.utf8("value");
    const ca::u16 intIdx   = cb.utf8("I");

    cb.setThis(cb.classRef(thisIdx));
    cb.setSuper(cb.classRef(superIdx));
    cb.setAccess(0x0021);
    cb.setVersion(52);

    cb.addField(0x0002, valueIdx, intIdx);

    const Bytes code = {0x2a, 0xb7, 0x00, 0x01, 0xb1};  // aload_0/invokespecial/return
    std::vector<ExcSpec> exc;
    exc.push_back(ExcSpec{0, 5, 6, 0});
    std::vector<AttrSpec> nested;
    nested.push_back(AttrSpec{weirdIdx, Bytes{0xDE, 0xAD, 0xBE, 0xEF}});

    std::vector<AttrSpec> methodAttrs;
    methodAttrs.push_back(AttrSpec{codeIdx, codeContent(2, 1, code, exc, nested)});
    methodAttrs.push_back(AttrSpec{weirdIdx, Bytes{0x01, 0x02, 0x03}});
    cb.addMethod(0x0001, initIdx, voidIdx, std::move(methodAttrs));

    cb.addClassAttr(weirdIdx, Bytes{0xAA, 0xBB});
    return cb.build();
}

}   // namespace jbc::test
