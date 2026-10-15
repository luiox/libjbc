package demo;

/**
 * sealed 层级：PermittedSubclasses 属性（JDK 17 产出）+ 反射读出面。
 * 本类不参与改名——允许表条目指向同文件嵌套类，跨改名 pass 验证的是
 * 「未映射名原样存活」，改名侧的表内条目重映射由上层 remapper 门覆盖。
 */
public sealed interface Seal permits Seal.Impl {
    int v();

    final class Impl implements Seal {
        public int v() {
            return 9;
        }
    }
}
