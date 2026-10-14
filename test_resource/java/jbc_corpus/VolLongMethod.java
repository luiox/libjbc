// VolLongMethod.java — libjbc 语料：长方法（大 code_length / 大帧 / 深合并点）。
package jbcx.corpus;

public final class VolLongMethod {
    private VolLongMethod() {
    }

    // 100 分支的查表方法：tableswitch 大跨度 + 每臂独立帧。
    static int bigSwitch(int k) {
        switch (k) {
        case 0: return 0;
        case 1: return 1;
        case 2: return 4;
        case 3: return 9;
        case 4: return 16;
        case 5: return 25;
        case 6: return 36;
        case 7: return 49;
        case 8: return 64;
        case 9: return 81;
        case 10: return 100;
        case 11: return 121;
        case 12: return 144;
        case 13: return 169;
        case 14: return 196;
        case 15: return 225;
        case 16: return 256;
        case 17: return 289;
        case 18: return 324;
        case 19: return 361;
        case 20: return 400;
        case 21: return 441;
        case 22: return 484;
        case 23: return 529;
        case 24: return 576;
        case 25: return 625;
        case 26: return 676;
        case 27: return 729;
        case 28: return 784;
        case 29: return 841;
        case 30: return 900;
        case 31: return 961;
        case 32: return 1024;
        case 33: return 1089;
        case 34: return 1156;
        case 35: return 1225;
        case 36: return 1296;
        case 37: return 1369;
        case 38: return 1444;
        case 39: return 1521;
        case 40: return -1;
        default: return k * k;
        }
    }

    // 长顺序语句块 + 周期性条件汇合（deep locals 演化）。
    static long longAccumulate(int n) {
        long acc = 0;
        int phase = 0;
        acc += 1;
        acc ^= n;
        acc <<= 1;
        if (acc > 10) { phase = 1; }
        acc += 2;
        acc ^= n + 1;
        acc <<= 1;
        if (acc > 20) { phase += 2; }
        acc += 3;
        acc ^= n + 2;
        acc <<= 1;
        if (acc > 30) { phase += 3; }
        acc += 4;
        acc ^= n + 3;
        acc <<= 1;
        if (acc > 40) { phase += 4; }
        acc += 5;
        acc ^= n + 4;
        acc <<= 1;
        if (acc > 50) { phase += 5; }
        acc += 6;
        acc ^= n + 5;
        acc <<= 1;
        if (acc > 60) { phase += 6; }
        acc += 7;
        acc ^= n + 6;
        acc <<= 1;
        if (acc > 70) { phase += 7; }
        acc += 8;
        acc ^= n + 7;
        acc <<= 1;
        if (acc > 80) { phase += 8; }
        acc += 9;
        acc ^= n + 8;
        acc <<= 1;
        if (acc > 90) { phase += 9; }
        acc += 10;
        acc ^= n + 9;
        acc <<= 1;
        if (acc > 100) { phase += 10; }
        acc += 11;
        acc ^= n + 10;
        acc <<= 1;
        if (acc > 110) { phase += 11; }
        acc += 12;
        acc ^= n + 11;
        acc <<= 1;
        if (acc > 120) { phase += 12; }
        acc += 13;
        acc ^= n + 12;
        acc <<= 1;
        if (acc > 130) { phase += 13; }
        acc += 14;
        acc ^= n + 13;
        acc <<= 1;
        if (acc > 140) { phase += 14; }
        acc += 15;
        acc ^= n + 14;
        acc <<= 1;
        if (acc > 150) { phase += 15; }
        acc += 16;
        acc ^= n + 15;
        acc <<= 1;
        if (acc > 160) { phase += 16; }
        acc += 17;
        acc ^= n + 16;
        acc <<= 1;
        if (acc > 170) { phase += 17; }
        acc += 18;
        acc ^= n + 17;
        acc <<= 1;
        if (acc > 180) { phase += 18; }
        acc += 19;
        acc ^= n + 18;
        acc <<= 1;
        if (acc > 190) { phase += 19; }
        acc += 20;
        acc ^= n + 19;
        acc <<= 1;
        if (acc > 200) { phase += 20; }
        return acc + phase;
    }

    // 多层嵌套循环 + 多合并点（worklist 收敛压力形态）。
    static int deepLoops(int n) {
        int s = 0;
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < i; ++j) {
                int k = j;
                while (k > 0) {
                    if ((k & 1) == 0) {
                        s += 1;
                    } else {
                        s -= 1;
                    }
                    k >>= 1;
                    if (s > 1000) { s = 0; }
                }
                if ((j & 3) == 0) { s += i; }
            }
            switch (i & 7) {
            case 0: s += 2; break;
            case 3: s -= 1; break;
            case 5: s += i; break;
            default: s += 0; break;
            }
        }
        do {
            --n;
            s += n;
        } while (n > 0);
        return s;
    }

    // 长字符串运算链（StringBuilder 长调用链）。
    static String longBuilder(int n) {
        StringBuilder sb = new StringBuilder();
        sb.append("start");
        for (int i = 0; i < n; ++i) {
            sb.append('-').append(i);
            if (i % 3 == 0) { sb.append('*'); }
            if (i % 5 == 0) { sb.append('#'); }
            if (sb.length() > 4096) { sb.setLength(0); }
        }
        return sb.toString();
    }
}
