/// @file        insn_codec.cpp
/// @brief       code 数组 ↔ 指令序列的纯函数编解码实现。
#include "jbc/core/insn_codec.hpp"

#include <cstring>

namespace jbc::core {

namespace {

/// cursor 上读 1/2/4 字节有符号/无符号整数（大端）——指令操作数的最小读集。
bool rdU1(ByteView v, ca::usize pos, ca::u8& out) noexcept
{
    if (pos >= v.size()) return false;
    out = v[pos];
    return true;
}

bool rdU2(ByteView v, ca::usize pos, ca::u32& out) noexcept
{
    if (pos + 2 > v.size()) return false;
    out = (static_cast<ca::u32>(static_cast<ca::u8>(v[pos])) << 8) |
          static_cast<ca::u8>(v[pos + 1]);
    return true;
}

bool rdS1(ByteView v, ca::usize pos, ca::i32& out) noexcept
{
    if (pos >= v.size()) return false;
    out = static_cast<ca::i8>(v[pos]);
    return true;
}

bool rdS2(ByteView v, ca::usize pos, ca::i32& out) noexcept
{
    if (pos + 2 > v.size()) return false;
    out = static_cast<ca::i16>(
        (static_cast<ca::u16>(static_cast<ca::u8>(v[pos])) << 8) |
        static_cast<ca::u8>(v[pos + 1]));
    return true;
}

bool rdS4(ByteView v, ca::usize pos, ca::i32& out) noexcept
{
    if (pos + 4 > v.size()) return false;
    ca::u32 u = 0;
    for (int i = 0; i < 4; ++i) {
        u = (u << 8) | static_cast<ca::u8>(v[pos + static_cast<ca::usize>(i)]);
    }
    std::memcpy(&out, &u, sizeof(out));
    return true;
}

}   // namespace

Result<std::vector<RawInsn>, ReadError> decodeInsns(ByteView code)
{
    std::vector<RawInsn> out;
    const ca::usize size = code.size();

    ca::usize pos = 0;
    while (pos < size) {
        RawInsn in;
        in.pc = static_cast<ca::u32>(pos);
        const ca::u8 byte = static_cast<ca::u8>(code[pos]);
        if (byte >= 0xca && byte != 0xc4) {
            // 0xCA/0xFE/0xFF 是保留码；0xC4 wide 在下方展开。
            return Err(ReadError::UnsupportedFeature);
        }
        ca::u8 opByte = byte;
        ++pos;

        if (opByte == 0xc4) {   // wide 前缀
            ca::u8 inner = 0;
            if (!rdU1(code, pos, inner)) {
                return Err(ReadError::Truncated);
            }
            ++pos;
            const Op innerOp = static_cast<Op>(inner);
            if (!isWideLocalOp(innerOp)) {
                return Err(ReadError::UnsupportedFeature);
            }
            in.op  = innerOp;
            in.wide = true;
            ca::u32 idx = 0;
            if (!rdU2(code, pos, idx)) {
                return Err(ReadError::Truncated);
            }
            pos += 2;
            in.a = static_cast<ca::i32>(idx);
            if (innerOp == Op::IINC) {
                ca::i32 c = 0;
                if (!rdS2(code, pos, c)) {
                    return Err(ReadError::Truncated);
                }
                pos += 2;
                in.b = c;
            }
            out.push_back(std::move(in));
            continue;
        }

        in.op = static_cast<Op>(opByte);
        switch (shapeOf(in.op)) {
        case Shape::None:
            break;
        case Shape::Imm1: {
            ca::i32 v = 0;
            if (!rdS1(code, pos, v)) return Err(ReadError::Truncated);
            in.a = v;
            pos += 1;
            break;
        }
        case Shape::Imm2: {
            ca::i32 v = 0;
            if (!rdS2(code, pos, v)) return Err(ReadError::Truncated);
            in.a = v;
            pos += 2;
            break;
        }
        case Shape::Local1: {
            if (pos >= size) return Err(ReadError::Truncated);
            in.a = static_cast<ca::u8>(code[pos]);
            pos += 1;
            break;
        }
        case Shape::Iinc1: {
            if (pos + 2 > size) return Err(ReadError::Truncated);
            in.a = static_cast<ca::u8>(code[pos]);
            in.b = static_cast<ca::i8>(code[pos + 1]);
            pos += 2;
            break;
        }
        case Shape::CP1: {
            if (pos >= size) return Err(ReadError::Truncated);
            in.cp = static_cast<ca::u8>(code[pos]);
            pos += 1;
            break;
        }
        case Shape::CP2: {
            ca::u32 v = 0;
            if (!rdU2(code, pos, v)) return Err(ReadError::Truncated);
            in.cp = v;
            pos += 2;
            break;
        }
        case Shape::CP2Count: {
            if (pos + 4 > size) return Err(ReadError::Truncated);
            ca::u32 v = 0;
            if (!rdU2(code, pos, v)) return Err(ReadError::Truncated);
            in.cp = v;
            in.a  = static_cast<ca::u8>(code[pos + 2]);   // count
            pos += 4;                                     // count + zero
            break;
        }
        case Shape::CP2Zero2: {
            if (pos + 4 > size) return Err(ReadError::Truncated);
            ca::u32 v = 0;
            if (!rdU2(code, pos, v)) return Err(ReadError::Truncated);
            in.cp = v;
            pos += 4;   // 两个 u8 保留 0（编码原样写 0）
            break;
        }
        case Shape::CP2Dims: {
            if (pos + 3 > size) return Err(ReadError::Truncated);
            ca::u32 v = 0;
            if (!rdU2(code, pos, v)) return Err(ReadError::Truncated);
            in.cp = v;
            in.a  = static_cast<ca::u8>(code[pos + 2]);   // 维数
            pos += 3;
            break;
        }
        case Shape::Atype1: {
            if (pos >= size) return Err(ReadError::Truncated);
            in.a = static_cast<ca::u8>(code[pos]);
            pos += 1;
            break;
        }
        case Shape::Branch2: {
            ca::i32 v = 0;
            if (!rdS2(code, pos, v)) return Err(ReadError::Truncated);
            in.t = v;
            pos += 2;
            break;
        }
        case Shape::Branch4: {
            ca::i32 v = 0;
            if (!rdS4(code, pos, v)) return Err(ReadError::Truncated);
            in.t = v;
            pos += 4;
            break;
        }
        case Shape::TableSwitch: {
            // 0–3 字节 padding 对齐到 pc+1 之后的 4 字节边界（相对 code 起点）。
            const ca::usize pad = (4 - (pos & 3)) & 3;
            if (pos + pad > size) return Err(ReadError::Truncated);
            pos += pad;
            ca::i32 def = 0, low = 0, high = 0;
            if (!rdS4(code, pos, def) || !rdS4(code, pos + 4, low) ||
                !rdS4(code, pos + 8, high)) {
                return Err(ReadError::Truncated);
            }
            pos += 12;
            if (high < low) return Err(ReadError::UnsupportedFeature);
            const ca::u64 count =
                static_cast<ca::u64>(high) - static_cast<ca::u64>(low) + 1;
            // case 数受剩余字节约束；上限取宽松的 2^16，防畸形声明撑爆内存。
            if (count > 0x10000 || pos + count * 4 > size) {
                return Err(ReadError::Truncated);
            }
            in.t = def;
            in.a = low;
            in.b = high;
            in.offs.resize(static_cast<ca::usize>(count));
            for (ca::u64 i = 0; i < count; ++i) {
                ca::i32 off = 0;
                if (!rdS4(code, pos, off)) return Err(ReadError::Truncated);
                in.offs[static_cast<ca::usize>(i)] = off;
                pos += 4;
            }
            break;
        }
        case Shape::LookupSwitch: {
            const ca::usize pad = (4 - (pos & 3)) & 3;
            if (pos + pad > size) return Err(ReadError::Truncated);
            pos += pad;
            ca::i32 def = 0, npairs = 0;
            if (!rdS4(code, pos, def) || !rdS4(code, pos + 4, npairs)) {
                return Err(ReadError::Truncated);
            }
            pos += 8;
            if (npairs < 0 || pos + static_cast<ca::u64>(npairs) * 8 > size) {
                return Err(ReadError::Truncated);
            }
            in.t = def;
            in.cases.resize(static_cast<ca::usize>(npairs));
            for (ca::i32 i = 0; i < npairs; ++i) {
                ca::i32 key = 0, off = 0;
                if (!rdS4(code, pos, key) || !rdS4(code, pos + 4, off)) {
                    return Err(ReadError::Truncated);
                }
                in.cases[static_cast<ca::usize>(i)] = SwitchCase{key, off};
                pos += 8;
            }
            break;
        }
        }

        // 分支目标必须落在数组内（是否正好是某条指令的起点由 ir 层 import 校验，
        // 这里先保证不越界，避免后续 pc+off 溢出）。
        const bool isBranch = shapeOf(in.op) == Shape::Branch2 ||
                              shapeOf(in.op) == Shape::Branch4 ||
                              shapeOf(in.op) == Shape::TableSwitch ||
                              shapeOf(in.op) == Shape::LookupSwitch;
        if (isBranch) {
            const ca::i64 from = static_cast<ca::i64>(in.pc);
            const ca::i64 dst  = from + in.t;
            if (dst < 0 || dst > static_cast<ca::i64>(size)) {
                return Err(ReadError::UnsupportedFeature);
            }
        }

        out.push_back(std::move(in));
    }
    return Ok(std::move(out));
}

namespace {

void wrU1(std::vector<ca::u8>& v, ca::u32 u) noexcept
{
    v.push_back(static_cast<ca::u8>(u & 0xff));
}

void wrU2(std::vector<ca::u8>& v, ca::u32 u) noexcept
{
    v.push_back(static_cast<ca::u8>((u >> 8) & 0xff));
    v.push_back(static_cast<ca::u8>(u & 0xff));
}

void wrS2(std::vector<ca::u8>& v, ca::i32 s) noexcept
{
    wrU2(v, static_cast<ca::u32>(s) & 0xffff);
}

void wrS4(std::vector<ca::u8>& v, ca::i32 s) noexcept
{
    const ca::u32 u = static_cast<ca::u32>(s);
    v.push_back(static_cast<ca::u8>((u >> 24) & 0xff));
    v.push_back(static_cast<ca::u8>((u >> 16) & 0xff));
    v.push_back(static_cast<ca::u8>((u >> 8) & 0xff));
    v.push_back(static_cast<ca::u8>(u & 0xff));
}

/// 单条指令的编码长度（在已知 pc 下——switch 依赖 4 字节对齐 padding）。
ca::u32 insnSize(const RawInsn& in, ca::u32 pc) noexcept
{
    if (in.wide) {
        // wide 前缀 + 真实 opcode + u16 索引（iinc 再加 i16 常量）。
        return in.op == Op::IINC ? 6 : 4;
    }
    ca::u32 n = 1;
    switch (shapeOf(in.op)) {
    case Shape::None: break;
    case Shape::Imm1:
    case Shape::Local1:
    case Shape::CP1:
    case Shape::Atype1: n += 1; break;
    case Shape::Imm2:
    case Shape::CP2:
    case Shape::Branch2: n += 2; break;
    case Shape::Iinc1: n += 2; break;
    case Shape::Branch4: n += 4; break;
    case Shape::CP2Count:
    case Shape::CP2Zero2: n += 4; break;
    case Shape::CP2Dims: n += 3; break;
    case Shape::TableSwitch: {
        const ca::u32 pad    = (4 - ((pc + n) & 3)) & 3;
        const ca::u32 nCases = static_cast<ca::u32>(in.offs.size());
        n += pad + 12 + nCases * 4;
        break;
    }
    case Shape::LookupSwitch: {
        const ca::u32 pad    = (4 - ((pc + n) & 3)) & 3;
        const ca::u32 npairs = static_cast<ca::u32>(in.cases.size());
        n += pad + 8 + npairs * 8;
        break;
    }
    }
    return n;
}

void encodeOne(const RawInsn& in, std::vector<ca::u8>& v) noexcept
{
    if (in.wide) {
        wrU1(v, 0xc4);
        wrU1(v, static_cast<ca::u32>(static_cast<ca::u8>(in.op)));
        wrU2(v, static_cast<ca::u32>(in.a));
        if (in.op == Op::IINC) {
            wrS2(v, in.b);
        }
        return;
    }
    wrU1(v, static_cast<ca::u32>(static_cast<ca::u8>(in.op)));
    switch (shapeOf(in.op)) {
    case Shape::None: break;
    case Shape::Imm1:
    case Shape::Atype1: wrU1(v, static_cast<ca::u32>(in.a)); break;
    case Shape::Local1: wrU1(v, static_cast<ca::u32>(in.a)); break;
    case Shape::Imm2: wrS2(v, in.a); break;
    case Shape::Iinc1:
        wrU1(v, static_cast<ca::u32>(in.a));
        wrU1(v, static_cast<ca::u32>(in.b) & 0xff);
        break;
    case Shape::CP1: wrU1(v, in.cp); break;
    case Shape::CP2: wrU2(v, in.cp); break;
    case Shape::CP2Count:
        wrU2(v, in.cp);
        wrU1(v, static_cast<ca::u32>(in.a));
        wrU1(v, 0);
        break;
    case Shape::CP2Zero2:
        wrU2(v, in.cp);
        wrU1(v, 0);
        wrU1(v, 0);
        break;
    case Shape::CP2Dims:
        wrU2(v, in.cp);
        wrU1(v, static_cast<ca::u32>(in.a));
        break;
    case Shape::Branch2: wrS2(v, in.t); break;
    case Shape::Branch4: wrS4(v, in.t); break;
    case Shape::TableSwitch: {
        const ca::u32 pad = (4 - (static_cast<ca::u32>(v.size()) & 3)) & 3;
        for (ca::u32 i = 0; i < pad; ++i) {
            v.push_back(0);
        }
        wrS4(v, in.t);
        wrS4(v, in.a);
        wrS4(v, in.b);
        for (ca::i32 off : in.offs) {
            wrS4(v, off);
        }
        break;
    }
    case Shape::LookupSwitch: {
        const ca::u32 pad = (4 - (static_cast<ca::u32>(v.size()) & 3)) & 3;
        for (ca::u32 i = 0; i < pad; ++i) {
            v.push_back(0);
        }
        wrS4(v, in.t);
        wrS4(v, static_cast<ca::i32>(in.cases.size()));
        for (const SwitchCase& c : in.cases) {
            wrS4(v, c.key);
            wrS4(v, c.off);
        }
        break;
    }
    }
}

}   // namespace

ca::u32 rawInsnSize(const RawInsn& in, ca::u32 pc) noexcept
{
    return insnSize(in, pc);
}

Result<std::vector<ca::u8>, WriteError> encodeInsns(std::vector<RawInsn>& insns)
{
    // 第一遍：按顺序排布，重算 pc（switch 对齐依赖 pc，必须顺序推进）。
    ca::u32 pc = 0;
    for (RawInsn& in : insns) {
        in.pc = pc;
        pc += insnSize(in, pc);
    }

    // 操作数域宽校验：i16 放不下的分支偏移、u1 放不下的 LDC 索引都必须
    // 显式失败——静默截断会产出貌似合法但跳错位置 / 引用错条目的 class。
    // goto→goto_w、LDC→LDC_W 拓宽是 editor 的显式变换（loadConst 已自动选）；
    // 其余域宽同理：CP2 族超 u16、u8/i8/i16 操作数越界
    // 一律 Err，唯一的合法出路是显式拓宽（wide 前缀 / 宽形状指令）。
    for (const RawInsn& in : insns) {
        const Shape sh = shapeOf(in.op);
        if (!in.wide && sh == Shape::Branch2) {
            if (in.t < -32768 || in.t > 32767) {
                return Err(WriteError::Unencodable);
            }
        }
        if (sh == Shape::CP1 && in.cp > 0xFF) {
            return Err(WriteError::Unencodable);
        }
        if ((sh == Shape::CP2 || sh == Shape::CP2Count ||
             sh == Shape::CP2Zero2 || sh == Shape::CP2Dims) &&
            in.cp > 0xFFFF) {
            return Err(WriteError::Unencodable);
        }
        if (!in.wide) {
            // u8/i8/i16 立即数与索引域：越界即 Err，不静默截断。
            if ((sh == Shape::Local1 || sh == Shape::Atype1 ||
                 sh == Shape::CP2Count || sh == Shape::CP2Dims) &&
                (in.a < 0 || in.a > 0xFF)) {
                return Err(WriteError::Unencodable);
            }
            if (sh == Shape::Imm1 && (in.a < -128 || in.a > 127)) {
                return Err(WriteError::Unencodable);
            }
            if (sh == Shape::Imm2 && (in.a < -32768 || in.a > 32767)) {
                return Err(WriteError::Unencodable);
            }
            if (sh == Shape::Iinc1 &&
                (in.a < 0 || in.a > 0xFF || in.b < -128 || in.b > 127)) {
                return Err(WriteError::Unencodable);
            }
        }
        else {
            // wide 路径：局部变量索引 u16、iinc 常量 i16。
            if (in.a < 0 || in.a > 0xFFFF) {
                return Err(WriteError::Unencodable);
            }
            if (in.op == Op::IINC && (in.b < -32768 || in.b > 32767)) {
                return Err(WriteError::Unencodable);
            }
        }
    }

    std::vector<ca::u8> out;
    out.reserve(pc);
    for (const RawInsn& in : insns) {
        const ca::u32 before = static_cast<ca::u32>(out.size());
        encodeOne(in, out);
        if (out.size() - before != insnSize(in, in.pc)) {
            // 内部不一致（大小计算与实际编码分叉）——防御性检查。
            return Err(WriteError::Unencodable);
        }
    }
    return Ok(std::move(out));
}

}   // namespace jbc::core
