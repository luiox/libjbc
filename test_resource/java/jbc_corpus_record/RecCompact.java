/**
 * record 语料：紧凑规范构造器形态（非参数化构造器体，字段赋值由编译器
 * 注入）。组件表为双基本类型组件，钉 u2 计数 + 组件三槽语义。
 */
public record RecCompact(int lo, int hi) {

    public RecCompact {
        if (lo > hi) {
            throw new IllegalArgumentException("lo > hi");
        }
    }

    public int width() {
        return hi - lo;
    }
}
