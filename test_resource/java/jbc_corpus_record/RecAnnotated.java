import java.lang.annotation.ElementType;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.lang.annotation.Target;
import java.util.List;

/**
 * record 语料：带注解组件——组件槽带 RuntimeVisibleAnnotations 嵌套属性
 * （RECORD_COMPONENT 目标的 RUNTIME 注解，javac 落在 Record 属性的组件
 * 属性表上，字段/访问器同步传播）。泛型接口组件另带 Signature 嵌套属性。
 */
@Retention(RetentionPolicy.RUNTIME)
@Target({ElementType.RECORD_COMPONENT, ElementType.FIELD, ElementType.METHOD})
@interface Marked {
    String tag();
}

public record RecAnnotated(@Marked(tag = "a") int a, List<String> tags) {

    public String firstTagOrEmpty() {
        return tags.isEmpty() ? "" : tags.get(0);
    }
}
