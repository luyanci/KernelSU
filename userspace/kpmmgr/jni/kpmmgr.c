#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <string.h>
#include <errno.h>

#define KERNEL_SU_OPTION 0xDEADBEEF
#define KSU_OPTIONS 0xdeadbeef

// KPM控制代码
#define CMD_KPM_CONTROL 28
#define CMD_KPM_CONTROL_MAX 7

// 控制代码
#define SUKISU_KPM_LOAD 1
#define SUKISU_KPM_UNLOAD 2
#define SUKISU_KPM_NUM 3
#define SUKISU_KPM_LIST 4
#define SUKISU_KPM_INFO 5
#define SUKISU_KPM_CONTROL 6
#define SUKISU_KPM_PRINT 7

#define CONTROL_CODE(n) (CMD_KPM_CONTROL + n - 1)

void print_usage(const char *prog) {
    printf("Usage: %s <command> [args]\n", prog);
    printf("Commands:\n");
    printf("  load <path> <args>    Load a KPM module\n");
    printf("  unload <name>         Unload a KPM module\n");
    printf("  num                   Get number of loaded modules\n");
    printf("  list                  List loaded KPM modules\n");
    printf("  info <name>           Get info of a KPM module\n");
    printf("  control <name> <args> Send control command to a KPM module\n");
    printf("  print                 Print KPM module list to stdout\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    int ret = -1;
    int out = -1;  // 存储返回值

    if (strcmp(argv[1], "load") == 0 && argc >= 3) {
        // 加载 KPM 模块
        ret = prctl(KSU_OPTIONS, CONTROL_CODE(SUKISU_KPM_LOAD), argv[2], (argc > 3 ? argv[3] : NULL), &out);
    } else if (strcmp(argv[1], "unload") == 0 && argc >= 3) {
        // 卸载 KPM 模块
        ret = prctl(KSU_OPTIONS, CONTROL_CODE(SUKISU_KPM_UNLOAD), argv[2], NULL, &out);
    } else if (strcmp(argv[1], "num") == 0) {
        // 获取加载的 KPM 数量
        ret = prctl(KSU_OPTIONS, CONTROL_CODE(SUKISU_KPM_NUM), NULL, NULL, &out);
        printf("%d", out);
        return 0;
    } else if (strcmp(argv[1], "list") == 0) {
        // 获取模块列表
        char buffer[1024] = {0};
        ret = prctl(KSU_OPTIONS, CONTROL_CODE(SUKISU_KPM_LIST), buffer, sizeof(buffer), &out);
        if (ret >= 0) {
            printf("%s", buffer);
        }
    } else if (strcmp(argv[1], "info") == 0 && argc >= 3) {
        // 获取指定模块信息
        char buffer[256] = {0};
        ret = prctl(KSU_OPTIONS, CONTROL_CODE(SUKISU_KPM_INFO), argv[2], buffer, &out);
        if (ret >= 0) {
            printf("%s\n", buffer);
        }
    } else if (strcmp(argv[1], "control") == 0 && argc >= 4) {
        // 控制 KPM 模块
        ret = prctl(KSU_OPTIONS, CONTROL_CODE(SUKISU_KPM_CONTROL), argv[2], argv[3], &out);
    } else if (strcmp(argv[1], "print") == 0) {
        // 在 stdout 输出 KPM 列表
        ret = prctl(KSU_OPTIONS, CONTROL_CODE(SUKISU_KPM_PRINT), NULL, NULL, &out);
    } else {
        print_usage(argv[0]);
        return 1;
    }

    if (out < 0) {
        printf("Error: %s\n", strerror(-out));
        return -1;
    }

    return 0;
}
