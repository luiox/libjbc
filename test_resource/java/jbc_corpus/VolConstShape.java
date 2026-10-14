// VolConstShape.java — libjbc 语料：静态常量族（CP Integer/Long/Float/Double/String/Class 全载荷）+ 静态初始化。
package jbcx.corpus;

public final class VolConstShape {
    static final int I_SMALL = 7;
    static final int I_BIG = 1_000_000_007;
    static final int I_NEG = -42;
    static final long L_SMALL = 9L;
    static final long L_BIG = 4_611_686_018_427_387_904L;
    static final float F_PI = 3.14159f;
    static final float F_INF = Float.POSITIVE_INFINITY;
    static final float F_NAN = Float.NaN;
    static final double D_E = 2.718281828459045;
    static final double D_MIN = Double.MIN_VALUE;
    static final String S_ASCII = "constant-ascii";
    static final String S_UTF8 = "常量中文-üñîçø∂é";
    static final String S_EMPTY = "";
    static final Class<?> C_STRING = String.class;
    static final Class<?> C_INT = int.class;
    static final Class<?> C_ARRAY = int[].class;
    static final int[] I_ARRAY = { 1, 2, 3 };
    static final String[] S_ARRAY = { "a", "b" };

    static int intShapes() { return I_SMALL + I_BIG + I_NEG; }
    static long longShapes() { return L_SMALL + L_BIG; }
    static float floatShapes() { return F_PI + F_INF + F_NAN; }
    static double doubleShapes() { return D_E + D_MIN; }
    static int stringShapes() { return S_ASCII.length() + S_UTF8.length() + S_EMPTY.length(); }
    static int classShapes() { return C_STRING.getName().length() + C_INT.getName().length() + C_ARRAY.getName().length(); }
    static int arrayConsts() { return I_ARRAY[0] + I_ARRAY[2] + S_ARRAY[0].length() + S_ARRAY[1].length(); }

    // ---- 静态初始化块（<clinit> 带控制流）----
    static int warm;
    static long checksum;
    static String derived;

    static {
        warm = I_SMALL * 2;
        checksum = L_SMALL + I_BIG;
        derived = S_ASCII + I_SMALL;
        if (checksum < 0) {
            throw new IllegalStateException("negative checksum");
        }
        for (int i = 0; i < 4; ++i) {
            warm += i;
            checksum += i;
        }
    }

    static int initShapes() { return warm + (int) checksum + derived.length(); }

    // ---- 实例常量 + final 字段（构造器赋值形态）----
    final int seed;
    final String tag;

    VolConstShape(int seed) {
        this.seed = seed;
        this.tag = "t" + seed;
    }

    int mix() { return seed + warm + tag.length() + intShapes(); }
    static VolConstShape make(int s) { return new VolConstShape(s % I_SMALL + 1); }

    // ---- 装箱缓存命中形态（Integer.valueOf 小值走缓存）----
    static Integer cachedBox(int v) { return v >= -128 && v <= 127 ? Integer.valueOf(v) : v; }
    static int boxShapes(int v) {
        Integer a = cachedBox(v);
        Integer b = Integer.valueOf(I_SMALL);
        return a.intValue() + b.intValue() + a.compareTo(b);
    }
}
