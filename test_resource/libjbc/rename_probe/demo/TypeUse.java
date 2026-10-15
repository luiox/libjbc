package demo;

import java.lang.annotation.ElementType;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.lang.annotation.Target;

/**
 * TYPE_USE 运行时可见注解：cls() 挂一个 Class 字面量，落在
 * RuntimeVisibleTypeAnnotations 的 'c' element_value 里——
 * 类名手术必须重写该描述符，反射经 getAnnotation(TypeUse.class)
 * .cls().getName() 读出改名后的名字（探针真跑断言的数据源）。
 */
@Retention(RetentionPolicy.RUNTIME)
@Target(ElementType.TYPE_USE)
public @interface TypeUse {
    Class<?> cls();
}
