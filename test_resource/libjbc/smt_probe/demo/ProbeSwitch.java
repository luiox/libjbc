package demo;

/** SMT 探针样本：tableswitch 与 String lookupswitch（switch 目标帧群）。 */
public final class ProbeSwitch {
    private ProbeSwitch() { }

    public static int table(int v) {
        switch (v) {
            case 1: return 10;
            case 2: return 20;
            case 5: return 50;
            default: return -1;
        }
    }

    public static int lookup(String s) {
        if (s == null) {
            return 0;
        }
        switch (s) {
            case "aa": return 1;
            case "bb": return 2;
            case "cc": return 3;
            default: return -1;
        }
    }
}
