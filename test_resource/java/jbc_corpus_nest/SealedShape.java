/**
 * nest 语料：sealed 层级（--release 17 在 sealed 类型上产出
 * PermittedSubclasses；允许表带两个条目，钉 u2 计数 + 条目表语义）。
 */
public sealed interface SealedShape permits Circle, Square {
    int area();
}

final class Circle implements SealedShape {
    private final int r;

    Circle(int r) {
        this.r = r;
    }

    public int area() {
        return r * r * 3;
    }
}

final class Square implements SealedShape {
    private final int a;

    Square(int a) {
        this.a = a;
    }

    public int area() {
        return a * a;
    }
}
