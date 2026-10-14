/// @file        opcode.cpp
/// @brief       码表元数据：形状、wide 修饰、助记符。
#include "jbc/core/opcode.hpp"

namespace jbc::core {

namespace {

/// 按 opcode 字节索引的助记符表（0x00–0xC9 共 202 项）。
const char* const kMnemonic[0xca] = {
    "nop",                   "aconst_null",       "iconst_m1",        "iconst_0",
    "iconst_1",              "iconst_2",          "iconst_3",         "iconst_4",
    "iconst_5",              "lconst_0",          "lconst_1",         "fconst_0",
    "fconst_1",              "fconst_2",          "dconst_0",         "dconst_1",
    "bipush",                "sipush",            "ldc",              "ldc_w",
    "ldc2_w",                "iload",             "lload",            "fload",
    "dload",                 "aload",             "iload_0",          "iload_1",
    "iload_2",               "iload_3",           "lload_0",          "lload_1",
    "lload_2",               "lload_3",           "fload_0",          "fload_1",
    "fload_2",               "fload_3",           "dload_0",          "dload_1",
    "dload_2",               "dload_3",           "aload_0",          "aload_1",
    "aload_2",               "aload_3",           "iaload",           "laload",
    "faload",                "daload",            "aaload",           "baload",
    "caload",                "saload",            "istore",           "lstore",
    "fstore",                "dstore",            "astore",           "istore_0",
    "istore_1",              "istore_2",          "istore_3",         "lstore_0",
    "lstore_1",              "lstore_2",          "lstore_3",         "fstore_0",
    "fstore_1",              "fstore_2",          "fstore_3",         "dstore_0",
    "dstore_1",              "dstore_2",          "dstore_3",         "astore_0",
    "astore_1",              "astore_2",          "astore_3",         "iastore",
    "lastore",               "fastore",           "dastore",          "aastore",
    "bastore",               "castore",           "sastore",          "pop",
    "pop2",                  "dup",               "dup_x1",           "dup_x2",
    "dup2",                  "dup2_x1",           "dup2_x2",          "swap",
    "iadd",                  "ladd",              "fadd",             "dadd",
    "isub",                  "lsub",              "fsub",             "dsub",
    "imul",                  "lmul",              "fmul",             "dmul",
    "idiv",                  "ldiv",              "fdiv",             "ddiv",
    "irem",                  "lrem",              "frem",             "drem",
    "ineg",                  "lneg",              "fneg",             "dneg",
    "ishl",                  "lshl",              "ishr",             "lshr",
    "iushr",                 "lushr",             "iand",             "land",
    "ior",                   "lor",               "ixor",             "lxor",
    "iinc",                  "i2l",               "i2f",              "i2d",
    "l2i",                   "l2f",               "l2d",              "f2i",
    "f2l",                   "f2d",               "d2i",              "d2l",
    "d2f",                   "i2b",               "i2c",              "i2s",
    "lcmp",                  "fcmpl",             "fcmpg",            "dcmpl",
    "dcmpg",                 "ifeq",              "ifne",             "iflt",
    "ifge",                  "ifgt",              "ifle",             "if_icmpeq",
    "if_icmpne",             "if_icmplt",         "if_icmpge",        "if_icmpgt",
    "if_icmple",             "if_acmpeq",         "if_acmpne",        "goto",
    "jsr",                   "ret",               "tableswitch",      "lookupswitch",
    "ireturn",               "lreturn",           "freturn",          "dreturn",
    "areturn",               "return",            "getstatic",        "putstatic",
    "getfield",              "putfield",          "invokevirtual",    "invokespecial",
    "invokestatic",          "invokeinterface",   "invokedynamic",    "new",
    "newarray",              "anewarray",         "arraylength",      "athrow",
    "checkcast",             "instanceof",        "monitorenter",     "monitorexit",
    "wide",                  "multianewarray",    "ifnull",           "ifnonnull",
    "goto_w",                "jsr_w",
};

}   // namespace

Shape shapeOf(Op op) noexcept
{
    switch (op) {
    case Op::BIPUSH: return Shape::Imm1;
    case Op::SIPUSH: return Shape::Imm2;
    case Op::LDC: return Shape::CP1;
    case Op::LDC_W:
    case Op::LDC2_W:
    case Op::GETSTATIC:
    case Op::PUTSTATIC:
    case Op::GETFIELD:
    case Op::PUTFIELD:
    case Op::INVOKEVIRTUAL:
    case Op::INVOKESPECIAL:
    case Op::INVOKESTATIC:
    case Op::NEW:
    case Op::ANEWARRAY:
    case Op::CHECKCAST:
    case Op::INSTANCEOF: return Shape::CP2;
    case Op::INVOKEINTERFACE: return Shape::CP2Count;
    case Op::INVOKEDYNAMIC: return Shape::CP2Zero2;
    case Op::MULTIANEWARRAY: return Shape::CP2Dims;
    case Op::NEWARRAY: return Shape::Atype1;
    case Op::IINC: return Shape::Iinc1;
    case Op::IFEQ:
    case Op::IFNE:
    case Op::IFLT:
    case Op::IFGE:
    case Op::IFGT:
    case Op::IFLE:
    case Op::IF_ICMPEQ:
    case Op::IF_ICMPNE:
    case Op::IF_ICMPLT:
    case Op::IF_ICMPGE:
    case Op::IF_ICMPGT:
    case Op::IF_ICMPLE:
    case Op::IF_ACMPEQ:
    case Op::IF_ACMPNE:
    case Op::IFNULL:
    case Op::IFNONNULL:
    case Op::GOTO:
    case Op::JSR: return Shape::Branch2;
    case Op::GOTO_W:
    case Op::JSR_W: return Shape::Branch4;
    case Op::TABLESWITCH: return Shape::TableSwitch;
    case Op::LOOKUPSWITCH: return Shape::LookupSwitch;
    // 局部变量访问族（wide 修饰前后同一形状入口，WideLocal/WideIinc 由解码器
    // 在展开 wide 前缀时改写）。
    case Op::ILOAD:
    case Op::LLOAD:
    case Op::FLOAD:
    case Op::DLOAD:
    case Op::ALOAD:
    case Op::ISTORE:
    case Op::LSTORE:
    case Op::FSTORE:
    case Op::DSTORE:
    case Op::ASTORE:
    case Op::RET: return Shape::Local1;
    default: return Shape::None;
    }
}

bool isWideLocalOp(Op op) noexcept
{
    switch (op) {
    case Op::ILOAD:
    case Op::LLOAD:
    case Op::FLOAD:
    case Op::DLOAD:
    case Op::ALOAD:
    case Op::ISTORE:
    case Op::LSTORE:
    case Op::FSTORE:
    case Op::DSTORE:
    case Op::ASTORE:
    case Op::RET:
    case Op::IINC: return true;
    default: return false;
    }
}

const char* mnemonicOf(Op op) noexcept
{
    const ca::u8 v = static_cast<ca::u8>(op);
    if (v < 0xca) {
        return kMnemonic[v];
    }
    return "<reserved>";
}

}   // namespace jbc::core
