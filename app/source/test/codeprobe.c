/*
 * codeprobe.c -- 见 codeprobe.h
 *
 * 为什么要单独一个探针：
 *   代码块落盘发生在一句**模型回复之后**，而回复要走网络 ——
 *   回归测试不该依赖外网。所以这里直接喂一段写死的"带围栏的回复"，
 *   验的是**解析与写文件**这两步（真正的触发点在 sc_chat）。
 */

#include "codeprobe.h"

#include "app.h"
#include "codebox.h"

void codeprobe_run(void)
{
    static const char *SAMPLE =
        "给你两个文件。\n"
        "```c\n"
        "int main(void) { return 0; }\n"
        "```\n"
        "第二个：\n"
        "```python\n"
        "print('hi')\n"
        "```\n"
        "再来一个没写语言的：\n"
        "```\n"
        "plain text\n"
        "```\n"
        "还有一个**没收尾**的（模型偶尔会忘记收）：\n"
        "```js\n"
        "console.log(1);\n";
    char names[160];
    int  n = codebox_extract(SAMPLE, names, sizeof names);

    app_note("codeprobe: saved=%d names=[%s]", n, names);
}
