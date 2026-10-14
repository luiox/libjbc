package demo;

/**
 * SMT 探针驱动器：每行输出固定，重生成 StackMapTable 后由 libjbc 测试
 * 断言整段输出。全部自然 javac 形态——main 末尾的反射 try/catch 覆盖
 * 两个裁决暴露形态（handler 入口单边赋值槽：ldc/forName 抛点先于 r 的
 * 首次赋值；try/catch 后 join 帧：catch 变量槽只在 handler 路径赋值）。
 * demo.UninitProbe 由测试用 libjbc 手工拼字节生成，运行期反射加载。
 */
public final class Main {
    private Main() { }

    public static void main(String[] args) {
        String[] a = new String[] {"ab"};
        System.out.println("exc=" + ProbeExc.grab(a, 0) + " exc2=" + ProbeExc.grab(a, 5));
        System.out.println("tbl=" + ProbeSwitch.table(1) + " " + ProbeSwitch.table(5)
            + " " + ProbeSwitch.table(9));
        System.out.println("lkp=" + ProbeSwitch.lookup("bb") + " " + ProbeSwitch.lookup("zz"));
        ProbeCtor p = new ProbeCtor(4);
        System.out.println("ctor=" + p.v);
        System.out.println("make=" + ProbeCtor.make(true).getClass().getName());
        try {
            Object r = Class.forName("demo.UninitProbe").getMethod("run").invoke(null);
            System.out.println("uninit=" + r);
        } catch (Throwable t) {
            System.out.println("uninit=FAILED " + t);
        }
    }
}
