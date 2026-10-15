/// @file        attr_codec.cpp
/// @brief       属性目录解析、Code 属性解码与属性写出。
#include "jbc/core/attr_codec.hpp"

#include <cstring>
#include <limits>

namespace jbc::core {

namespace {

/// @brief 「Code」属性名（modified UTF-8 下与 ASCII 一致，可直接 memcmp）。
constexpr std::string_view kCodeAttrName = "Code";
constexpr std::string_view kBootstrapMethodsName = "BootstrapMethods";
constexpr std::string_view kLineNumberTableName  = "LineNumberTable";
constexpr std::string_view kStackMapTableName    = "StackMapTable";
constexpr std::string_view kLocalVariableTableName     = "LocalVariableTable";
constexpr std::string_view kLocalVariableTypeTableName = "LocalVariableTypeTable";
constexpr std::string_view kRvaName = "RuntimeVisibleAnnotations";
constexpr std::string_view kRiaName = "RuntimeInvisibleAnnotations";
constexpr std::string_view kRvtaName = "RuntimeVisibleTypeAnnotations";
constexpr std::string_view kRitaName = "RuntimeInvisibleTypeAnnotations";
constexpr std::string_view kSignatureName = "Signature";
constexpr std::string_view kCharacterRangeTableName = "CharacterRangeTable";
constexpr std::string_view kSourceFileName = "SourceFile";
constexpr std::string_view kInnerClassesName = "InnerClasses";
constexpr std::string_view kEnclosingMethodName = "EnclosingMethod";
constexpr std::string_view kExceptionsName = "Exceptions";

/// @brief u16 域宽检查：编码闸门的基础谓词（design.md §27.2——超界显式
///        Err，禁止 static_cast 静默截断）。
bool fitsU16(ca::u32 v) noexcept
{
    return v <= std::numeric_limits<ca::u16>::max();
}

bool attrNeedsReencode(const AttrSlot& slot)
{
    if (slot.needsReencode()) {
        return true;
    }
    if (slot.body == nullptr || slot.body->getType() != static_cast<int>(AttrKind::Code)) {
        return false;
    }
    const auto& code = static_cast<const CodeAttr&>(*slot.body);
    for (const AttrSlot& nested : code.attrs) {
        if (attrNeedsReencode(nested)) {
            return true;
        }
    }
    return false;
}

Result<std::unique_ptr<AttrBody>, ReadError> decodeCodeAttr(ClassFile& cf, const AttrSlot& slot)
{
    const ByteView data = cf.source().spanOrEmpty(slot.data);
    if (data.size() != slot.data.length) {
        return Err(ReadError::Truncated);
    }

    Cursor cur{data};
    auto c = std::make_unique<CodeAttr>();

    const Result<ca::u16, ReadError> rs = cur.u16();
    if (rs.is_err()) {
        return Err(rs.unwrap_err());
    }
    c->maxStack = rs.unwrap();

    const Result<ca::u16, ReadError> rl = cur.u16();
    if (rl.is_err()) {
        return Err(rl.unwrap_err());
    }
    c->maxLocals = rl.unwrap();

    const Result<ca::u32, ReadError> rc = cur.u32();
    if (rc.is_err()) {
        return Err(rc.unwrap_err());
    }
    const ca::u32 codeLen = rc.unwrap();
    // JVMS 4.7.3：code_length 必须 > 0 且 ≤ 65535。
    if (codeLen == 0 || codeLen > 0xFFFFu) {
        return Err(ReadError::BadCodeLength);
    }

    const Result<ByteView, ReadError> rb = cur.take(codeLen);
    if (rb.is_err()) {
        return Err(rb.unwrap_err());
    }
    const ByteView code = rb.unwrap();
    c->code.assign(code.data(), code.data() + code.size());

    const Result<ca::u16, ReadError> re = cur.u16();
    if (re.is_err()) {
        return Err(re.unwrap_err());
    }
    const ca::u16 excCount = re.unwrap();
    c->excTable.reserve(excCount);
    for (ca::u16 i = 0; i < excCount; ++i) {
        CodeAttr::ExcEntry e;
        const Result<ca::u16, ReadError> r0 = cur.u16();
        if (r0.is_err()) {
            return Err(r0.unwrap_err());
        }
        e.startPc = r0.unwrap();
        const Result<ca::u16, ReadError> r1 = cur.u16();
        if (r1.is_err()) {
            return Err(r1.unwrap_err());
        }
        e.endPc = r1.unwrap();
        const Result<ca::u16, ReadError> r2 = cur.u16();
        if (r2.is_err()) {
            return Err(r2.unwrap_err());
        }
        e.handlerPc = r2.unwrap();
        const Result<ca::u16, ReadError> r3 = cur.u16();
        if (r3.is_err()) {
            return Err(r3.unwrap_err());
        }
        e.catchType = r3.unwrap();
        c->excTable.push_back(e);
    }

    // Code 内嵌套属性（StackMapTable / LineNumberTable / ...）：同样只读目录。
    // 游标基于 Code 内容子视图，须把 Code 内容在源流中的偏移加进去，span 才是绝对的。
    const Result<void, ReadError> ra =
        readAttrSlots(cur, c->attrs, cf.diagnostics(), slot.data.offset);
    if (ra.is_err()) {
        return Err(ra.unwrap_err());
    }

    // 合法 Code 属性在嵌套属性表之后必须正好结束。畸形输入仍然可以被低层
    // DOM 表达：尾部作为 opaque bytes 保留，避免后续修改 Code 时静默丢数据。
    if (cur.remaining() != 0) {
        const Result<ByteView, ReadError> rt = cur.take(cur.remaining());
        if (rt.is_err()) {
            return Err(rt.unwrap_err());
        }
        const ByteView tail = rt.unwrap();
        c->trailing.assign(tail.data(), tail.data() + tail.size());
        cf.diagnostics().warn(ReadError::UnsupportedFeature, slot.data.offset);
    }

    return Ok(std::unique_ptr<AttrBody>(std::move(c)));
}

/// @brief 编码 Code 属性体（不含 name_index 与 length）。
Result<void, WriteError> encodeCodeBody(const ClassFile& cf, const CodeAttr& c, ByteSink& out, ByteView source)
{
    if (c.code.empty() || c.code.size() > std::numeric_limits<ca::u16>::max() ||
        c.excTable.size() > std::numeric_limits<ca::u16>::max() ||
        c.attrs.size() > std::numeric_limits<ca::u16>::max()) {
        return Err(WriteError::Unencodable);
    }
    out.u16(c.maxStack);
    out.u16(c.maxLocals);
    out.u32(static_cast<ca::u32>(c.code.size()));
    if (!c.code.empty()) {
        out.raw(ByteView{c.code.data(), c.code.size()});
    }
    out.u16(static_cast<ca::u16>(c.excTable.size()));
    for (const CodeAttr::ExcEntry& e : c.excTable) {
        if (e.startPc > 0xFFFFu || e.endPc > 0xFFFFu || e.handlerPc > 0xFFFFu ||
            e.catchType > 0xFFFFu) {
            return Err(WriteError::Unencodable);
        }
        out.u16(static_cast<ca::u16>(e.startPc));
        out.u16(static_cast<ca::u16>(e.endPc));
        out.u16(static_cast<ca::u16>(e.handlerPc));
        out.u16(static_cast<ca::u16>(e.catchType));
    }
    out.u16(static_cast<ca::u16>(c.attrs.size()));
    for (const AttrSlot& s : c.attrs) {
        // 嵌套属性编码失败会破坏 Code 的长度前缀，必须向上传播。
        const Result<void, WriteError> r = encodeAttr(cf, s, out, source);
        if (r.is_err()) {
            return Err(r.unwrap_err());
        }
    }
    if (!c.trailing.empty()) {
        out.raw(ByteView{c.trailing});
    }
    return Ok();
}

}   // namespace

Result<void, ReadError> readAttrSlots(Cursor& cur,
                                      std::vector<AttrSlot>& out,
                                      Diagnostics& diag,
                                      ca::u32 baseOffset)
{
    const Result<ca::u16, ReadError> rc = cur.u16();
    if (rc.is_err()) {
        return Err(rc.unwrap_err());
    }
    const ca::u16 count = rc.unwrap();
    out.reserve(out.size() + count);

    for (ca::u16 i = 0; i < count; ++i) {
        const Result<ca::u16, ReadError> rn = cur.u16();
        if (rn.is_err()) {
            return Err(rn.unwrap_err());
        }
        const ca::u32 nameCp = rn.unwrap();

        const Result<ca::u32, ReadError> rl = cur.u32();
        if (rl.is_err()) {
            return Err(rl.unwrap_err());
        }
        const ca::u32 length = rl.unwrap();

        // span 记录的是「属性内容」的范围（不含 name_index 与 length 本身），
        // 且一律为源字节流中的绝对偏移——写回时按它直接取字节。
        const ca::usize contentStart = cur.pos();
        if (cur.remaining() < length) {
            diag.error(ReadError::BadAttributeLength, static_cast<ca::u32>(contentStart));
            return Err(ReadError::BadAttributeLength);
        }

        if (contentStart > std::numeric_limits<ca::u32>::max() ||
            baseOffset > std::numeric_limits<ca::u32>::max() -
                             static_cast<ca::u32>(contentStart)) {
            return Err(ReadError::BadAttributeLength);
        }
        AttrSlot s;
        s.nameCp = nameCp;
        s.data   = ByteSpan{baseOffset + static_cast<ca::u32>(contentStart), length};
        s.state  = AttrSlot::State::Untouched;

        // lazy 的全部实现：跳过内容，不解析、不分配。
        const Result<void, ReadError> rs = cur.skip(length);
        if (rs.is_err()) {
            return Err(rs.unwrap_err());
        }
        out.push_back(std::move(s));
    }
    return Ok();
}


/// @brief 内部紧凑读取器：任一读失败置 ok=false，后续读返回 0。
///
/// 长表驱动的属性解码（StackMapTable/注解）每字段三行 Result 样板会淹没
/// 结构本身；统一在解码函数末尾检查一次。不用异常——畸形输入是预期输入。
class Ard
{
public:
    explicit Ard(ByteView v) : cur_{v} {}

