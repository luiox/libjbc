// VolEnumShape.java — libjbc 语料：枚举全形态（常量体/抽象方法/字段/静态块/values/valuesOf）。
package jbcx.corpus;

public enum VolEnumShape {
    // 带构造参数的常量
    PLAIN("plain"),
    WITH_BODY("with-body") {
        @Override int weight() { return 20; }
        @Override String greet() { return "hi from " + label; }
    },
    MULTI("multi", 3) {
        @Override int weight() { return area * 10; }
    };

    final String label;
    final int area;

    VolEnumShape(String label) {
        this(label, 1);
    }

    VolEnumShape(String label, int area) {
        this.label = label;
        this.area = area;
    }

    // 非抽象基实现 + 常量体覆盖（常量体匿名类形态）
    int weight() { return area; }

    String greet() { return label; }

    int total() { return weight() + area + label.length(); }

    static final VolEnumShape DEFAULT = PLAIN;

    static int allWeights() {
        int s = 0;
        for (VolEnumShape e : values()) { s += e.weight(); }
        return s;
    }

    static VolEnumShape parse(String s) {
        for (VolEnumShape e : values()) {
            if (e.label.equals(s)) { return e; }
        }
        return DEFAULT;
    }

    static int describeAll() {
        int n = 0;
        for (VolEnumShape e : VolEnumShape.values()) { n += e.greet().length() + e.total(); }
        return n;
    }

    static int byNameLen(int len) {
        int hit = 0;
        for (VolEnumShape e : values()) {
            if (e.label.length() > len) { ++hit; }
        }
        return hit;
    }

    static {
        // 枚举的静态初始化块（<clinit> 中常量构造之后追加的形态）
        int x = DEFAULT.weight();
        if (x < 0) { throw new IllegalStateException("bad enum init"); }
    }
}
