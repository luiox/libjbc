// SigRefSamples.java — B1 反射对照样本：泛型声明全形态，Signature 属性与
// 反射 Type 树一一对照（jbc::type 签名树的验收 oracle）。
// 覆盖：嵌套泛型 / 递归类型边界 / 通配符三态与捕获形态 / 递归实参 /
// 泛型父类 / throws 类型变量 / 数组×泛型交叠。
package jbcx.sigref;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

public class SigRefSamples extends ArrayList<Map<String, List<Integer>>>
        implements Comparable<SigRefSamples> {

    // 对照样本 1：嵌套泛型 Map<String, List<Integer>>
    public Map<String, List<Integer>> nested;

    public Map<String, List<Integer>> makeNested() {
        return null;
    }

    public int compareTo(SigRefSamples o) {
        return 0;
    }

    // 对照样本 2：递归类型边界 <T extends Comparable<T>>
    public static final class Store<T extends Comparable<T>> {
        public T pick(T x, List<T> xs) {
            return x;
        }
    }

    // 对照样本 3：通配符三态（含捕获形态 <? super Integer> 作类型变量界）
    public static final class Wild {
        public List<? extends Number> upper;
        public Map<? super Integer, ?> lower;

        public static <C extends List<? super Integer>> C capture(C c) {
            return c;
        }
    }

    // 递归实参：V extends List<K> + 泛型父类
    public static final class Pair<K, V extends List<K>> extends HashMap<K, V> {
    }

    // throws 类型变量 + 具体类
    public static final class Thrower<T extends Exception> {
        public void blast() throws T, IllegalStateException {
            throw new IllegalStateException();
        }
    }

    // 数组 × 泛型交叠
    public static final class Hold {
        public ArrayList<Hold[]> grid;
    }
}
