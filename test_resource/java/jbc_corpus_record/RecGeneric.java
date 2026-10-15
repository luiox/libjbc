/**
 * record 语料：泛型组件——组件槽带 Signature 嵌套属性（JVMS 4.7.30 组件
 * 属性表面）。主语料的 --release 8 产物是 v52，构造上不带 Record 属性，
 * 故立独立小语料以 --release 16 现场编译。
 */
public record RecGeneric<T extends Comparable<T>>(T value, int rank) {

    public int compareRank(RecGeneric<?> other) {
        return Integer.compare(rank, other.rank);
    }
}
