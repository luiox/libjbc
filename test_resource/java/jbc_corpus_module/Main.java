package app;

import java.util.ServiceLoader;
import svc.Svc;

/**
 * module 语料：服务消费方（uses svc.Svc 的代码侧背书；exports app to
 * java.logging 的被导出包）。
 */
public class Main {
    public static void main(String[] args) {
        for (Svc s : ServiceLoader.load(Svc.class)) {
            System.out.println(s.run());
        }
    }
}
