# 里程碑 0.1：Windows x64 上的 Hello, World

## 目标

> 在 Windows 目标下用自己的 C 库成功运行 Hello, World 程序。
>
> 后端暂时用 LLVM，但设计成可替换、健壮。

这个里程碑不是“调用 clang 编译一个 hello.c”，而是：

1. ETCC 自己完成预处理、词法、语法、语义和 IR 生成；
2. ETCC 生成后端无关的 typed IR；
3. LLVM 后端只负责把 IR 变成 COFF；
4. ETCC 自己的 C 运行时提供入口、`puts`、`printf`、`malloc` 等；
5. 链接时使用 `/nodefaultlib`，**不链接 MSVC CRT / UCRT**；
6. 生成的 EXE 成功输出 `Hello, World!`。

## 复现

```bat
cmake --preset mingw
cmake --build --preset mingw
build\etcc.exe tests\hello.c -o build\hello.exe
build\hello.exe
```

输出：

```text
Hello, World!
```

## 验证：EXE 只依赖 KERNEL32

```bat
llvm-readobj --coff-imports build\hello.exe
llvm-readobj --file-headers build\hello.exe
```

关键结果：

```text
Format: COFF-x86-64
Machine: IMAGE_FILE_MACHINE_AMD64 (0x8664)
Subsystem: IMAGE_SUBSYSTEM_WINDOWS_CUI (0x3)

Import {
  Name: KERNEL32.dll
  Symbol: ExitProcess
  Symbol: GetCommandLineA
  Symbol: GetProcessHeap
  Symbol: GetStdHandle
  Symbol: HeapAlloc
  Symbol: HeapFree
  Symbol: HeapReAlloc
  Symbol: WriteFile
}
```

没有 `vcruntime*.dll`、`ucrtbase.dll`、`msvcrt.dll`。入口点由运行时提供：

```text
/entry:etcc_entry
```

## 这条链路里什么是“自己的”

| 部分 | 实现 |
|---|---|
| 预处理 | `src/pp.c`：include、条件编译、对象宏、source map |
| 词法 | `src/lexer.c`：C token、转义、宏帧展开 |
| 语法 | `src/parser.c`：递归下降、AST、错误恢复 |
| 类型系统 | `src/ctype.c` + `src/sema.c`：Windows LLP64、整数提升、指针算术 |
| 中间表示 | `src/ir.c` + `include/etcc/ir.h`：typed IR、基本块、终止符 |
| 后端接口 | `include/etcc/backend.h` + `src/backend.c`：ops + 注册表 |
| LLVM 后端 | `src/backend_llvm.c`：IR → LLVM 文本 → clang → COFF |
| 驱动 | `src/driver.c`：参数、工具链发现、链接 |
| C 运行时 | `runtime/src/*`：`etcc_entry`、libc 子集、`kernel32` 导入 |
| 头文件 | `runtime/include/*`：`stdio.h`、`stdlib.h` 等 |

LLVM 只承担“COFF 汇编器/优化器”的角色。IR 是 ETCC 的，前端不包含 LLVM 头文件，`backend_llvm.c` 是唯一了解 LLVM 文本语法的模块。

## 已经跑通的测试

```text
49 passed, 0 failed
```

覆盖：

- Hello World；
- EXE 只导入 `KERNEL32.dll`，没有系统 CRT；
- `argc/argv`（运行时自行解析 `GetCommandLineA`）；
- 递归、循环、`break`/`continue`、`do/while`、作用域；
- 数组、二维数组、二级指针、指针算术、指针转换；
- 短路求值、三元、逗号、复合赋值、自增自减；
- 字符串/字符转义与相邻字符串拼接；
- include path、include guard、`#pragma once`、命令行宏、条件编译；
- 预定义宏；
- `-S` IR、`-c` COFF、`-E`；
- 诊断定位；
- 后端注册表与未知后端错误。

## 关键设计决定

### 1. typed IR 是唯一契约

```c
IrModule → IrFunction → IrBlock → IrInstr
```

后端只消费 IR。换后端不需要改 PP、lexer、parser、sema 或 runtime 链接契约。

### 2. 没有 PHI，先保证正确和可移植

局部变量统一 alloca/load/store；`&&`/`||`/`?:` 用临时槽 + CFG。这样：

- 前端不需要构造 SSA；
- 新后端实现简单；
- 后续可以在 IR 上加 mem2reg/优化 pass，而不改变前端。

### 3. 运行时不依赖 CRT

`etcc_entry` 直接调用 `kernel32`：

- `GetCommandLineA` → `argc/argv`
- `GetProcessHeap` / `HeapAlloc` / `HeapReAlloc` / `HeapFree` → `malloc` 族
- `GetStdHandle` / `WriteFile` → stdout
- `ExitProcess` → 进程退出

这就是“自己的 C 库”的实际含义：不是把系统 libc 改名，而是从入口到分配器、到 `printf` 都是本项目实现。

### 4. 工具链可发现、可覆盖

自动找 Windows SDK、`kernel32.lib`、`etccrt.lib`、runtime include；全部可用环境变量或命令行覆盖。进程调用实现了 MSVC 参数引用规则，避免 `C:\Program Files (x86)` 被拆成多个参数。

### 5. 错误处理集中化

词法、语法、语义、IR 校验、后端、链接失败统一进入 `EtccDiagnostics`；错误带文件、行、列和源码 caret。解析器支持错误恢复。

## 下一个里程碑建议

1. `struct` / `union` / `enum` + 成员访问 + 结构体布局（ABI 对齐）。
2. 初始化器：数组/结构体花括号、字符串数组长度推导。
3. `switch`、`goto`、函数指针、变参函数定义。
4. 预处理器：函数式宏、`#`/`##`、完整 `-E`。
5. x86-64 原生后端：直接生成 COFF，验证 `--backend native` 与 LLVM 后端跑同一套测试。
6. 自举：用 ETCC 编译 runtime，再编译 ETCC 自身。

> 里程碑 0.2 已完成第 1、2 项的大部分：见 [MILESTONE-0.2.md](MILESTONE-0.2.md)。
