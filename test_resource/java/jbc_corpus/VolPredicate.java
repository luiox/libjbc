// VolPredicate.java — libjbc 语料：谓词/比较一出行（条件跳转全族 + 装箱比较 + 边界形状）。
package jbcx.corpus;

public final class VolPredicate {
    private VolPredicate() {
    }

    static boolean p01(int a) { return a > 0; }
    static boolean p02(int a) { return a >= 0; }
    static boolean p03(int a) { return a < 0; }
    static boolean p04(int a) { return a <= 0; }
    static boolean p05(int a) { return a == 0; }
    static boolean p06(int a) { return a != 0; }
    static boolean p07(int a, int b) { return a == b; }
    static boolean p08(int a, int b) { return a != b; }
    static boolean p09(int a, int b) { return a < b; }
    static boolean p10(int a, int b) { return a <= b; }
    static boolean p11(int a, int b) { return a > b; }
    static boolean p12(int a, int b) { return a >= b; }
    static boolean p13(long a, long b) { return a == b; }
    static boolean p14(long a, long b) { return a < b; }
    static boolean p15(long a, long b) { return a > b; }
    static boolean p16(float a, float b) { return a == b; }
    static boolean p17(float a, float b) { return a < b; }
    static boolean p18(float a, float b) { return a > b; }
    static boolean p19(double a, double b) { return a == b; }
    static boolean p20(double a, double b) { return a < b; }
    static boolean p21(double a, double b) { return a > b; }
    static boolean p22(Object a) { return a == null; }
    static boolean p23(Object a) { return a != null; }
    static boolean p24(Object a, Object b) { return a == b; }
    static boolean p25(Object a, Object b) { return a != b; }
    static boolean p26(Object a, Object b) { return a.equals(b); }
    static boolean p27(String a, String b) { return a.equals(b); }
    static boolean p28(String a) { return a.isEmpty(); }
    static boolean p29(String a) { return a.length() > 3; }
    static boolean p30(String a, String b) { return a.length() == b.length(); }
    static boolean p31(int[] a, int v) { return a.length > v; }
    static boolean p32(int[] a) { return a.length == 0; }
    static boolean p33(Integer a, Integer b) { return a.intValue() == b.intValue(); }
    static boolean p34(Integer a, int b) { return a == b; }
    static boolean p35(Long a, long b) { return a == b; }
    static boolean p36(Character c) { return Character.isDigit(c); }
    static boolean p37(Boolean a, Boolean b) { return a && b; }
    static boolean p38(byte a) { return a >= 0; }
    static boolean p39(short a) { return a < 0; }
    static boolean p40(char c) { return c >= 'a' && c <= 'z'; }
    static boolean p41(char c) { return c == ' ' || c == '\t'; }
    static boolean p42(int a, int b, int c) { return a < b && b < c; }
    static boolean p43(int a, int b, int c) { return a == b || b == c || a == c; }
    static boolean p44(int a) { return (a & 1) == 0; }
    static boolean p45(int a) { return (a & 0xF) == 0xF; }
    static boolean p46(int a) { return (a % 3) == 0; }
    static boolean p47(int a) { return (a % 2) != 0 && a > 100; }
    static boolean p48(long a) { return a > Integer.MAX_VALUE; }
    static boolean p49(long a) { return a < Integer.MIN_VALUE || a > Integer.MAX_VALUE; }
    static boolean p50(double a) { return a > -1.0 && a < 1.0; }
    static boolean p51(int lo, int v, int hi) { return lo <= v && v <= hi; }
    static boolean p52(int lo, int v, int hi) { return v < lo || v > hi; }
    static boolean p53(String[] xs) { return xs == null || xs.length == 0; }
    static boolean p54(Object[] xs, int i) { return i >= 0 && i < xs.length; }
    static boolean p55(long a, long b, long c) { return a + b == c; }
    static boolean p56(int a) { return a == Integer.MIN_VALUE; }
    static boolean p57(int a) { return a + 1 > a; }
    static boolean p58(double a) { return a == (long) a; }
    static boolean p59(float a) { return a == (int) a; }
    static boolean p60(Comparable<Integer> c, int v) { return c.compareTo(v) >= 0; }

    // ---- 谓词组合体（多级短路）----
    static boolean c01(int a, int b) { return a > 0 && b > 0 && a != b; }
    static boolean c02(int a, int b) { return a > 0 || b > 0 || a == b; }
    static boolean c03(Object a, Object b) { return a != null && b != null && a.equals(b); }
    static boolean c04(String s) { return s != null && !s.isEmpty() && s.length() < 64; }
    static boolean c05(int[] a) { return a != null && a.length > 0 && a[0] > 0; }
    static boolean c06(int a, int b) { return !(a > b) && !(b < 0); }
    static boolean c07(boolean x, boolean y, boolean z) { return (x && y) || (y && z) || (x && z); }
    static boolean c08(int a) { return a > 1 << 20 || a < -(1 << 20); }
    static boolean c09(long a, long b) { return (a ^ b) < 0; }
    static boolean c10(int a, int b) { return (a & b) == a; }
    static int i01(boolean a, boolean b) { return a == b ? 0 : (a ? 1 : -1); }
    static int i02(int a, int b) { return a < b ? -1 : (a == b ? 0 : 1); }
    static int i03(Object a, Object b) { return a == null ? (b == null ? 0 : 1) : (b == null ? -1 : 2); }
    static int i04(String a) { return a == null ? 0 : a.length(); }
    static int i05(int[] a) { return a == null ? -1 : a.length; }
    static int i06(Integer a) { return a == null ? 0 : a; }
    static long l01(Integer a, Long b) { return a == null ? 0L : b == null ? (long) a : a + b; }
    static double d01(Number n) { return n == null ? 0.0 : n.doubleValue(); }
}
