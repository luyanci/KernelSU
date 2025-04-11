package io.sukisu.ultra;

import static shirkneko.zako.sukisu.ui.util.KsuCliKt.getKpmmgrPath;
import shirkneko.zako.sukisu.ui.util.KsuCli;

public class UltraToolInstall {
    private static final String OUTSIDE_KPMMGR_PATH = "/data/adb/ksu/bin/kpmmgr";
    public static void tryToInstall() {
        String kpmmgrPath = getKpmmgrPath();
        if (!UltraShellHelper.isPathExists(OUTSIDE_KPMMGR_PATH)) {
            UltraShellHelper.CopyFileTo(kpmmgrPath, OUTSIDE_KPMMGR_PATH);
            UltraShellHelper.runCmd("chmod a+rx " + OUTSIDE_KPMMGR_PATH);
        }
    }
}
