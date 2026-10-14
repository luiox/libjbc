package demo;

/** SMT 探针样本：构造器链（super 前分支 → UninitThis 帧）+ new 在分支臂。 */
public class ProbeCtor extends Base {
    int v;

    ProbeCtor(int base) {
        super(base > 0 ? 1 : -1);
        if (base > 0) {
            v = base;
        } else {
            v = -base;
        }
    }

    public static Object make(boolean b) {
        Object o;
        if (b) {
            o = new ProbeCtor(3);
        } else {
            o = "s";
        }
        return o;
    }
}

/** super(int) 的载体：让 ProbeCtor 构造器首语句带参 super 合法。 */
class Base {
    final int seed;

    Base(int seed) {
        this.seed = seed;
    }
}
