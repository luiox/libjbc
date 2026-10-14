// VolGeneric.java — libjbc 语料：泛型全形态（有界类型参数/通配符/泛型方法/桥方法/Signature 属性）。
package jbcx.corpus;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;

public final class VolGeneric {
    private VolGeneric() {
    }

    // ---- 泛型类（有界参数）----
    static class Box<T extends Comparable<T>> implements Comparable<Box<T>> {
        private final T value;

        Box(T value) { this.value = value; }

        T get() { return value; }

        @Override public int compareTo(Box<T> o) { return value.compareTo(o.value); }

        @Override public String toString() { return "Box(" + value + ")"; }
    }

    static class NamedBox<T extends Comparable<T>> extends Box<T> {
        private final String name;

        NamedBox(String name, T value) {
            super(value);
            this.name = name;
        }

        String name() { return name; }

        // 覆写泛型父类方法 → 桥方法
        @Override public int compareTo(Box<T> o) {
            final int c = super.compareTo(o);
            return c != 0 ? c : name.compareTo(((NamedBox<T>) o).name);
        }
    }

    static <T extends Comparable<T>> T maxOf(T a, T b) { return a.compareTo(b) >= 0 ? a : b; }
    static <T> T firstOf(List<T> xs) { return xs.isEmpty() ? null : xs.get(0); }
    static <T> void swapIn(List<T> xs, int i, int j) {
        T t = xs.get(i);
        xs.set(i, xs.get(j));
        xs.set(j, t);
    }
    static <T extends Number> double sumOf(List<T> xs) {
        double s = 0.0;
        for (T x : xs) { s += x.doubleValue(); }
        return s;
    }
    static double sumWildcard(List<? extends Number> xs) {
        double s = 0.0;
        for (Number n : xs) { s += n.doubleValue(); }
        return s;
    }
    static void fillWildcard(List<? super Integer> sink, int n) {
        for (int i = 0; i < n; ++i) { sink.add(i); }
    }
    static <T> List<T> listOf(T... items) {
        List<T> out = new ArrayList<T>();
        for (T t : items) { out.add(t); }
        return out;
    }
    static <K, V> String pairText(K k, V v) { return String.valueOf(k) + "=" + String.valueOf(v); }
    static <T extends Comparable<T>> int cmpBoxed(Box<T> a, Box<T> b) { return a.compareTo(b); }

    static int useBoxes() {
        Box<Integer> a = new Box<Integer>(3);
        NamedBox<Integer> n = new NamedBox<Integer>("n", 4);
        return a.compareTo(n) + n.compareTo(a) + maxOf(2, 3) + cmpBoxed(a, n);
    }
    static int useLists() {
        List<Integer> xs = listOf(1, 2, 3);
        swapIn(xs, 0, 2);
        List<String> ss = listOf("a", "b");
        return firstOf(xs) + firstOf(ss).length() + (int) sumOf(xs) + (int) sumWildcard(xs);
    }
    static int useWildcards() {
        List<Integer> sink = new ArrayList<Integer>();
        fillWildcard(sink, 5);
        return sink.size() + pairText(1, "a").length();
    }
    static Comparator<Box<Integer>> boxCmp() {
        return new Comparator<Box<Integer>>() {
            @Override public int compare(Box<Integer> a, Box<Integer> b) { return a.compareTo(b); }
        };
    }

    // ---- 泛型接口实现（桥方法 + checkcast）----
    interface Transformer<F, T> {
        T apply(F from);
    }

    static final class StrLen implements Transformer<String, Integer> {
        @Override public Integer apply(String from) { return from.length(); }
    }

    static int useTransformer(String s) {
        Transformer<String, Integer> t = new StrLen();
        return t.apply(s) + t.apply(s + "x");
    }
    static <F, T> T run(Transformer<F, T> t, F v) { return t.apply(v); }
    static int useRun(String s) { return run(new StrLen(), s); }

    // ---- 泛型 + 数组（checkcast 形态）----
    static <T> T[] arrayOf(T a, T b) {
        @SuppressWarnings("unchecked")
        T[] r = (T[]) new Object[] { a, b };
        return r;
    }
    static int useArrayOf() {
        String[] ss = arrayOf("a", "b");
        return ss.length + ss[0].length() + ss[1].length();
    }
    static <T extends Comparable<T>> int clampCmp(T v, T lo, T hi) {
        if (v.compareTo(lo) < 0) { return -1; }
        return v.compareTo(hi) > 0 ? 1 : 0;
    }
    static int useClamp() {
        return clampCmp(5, 1, 10) + clampCmp("m", "a", "z") + clampCmp(99L, 1L, 10L);
    }
}
