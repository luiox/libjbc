// VolObject.java — libjbc 语料：instanceof/checkcast/monitor/StringBuilder/Object 方法族。
package jbcx.corpus;

public final class VolObject {
    private final int id;
    private Object peer;

    public VolObject(int id) { this.id = id; }

    public int id() { return id; }

    @Override public int hashCode() { return id * 31 + (peer == null ? 0 : peer.hashCode()); }

    @Override public boolean equals(Object o) {
        if (this == o) { return true; }
        if (!(o instanceof VolObject)) { return false; }
        return ((VolObject) o).id == this.id;
    }

    @Override public String toString() { return "VolObject(" + id + ")"; }

    // ---- instanceof / checkcast 全形态 ----
    static boolean isObj(Object o) { return o instanceof Object; }
    static boolean isStr(Object o) { return o instanceof String; }
    static boolean isNum(Object o) { return o instanceof Number; }
    static boolean isInt(Number n) { return n instanceof Integer; }
    static boolean isArr(Object o) { return o instanceof int[]; }
    static boolean isArr2(Object o) { return o instanceof Object[][]; }
    static boolean notNull(Object o) { return o != null && o instanceof Runnable; }
    static int asStr(Object o) { return ((String) o).length(); }
    static int asStrSafe(Object o) { return o instanceof String ? ((String) o).length() : -1; }
    static int asNum(Object o) { return ((Number) o).intValue(); }
    static int asNumSafe(Object o) {
        if (!(o instanceof Number)) { return 0; }
        Number n = (Number) o;
        return n.intValue() + n.hashCode();
    }
    static int downUp(Number n) { return (int) (double) (Double) n; }
    static int chainCast(Object o) {
        if (o instanceof String) { return ((String) o).length(); }
        if (o instanceof Number) { return ((Number) o).intValue(); }
        if (o instanceof CharSequence) { return ((CharSequence) o).charAt(0); }
        return -1;
    }
    static Class<?> classOf(Object o) { return o.getClass(); }
    static boolean sameClass(Object a, Object b) { return a.getClass() == b.getClass(); }
    static String typeName(Object o) { return o.getClass().getSimpleName(); }

    // ---- monitor（synchronized 块）----
    private final Object lock = new Object();
    private int counter;

    int syncInc() {
        synchronized (lock) {
            return ++counter;
        }
    }
    int syncRead() {
        synchronized (lock) {
            return counter;
        }
    }
    int nestedSync() {
        synchronized (this) {
            synchronized (lock) {
                counter += 2;
                return counter;
            }
        }
    }
    static int syncStatic() {
        synchronized (VolObject.class) {
            return 1;
        }
    }
    int syncLoop(int n) {
        int s = 0;
        for (int i = 0; i < n; ++i) {
            synchronized (lock) { s += i; }
        }
        return s;
    }
    synchronized int syncMethod() { return ++counter; }
    static synchronized int syncStaticMethod() { return 2; }

    // ---- StringBuilder / 字符串拼接（release 8 → StringBuilder 调用链）----
    static String concat2(String a, String b) { return a + b; }
    static String concat3(String a, int b, Object c) { return a + b + c; }
    static String concatWide(long a, double b, boolean c, char d) { return "" + a + b + c + d; }
    static String buildLoop(int n) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < n; ++i) { sb.append(i).append(','); }
        return sb.toString();
    }
    static String buildRev(String s) { return new StringBuilder(s).reverse().toString(); }
    static int sbLen(String s) { return new StringBuilder(s).length() + s.length(); }
    static String joinBy(String sep, String a, String b, String c) {
        StringBuilder sb = new StringBuilder(a);
        sb.append(sep).append(b).append(sep).append(c);
        return sb.toString();
    }

    // ---- Object / 包装类方法 ----
    static int iHashCode(Object o) { return System.identityHashCode(o); }
    static String intToString(int v) { return Integer.toString(v); }
    static String intToHex(int v) { return Integer.toHexString(v); }
    static Integer box(int v) { return Integer.valueOf(v); }
    static int unbox(Integer v) { return v.intValue(); }
    static int cmpBoxed(Integer a, Integer b) { return a.compareTo(b); }
    static boolean boxedEq(Integer a, Integer b) { return a.equals(b); }
    static Long parseLong(String s) { return Long.valueOf(s); }
    static String boolStr(boolean b) { return Boolean.toString(b); }
    static Object pick(int k) {
        switch (k & 3) {
        case 0: return "str";
        case 1: return Integer.valueOf(1);
        case 2: return Double.valueOf(1.0);
        default: return new Object();
        }
    }
    static int describe(Object o) {
        String t = o == null ? "null" : o.getClass().getName();
        return t.length() + o.hashCode();
    }
    static int fieldPeer(VolObject v, Object p) {
        v.peer = p;
        return v.peer == null ? 0 : 1;
    }

    // ---- throw 形态（athrow）----
    static int requirePositive(int v) {
        if (v <= 0) { throw new IllegalArgumentException("v=" + v); }
        return v;
    }
    static int rethrow(IOExceptionShape s) {
        try {
            return s.code();
        } catch (IllegalStateException e) {
            throw e;
        }
    }
    static RuntimeException todo(String why) { return new UnsupportedOperationException(why); }

    static final class IOExceptionShape extends RuntimeException {
        IOExceptionShape(String m) { super(m); }
        int code() { return getMessage() == null ? 0 : getMessage().length(); }
    }
}
