#include "jbc/consumer/summary.hpp"

#include "jbc/core/reader.hpp"

namespace jbc::consumer {

namespace {

using jbc::core::ByteView;
using jbc::core::ClassFile;
using jbc::core::MemberInfo;
using jbc::core::ReadOptions;

/// @brief 成员表骨架行扫描（hasCode 按属性名命中判定，不解体）。
SummaryMethod scanMethod(const ClassFile& cf, const MemberInfo& m)
{
    SummaryMethod out;
    out.name   = std::string(cf.nameOf(m));
    out.desc   = std::string(cf.descOf(m));
    out.access = m.access;
    for (const jbc::core::AttrSlot& slot : m.attrs) {
        if (cf.cp.utf8Equals(slot.nameCp, "Code")) {
            out.hasCode = true;
            break;   // 单成员至多一个 Code 属性；命中即停（不解体）
        }
    }
    return out;
}

}   // namespace

ConsumerResult<ClassSummary> SummaryScanner::scan(const ByteView bytes) const
{
    // 摘要态不承担 copy-through，源字节关到最小（§21 for 改变摆账）。
    ReadOptions opt;
    opt.preserveSourceBytes = false;
    auto r = jbc::core::readClass(bytes, opt);
    if (r.is_err()) {
        return ca::core::Err(ConsumerError::BadClassFile);
    }
    auto rr = std::move(r).unwrap();   // ReadResult 具名持活（file 归它所有）
    const ClassFile& cf = *rr.file;

    ClassSummary out;
    out.internalName = std::string(cf.thisName());
    out.superClass   = std::string(cf.superName());
    out.version      = (static_cast<ca::u32>(cf.major) << 16) | static_cast<ca::u32>(cf.minor);
    out.access       = cf.access;
    for (const ca::u16 iface : cf.interfaces) {
        out.interfaces.push_back(std::string(cf.className(iface)));
    }
    out.fields.reserve(cf.fields.size());
    for (const MemberInfo& f : cf.fields) {
        SummaryField row;
        row.name   = std::string(cf.nameOf(f));
        row.desc   = std::string(cf.descOf(f));
        row.access = f.access;
        out.fields.push_back(std::move(row));
    }
    out.methods.reserve(cf.methods.size());
    for (const MemberInfo& m : cf.methods) {
        out.methods.push_back(scanMethod(cf, m));
    }
    return ca::core::Ok(std::move(out));
}

ConsumerResult<std::vector<ClassSummary>> SummaryScanner::scanSource(const IClassSource& source) const
{
    std::vector<ClassSummary> out;
    std::vector<std::string_view> names;
    source.listClasses(names);
    out.reserve(names.size());
    for (const std::string_view name : names) {
        const ConsumerResult<ByteView> got = source.bytes(name);
        if (got.is_err()) {
            return ca::core::Err(got.unwrap_err());  // fail-closed 首错
        }
        const ConsumerResult<ClassSummary> one = scan(got.unwrap());
        if (one.is_err()) {
            return ca::core::Err(one.unwrap_err());
        }
        out.push_back(std::move(one).unwrap());
    }
    return ca::core::Ok(std::move(out));
}

}   // namespace jbc::consumer
