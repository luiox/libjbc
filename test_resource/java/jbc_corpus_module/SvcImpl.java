package impl;

import svc.Svc;

/**
 * module 语料：服务实现（provides...with 的 with 侧；opens impl 的开放包）。
 */
public class SvcImpl implements Svc {
    @Override
    public String run() {
        return "svc-ok";
    }
}
