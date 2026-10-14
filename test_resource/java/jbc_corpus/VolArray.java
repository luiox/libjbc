// VolArray.java — libjbc 语料：数组全族（newarray 全 atype / anewarray / multianewarray / 载荷存储 / arraylength）。
package jbcx.corpus;

public final class VolArray {
    private VolArray() {
    }

    // ---- 一维创建（newarray 全类型 + anewarray）----
    static int[] ints(int n) { return new int[n]; }
    static long[] longs(int n) { return new long[n]; }
    static float[] floats(int n) { return new float[n]; }
    static double[] doubles(int n) { return new double[n]; }
    static boolean[] bools(int n) { return new boolean[n]; }
    static byte[] bytes(int n) { return new byte[n]; }
    static char[] chars(int n) { return new char[n]; }
    static short[] shorts(int n) { return new short[n]; }
    static Object[] objs(int n) { return new Object[n]; }
    static String[] strings(int n) { return new String[n]; }
    static int[][] grid(int n) { return new int[n][n]; }
    static int[][][] cube(int n) { return new int[n][n][n]; }
    static Object[][][] deep3(int n) { return new Object[n][n + 1][n + 2]; }
    static int[][] ragged(int n) {
        int[][] r = new int[n][];
        for (int i = 0; i < n; ++i) { r[i] = new int[i + 1]; }
        return r;
    }
    static int[] literal() { return new int[] { 1, 2, 3, 4, 5 }; }
    static String[] strLiteral() { return new String[] { "a", "b", "c" }; }
    static long[] longLiteral() { return new long[] { 1L, 2L, 1L << 40 }; }
    static double[] dblLiteral() { return new double[] { 0.5, 1.5, 2.5 }; }

    // ---- 载荷/存储（xaload / xastore 全族）----
    static int iAt(int[] a, int i) { return a[i]; }
    static long lAt(long[] a, int i) { return a[i]; }
    static float fAt(float[] a, int i) { return a[i]; }
    static double dAt(double[] a, int i) { return a[i]; }
    static boolean zAt(boolean[] a, int i) { return a[i]; }
    static byte bAt(byte[] a, int i) { return a[i]; }
    static char cAt(char[] a, int i) { return a[i]; }
    static short sAt(short[] a, int i) { return a[i]; }
    static Object oAt(Object[] a, int i) { return a[i]; }
    static void iPut(int[] a, int i, int v) { a[i] = v; }
    static void lPut(long[] a, int i, long v) { a[i] = v; }
    static void fPut(float[] a, int i, float v) { a[i] = v; }
    static void dPut(double[] a, int i, double v) { a[i] = v; }
    static void bPut(byte[] a, int i, byte v) { a[i] = v; }
    static void cPut(char[] a, int i, char v) { a[i] = v; }
    static void oPut(Object[] a, int i, Object v) { a[i] = v; }
    static int bump(int[] a, int i) { return ++a[i]; }
    static int compoundAdd(int[] a, int i, int v) { a[i] += v; return a[i]; }
    static int compoundMul(int[] a, int i, int v) { a[i] *= v; return a[i]; }
    static int swap(int[] a, int i, int j) { int t = a[i]; a[i] = a[j]; a[j] = t; return t; }

    // ---- arraylength / 边界 ----
    static int len(int[] a) { return a.length; }
    static int len2(int[][] a) { return a.length + a[0].length; }
    static int lenStr(String s) { return s.toCharArray().length; }
    static int safeAt(int[] a, int i) {
        try {
            return a[i];
        } catch (ArrayIndexOutOfBoundsException e) {
            return -1;
        }
    }
    static int lastOf(int[] a) { return a[a.length - 1]; }

    // ---- 数组循环 / 复制 / 填充 ----
    static int sum(int[] a) {
        int s = 0;
        for (int v : a) { s += v; }
        return s;
    }
    static long sumL(long[] a) {
        long s = 0;
        for (long v : a) { s += v; }
        return s;
    }
    static double sumD(double[] a) {
        double s = 0.0;
        for (double v : a) { s += v; }
        return s;
    }
    static int sumGrid(int[][] g) {
        int s = 0;
        for (int[] row : g) {
            for (int v : row) { s += v; }
        }
        return s;
    }
    static void fill(int[] a, int v) {
        for (int i = 0; i < a.length; ++i) { a[i] = v; }
    }
    static int[] iota(int n) {
        int[] a = new int[n];
        for (int i = 0; i < n; ++i) { a[i] = i; }
        return a;
    }
    static int[] copy(int[] a) {
        int[] b = new int[a.length];
        for (int i = 0; i < a.length; ++i) { b[i] = a[i]; }
        return b;
    }
    static int[] sysCopy(int[] a) {
        int[] b = new int[a.length];
        System.arraycopy(a, 0, b, 0, a.length);
        return b;
    }
    static int[] cloneIt(int[] a) { return a.clone(); }
    static int[] grow(int[] a, int extra) {
        int[] b = new int[a.length + extra];
        System.arraycopy(a, 0, b, 0, a.length);
        return b;
    }
    static int[] concat(int[] a, int[] b) {
        int[] r = new int[a.length + b.length];
        System.arraycopy(a, 0, r, 0, a.length);
        System.arraycopy(b, 0, r, a.length, b.length);
        return r;
    }
    static int maxOf(int[] a) {
        int m = a[0];
        for (int i = 1; i < a.length; ++i) { if (a[i] > m) { m = a[i]; } }
        return m;
    }
    static int indexOf(int[] a, int v) {
        for (int i = 0; i < a.length; ++i) { if (a[i] == v) { return i; } }
        return -1;
    }
    static boolean eq(int[] a, int[] b) { return java.util.Arrays.equals(a, b); }
    static String show(int[] a) { return java.util.Arrays.toString(a); }
    static void sortIt(int[] a) { java.util.Arrays.sort(a); }

    // ---- 二维下标 / 多维访问 ----
    static int at2(int[][] g, int i, int j) { return g[i][j]; }
    static void put2(int[][] g, int i, int j, int v) { g[i][j] = v; }
    static int at3(int[][][] c, int i, int j, int k) { return c[i][j][k]; }
    static int diagSum(int[][] g) {
        int s = 0;
        for (int i = 0; i < g.length; ++i) { s += g[i][i]; }
        return s;
    }
    static int[][] transpose(int[][] g) {
        int[][] r = new int[g[0].length][g.length];
        for (int i = 0; i < g.length; ++i) {
            for (int j = 0; j < g[0].length; ++j) { r[j][i] = g[i][j]; }
        }
        return r;
    }
    static Object[] boxed(int[] a) {
        Object[] r = new Object[a.length];
        for (int i = 0; i < a.length; ++i) { r[i] = a[i]; }
        return r;
    }
    static int[] unbox(Object[] a) {
        int[] r = new int[a.length];
        for (int i = 0; i < a.length; ++i) { r[i] = (Integer) a[i]; }
        return r;
    }
    static int argsLen(String[] args) { return args == null ? 0 : args.length; }
    static char[] fromString(String s) { return s.toCharArray(); }
    static String byteString(byte[] bs) { return new String(bs, 0, bs.length); }
}
