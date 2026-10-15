package demo;

/**
 * 供体类：待搬走的 static 字段与 static 方法（搬移后宿主必须行为等价）。
 *
 * classify/dive/pickName 是分支体 adopt 回归面（member_move 夹具零分支体
 * 缺口）：for 循环的后向 goto、if 的前向分支、int tableswitch、String
 * lookupswitch、try/catch 异常表，以及体内引用供体自身私有成员（dive）的
 * 重映射面——javac 产物经 adoptMethod 搬移后 JVM 真跑必须与供体侧 oracle
 * 等价。
 */
public final class Donor {
    public static int stash = 7;

    public static int grab() {
        stash = 7;
        return stash;
    }

    public static int classify(int x) {
        int acc = 0;
        for (int i = 0; i < x; ++i) {
            acc += i;
        }
        switch (x & 1) {
            case 0:
                acc += 100;
                break;
            case 1:
                acc += 10;
                break;
            default:
                acc = -1;
                break;
        }
        try {
            acc = dive(acc);
        } catch (IllegalStateException e) {
            acc = -acc;
        }
        if (acc > 1000) {
            return 1000;
        }
        return acc;
    }

    public static int pickName(String name) {
        int acc = 5;
        switch (name) {
            case "aa":
                acc = 1;
                break;
            case "bb":
                acc = 2;
                break;
            default:
                acc = 9;
                break;
        }
        return acc;
    }

    private static int dive(int v) {
        if (v < 0) {
            throw new IllegalStateException("neg");
        }
        return v * 2;
    }
}
