// VolAdapter.java — libjbc 语料：委托/适配器/DTO 形态（大量小方法：转发/装箱/边界/格式化）。
package jbcx.corpus;

public final class VolAdapter {
    private final int value;

    public VolAdapter(int value) { this.value = value; }

    public int get() { return value; }

    // ---- 转发形态（一层委托）----
    int toAdd(VolAdapter o) { return value + o.value; }
    int toSub(VolAdapter o) { return value - o.value; }
    int toMul(VolAdapter o) { return value * o.value; }
    int toMin(VolAdapter o) { return Math.min(value, o.value); }
    int toMax(VolAdapter o) { return Math.max(value, o.value); }
    boolean toEq(VolAdapter o) { return value == o.value; }
    int toCmp(VolAdapter o) { return Integer.compare(value, o.value); }
    VolAdapter toNext() { return new VolAdapter(value + 1); }
    VolAdapter toPrev() { return value > Integer.MIN_VALUE ? new VolAdapter(value - 1) : this; }
    static VolAdapter of(int v) { return new VolAdapter(v); }
    static VolAdapter parse(String s) { return new VolAdapter(Integer.parseInt(s)); }
    static VolAdapter zero() { return new VolAdapter(0); }
    static VolAdapter one() { return new VolAdapter(1); }
    static VolAdapter[] range(int n) {
        VolAdapter[] r = new VolAdapter[n];
        for (int i = 0; i < n; ++i) { r[i] = new VolAdapter(i); }
        return r;
    }
    static int sumOf(VolAdapter[] xs) {
        int s = 0;
        for (VolAdapter x : xs) { s += x.value; }
        return s;
    }

    // ---- 边界/钳制家族 ----
    int clamp0_255() { return value < 0 ? 0 : (value > 255 ? 255 : value); }
    int clamp0_100() { return value < 0 ? 0 : (value > 100 ? 100 : value); }
    int clampNeg() { return Math.max(value, 0); }
    int clampPos() { return Math.min(value, 0); }
    int satAdd(VolAdapter o) {
        long r = (long) value + o.value;
        return r > Integer.MAX_VALUE ? Integer.MAX_VALUE : (r < Integer.MIN_VALUE ? Integer.MIN_VALUE : (int) r);
    }
    int satSub(VolAdapter o) {
        long r = (long) value - o.value;
        return r > Integer.MAX_VALUE ? Integer.MAX_VALUE : (r < Integer.MIN_VALUE ? Integer.MIN_VALUE : (int) r);
    }
    int absValue() { return value < 0 ? -value : value; }
    int signValue() { return Integer.signum(value); }
    int align4() { return (value + 3) & ~3; }
    int align16() { return (value + 15) & ~15; }
    boolean isPow2() { return value > 0 && (value & (value - 1)) == 0; }
    int nextPow2() {
        int v = value - 1;
        v |= v >> 1;
        v |= v >> 2;
        v |= v >> 4;
        v |= v >> 8;
        v |= v >> 16;
        return v + 1;
    }
    int divCeil(VolAdapter o) { return o.value == 0 ? 0 : (value + o.value - 1) / o.value; }
    int mod(VolAdapter o) { return o.value == 0 ? 0 : ((value % o.value) + o.value) % o.value; }

    // ---- 装箱/类型转换家族 ----
    Integer boxed() { return value; }
    Long asLong() { return (long) value; }
    Double asDouble() { return (double) value; }
    String asText() { return Integer.toString(value); }
    String asHex() { return "0x" + Integer.toHexString(value); }
    String asBin() { return Integer.toBinaryString(value); }
    String asOct() { return Integer.toOctalString(value); }
    byte asByte() { return (byte) value; }
    short asShort() { return (short) value; }
    char asChar() { return (char) value; }
    float asFloat() { return (float) value; }
    boolean asBool() { return value != 0; }
    static int fromBoxed(Integer v) { return v == null ? 0 : v; }
    static int fromBool(boolean b) { return b ? 1 : 0; }
    static int fromChar(char c) { return c; }
    static int fromBytes(byte hi, byte lo) { return (hi << 8) | (lo & 0xFF); }

    // ---- 格式化/组合家族 ----
    String describe() { return "VolAdapter{" + value + "}"; }
    String pairWith(VolAdapter o) { return value + ":" + o.value; }
    String tripleWith(VolAdapter a, VolAdapter b) { return value + ":" + a.value + ":" + b.value; }
    String repeat(int n) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < n; ++i) { sb.append(value); }
        return sb.toString();
    }
    static String joinAdapters(VolAdapter[] xs, String sep) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < xs.length; ++i) {
            if (i > 0) { sb.append(sep); }
            sb.append(xs[i].value);
        }
        return sb.toString();
    }
    static VolAdapter maxOfAll(VolAdapter[] xs) {
        VolAdapter best = xs[0];
        for (VolAdapter x : xs) { if (x.value > best.value) { best = x; } }
        return best;
    }
    static VolAdapter minOfAll(VolAdapter[] xs) {
        VolAdapter best = xs[0];
        for (VolAdapter x : xs) { if (x.value < best.value) { best = x; } }
        return best;
    }
    static int chain(VolAdapter a, VolAdapter b, VolAdapter c) {
        int added = a.toNext().toAdd(b);
        return c == null ? added : added * c.value;
    }

    // ---- 覆写三件套（equals/hashCode/toString 协同）----
    @Override public int hashCode() { return value ^ (value << 8); }
    @Override public boolean equals(Object o) {
        if (this == o) { return true; }
        if (!(o instanceof VolAdapter)) { return false; }
        return ((VolAdapter) o).value == value;
    }
    @Override public String toString() { return Integer.toString(value); }

    // ---- Comparable + 排序协同 ----
    public int compareTo(VolAdapter o) { return Integer.compare(value, o.value); }
    static void sortAll(VolAdapter[] xs) {
        for (int i = 1; i < xs.length; ++i) {
            VolAdapter key = xs[i];
            int j = i - 1;
            while (j >= 0 && xs[j].value > key.value) {
                xs[j + 1] = xs[j];
                --j;
            }
            xs[j + 1] = key;
        }
    }
    static boolean isSorted(VolAdapter[] xs) {
        for (int i = 1; i < xs.length; ++i) {
            if (xs[i - 1].value > xs[i].value) { return false; }
        }
        return true;
    }
    static VolAdapter median3(VolAdapter a, VolAdapter b, VolAdapter c) {
        return a.toCmp(b) > 0
            ? (b.toCmp(c) > 0 ? b : (a.toCmp(c) > 0 ? c : a))
            : (a.toCmp(c) > 0 ? a : (b.toCmp(c) > 0 ? c : b));
    }
}
