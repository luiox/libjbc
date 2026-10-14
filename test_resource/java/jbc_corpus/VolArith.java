// VolArith.java — libjbc 语料：int/long/float/double 全算术族 + 移位 + 位运算 + 数值转换 + 常量装载。
// 形态语料（无行为基准要求）：每个方法一条表达式，覆盖一个 opcode 形态。
package jbcx.corpus;

public final class VolArith {
    private VolArith() {
    }

    // ---- int 算术 ----
    static int iadd(int a, int b) { return a + b; }
    static int isub(int a, int b) { return a - b; }
    static int imul(int a, int b) { return a * b; }
    static int idiv(int a, int b) { return a / b; }
    static int irem(int a, int b) { return a % b; }
    static int ineg(int a) { return -a; }
    static int iinc(int a) { return a + 1; }
    static int idec(int a) { return a - 1; }
    static int iincBy(int a, int k) { return a + k; }
    static int icompound(int a, int b) { a += b; return a; }
    static int isubCompound(int a, int b) { a -= b; return a; }
    static int imulCompound(int a, int b) { a *= b; return a; }
    static int idivCompound(int a, int b) { a /= b; return a; }
    static int iremCompound(int a, int b) { a %= b; return a; }
    static int iand(int a, int b) { return a & b; }
    static int ior(int a, int b) { return a | b; }
    static int ixor(int a, int b) { return a ^ b; }
    static int ishl(int a, int n) { return a << n; }
    static int ishr(int a, int n) { return a >> n; }
    static int iushr(int a, int n) { return a >>> n; }
    static int iandC(int a) { return a & 0x0F0F; }
    static int iorC(int a) { return a | 0x0F0F; }
    static int ixorC(int a) { return a ^ 0x55AA5555; }
    static int ishlC(int a) { return a << 3; }
    static int ishrC(int a) { return a >> 3; }
    static int iushrC(int a) { return a >>> 3; }
    static int not(int a) { return ~a; }
    static int preInc(int a) { return ++a; }
    static int postInc(int a) { int r = a++; return r; }
    static int preDec(int a) { return --a; }
    static int chain1(int a, int b, int c) { return a + b * c - a / (b + 1); }
    static int chain2(int a, int b, int c) { return (a & b) | (c ^ a); }
    static int chain3(int a, int b) { return (a << 2) ^ (b >>> 1); }
    static int popCount(int v) {
        int c = 0;
        while (v != 0) { c += v & 1; v >>>= 1; }
        return c;
    }
    static int clamp255(int v) { return v > 255 ? 255 : (v < 0 ? 0 : v); }

    // ---- long 算术（cat2 槽位形态）----
    static long ladd(long a, long b) { return a + b; }
    static long lsub(long a, long b) { return a - b; }
    static long lmul(long a, long b) { return a * b; }
    static long ldiv(long a, long b) { return a / b; }
    static long lrem(long a, long b) { return a % b; }
    static long lneg(long a) { return -a; }
    static long land(long a, long b) { return a & b; }
    static long lor(long a, long b) { return a | b; }
    static long lxor(long a, long b) { return a ^ b; }
    static long lshl(long a, int n) { return a << n; }
    static long lshr(long a, int n) { return a >> n; }
    static long lushr(long a, int n) { return a >>> n; }
    static long landC(long a) { return a & 0xFF00FF00FF00FF00L; }
    static long lorC(long a) { return a | 0x00FF00FF00FF00FFL; }
    static long lxorC(long a) { return a ^ 0x1122334455667788L; }
    static long lshlC(long a) { return a << 7; }
    static long lshrC(long a) { return a >> 5; }
    static long lushrC(long a) { return a >>> 5; }
    static long lcmpToI(long a, long b) { return a < b ? -1 : (a > b ? 1 : 0); }
    static long lmix(long a, long b, long c) { return (a + b) * c - a % (b | 1); }
    static long lpow2(int n) { return 1L << n; }
    static long lcompound(long a, long b) { a += b; a ^= 0xFL; return a; }

    // ---- float 算术 ----
    static float fadd(float a, float b) { return a + b; }
    static float fsub(float a, float b) { return a - b; }
    static float fmul(float a, float b) { return a * b; }
    static float fdiv(float a, float b) { return a / b; }
    static float frem(float a, float b) { return a % b; }
    static float fneg(float a) { return -a; }
    static float fmix(float a, float b, float c) { return a * b + c / (a - b); }
    static float fcompound(float a, float b) { a += b; a *= 2.0f; return a; }
    static float favg(float a, float b) { return (a + b) / 2.0f; }
    static float fscale(float a, int k) { return a * k; }
    static float fconst0() { return 0.0f; }
    static float fconst1() { return 1.0f; }
    static float fconst2() { return 2.0f; }
    static float fldc() { return 3.14159f; }
    static float fldcBig() { return 1.0e10f; }
    static float fldcSmall() { return 1.0e-10f; }

