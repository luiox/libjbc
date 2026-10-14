// VolField.java — libjbc 语料：字段访问全族（getfield/putfield/getstatic/putstatic + 常量字段 + 复合赋值）。
package jbcx.corpus;

public final class VolField {
    private int f0;
    private int f1;
    private int f2;
    private int f3;
    private int f4;
    private int f5;
    private int f6;
    private int f7;
    private int f8;
    private int f9;
    private long g0;
    private long g1;
    private long g2;
    private long g3;
    private double d0;
    private double d1;
    private double d2;
    private float q0;
    private float q1;
    private Object r0;
    private Object r1;
    private String s0;
    private String s1;
    private int[] a0;
    private boolean z0;
    private byte b0;
    private char c0;
    private short w0;

    static int sf0;
    static long sg0;
    static double sd0;
    static String ss0 = "static-init";
    static final int K0 = 11;
    static final long K1 = 22L;
    static final String K2 = "const-string";
    static final double K3 = 0.5;

    int getF0() { return f0; }
    void setF0(int v) { f0 = v; }
    int getF1() { return f1; }
    void setF1(int v) { f1 = v; }
    int getF2() { return f2; }
    void setF2(int v) { f2 = v; }
    int getF3() { return f3; }
    void setF3(int v) { f3 = v; }
    int getF4() { return f4; }
    void setF4(int v) { f4 = v; }
    int getF5() { return f5; }
    void setF5(int v) { f5 = v; }
    int getF6() { return f6; }
    void setF6(int v) { f6 = v; }
    int getF7() { return f7; }
    void setF7(int v) { f7 = v; }
    int getF8() { return f8; }
    void setF8(int v) { f8 = v; }
    int getF9() { return f9; }
    void setF9(int v) { f9 = v; }

    long getG0() { return g0; }
    void setG0(long v) { g0 = v; }
    long getG1() { return g1; }
    void setG1(long v) { g1 = v; }
    long getG2() { return g2; }
    void setG2(long v) { g2 = v; }
    long getG3() { return g3; }
    void setG3(long v) { g3 = v; }

    double getD0() { return d0; }
    void setD0(double v) { d0 = v; }
    double getD1() { return d1; }
    void setD1(double v) { d1 = v; }
    double getD2() { return d2; }
    void setD2(double v) { d2 = v; }

    float getQ0() { return q0; }
    void setQ0(float v) { q0 = v; }
    float getQ1() { return q1; }
    void setQ1(float v) { q1 = v; }

    Object getR0() { return r0; }
    void setR0(Object v) { r0 = v; }
    Object getR1() { return r1; }
    void setR1(Object v) { r1 = v; }

    String getS0() { return s0; }
    void setS0(String v) { s0 = v; }
    String getS1() { return s1; }
    void setS1(String v) { s1 = v; }

    int[] getA0() { return a0; }
    void setA0(int[] v) { a0 = v; }
    boolean isZ0() { return z0; }
    void setZ0(boolean v) { z0 = v; }
    byte getB0() { return b0; }
    void setB0(byte v) { b0 = v; }
    char getC0() { return c0; }
    void setC0(char v) { c0 = v; }
    short getW0() { return w0; }
    void setW0(short v) { w0 = v; }

    static int getSf0() { return sf0; }
    static void setSf0(int v) { sf0 = v; }
    static long getSg0() { return sg0; }
    static void setSg0(long v) { sg0 = v; }
    static double getSd0() { return sd0; }
    static void setSd0(double v) { sd0 = v; }
    static String getSs0() { return ss0; }
    static void setSs0(String v) { ss0 = v; }

    // ---- 复合赋值 / 自增（get+put 协同）----
    int bumpF0() { return ++f0; }
    int bumpF1By(int k) { f1 += k; return f1; }
    int scaleF2(int k) { f2 *= k; return f2; }
    long widenG0() { return ++g0; }
    double halfD0() { return d0 /= 2.0; }
    String tieS0(String v) { return s0 += v; }
    int toggleZ0() { z0 = !z0; return z0 ? 1 : 0; }
    int swap01() { int t = f0; f0 = f1; f1 = t; return t; }
    int sumAll() { return f0 + f1 + f2 + f3 + f4 + f5 + f6 + f7 + f8 + f9; }
    long sumG() { return g0 + g1 + g2 + g3; }
    double sumD() { return d0 + d1 + d2 + q0 + q1; }
    int sumK() { return K0 + (int) K1 + K2.length() + (int) K3; }
    static int staticSum() { return sf0 + (int) sg0 + (int) sd0 + ss0.length(); }
    static int constFoldable() { return K0 * 2 + (int) (K1 * 2); }

    // ---- 对象链上的字段访问（null 检查形态）----
    int copyFrom(VolField o) {
        if (o == null) { return -1; }
        this.f0 = o.f0;
        this.f1 = o.f1;
        this.g0 = o.g0;
        this.d0 = o.d0;
        this.s0 = o.s0;
        return 0;
    }
    static int transfer(VolField from, VolField to) {
        if (from == null || to == null) { return -1; }
        to.r0 = from.r0;
        to.r1 = from.r1;
        to.a0 = from.a0;
        from.r0 = null;
        from.r1 = null;
        from.a0 = null;
        return 0;
    }
    int arrayInit(int n) {
        this.a0 = new int[n];
        for (int i = 0; i < n; ++i) { this.a0[i] = i * i; }
        return this.a0.length;
    }
    static void resetStatic() {
        sf0 = 0;
        sg0 = 0L;
        sd0 = 0.0;
        ss0 = "";
    }

    static {
        sf0 = K0;
        sg0 = K1;
        sd0 = K3;
    }
}
