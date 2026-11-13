/// @file        session_lifecycle.cpp
/// @brief       ClassSession 构造与会话入口（镜像打开 / 种子从零建 / 字节读入）。
#include "jbc/ir/units.hpp"

#include "jbc/ir/sem_index.hpp"

#include <utility>

namespace jbc::ir {

ClassSession::ClassSession(std::unique_ptr<core::ClassFile> backing)
    : backing_(std::move(backing))
{
    core::ClassFile& cf = *backing_;
    ClassUnit& u        = unit_;

    u.version = (static_cast<ca::u32>(cf.major) << 16) | cf.minor;
    u.access  = cf.access;
    // this/super/接口表是 Class 索引，成员名是 Utf8 索引，两种坐标系分开解析。
    auto internResolved = [&](std::string_view v) { return u.symbols().intern(v); };
    u.thisClass  = internResolved(cf.thisName());
    u.superClass = internResolved(cf.superName());
    u.interfaces.reserve(cf.interfaces.size());
    for (ca::u16 i : cf.interfaces) {
        u.interfaces.push_back(internResolved(cf.className(i)));
    }
    u.fields.reserve(cf.fields.size());
    for (ca::usize i = 0; i < cf.fields.size(); ++i) {
        const core::MemberInfo& f = cf.fields[i];
        FieldUnit fu;
        fu.id           = u.makeFieldId(static_cast<ca::u32>(i));
        fu.name         = u.symbols().intern(cf.nameOf(f));
        fu.desc         = u.symbols().intern(cf.descOf(f));
        fu.access       = f.access;
        fu.backingIndex = i;
        u.fields.push_back(std::move(fu));
    }
    u.methods.reserve(cf.methods.size());
    for (ca::usize i = 0; i < cf.methods.size(); ++i) {
        const core::MemberInfo& m = cf.methods[i];
        MethodUnit mu;
        mu.id      = u.makeMethodId(static_cast<ca::u32>(i));
        mu.name    = u.symbols().intern(cf.nameOf(m));
        mu.desc    = u.symbols().intern(cf.descOf(m));
        mu.access  = m.access;
        mu.backingIndex = i;
        u.methods.push_back(std::move(mu));
    }
}

Result<std::shared_ptr<ClassSession>, PromoteError>
makeSession(std::unique_ptr<core::ClassFile> backing)
{
    if (backing == nullptr) {
        return Err(PromoteError::NotModeled);
    }
    // 构造函数私有（仅本函数是友元）：shared_ptr 接管寿命，同时使能
    // shared_from_this，保活句柄（CodeUnitRef / *SemRef）依赖它。
    return Ok(std::shared_ptr<ClassSession>(new ClassSession(std::move(backing))));
}

Result<std::shared_ptr<ClassSession>, PromoteError> makeSession(const NewClassSeed& seed)
{
    // 种子预检：任何越界/缺名都在建模型之前拒绝，不产出半截会话。
    if (seed.thisName.empty() || seed.thisName.size() > 0xFFFFu
        || seed.superName.size() > 0xFFFFu || seed.access > 0xFFFFu
        || seed.interfaces.size() > 0xFFFFu) {
        return Err(PromoteError::InvalidSeed);
    }
    for (std::string_view n : seed.interfaces) {
        if (n.empty() || n.size() > 0xFFFFu) {
            return Err(PromoteError::InvalidSeed);
        }
    }

    // 空类起步：CP 从零引导（池此刻为空，无去重顾虑，直接追加）。
    // this/super/接口的 Utf8 + Class 双条目落位后，backing 从第一刻起
    // 就是自洽模型（validClassReferences 通过，无源字节可写出）。
    auto backing = std::make_unique<core::ClassFile>();
    backing->major    = static_cast<ca::u16>(seed.version >> 16);
    backing->minor    = static_cast<ca::u16>(seed.version & 0xFFFFu);
    backing->access   = static_cast<ca::u16>(seed.access);
    const ca::u32 thisNameCp = backing->cp.addUtf8(seed.thisName);
    backing->thisClass = static_cast<ca::u16>(backing->cp.addClass(thisNameCp));
    if (!seed.superName.empty()) {
        const ca::u32 superNameCp = backing->cp.addUtf8(seed.superName);
        backing->superClass = static_cast<ca::u16>(backing->cp.addClass(superNameCp));
    }
    backing->interfaces.reserve(seed.interfaces.size());
    for (std::string_view n : seed.interfaces) {
        const ca::u32 nameCp = backing->cp.addUtf8(n);
        backing->interfaces.push_back(
            static_cast<ca::u16>(backing->cp.addClass(nameCp)));
    }

    // 既有构造路径接管：unit 的 version/access/this/super/接口/成员簿记
    // 全部从 backing 镜像（成员面为空），两套状态天然一致。
    auto made = makeSession(std::move(backing));
    if (made.is_ok()) {
        // 记账：从零建出 = 整类皆新，消费者按整类重建。
        ChangeRecord created;
        created.kind = ChangeKind::ClassCreated;
        made.unwrap()->noteChange(created);
    }
    return made;
}

Result<std::shared_ptr<ClassSession>, SessionError>
makeSessionFromBytes(core::ByteView bytes, const core::ReadOptions& opt)
{
    auto r = core::readClass(bytes, opt);
    if (r.is_err()) {
        SessionError e;
        e.stage = SessionError::Stage::Read;
        e.read  = r.unwrap_err();
        return Err(e);
    }
    auto ses = makeSession(std::move(r).unwrap().file);
    if (ses.is_err()) {
        // makeSession 只在 backing 为空时失败；readClass 的成功产物非空，
        // 此分支按契约不可达，但错误通道必须保持完备（不猜、不静默）。
        SessionError e;
        e.stage   = SessionError::Stage::Session;
        e.session = ses.unwrap_err();
        return Err(e);
    }
    return Ok(std::move(ses).unwrap());
}

}   // namespace jbc::ir
