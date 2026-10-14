// VolHierarchy.java — libjbc 语料：继承层次（resolver/merge 语料）+ 抽象类 + varargs + 字段遮蔽。
package jbcx.corpus;

public class VolHierarchy {
    static class Animal {
        String name;
        int legs;

        Animal(String name, int legs) {
            this.name = name;
            this.legs = legs;
        }

        String sound() { return "..."; }
        String describe() { return name + "/" + legs + "/" + sound(); }
        static String king() { return "Animal"; }
    }

    static class Dog extends Animal {
        Dog(String name) { super(name, 4); }

        @Override String sound() { return "woof"; }
        String fetch(String what) { return name + " fetches " + what; }
    }

    static class Puppy extends Dog {
        Puppy(String name) { super(name); }

        @Override String sound() { return "yip"; }

        @Override String describe() { return super.describe() + "/puppy"; }
    }

    static class Bird extends Animal {
        Bird(String name) { super(name, 2); }

        @Override String sound() { return "tweet"; }
        String fly() { return name + " flies"; }
    }

    static int describeAll(Animal[] xs) {
        int n = 0;
        for (Animal a : xs) { n += a.describe().length(); }
        return n;
    }
    static int dispatchChain() {
        Animal a = new Animal("a", 4);
        Dog d = new Dog("d");
        Puppy p = new Puppy("p");
        Bird b = new Bird("b");
        return a.sound().length() + d.sound().length() + p.sound().length() + b.sound().length()
            + d.fetch("x").length() + b.fly().length() + p.describe().length();
    }
    static int castAround(Animal a) {
        if (a instanceof Dog) {
            Dog d = (Dog) a;
            return d.fetch("y").length();
        }
        if (a instanceof Bird) {
            return ((Bird) a).fly().length();
        }
        return a.sound().length();
    }
    static String superCall() { return Dog.king() + Animal.king(); }

    // ---- 抽象类 + 全实现层次 ----
    abstract static class Shape2 {
        abstract double area();
        double perimeter() { return 0.0; }
        String tag() { return getClass().getSimpleName() + "@" + (int) area(); }
    }

    static final class Circle extends Shape2 {
        final double r;
        Circle(double r) { this.r = r; }
        @Override double area() { return 3.14159 * r * r; }
        @Override double perimeter() { return 2 * 3.14159 * r; }
    }

    static final class Rect extends Shape2 {
        final double w, h;
        Rect(double w, double h) { this.w = w; this.h = h; }
        @Override double area() { return w * h; }
        @Override double perimeter() { return 2 * (w + h); }
    }

    static double totalArea(Shape2[] xs) {
        double s = 0.0;
        for (Shape2 x : xs) { s += x.area() + x.perimeter(); }
        return s;
    }
    static String[] tags(Shape2[] xs) {
        String[] out = new String[xs.length];
        for (int i = 0; i < xs.length; ++i) { out[i] = xs[i].tag(); }
        return out;
    }

    // ---- 字段遮蔽（子类同名字段 + super 区分）----
    static class Shadow {
        int v = 1;
        int readV() { return v; }
    }

    static final class ShadowSub extends Shadow {
        int v = 100;
        int readSub() { return v; }
        int readSuper() { return super.v; }
        int sum() { return v + super.v; }
        int castRead() { return ((Shadow) this).v; }
    }

    static int useShadow() {
        ShadowSub s = new ShadowSub();
        Shadow up = s;
        return s.readSub() + s.readSuper() + s.sum() + s.castRead() + up.readV() + up.v;
    }

    // ---- varargs 全形态 ----
    static int vsum(int... xs) {
        int s = 0;
        for (int x : xs) { s += x; }
        return s;
    }
    static String joinAll(String sep, Object... xs) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < xs.length; ++i) {
            if (i > 0) { sb.append(sep); }
            sb.append(xs[i]);
        }
        return sb.toString();
    }
    static int useVarargs() {
        return vsum() + vsum(1) + vsum(1, 2, 3) + joinAll(",", "a", 1, 2.0).length();
    }
    static int passThrough(Object[] xs) {
        return xs.length + (xs.length > 0 ? vsum(1, 2, 3) : 0);
    }
}
