// VolSwitchMap.java — libjbc 语料：enum switch（$SwitchMap 合成类 + <clinit> 大形态）与枚举互操作。
package jbcx.corpus;

public final class VolSwitchMap {
    private VolSwitchMap() {
    }

    // ---- enum switch × 单枚举 ----
    static int colorCode(Color c) {
        switch (c) {
        case RED: return 1;
        case GREEN: return 2;
        case BLUE: return 3;
        default: return 0;
        }
    }
    static int colorCodeWide(Color c) {
        int r;
        switch (c) {
        case RED: r = 100; break;
        case GREEN: r = 200; break;
        case BLUE: r = 300; break;
        case BLACK: r = 0; break;
        case WHITE: r = 999; break;
        default: r = -1; break;
        }
        return r;
    }
    static String colorName(Color c) {
        switch (c) {
        case RED: return "red";
        case GREEN: return "green";
        case BLUE: return "blue";
        case BLACK: return "black";
        case WHITE: return "white";
        default: return "?";
        }
    }
    static int colorLoop(Color[] cs) {
        int s = 0;
        for (Color c : cs) { s += colorCode(c); }
        return s;
    }

    // ---- enum switch × 双枚举（两张 switch map）----
    static int pair(Shape s, Size z) {
        switch (s) {
        case CIRCLE: return z.ordinal();
        case SQUARE: return z.ordinal() * 2;
        case TRIANGLE: return z.ordinal() * 3;
        default: return -1;
        }
    }
    static int pair2(Shape s, Size z) {
        switch (z) {
        case SMALL: return s.ordinal() + 1;
        case MEDIUM: return s.ordinal() + 2;
        case LARGE: return s.ordinal() + 3;
        default: return -1;
        }
    }
    static int matrix(Shape s, Size z, Color c) {
        return pair(s, z) + colorCode(c) + s.name().length() + z.name().length();
    }

    static int enumMethods(Color c) {
        return c.ordinal() + c.name().length() + c.hashCode() + Color.valueOf("RED").ordinal();
    }
    static Color parseColor(String s) {
        try {
            return Color.valueOf(s);
        } catch (IllegalArgumentException e) {
            return Color.BLACK;
        }
    }
    static int enumValues(Color c) {
        int n = 0;
        for (Color x : Color.values()) {
            if (x == c) { return n; }
            ++n;
        }
        return -1;
    }
    static int enumCompare(Color a, Color b) { return a.compareTo(b); }

    // ---- 枚举形态本体在其他文件；这里补 switch-on-ordinal 回退路径 ----
    static int byOrdinal(int k) {
        Color[] all = Color.values();
        return k >= 0 && k < all.length ? all[k].ordinal() : -1;
    }
    static double mixedEnum(Shape s) {
        switch (s) {
        case CIRCLE: return 3.14;
        case SQUARE: return 4.0;
        default: return 0.0;
        }
    }

    enum Color {
        RED, GREEN, BLUE, BLACK, WHITE
    }

    enum Shape {
        CIRCLE, SQUARE, TRIANGLE
    }

    enum Size {
        SMALL(1), MEDIUM(4), LARGE(9);

        final int area;

        Size(int area) { this.area = area; }

        int area() { return area; }
    }
}