    // ---- double 算术 ----
    static double dadd(double a, double b) { return a + b; }
    static double dsub(double a, double b) { return a - b; }
    static double dmul(double a, double b) { return a * b; }
    static double ddiv(double a, double b) { return a / b; }
    static double drem(double a, double b) { return a % b; }
    static double dneg(double a) { return -a; }
    static double dmix(double a, double b, double c) { return a * b - c / (a + b); }
    static double dcompound(double a, double b) { a += b; a /= 3.0; return a; }
    static double davg(double a, double b) { return (a + b) / 2.0; }
    static double dpow(double base, int n) {
        double r = 1.0;
        for (int i = 0; i < n; ++i) { r *= base; }
        return r;
    }
    static double dconst0() { return 0.0; }
    static double dconst1() { return 1.0; }
    static double dldc() { return 2.718281828459045; }
    static double dldcBig() { return 1.0e100; }
    static double dldcSmall() { return 4.9e-324; }

    // ---- 数值转换（i2x / l2x / f2x / d2x 全族）----
    static long i2l(int a) { return (long) a; }
    static float i2f(int a) { return (float) a; }
    static double i2d(int a) { return (double) a; }
    static byte i2b(int a) { return (byte) a; }
    static char i2c(int a) { return (char) a; }
    static short i2s(int a) { return (short) a; }
    static int l2i(long a) { return (int) a; }
    static float l2f(long a) { return (float) a; }
    static double l2d(long a) { return (double) a; }
    static int f2i(float a) { return (int) a; }
    static long f2l(float a) { return (long) a; }
    static double f2d(float a) { return (double) a; }
    static int d2i(double a) { return (int) a; }
    static long d2l(double a) { return (long) a; }
    static float d2f(double a) { return (float) a; }
    static int roundTrip(long a) { return (int) (long) (float) (double) a; }
    static byte narrow(int a) { return (byte) (short) (char) a; }

    // ---- 比较压栈（lcmp/fcmpg/fcmpl/dcmpg/dcmpl）----
    static int lcmpBr(long a, long b) { if (a == b) { return 0; } return a < b ? -1 : 1; }
    static int fcmpBr(float a, float b) { if (a == b) { return 0; } return a < b ? -1 : 1; }
    static int dcmpBr(double a, double b) { if (a == b) { return 0; } return a < b ? -1 : 1; }
    static int fcmpG(float a, float b) { return a > b ? 1 : 0; }
    static int fcmpL(float a, float b) { return a < b ? -1 : 0; }
    static int dcmpG(double a, double b) { return a > b ? 1 : 0; }
    static int dcmpL(double a, double b) { return a < b ? -1 : 0; }
    static boolean isNaNf(float a) { return a != a; }
    static boolean isNAND(double a) { return a != a; }

    // ---- 常量装载（iconst/bipush/sipush/ldc 族）----
    static int cM1() { return -1; }
    static int c0() { return 0; }
    static int c1() { return 1; }
    static int c2() { return 2; }
    static int c3() { return 3; }
    static int c4() { return 4; }
    static int c5() { return 5; }
    static int b127() { return 127; }
    static int bm128() { return -128; }
    static int s32767() { return 32767; }
    static int sm32768() { return -32768; }
    static int bigLdc() { return 100000; }
    static int minInt() { return Integer.MIN_VALUE; }
    static int maxInt() { return Integer.MAX_VALUE; }
    static long lMin() { return Long.MIN_VALUE; }
    static long lMax() { return Long.MAX_VALUE; }
    static long lBig() { return 9_000_000_000L; }
    static String sLdc() { return "corpus-string"; }
    static Class<?> cLdc() { return String.class; }
    static int intLdcHex() { return 0xDEADBEEF; }

    // ---- 混合表达式（多 opcode 协同）----
    static int mix1(int a, long l, float f) { return (int) (l + (long) (f * a)); }
    static long mix2(int a, double d) { return (long) d ^ (long) a; }
    static double mix3(long l, float f) { return (double) l * (double) f; }
    static float mix4(int a, long l) { return (float) a / (float) l; }
    static int bits1(int a, int b) { return ((a << 5) | (b >>> 3)) & ~(a ^ b); }
    static long bits2(long a, int n) { return (a << n) | (a >>> (64 - n)); }
    static int hash0(int a, int b, int c) { return (a * 31 + b) * 31 + c; }
    static int hash1(long a, Object o) { return (int) (a ^ (a >>> 32)) + o.hashCode(); }
    static int booleanToInt(boolean a, boolean b) { return (a ? 1 : 0) + (b ? 2 : 0); }
    static boolean xorBool(boolean a, boolean b) { return a ^ b; }
    static int triangle(int n) { return n * (n + 1) / 2; }
    static int gcd(int a, int b) {
        while (b != 0) { int t = b; b = a % b; a = t; }
        return a;
    }
    static long fib(int n) {
        long a = 0, b = 1;
        for (int i = 0; i < n; ++i) { long t = a + b; a = b; b = t; }
        return a;
    }
    static double mean(int[] xs) {
        double s = 0.0;
        for (int x : xs) { s += x; }
        return xs.length == 0 ? 0.0 : s / xs.length;
    }
    static int sat(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
}