    bool ok() const noexcept { return ok_; }
    ReadError err() const noexcept { return err_; }

    ca::u32 u1()
    {
        if (!ensure(1)) return 0;
        const Result<ca::u8, ReadError> r = cur_.u8();
        return take(r);
    }
    ca::u32 u2()
    {
        if (!ensure(2)) return 0;
        const Result<ca::u16, ReadError> r = cur_.u16();
        return take(r);
    }
    ca::u32 u4()
    {
        if (!ensure(4)) return 0;
        const Result<ca::u32, ReadError> r = cur_.u32();
        return take(r);
    }

    /// @brief 剩余未读字节的视图（游标不前进）。声明结构读完后的 trailing
    ///        保留（design.md §27.1）据此取字节；仅在 ok() 时有意义。
    ByteView rest() const noexcept
    {
        return cur_.view().subOrEmpty(cur_.pos(), cur_.remaining());
    }

private:
    bool ensure(ca::u32 n)
    {
        if (!ok_) return false;
        if (cur_.remaining() < n) {
            ok_  = false;
            err_ = ReadError::Truncated;
            return false;
        }
        return true;
    }
    ca::u32 take(const Result<ca::u8, ReadError>& r)
    {
        if (r.is_err()) {
            ok_  = false;
            err_ = r.unwrap_err();
            return 0;
        }
        return r.unwrap();
    }
    ca::u32 take(const Result<ca::u16, ReadError>& r)
    {
        if (r.is_err()) {
            ok_  = false;
            err_ = r.unwrap_err();
            return 0;
        }
        return r.unwrap();
    }
    ca::u32 take(const Result<ca::u32, ReadError>& r)
    {
        if (r.is_err()) {
            ok_  = false;
            err_ = r.unwrap_err();
            return 0;
        }
        return r.unwrap();
    }

