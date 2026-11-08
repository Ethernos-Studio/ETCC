# 里程碑 0.3：控制流与枚举

## 目标

在 0.1（Windows x64 Hello World + 自有 C 运行时）和 0.2（聚合类型/初始化器）之上，补齐 C 程序最常用的控制流：

- `switch` / `case` / `default`，包括 fallthrough 与 `break`；
- 嵌套 switch、switch 内 `continue`；
- `goto` 与函数内标签，支持前向/后向、跳入/跳出循环和 switch；
- `enum` 定义、显式/隐式枚举值、作用域遮蔽；
- 枚举常量用于 `case`、数组长度和全局初始化器。

构建仍使用 CMake；IR 不新增控制流指令，所有控制流继续降级为已有基本块 + `br` / `cond_br`。

## 示例

```c
#include <stdio.h>

enum Color { RED, GREEN = 5, BLUE };

int classify(int x) {
    switch (x) {
    case 0: return 100;
    case GREEN: return 105;
    default: return 999;
    }
}

int main(void) {
    int n = 0;
loop:
    n++;
    if (n < 3) goto loop;
    printf("%d %d %d\n", RED, GREEN, classify(GREEN));
    return 0;
}
```

## switch 的 lowering

`switch` 在前端被降级为一条 case 比较链：

```text
entry:
  %cond = ...
  %match0 = icmp eq %cond, case0
  br %match0, label %case0, label %dispatch1
dispatch1:
  %match1 = icmp eq %cond, case1
  br %match1, label %case1, label %dispatch2
...
dispatchN:
  br label %default (或 end)

case0:
  ... case body ...
  br label %case1   ; fallthrough
```

- case 值在 sema 中用整数常量表达式求值；
- 重复 case / 重复 default 明确报错；
- case 块按源码顺序排列，fallthrough 由前一个 case 块 `br` 到下一个 case 块实现；
- `break` 跳到 switch 的 end 块；
- switch 内的 `continue` 仍然跳向外层循环的 continue 目标。

## goto 与标签

- 函数体分析前先扫描所有标签，为每个标签创建基本块；
- `goto` 直接 `br` 到目标标签块，因此前向、后向、跨块跳转都自然成立；
- 标签与变量名属于不同命名空间；
- 重复标签、未知标签在 sema 报错；
- 对语法上不可达的代码也继续生成死块（dead block），使 `goto` 可以跳入循环/if/switch 内部的标签；死块没有前驱，不影响运行时语义。

## enum

- parser 把 enumerator 解析为 `AST_ENUM_CONST` 声明节点，值支持常量表达式；
- 枚举常量进入普通标识符作用域，可被局部变量正确遮蔽；
- 枚举类型在 ETCC 中按 `int` 表示（`sizeof(enum E) == 4`）；
- 枚举常量可用于 `case`、数组长度、全局初始化器等常量上下文。

## 测试

新增 17 个测试项，覆盖：

- switch 基本分派、fallthrough、default、break；
- 嵌套 switch、switch 内 continue、负 case 值；
- goto 前向/后向、跳入循环、跳出 switch、标签 after return；
- enum 隐式/显式值、表达式初值、作用域遮蔽、数组长度、全局初始化；
- 错误诊断：case 在 switch 外、重复 case/default、非恒定 case、switch 指针、未知标签、重复标签、枚举重定义、break 在循环/switch 外。

当前总结果：

```text
85 passed, 0 failed
```

## 仍然不支持

- 结构体/联合体按值传参、返回；
- 位域、flexible array member；
- 指定初始化器、花括号省略；
- 全局聚合中的非空指针重定位；
- 函数指针、变参函数定义、浮点；
- `switch` 的跳转表优化（当前是线性比较链，后续可加）。

这些都不改变 IR/后端契约；后续可以在 sema 的 switch lowering 中增加 dense jump table 优化。