    Cursor cur_;
    bool ok_ = true;
    ReadError err_ = ReadError::Truncated;
};

/// 视图有效性 + 构造读取器的公共前奏。
Ard makeReader(const ClassFile& cf, const AttrSlot& slot, bool& spanOk)
{
    const ByteView data = cf.source().spanOrEmpty(slot.data);
    spanOk = data.size() == slot.data.length;
    return Ard{data};
}

/// @brief 声明结构读完后的 trailing 保留（design.md §27.1）。
///
/// 合法属性在声明结构之后应正好结束；多余字节属于畸形输入，但边界可确定——
/// 保留进属性体的 trailing 副本并降级记录，mutate 触发重编码时原样写回，
/// 不静默丢弃。与 @ref decodeCodeAttr 的 Code 体 trailing 同族。
void keepBodyTrailing(ClassFile& cf, const AttrSlot& slot, std::vector<ca::u8>& out, const Ard& r)
{
    const ByteView tail = r.rest();
    if (!tail.empty()) {
        out.assign(tail.data(), tail.data() + tail.size());
        cf.diagnostics().warn(ReadError::UnsupportedFeature, slot.data.offset);
    }
}

Result<std::unique_ptr<AttrBody>, ReadError> decodeBootstrapMethods(ClassFile& cf, const AttrSlot& slot)
{
    bool spanOk = false;
    Ard r = makeReader(cf, slot, spanOk);
    if (!spanOk) {
        return Err(ReadError::Truncated);
    }
    auto b = std::make_unique<BootstrapMethodsAttr>();
    const ca::u32 n = r.u2();
    b->entries.reserve(n > 1000 ? 1000 : n);
    for (ca::u32 i = 0; i < n; ++i) {
        BootstrapMethodsAttr::Entry e;
        e.methodRef = r.u2();
        const ca::u32 k = r.u2();
        for (ca::u32 j = 0; j < k; ++j) {
            e.args.push_back(r.u2());
        }
        b->entries.push_back(std::move(e));
    }
    if (!r.ok()) {
        return Err(r.err());
    }
    keepBodyTrailing(cf, slot, b->trailing, r);
    return Ok(std::unique_ptr<AttrBody>(std::move(b)));
}

Result<std::unique_ptr<AttrBody>, ReadError> decodeLineNumberTable(ClassFile& cf, const AttrSlot& slot)
{
    bool spanOk = false;
    Ard r = makeReader(cf, slot, spanOk);
    if (!spanOk) {
        return Err(ReadError::Truncated);
    }
    auto b = std::make_unique<LineNumberTableAttr>();
    const ca::u32 n = r.u2();
    b->rows.reserve(n > 10000 ? 10000 : n);
    for (ca::u32 i = 0; i < n; ++i) {
        b->rows.push_back(LineNumberTableAttr::Row{r.u2(), r.u2()});
    }
    if (!r.ok()) {
        return Err(r.err());
    }
    keepBodyTrailing(cf, slot, b->trailing, r);
    return Ok(std::unique_ptr<AttrBody>(std::move(b)));
}

Result<std::unique_ptr<AttrBody>, ReadError> decodeLocalVariableTable(ClassFile& cf,
                                                                     const AttrSlot& slot)
{
    bool spanOk = false;
    Ard r = makeReader(cf, slot, spanOk);
    if (!spanOk) {
        return Err(ReadError::Truncated);
    }
    auto b = std::make_unique<LocalVariableTableAttr>();
    const ca::u32 n = r.u2();
    b->rows.reserve(n > 10000 ? 10000 : n);
    for (ca::u32 i = 0; i < n; ++i) {
        LocalVariableTableAttr::Row row;
        row.startPc = r.u2();
        row.length  = r.u2();
        row.nameCp  = r.u2();
        row.descCp  = r.u2();
        row.slot    = r.u2();
        b->rows.push_back(row);
    }
    if (!r.ok()) {
        return Err(r.err());
    }
    keepBodyTrailing(cf, slot, b->trailing, r);
    return Ok(std::unique_ptr<AttrBody>(std::move(b)));
}

StackMapTableAttr::VType readVType(Ard& r)
{
    StackMapTableAttr::VType v;
    v.tag = static_cast<ca::u8>(r.u1());
    if (v.tag == StackMapTableAttr::VType::kObject ||
        v.tag == StackMapTableAttr::VType::kUninitialized) {
        v.cp = r.u2();
    }
    return v;
}

Result<std::unique_ptr<AttrBody>, ReadError> decodeStackMapTable(ClassFile& cf, const AttrSlot& slot)
{
    bool spanOk = false;
    Ard r = makeReader(cf, slot, spanOk);
    if (!spanOk) {
        return Err(ReadError::Truncated);
    }
    auto b = std::make_unique<StackMapTableAttr>();
    const ca::u32 n = r.u2();   // number_of_entries 是 u2（JVMS 4.7.4）
    b->frames.reserve(n > 1000 ? 1000 : n);
    for (ca::u32 i = 0; i < n; ++i) {
        StackMapTableAttr::Frame f;
        f.type = static_cast<ca::u8>(r.u1());
        if (f.type <= StackMapTableAttr::Frame::kSameMax) {
            // same_frame：offset 即类型值。
            f.offset = f.type;
        } else if (f.type <= StackMapTableAttr::Frame::kSame1Max) {
            // same_locals_1_stack_item：offset = type - 64，一个栈项。
            f.offset = static_cast<ca::u32>(f.type) - StackMapTableAttr::Frame::kSame1Base;
            f.stack.push_back(readVType(r));
        } else if (f.type == StackMapTableAttr::Frame::kSame1Ext) {
            // same_locals_1_stack_item_extended
            f.offset = r.u2();
            f.stack.push_back(readVType(r));
        } else if (f.type >= StackMapTableAttr::Frame::kChopMax &&
                   f.type <= StackMapTableAttr::Frame::kChopMin) {
            // chop_frame：k = 251 - type（隐含在类型里，不另存）。
            f.offset = r.u2();
        } else if (f.type == StackMapTableAttr::Frame::kSameExt) {
            // same_frame_extended
            f.offset = r.u2();
        } else if (f.type >= StackMapTableAttr::Frame::kAppendMin &&
                   f.type <= StackMapTableAttr::Frame::kAppendMax) {
            // append_frame：k = type - 251 个追加 locals。
            f.offset = r.u2();
            const ca::u32 k = static_cast<ca::u32>(f.type) - 251;
            for (ca::u32 j = 0; j < k; ++j) {
                f.locals.push_back(readVType(r));
            }
        } else if (f.type == StackMapTableAttr::Frame::kFull) {
            // full_frame
            f.offset = r.u2();
            const ca::u32 nl = r.u2();
            for (ca::u32 j = 0; j < nl; ++j) {
                f.locals.push_back(readVType(r));
            }
            const ca::u32 ns = r.u2();
            for (ca::u32 j = 0; j < ns; ++j) {
                f.stack.push_back(readVType(r));
            }
        } else {
            // 128..246 保留：明确拒绝。
            return Err(ReadError::UnsupportedFeature);
        }
        b->frames.push_back(std::move(f));
    }
    if (!r.ok()) {
        return Err(r.err());
    }
    keepBodyTrailing(cf, slot, b->trailing, r);
    return Ok(std::unique_ptr<AttrBody>(std::move(b)));
}

std::unique_ptr<AnnotationsAttr::ElemValue> readElemValue(Ard& r);

std::unique_ptr<AnnotationsAttr::Annotation> readAnnotation(Ard& r)
{
    auto a    = std::make_unique<AnnotationsAttr::Annotation>();
    a->typeCp = r.u2();
    const ca::u32 nv = r.u2();
    for (ca::u32 i = 0; i < nv; ++i) {
        const ca::u32 nameIdx = r.u2();
        auto v = readElemValue(r);
        if (v == nullptr) {
            return nullptr;
        }
        a->values.emplace_back(nameIdx, std::move(*v));
    }
    return r.ok() ? std::move(a) : nullptr;
}

std::unique_ptr<AnnotationsAttr::ElemValue> readElemValue(Ard& r)
{
    auto e = std::make_unique<AnnotationsAttr::ElemValue>();
    e->tag = static_cast<ca::u8>(r.u1());
    switch (e->tag) {
    case 'e':
        e->typeNameCp  = r.u2();
        e->constNameCp = r.u2();
        break;
    case '@':
        e->nested = readAnnotation(r);
        if (e->nested == nullptr) {
            return nullptr;
        }
        break;
    case '[': {
        const ca::u32 n = r.u2();
        for (ca::u32 i = 0; i < n; ++i) {
            auto v = readElemValue(r);
            if (v == nullptr) {
                return nullptr;
            }
            e->items.push_back(std::move(*v));
        }
        break;
    }
    default:
        // 'B' 'C' 'D' 'F' 'I' 'J' 'S' 'Z' 's' 'c'：单 u2 常量/类索引。
        e->cp = r.u2();
        break;
    }
    return r.ok() ? std::move(e) : nullptr;
}

Result<std::unique_ptr<AttrBody>, ReadError> decodeAnnotations(ClassFile& cf, const AttrSlot& slot)
{
    bool spanOk = false;
    Ard r = makeReader(cf, slot, spanOk);
    if (!spanOk) {
        return Err(ReadError::Truncated);
    }
    auto b = std::make_unique<AnnotationsAttr>();
    const ca::u32 n = r.u2();
    for (ca::u32 i = 0; i < n; ++i) {
        auto a = readAnnotation(r);
        if (a == nullptr) {
            return Err(r.err());
        }
        b->annos.push_back(std::move(*a));
    }
    if (!r.ok()) {
        return Err(r.err());
    }
    keepBodyTrailing(cf, slot, b->trailing, r);
    return Ok(std::unique_ptr<AttrBody>(std::move(b)));
}

Result<std::unique_ptr<AttrBody>, ReadError> decodeSignature(ClassFile& cf, const AttrSlot& slot)
{
    bool spanOk = false;
    Ard r = makeReader(cf, slot, spanOk);
    if (!spanOk) {
        return Err(ReadError::Truncated);
    }
    auto b = std::make_unique<SignatureAttr>();
    b->signatureCp = r.u2();
    if (!r.ok()) {
        return Err(r.err());
    }
    keepBodyTrailing(cf, slot, b->trailing, r);
    return Ok(std::unique_ptr<AttrBody>(std::move(b)));
}

/// @brief CharacterRangeTable 解码（javac -Xjcov 私有属性，规格见属性体
///        注释）：u2 行数 + 每行 {u2, u2, u4, u4, u2}。行数声明与剩余字节
///        不符（半截行 / 声明超出实际）时 Ard 置 Truncated，显式失败——
///        边界可确定的部分不猜测容忍（§27 同纪律）。
Result<std::unique_ptr<AttrBody>, ReadError> decodeCharacterRangeTable(ClassFile& cf,
                                                                       const AttrSlot& slot)
{
    bool spanOk = false;
    Ard r = makeReader(cf, slot, spanOk);
    if (!spanOk) {
        return Err(ReadError::Truncated);
    }
    auto b = std::make_unique<CharacterRangeTableAttr>();
    const ca::u32 n = r.u2();
    b->rows.reserve(n > 1000 ? 1000 : n);
    for (ca::u32 i = 0; i < n; ++i) {
        CharacterRangeTableAttr::Row row;
        row.startPc   = r.u2();
        row.endPc     = r.u2();
        row.charStart = r.u4();
        row.charEnd   = r.u4();
        row.flags     = r.u2();
        b->rows.push_back(row);
    }
    if (!r.ok()) {
        return Err(r.err());
    }
    keepBodyTrailing(cf, slot, b->trailing, r);
    return Ok(std::unique_ptr<AttrBody>(std::move(b)));
}

/// @brief B3 四件结构元数据（SourceFile/InnerClasses/EnclosingMethod/
///        Exceptions）解码器族，形状与既有 typed 解码同铁律（半截体 Err、
///        trailing 保留）。只挂类级/成员级属性表、无 pc 载荷，relocation
///        契约（§33 封闭集）不涉及。四件均已接 decodeAttrBody 分派
///        （design.md §32.9）；公开入口保留为直接解码面（不触碰槽状态）。
Result<std::unique_ptr<AttrBody>, ReadError> decodeSourceFile(ClassFile& cf, const AttrSlot& slot)
{
    bool spanOk = false;
    Ard r = makeReader(cf, slot, spanOk);
    if (!spanOk) {
        return Err(ReadError::Truncated);
    }
    auto b = std::make_unique<SourceFileAttr>();
    b->sourceFileCp = r.u2();
    if (!r.ok()) {
        return Err(r.err());
    }
    keepBodyTrailing(cf, slot, b->trailing, r);
    return Ok(std::unique_ptr<AttrBody>(std::move(b)));
}

/// @brief InnerClasses typed 解码（公开直读入口，不触碰槽状态；分派面走
///        decodeAttrBody）。
Result<std::unique_ptr<AttrBody>, ReadError> decodeInnerClassesBody(ClassFile& cf,
                                                                    const AttrSlot& slot)
{
    bool spanOk = false;
    Ard r = makeReader(cf, slot, spanOk);
    if (!spanOk) {
        return Err(ReadError::Truncated);
    }
    auto b = std::make_unique<InnerClassesAttr>();
    const ca::u32 n = r.u2();
    b->rows.reserve(n > 1000 ? 1000 : n);
    for (ca::u32 i = 0; i < n; ++i) {
        InnerClassesAttr::Row row;
        row.innerClassInfoCp = r.u2();
        row.outerClassInfoCp = r.u2();
        row.innerNameCp      = r.u2();
        row.accessFlags      = r.u2();
        b->rows.push_back(row);
    }
    if (!r.ok()) {
        return Err(r.err());
    }
    keepBodyTrailing(cf, slot, b->trailing, r);
    return Ok(std::unique_ptr<AttrBody>(std::move(b)));
}

/// @brief EnclosingMethod typed 解码（公开直读入口，契约同
///        @ref decodeInnerClassesBody）。
Result<std::unique_ptr<AttrBody>, ReadError> decodeEnclosingMethodBody(ClassFile& cf,
                                                                       const AttrSlot& slot)
{
    bool spanOk = false;
    Ard r = makeReader(cf, slot, spanOk);
    if (!spanOk) {
        return Err(ReadError::Truncated);
    }
    auto b = std::make_unique<EnclosingMethodAttr>();
    b->classCp     = r.u2();
    b->methodNatCp = r.u2();
    if (!r.ok()) {
        return Err(r.err());
    }
    keepBodyTrailing(cf, slot, b->trailing, r);
    return Ok(std::unique_ptr<AttrBody>(std::move(b)));
}

/// @brief Exceptions typed 解码（公开直读入口，契约同
///        @ref decodeInnerClassesBody）。
Result<std::unique_ptr<AttrBody>, ReadError> decodeExceptionsBody(ClassFile& cf,
                                                                  const AttrSlot& slot)
{
    bool spanOk = false;
    Ard r = makeReader(cf, slot, spanOk);
    if (!spanOk) {
        return Err(ReadError::Truncated);
    }
    auto b = std::make_unique<ExceptionsAttr>();
    const ca::u32 n = r.u2();
    b->exceptionCps.reserve(n > 100 ? 100 : n);
    for (ca::u32 i = 0; i < n; ++i) {
        b->exceptionCps.push_back(r.u2());
    }
    if (!r.ok()) {
        return Err(r.err());
    }
    keepBodyTrailing(cf, slot, b->trailing, r);
    return Ok(std::unique_ptr<AttrBody>(std::move(b)));
}

/// @brief type_path 读取（JVMS 4.7.20.2）：path_length + 若干 (kind,
///        type_argument_index) 对；kind > 3 属非法，显式拒绝。
Result<std::vector<TypeAnnotationsAttr::TypePathEntry>, ReadError>
readTypePath(Ard& r)
{
    std::vector<TypeAnnotationsAttr::TypePathEntry> out;
    const ca::u32 n = r.u1();
    out.reserve(n);
    for (ca::u32 i = 0; i < n; ++i) {
        TypeAnnotationsAttr::TypePathEntry e;
        e.kind = static_cast<ca::u8>(r.u1());
        e.typeArgIndex = static_cast<ca::u8>(r.u1());
        if (e.kind > 3) {
            return Err(ReadError::UnsupportedFeature);
        }
        out.push_back(e);
    }
    if (!r.ok()) {
        return Err(r.err());
    }
    return Ok(std::move(out));
}

/// @brief target_info 读取：按 target_type 分派（JVMS 4.7.20.1 表）；
///        未定义的 target_type 显式拒绝（结构与边界都可确定之外的东西
///        不猜测容忍，§27.2 同纪律）。
Result<void, ReadError> readTargetInfo(Ard& r, TypeAnnotationsAttr::Annotation& a)
{
    using TA = TypeAnnotationsAttr;
    switch (a.targetType) {
    case TA::kClassTypeParameter:
    case TA::kMethodTypeParameter:
        a.typeParameterIndex = static_cast<ca::u8>(r.u1());
        break;
    case TA::kSupertype:
        a.supertypeIndex = r.u2();
        break;
    case TA::kClassTypeParameterBound:
    case TA::kMethodTypeParameterBound:
        a.typeParameterIndex = static_cast<ca::u8>(r.u1());
        a.boundIndex = static_cast<ca::u8>(r.u1());
        break;
    case TA::kField:
    case TA::kMethodReturn:
    case TA::kMethodReceiver:
        break;
    case TA::kFormalsParameter:
        a.formalParameterIndex = static_cast<ca::u8>(r.u1());
        break;
    case TA::kThrows:
        a.throwsIndex = r.u2();
        break;
    case TA::kLocalVar:
    case TA::kLocalVarResource: {
        const ca::u32 n = r.u2();
        a.localVars.reserve(n > 1000 ? 1000 : n);
        for (ca::u32 i = 0; i < n; ++i) {
            TA::LocalVarEntry e;
            e.startPc = r.u2();
            e.length  = r.u2();
            e.slot    = r.u2();
            a.localVars.push_back(e);
        }
        break;
    }
    case TA::kCatch:
        a.catchIndex = r.u2();
        break;
    default:
        // 0x43..0x46：instanceof/new/方法引用——offset 载荷；0x47 cast——
        // offset + type_index；0x48..0x4B：调用/引用类型实参——offset +
        // type_argument_index。
        if (a.targetType >= TA::kOffsetBase && a.targetType <= TA::kOffsetMax) {
            a.offset = r.u2();
            break;
        }
        if (a.targetType == TA::kCast) {
            a.offset = r.u2();
            a.typeArgumentIndex = static_cast<ca::u8>(r.u1());   ///< cast 的 type_index
            break;
        }
        if (a.targetType >= TA::kTypeArgBase && a.targetType <= TA::kTypeArgMax) {
            a.offset = r.u2();
            a.typeArgumentIndex = static_cast<ca::u8>(r.u1());
            break;
        }
        return Err(ReadError::UnsupportedFeature);
    }
    if (!r.ok()) {
        return Err(r.err());
    }
    return Ok();
}

/// @brief type_annotation 读取：target_info + type_path + 注解体。
Result<std::unique_ptr<TypeAnnotationsAttr::Annotation>, ReadError>
readTypeAnnotation(Ard& r)
{
    auto a = std::make_unique<TypeAnnotationsAttr::Annotation>();
    a->targetType = static_cast<ca::u8>(r.u1());
    auto t = readTargetInfo(r, *a);
    if (t.is_err()) {
        return Err(t.unwrap_err());
    }
    auto p = readTypePath(r);
    if (p.is_err()) {
        return Err(p.unwrap_err());
    }
    a->typePath = std::move(p).unwrap();
    a->typeIndexCp = r.u2();
    const ca::u32 nv = r.u2();
    a->values.reserve(nv > 16 ? 16 : nv);
    for (ca::u32 i = 0; i < nv; ++i) {
        const ca::u32 nameIdx = r.u2();
        auto v = readElemValue(r);
        if (v == nullptr) {
            return Err(r.err());
        }
        a->values.emplace_back(nameIdx, std::move(*v));
    }
    if (!r.ok()) {
        return Err(r.err());
    }
    return Ok(std::move(a));
}

Result<std::unique_ptr<AttrBody>, ReadError> decodeTypeAnnotations(ClassFile& cf, const AttrSlot& slot)
{
    bool spanOk = false;
    Ard r = makeReader(cf, slot, spanOk);
    if (!spanOk) {
        return Err(ReadError::Truncated);
    }
    auto b = std::make_unique<TypeAnnotationsAttr>();
    const ca::u32 n = r.u2();
    b->annotations.reserve(n > 64 ? 64 : n);
    for (ca::u32 i = 0; i < n; ++i) {
        auto a = readTypeAnnotation(r);
        if (a.is_err()) {
            return Err(a.unwrap_err());
        }
        b->annotations.push_back(std::move(*std::move(a).unwrap()));
    }
    if (!r.ok()) {
        return Err(r.err());
    }
    keepBodyTrailing(cf, slot, b->trailing, r);
    return Ok(std::unique_ptr<AttrBody>(std::move(b)));
}

Result<void, WriteError> writeVType(ByteSink& o, const StackMapTableAttr::VType& v)
{
    o.u8(v.tag);
    if (v.tag == StackMapTableAttr::VType::kObject ||
        v.tag == StackMapTableAttr::VType::kUninitialized) {
        if (!fitsU16(v.cp)) {
            return Err(WriteError::Unencodable);
        }
        o.u16(static_cast<ca::u16>(v.cp));
    }
    return Ok();
}

Result<void, WriteError> writeElemValue(ByteSink& o, const AnnotationsAttr::ElemValue& e);

Result<void, WriteError> writeAnnotation(ByteSink& o, const AnnotationsAttr::Annotation& a)
{
    if (!fitsU16(a.typeCp) ||
        a.values.size() > std::numeric_limits<ca::u16>::max()) {
        return Err(WriteError::Unencodable);
    }
    o.u16(static_cast<ca::u16>(a.typeCp));
    o.u16(static_cast<ca::u16>(a.values.size()));
    for (const auto& kv : a.values) {
        if (!fitsU16(kv.first)) {
            return Err(WriteError::Unencodable);
        }
        o.u16(static_cast<ca::u16>(kv.first));
        const Result<void, WriteError> rv = writeElemValue(o, kv.second);
        if (rv.is_err()) {
            return Err(rv.unwrap_err());
        }
    }
    return Ok();
}

Result<void, WriteError> writeElemValue(ByteSink& o, const AnnotationsAttr::ElemValue& e)
{
    o.u8(e.tag);
    switch (e.tag) {
    case 'e':
        if (!fitsU16(e.typeNameCp) || !fitsU16(e.constNameCp)) {
            return Err(WriteError::Unencodable);
        }
        o.u16(static_cast<ca::u16>(e.typeNameCp));
        o.u16(static_cast<ca::u16>(e.constNameCp));
        break;
    case '@':
        {
            if (e.nested == nullptr) {
                return Err(WriteError::Unencodable);
            }
            const Result<void, WriteError> rn = writeAnnotation(o, *e.nested);
            if (rn.is_err()) {
                return Err(rn.unwrap_err());
            }
        }
        break;
    case '[':
        {
            if (e.items.size() > std::numeric_limits<ca::u16>::max()) {
                return Err(WriteError::Unencodable);
            }
            o.u16(static_cast<ca::u16>(e.items.size()));
            for (const auto& it : e.items) {
                const Result<void, WriteError> ri = writeElemValue(o, it);
                if (ri.is_err()) {
                    return Err(ri.unwrap_err());
                }
            }
        }
        break;
    default:
        if (!fitsU16(e.cp)) {
            return Err(WriteError::Unencodable);
        }
        o.u16(static_cast<ca::u16>(e.cp));
        break;
    }
    return Ok();
}

/// @brief type_path 写回（与 readTypePath 对称）。
Result<void, WriteError> writeTypePath(ByteSink& o,
                                       const std::vector<TypeAnnotationsAttr::TypePathEntry>& p)
{
    if (p.size() > std::numeric_limits<ca::u8>::max()) {
        return Err(WriteError::Unencodable);
    }
    o.u8(static_cast<ca::u8>(p.size()));
    for (const auto& e : p) {
        o.u8(e.kind);
        o.u8(e.typeArgIndex);
    }
    return Ok();
}

/// @brief target_info 写回：按 target_type 分派（与 readTargetInfo 对称）。
Result<void, WriteError> writeTargetInfo(ByteSink& o,
                                         const TypeAnnotationsAttr::Annotation& a)
{
    using TA = TypeAnnotationsAttr;
    switch (a.targetType) {
    case TA::kClassTypeParameter:
    case TA::kMethodTypeParameter:
        o.u8(a.typeParameterIndex);
        break;
    case TA::kSupertype:
        if (!fitsU16(a.supertypeIndex)) {
            return Err(WriteError::Unencodable);
        }
        o.u16(static_cast<ca::u16>(a.supertypeIndex));
        break;
    case TA::kClassTypeParameterBound:
    case TA::kMethodTypeParameterBound:
        o.u8(a.typeParameterIndex);
        o.u8(a.boundIndex);
        break;
    case TA::kField:
    case TA::kMethodReturn:
    case TA::kMethodReceiver:
        break;
    case TA::kFormalsParameter:
        o.u8(a.formalParameterIndex);
        break;
    case TA::kThrows:
        if (!fitsU16(a.throwsIndex)) {
            return Err(WriteError::Unencodable);
        }
        o.u16(static_cast<ca::u16>(a.throwsIndex));
        break;
    case TA::kLocalVar:
    case TA::kLocalVarResource:
        if (a.localVars.size() > std::numeric_limits<ca::u16>::max()) {
            return Err(WriteError::Unencodable);
        }
        o.u16(static_cast<ca::u16>(a.localVars.size()));
        for (const auto& e : a.localVars) {
            if (!fitsU16(e.startPc) || !fitsU16(e.length) || !fitsU16(e.slot)) {
                return Err(WriteError::Unencodable);
            }
            o.u16(static_cast<ca::u16>(e.startPc));
            o.u16(static_cast<ca::u16>(e.length));
            o.u16(static_cast<ca::u16>(e.slot));
        }
        break;
    case TA::kCatch:
        if (!fitsU16(a.catchIndex)) {
            return Err(WriteError::Unencodable);
        }
        o.u16(static_cast<ca::u16>(a.catchIndex));
        break;
    default:
        if (a.targetType >= TA::kOffsetBase && a.targetType <= TA::kOffsetMax) {
            if (!fitsU16(a.offset)) {
                return Err(WriteError::Unencodable);
            }
            o.u16(static_cast<ca::u16>(a.offset));
            break;
        }
        if (a.targetType == TA::kCast) {
            if (!fitsU16(a.offset)) {
                return Err(WriteError::Unencodable);
            }
            o.u16(static_cast<ca::u16>(a.offset));
            o.u8(a.typeArgumentIndex);
            break;
        }
        if (a.targetType >= TA::kTypeArgBase && a.targetType <= TA::kTypeArgMax) {
            if (!fitsU16(a.offset)) {
                return Err(WriteError::Unencodable);
            }
            o.u16(static_cast<ca::u16>(a.offset));
            o.u8(a.typeArgumentIndex);
            break;
        }
        return Err(WriteError::Unencodable);
    }
    return Ok();
}

/// @brief type_annotation 写回：target_info + type_path + 注解体。
Result<void, WriteError> writeTypeAnnotation(ByteSink& o,
                                             const TypeAnnotationsAttr::Annotation& a)
{
    if (!fitsU16(a.typeIndexCp) ||
        a.values.size() > std::numeric_limits<ca::u16>::max()) {
        return Err(WriteError::Unencodable);
    }
    o.u8(a.targetType);
    const Result<void, WriteError> rt = writeTargetInfo(o, a);
    if (rt.is_err()) {
        return Err(rt.unwrap_err());
    }
    const Result<void, WriteError> rp = writeTypePath(o, a.typePath);
    if (rp.is_err()) {
        return Err(rp.unwrap_err());
    }
    o.u16(static_cast<ca::u16>(a.typeIndexCp));
    o.u16(static_cast<ca::u16>(a.values.size()));
    for (const auto& kv : a.values) {
        if (!fitsU16(kv.first)) {
            return Err(WriteError::Unencodable);
        }
        o.u16(static_cast<ca::u16>(kv.first));
        const Result<void, WriteError> rv = writeElemValue(o, kv.second);
        if (rv.is_err()) {
            return Err(rv.unwrap_err());
        }
    }
    return Ok();
}

Result<AttrBody*, ReadError> decodeAttrBody(ClassFile& cf, AttrSlot& slot)
{
    const std::string_view name = cf.cp.utf8Of(slot.nameCp);

    if (name == kCodeAttrName) {
        auto r = decodeCodeAttr(cf, slot);
        if (r.is_err()) {
            return Err(r.unwrap_err());
        }
        slot.setDecoded(std::move(r).unwrap());
        return Ok(slot.body.get());
    }

    auto attach = [&slot](std::unique_ptr<AttrBody> b) -> Result<AttrBody*, ReadError> {
        slot.setDecoded(std::move(b));
        return Ok(slot.body.get());
    };

    if (name == kBootstrapMethodsName) {
        auto r = decodeBootstrapMethods(cf, slot);
        if (r.is_err()) return Err(r.unwrap_err());
        return attach(std::move(r).unwrap());
    }
    if (name == kLineNumberTableName) {
        auto r = decodeLineNumberTable(cf, slot);
        if (r.is_err()) return Err(r.unwrap_err());
        return attach(std::move(r).unwrap());
    }
    if (name == kStackMapTableName) {
        auto r = decodeStackMapTable(cf, slot);
        if (r.is_err()) return Err(r.unwrap_err());
        return attach(std::move(r).unwrap());
    }
    if (name == kLocalVariableTableName || name == kLocalVariableTypeTableName) {
        auto r = decodeLocalVariableTable(cf, slot);
        if (r.is_err()) return Err(r.unwrap_err());
        return attach(std::move(r).unwrap());
    }
    if (name == kRvaName || name == kRiaName) {
        auto r = decodeAnnotations(cf, slot);
        if (r.is_err()) return Err(r.unwrap_err());
        return attach(std::move(r).unwrap());
    }
    if (name == kRvtaName || name == kRitaName) {
        auto r = decodeTypeAnnotations(cf, slot);
        if (r.is_err()) return Err(r.unwrap_err());
        return attach(std::move(r).unwrap());
    }
    if (name == kSignatureName) {
        auto r = decodeSignature(cf, slot);
        if (r.is_err()) return Err(r.unwrap_err());
        return attach(std::move(r).unwrap());
    }
    if (name == kCharacterRangeTableName) {
        auto r = decodeCharacterRangeTable(cf, slot);
        if (r.is_err()) return Err(r.unwrap_err());
        return attach(std::move(r).unwrap());
    }
    if (name == kSourceFileName) {
        auto r = decodeSourceFile(cf, slot);
        if (r.is_err()) return Err(r.unwrap_err());
        return attach(std::move(r).unwrap());
    }
    if (name == kInnerClassesName) {
        auto r = decodeInnerClassesBody(cf, slot);
        if (r.is_err()) return Err(r.unwrap_err());
        return attach(std::move(r).unwrap());
    }
    if (name == kEnclosingMethodName) {
        auto r = decodeEnclosingMethodBody(cf, slot);
        if (r.is_err()) return Err(r.unwrap_err());
        return attach(std::move(r).unwrap());
    }
    if (name == kExceptionsName) {
        auto r = decodeExceptionsBody(cf, slot);
        if (r.is_err()) return Err(r.unwrap_err());
        return attach(std::move(r).unwrap());
    }

    // 未建模属性：原样保留字节（保真的兜底形态）。
    const ByteView data = cf.source().spanOrEmpty(slot.data);
    if (data.size() != slot.data.length) {
        return Err(ReadError::Truncated);
    }
    auto raw = std::make_unique<RawAttr>(
        std::vector<ca::u8>(data.data(), data.data() + data.size()));
    slot.setDecoded(std::move(raw));
    return Ok(slot.body.get());
}

Result<void, ReadError> decodeAllAttrs(ClassFile& cf)
{
    for (AttrSlot& s : cf.attrs) {
        const Result<AttrBody*, ReadError> r = cf.decodeAttr(s);
        if (r.is_err()) {
            return Err(r.unwrap_err());
        }
    }
    for (MemberInfo& m : cf.fields) {
        for (AttrSlot& s : m.attrs) {
            const Result<AttrBody*, ReadError> r = cf.decodeAttr(s);
            if (r.is_err()) {
                return Err(r.unwrap_err());
            }
        }
    }
    for (MemberInfo& m : cf.methods) {
        for (AttrSlot& s : m.attrs) {
            const Result<AttrBody*, ReadError> r = cf.decodeAttr(s);
            if (r.is_err()) {
                return Err(r.unwrap_err());
            }
            if (s.body != nullptr && s.body->getType() == static_cast<int>(AttrKind::Code)) {
                // Code 内的嵌套属性同样递归解开。
                auto& code = static_cast<CodeAttr&>(*s.body);
                for (AttrSlot& nested : code.attrs) {
                    const Result<AttrBody*, ReadError> rn = cf.decodeAttr(nested);
                    if (rn.is_err()) {
                        return Err(rn.unwrap_err());
                    }
                }
            }
        }
    }
    return Ok();
}

Result<void, WriteError> encodeAttr(const ClassFile& cf,
                                    const AttrSlot& slot,
                                    ByteSink& out,
                                    ByteView source)
{
    if (slot.nameCp > std::numeric_limits<ca::u16>::max()) {
        return Err(WriteError::Unencodable);
    }
    out.u16(static_cast<ca::u16>(slot.nameCp));

    // 父 Code 槽即使自身未置脏，只要任一嵌套属性被修改，也必须重编码。
    // 否则 copy-through 会把嵌套修改静默吞掉。
    if (!attrNeedsReencode(slot)) {
        // 保真路径：原样 copy 源字节，内容对编码器完全不可见。
        const ByteView v = source.spanOrEmpty(slot.data);
        if (v.size() != slot.data.length) {
            return Err(WriteError::BadSpan);
        }
        out.u32(slot.data.length);
        out.raw(v);
        return Ok();
    }

    if (slot.body == nullptr) {
        return Err(WriteError::Unencodable);
    }

    // 属性体需要前置长度：先编码到临时缓冲，再写 length + 内容。
    ByteSink body;
    switch (static_cast<AttrKind>(slot.body->getType())) {
        case AttrKind::Code:
            {
                const Result<void, WriteError> r =
                    encodeCodeBody(cf, static_cast<const CodeAttr&>(*slot.body), body, source);
                if (r.is_err()) {
                    return Err(r.unwrap_err());
                }
            }
            break;
        case AttrKind::BootstrapMethods: {
            const auto& b = static_cast<const BootstrapMethodsAttr&>(*slot.body);
            if (b.entries.size() > std::numeric_limits<ca::u16>::max()) {
                return Err(WriteError::Unencodable);
            }
            for (const auto& e : b.entries) {
                if (!fitsU16(e.methodRef) ||
                    e.args.size() > std::numeric_limits<ca::u16>::max()) {
                    return Err(WriteError::Unencodable);
                }
                for (const ca::u32 a : e.args) {
                    if (!fitsU16(a)) {
                        return Err(WriteError::Unencodable);
                    }
                }
            }
            body.u16(static_cast<ca::u16>(b.entries.size()));
            for (const auto& e : b.entries) {
                body.u16(static_cast<ca::u16>(e.methodRef));
                body.u16(static_cast<ca::u16>(e.args.size()));
                for (ca::u32 a : e.args) {
                    body.u16(static_cast<ca::u16>(a));
                }
            }
            body.raw(ByteView{b.trailing});
            break;
        }
        case AttrKind::LineNumberTable: {
            const auto& b = static_cast<const LineNumberTableAttr&>(*slot.body);
            if (b.rows.size() > std::numeric_limits<ca::u16>::max()) {
                return Err(WriteError::Unencodable);
            }
            for (const auto& row : b.rows) {
                if (!fitsU16(row.startPc) || !fitsU16(row.line)) {
                    return Err(WriteError::Unencodable);
                }
            }
            body.u16(static_cast<ca::u16>(b.rows.size()));
            for (const auto& row : b.rows) {
                body.u16(static_cast<ca::u16>(row.startPc));
                body.u16(static_cast<ca::u16>(row.line));
            }
            body.raw(ByteView{b.trailing});
            break;
        }
        case AttrKind::LocalVariableTable: {
            const auto& b = static_cast<const LocalVariableTableAttr&>(*slot.body);
            if (b.rows.size() > std::numeric_limits<ca::u16>::max()) {
                return Err(WriteError::Unencodable);
            }
            for (const auto& row : b.rows) {
                if (!fitsU16(row.startPc) || !fitsU16(row.length) ||
                    !fitsU16(row.nameCp) || !fitsU16(row.descCp) || !fitsU16(row.slot)) {
                    return Err(WriteError::Unencodable);
                }
            }
            body.u16(static_cast<ca::u16>(b.rows.size()));
            for (const auto& row : b.rows) {
                body.u16(static_cast<ca::u16>(row.startPc));
                body.u16(static_cast<ca::u16>(row.length));
                body.u16(static_cast<ca::u16>(row.nameCp));
                body.u16(static_cast<ca::u16>(row.descCp));
                body.u16(static_cast<ca::u16>(row.slot));
            }
            body.raw(ByteView{b.trailing});
            break;
        }
        case AttrKind::StackMapTable: {
            const auto& b = static_cast<const StackMapTableAttr&>(*slot.body);
            using SmtFrame = StackMapTableAttr::Frame;
            if (b.frames.size() > std::numeric_limits<ca::u16>::max()) {
                return Err(WriteError::Unencodable);
            }
            for (const auto& f : b.frames) {
                if ((f.type == SmtFrame::kSame1Ext || f.type >= SmtFrame::kChopMax) &&
                    !fitsU16(f.offset)) {
                    return Err(WriteError::Unencodable);
                }
                if (f.type == SmtFrame::kFull &&
                    (f.locals.size() > std::numeric_limits<ca::u16>::max() ||
                     f.stack.size() > std::numeric_limits<ca::u16>::max())) {
                    return Err(WriteError::Unencodable);
                }
            }
            body.u16(static_cast<ca::u16>(b.frames.size()));
            for (const auto& f : b.frames) {
                body.u8(f.type);
                if (f.type == SmtFrame::kSame1Ext || f.type >= SmtFrame::kChopMax) {
                    body.u16(static_cast<ca::u16>(f.offset));
                }
                if (f.type == SmtFrame::kSame1Ext ||
                    (f.type >= SmtFrame::kSame1Base && f.type <= SmtFrame::kSame1Max)) {
                    for (const auto& v : f.stack) {
                        const Result<void, WriteError> rv = writeVType(body, v);
                        if (rv.is_err()) {
                            return Err(rv.unwrap_err());
                        }
                    }
                }
                if (f.type >= SmtFrame::kAppendMin && f.type <= SmtFrame::kAppendMax) {
                    for (const auto& v : f.locals) {
                        const Result<void, WriteError> rv = writeVType(body, v);
                        if (rv.is_err()) {
                            return Err(rv.unwrap_err());
                        }
                    }
                }
                if (f.type == SmtFrame::kFull) {
                    body.u16(static_cast<ca::u16>(f.locals.size()));
                    for (const auto& v : f.locals) {
                        const Result<void, WriteError> rv = writeVType(body, v);
                        if (rv.is_err()) {
                            return Err(rv.unwrap_err());
                        }
                    }
                    body.u16(static_cast<ca::u16>(f.stack.size()));
                    for (const auto& v : f.stack) {
                        const Result<void, WriteError> rv = writeVType(body, v);
                        if (rv.is_err()) {
                            return Err(rv.unwrap_err());
                        }
                    }
                }
            }
            body.raw(ByteView{b.trailing});
            break;
        }
        case AttrKind::Annotations: {
            const auto& b = static_cast<const AnnotationsAttr&>(*slot.body);
            if (b.annos.size() > std::numeric_limits<ca::u16>::max()) {
                return Err(WriteError::Unencodable);
            }
            body.u16(static_cast<ca::u16>(b.annos.size()));
            for (const auto& a : b.annos) {
                const Result<void, WriteError> ra = writeAnnotation(body, a);
                if (ra.is_err()) {
                    return Err(ra.unwrap_err());
                }
            }
            body.raw(ByteView{b.trailing});
            break;
        }
        case AttrKind::Signature: {
            const auto& b = static_cast<const SignatureAttr&>(*slot.body);
            if (!fitsU16(b.signatureCp)) {
                return Err(WriteError::Unencodable);
            }
            body.u16(static_cast<ca::u16>(b.signatureCp));
            body.raw(ByteView{b.trailing});
            break;
        }
        case AttrKind::TypeAnnotations: {
            const auto& b = static_cast<const TypeAnnotationsAttr&>(*slot.body);
            if (b.annotations.size() > std::numeric_limits<ca::u16>::max()) {
                return Err(WriteError::Unencodable);
            }
            body.u16(static_cast<ca::u16>(b.annotations.size()));
            for (const auto& a : b.annotations) {
                const Result<void, WriteError> ra = writeTypeAnnotation(body, a);
                if (ra.is_err()) {
                    return Err(ra.unwrap_err());
                }
            }
            body.raw(ByteView{b.trailing});
            break;
        }
        case AttrKind::CharacterRangeTable: {
            // u2 域宽闸门（§27.2）：start_pc/end_pc/flags 是 u2 编码，超界
            // 显式 Err；char_start/char_end 本就是 u4 编码，无闸门。
            const auto& b = static_cast<const CharacterRangeTableAttr&>(*slot.body);
            if (b.rows.size() > std::numeric_limits<ca::u16>::max()) {
                return Err(WriteError::Unencodable);
            }
            for (const auto& row : b.rows) {
                if (!fitsU16(row.startPc) || !fitsU16(row.endPc) || !fitsU16(row.flags)) {
                    return Err(WriteError::Unencodable);
                }
            }
            body.u16(static_cast<ca::u16>(b.rows.size()));
            for (const auto& row : b.rows) {
                body.u16(static_cast<ca::u16>(row.startPc));
                body.u16(static_cast<ca::u16>(row.endPc));
                body.u32(row.charStart);
                body.u32(row.charEnd);
                body.u16(static_cast<ca::u16>(row.flags));
            }
            body.raw(ByteView{b.trailing});
            break;
        }
        case AttrKind::SourceFile: {
            // u2 域宽闸门（§27.2）：sourcefile_index 超 u16 显式 Err。
            const auto& b = static_cast<const SourceFileAttr&>(*slot.body);
            if (!fitsU16(b.sourceFileCp)) {
                return Err(WriteError::Unencodable);
            }
            body.u16(static_cast<ca::u16>(b.sourceFileCp));
            body.raw(ByteView{b.trailing});
            break;
        }
        case AttrKind::InnerClasses: {
            const auto& b = static_cast<const InnerClassesAttr&>(*slot.body);
            if (b.rows.size() > std::numeric_limits<ca::u16>::max()) {
                return Err(WriteError::Unencodable);
            }
            for (const auto& row : b.rows) {
                if (!fitsU16(row.innerClassInfoCp) || !fitsU16(row.outerClassInfoCp) ||
                    !fitsU16(row.innerNameCp) || !fitsU16(row.accessFlags)) {
                    return Err(WriteError::Unencodable);
                }
            }
            body.u16(static_cast<ca::u16>(b.rows.size()));
            for (const auto& row : b.rows) {
                body.u16(static_cast<ca::u16>(row.innerClassInfoCp));
                body.u16(static_cast<ca::u16>(row.outerClassInfoCp));
                body.u16(static_cast<ca::u16>(row.innerNameCp));
                body.u16(static_cast<ca::u16>(row.accessFlags));
            }
            body.raw(ByteView{b.trailing});
            break;
        }
        case AttrKind::EnclosingMethod: {
            const auto& b = static_cast<const EnclosingMethodAttr&>(*slot.body);
            if (!fitsU16(b.classCp) || !fitsU16(b.methodNatCp)) {
                return Err(WriteError::Unencodable);
            }
            body.u16(static_cast<ca::u16>(b.classCp));
            body.u16(static_cast<ca::u16>(b.methodNatCp));
            body.raw(ByteView{b.trailing});
            break;
        }
        case AttrKind::Exceptions: {
            const auto& b = static_cast<const ExceptionsAttr&>(*slot.body);
            if (b.exceptionCps.size() > std::numeric_limits<ca::u16>::max()) {
                return Err(WriteError::Unencodable);
            }
            for (const ca::u32 cp : b.exceptionCps) {
                if (!fitsU16(cp)) {
                    return Err(WriteError::Unencodable);
                }
            }
            body.u16(static_cast<ca::u16>(b.exceptionCps.size()));
            for (const ca::u32 cp : b.exceptionCps) {
                body.u16(static_cast<ca::u16>(cp));
            }
            body.raw(ByteView{b.trailing});
            break;
        }
        case AttrKind::Raw: {
            const std::vector<ca::u8>& b = static_cast<const RawAttr&>(*slot.body).bytes();
            if (!b.empty()) {
                body.raw(ByteView{b});
            }
            break;
        }
        default:
            return Err(WriteError::NotModeled);
    }

    if (body.size() > std::numeric_limits<ca::u32>::max()) {
        return Err(WriteError::Unencodable);
    }
    out.u32(static_cast<ca::u32>(body.size()));
    out.raw(ByteView{body.bytes()});
    return Ok();
}

}   // namespace jbc::core
