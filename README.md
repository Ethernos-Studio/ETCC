# Ethernos Tiny C Compiler (ETCC)

一个面向 **Windows x64** 的 C 子集编译器。前端、类型系统、语义分析、后端无关的 typed IR 和独立 C 运行时全部由本项目实现；当前代码生成通过 **LLVM 文本 IR + Clang + LLD** 完成。

> 第一个里程碑（已完成）：**用 ETCC 编译 Hello World，链接 ETCC 自己的 C 运行时，生成不依赖 MSVC CRT / UCRT 的 Windows EXE 并成功运行。**

```text
$ build\etcc.exe tests\hello.c -o build\hello.exe
$ build\hello.exe
Hello, World!
```

## 为什么不是“再写一个 TCC”

ETCC 从第一天就按可替换后端设计：

```
C source
   │
   ▼
preprocessor ──► lexer ──► parser ──► semantic analysis
                                             │
                                             ▼
                                  backend-independent typed IR
                                             │
                         ┌───────────────────┴───────────────────┐
                         ▼                                       ▼
                    LLVM backend                          future backends
              (textual IR → COFF)                    (x86-64 / C / WASM / ...)
                         │
                         ▼
                 ETCC runtime (own libc, no CRT)
                         │
                         ▼
                    lld-link → .exe
```

前端不会 import 任何 LLVM 头文件，IR 中也不出现 `llvm::Value`。换后端只需要实现 `EtccBackendOps`，前端和运行时链接契约保持不变。

## 快速开始

### 依赖

| 组件 | 用途 | 本机验证版本 |
|---|---|---|
| Windows 10/11 x64 | 目标平台 | Windows |
| CMake | 构建系统 | 4.4.4 |
| MinGW-w64 GCC | 编译编译器本身 | 16.2.0 |
| LLVM / Clang / llc / LLD | LLVM 后端、COFF 汇编与链接 | 22.1.8 |
| Windows SDK | `kernel32.lib` | 10.0.26100.0 |
| Python 3 | 端到端测试 | 3.15 |

### 构建

```bat
cmake --preset mingw
cmake --build --preset mingw
```

构建产物：

```text
build\etcc.exe              编译器驱动
build\runtime\etccrt.lib    自己的 C 运行时（crt0 + libc 子集）
```

`cmake --preset mingw` 使用 **MinGW Makefiles** 生成器，并固定使用 `gcc` 构建编译器；运行时由 Clang 编译、`llvm-lib` 归档。可用 `-DETCC_CLANG=<path>` / `-DETCC_LLVM_LIB=<path>` 覆盖运行时工具链。若不用 preset，也可：

```bat
cmake -G "MinGW Makefiles" -S . -B build -DCMAKE_C_COMPILER=gcc
cmake --build build
```

安装到指定前缀（会得到 `bin/etcc.exe`、`lib/etccrt.lib`、`include/*.h`，驱动能按安装布局自动发现运行时）：

```bat
cmake --install build --prefix build\install
build\install\bin\etcc.exe tests\hello.c -o build\install\hello.exe
```

### 编译并运行

```bat
build\etcc.exe tests\hello.c -o build\hello.exe
build\hello.exe
```

编译器会自动发现：

- `clang` / `lld-link`（PATH，或环境变量）
- Windows SDK 下的 `kernel32.lib`
- `build\runtime\etccrt.lib`
- `runtime\include`（自己的 C 库头文件）

可用环境变量覆盖：

```text
ETCC_CLANG
ETCC_LLC
ETCC_LINK
ETCC_LLD_LINK
ETCC_KERNEL32_LIB
ETCC_RUNTIME_LIB
ETCC_RUNTIME_INCLUDE
```

也可用命令行覆盖：`--clang`、`--llc`、`--linker`、`--lld-link`、`--kernel32`、`--runtime-lib`、`--runtime-include`。

## 已实现的语言子集

### 类型

- `void`、`_Bool`
- `char` / `signed char` / `unsigned char`
- `short` / `unsigned short`
- `int` / `unsigned int`
- `long` / `unsigned long`（Windows LLP64：32 位）
- `long long` / `unsigned long long`
- 多级指针、多维数组、函数类型
- `struct` / `union`：具名/匿名标签、前置声明、自引用指针、嵌套、数组、成员布局与对齐
- `enum`：枚举定义、显式/隐式值、作用域内常量
- `const` / `volatile` / `restrict` 限定符（`const` 参与类型表示）
- `typedef`

### 声明与语句

- 全局变量、局部变量、函数定义、函数原型
- `static`、`extern`（局部 `static` 目前会明确报错）
- 复合语句、`if` / `else`、`while`、`do` / `while`、`for`
- `switch` / `case` / `default`，支持 fallthrough 与 `break`
- `goto` 与函数内标签（前向/后向、跨块跳转）
- `break`、`continue`、`return`
- C99 风格的块内声明、`for (int i = 0; ...)`

### 表达式

- 完整优先级的算术、位运算、比较、逻辑运算
- `&&` / `||` 短路求值（通过 CFG + 临时槽实现）
- 赋值与复合赋值：`= += -= *= /= %= &= |= ^= <<= >>=`
- 前置/后置 `++` / `--`
- 三元表达式、逗号表达式
- 函数调用、数组下标、`&` / `*`
- `struct` / `union` 成员访问：`.`、`->`、嵌套成员、成员数组
- `struct` / `union` 整体赋值
- 显式类型转换、`sizeof(type)` / `sizeof expr`
- 整数提升、常用算术转换、指针算术、指针相减

### 聚合类型与初始化器

- 具名 `struct` / `union`、匿名定义、`typedef struct { ... } Name`
- 前置声明 `struct S;`、自引用 `struct Node { struct Node *next; };`
- Windows x64 自然对齐布局：字段偏移、填充、结构体/联合体大小
- 结构体/联合体指针、结构体数组、嵌套结构体
- `s.x`、`p->x`、`s.a.b`、`p->a[i].x`
- `struct S a = b;` 整体复制
- `struct` / `union` 按值传参和返回：1/2/4/8 字节走整数寄存器，更大的走隐藏指针（Windows x64 MSVC ABI）
- 局部与全局的 `{ ... }` 聚合初始化，支持嵌套、部分初始化补零
- `char s[] = "abc"`、`char buf[8] = "hi"`、`int a[] = {1, 2, 3}` 长度推导
- 全局整数/字符数组、结构体、联合体初始化（编译期写为静态字节数据）

### 控制流与枚举

- `switch` 整数分派：case 链、`default`、fallthrough、`break`
- 嵌套 `switch`、switch 内 `continue` 到外层循环
- `goto` 标签：前向/后向、跳入/跳出循环与 switch、函数级标签作用域
- 枚举常量：`enum { A, B = 5, C }`、表达式初值、作用域遮蔽
- 枚举常量可用于 `case`、数组长度、全局初始化器

### 预处理器

- `#include "..."` / `#include <...>`
- `-I` 搜索路径、`-D` / `-U`
- 对象式宏、宏递归展开保护
- `#if` / `#ifdef` / `#ifndef` / `#elif` / `#else` / `#endif`
- `#if` 中的 `defined`、整数运算、比较、逻辑运算
- `#error` / `#warning`
- include guard、`#pragma once`
- 预定义宏：`__ETCC__`、`__STDC__`、`__STDC_VERSION__`、`_WIN32`、`_WIN64`、`_M_X64`、`__x86_64__`

### 运行时（自己的 C 库）

`runtime/` 不链接 MSVC CRT / UCRT，只导入 `kernel32.dll`：

- 自定义入口 `etcc_entry`：构造 `argc/argv`，调用 `main(argc, argv)`，`ExitProcess`
- `puts`、`printf`、`putchar`、`fputs`、`fflush`
- `malloc`、`calloc`、`realloc`、`free`
- `exit`、`abort`
- `memcpy`、`memmove`、`memset`、`memcmp`
- `strlen`、`strcmp`、`strncmp`、`strcpy`、`strncpy`、`strcat`
- 头文件：`stdio.h`、`stdlib.h`、`string.h`、`stddef.h`、`stdint.h`、`stdbool.h`、`limits.h`

`printf` 支持 `%d %i %u %o %x %X %c %s %p %%`，以及 `hh/h/l/ll/z/j/t` 长度修饰、宽度、精度和常用 flag。

## 驱动用法

```bat
etcc [options] <input.c>

  -o <file>              输出文件
  -c                     只生成 COFF 目标文件
  -S, --emit-llvm        输出后端文本 IR
  -E, --emit-pp          输出预处理结果
  -I <dir>               添加 include 搜索路径
  -D <name>[=<value>]    定义对象宏
  -U <name>              取消宏
  --backend <name>       选择后端（当前为 llvm）
  --clang <path>         LLVM 后端使用的 clang（cc1 回退路径）
  --llc <path>           llc（优先的 IR -> COFF 路径）
  --linker <path>        链接器（lld-link 或 MSVC link.exe）
  --lld-link <path>      --linker 的别名
  --kernel32 <path>      kernel32.lib
  --runtime-lib <path>   ETCC 运行时库
  --runtime-include <dir> ETCC 运行时头文件
  -v, --verbose          打印工具链路径和实际命令
  --save-temps           保留链接阶段的临时目标文件
  --version
```

## 测试

```bat
ctest --preset mingw
```

等价地：

```bat
cmake --build --preset mingw --target check
```

测试覆盖：

- Hello World（里程碑）
- EXE 只导入 `KERNEL32.dll`，没有 VCRUNTIME / UCRT / MSVCRT
- `argc/argv`
- 表达式、控制流、递归、作用域、位运算、64 位整数
- 数组、二级指针、指针算术、显式类型转换
- `struct` / `union` 布局、嵌套、成员访问、自引用链表、整体赋值
- 局部/全局聚合初始化、部分初始化补零、字符串数组长度推导
- 结构体数组、结构体指针算术、`->` 链式访问
- `struct` / `union` 1/2/4/8/12/16 字节按值传参/返回、隐藏 sret、按值复制语义
- 条件表达式/二元表达式/`sizeof` 的类型推断，`_Bool`/`void *`/指针兼容性转换
- 十进制/十六进制字面量与 `U`/`L`/`LL` 后缀的 C99 类型选择
- 常量表达式短路求值、溢出诊断；复合移位赋值按左操作数类型执行
- `switch`/fallthrough/嵌套 switch、`goto` 跨块跳转、`enum` 常量与作用域
- `case` 常量、重复 case/default、未知标签等错误诊断
- 字符串/字符转义、相邻字符串拼接
- `printf` 宽度、精度、符号、十六进制/八进制/指针/64 位格式
- include path、include guard、`#pragma once`、命令行宏、条件编译
- 预定义宏
- `-S` LLVM IR、`-c` COFF 目标文件
- 错误诊断（文件/行/列 + 源码 caret）
- 预处理输出
- 后端注册表与未知后端错误

最近一次结果：**104 passed, 0 failed**。

## 性能：瓶颈在哪里

在本机（Windows x64，LLVM 22.1.8）用 `tests/hello.c` 拆分计时，结果大致为：

| 阶段 | 中位耗时 |
|---|---:|
| ETCC 前端（`-S`：预处理/parse/sema/IR 写出） | **~25 ms** |
| `llc -filetype=obj` 把 IR 汇编成 COFF | **~100 ms** |
| `lld-link` 链接 | **~250 ms** |
| 完整 `etcc hello.c`（llc + lld-link） | **~305 ms** |
| 完整 `etcc hello.c`（llc + MSVC link.exe） | **~198 ms** |

结论：**不是 ETCC 前端慢**。`clang.exe` 约 108 MB、`llc.exe` 约 66 MB、`lld-link.exe` 约 72 MB，Windows 上每次启动它们都要付出固定的进程/加载/扫描成本；Hello World 只有 768 字节目标文件，链接和汇编的固定开销远大于实际代码处理。Cavvy 的 `llc+lld` 比 `clang` 快约 200 ms，也是同一个现象。

已经做的优化：

- LLVM 后端优先使用 **`llc -filetype=obj`** 直接生成 COFF；找不到 llc 时回退到 `clang -cc1`，再回退到常规 `clang -c`。
- 驱动新增 `--llc` / `ETCC_LLC` 和 `--linker` / `ETCC_LINK`。
- 使用 MSVC `link.exe` 时，驱动会把 `TMP`/`TEMP` 指向当前目录，避免受限环境下的临时文件问题。
- `-c` 只生成 COFF，不启动链接器；`-S` 只跑前端。

示例：

```bat
set ETCC_LINK=<VS>\VC\Tools\MSVC\<version>\bin\Hostx64\x64\link.exe
build\etcc.exe tests\hello.c -o build\hello.exe
```

如果追求“彻底摆脱巨无霸”，下一步需要：直接生成 COFF 的原生 x86-64 后端，或把 LLVM/LLD 作为库集成，避免每次编译启动外部进程。当前架构已经把这两条路留在 backend 接口之后，前端不需要改。

## 目录结构

```text
include/etcc/          编译器内部头文件
  common.h             arena / 动态字符串 / vector 工具
  source.h             源文件与 source map
  diag.h               诊断
  token.h / lexer.h    词法
  pp.h                 预处理器
  ast.h / parser.h     AST 与语法分析
  ctype.h              C 类型系统
  ir.h                 后端无关 typed IR
  sema.h               语义分析 + IR 降级
  backend.h            后端接口 / 注册表
  platform.h           宿主平台（路径、进程）
src/                   对应实现 + driver.c
runtime/include/       自己的 C 库头文件
runtime/src/           自己的 crt0 + libc 实现
tests/                 端到端测试与测试驱动
docs/                  架构与后端开发文档
CMakeLists.txt         CMake 构建定义
CMakePresets.json      MinGW Makefiles 配置/构建/测试 preset
```

## 设计要点

1. **typed IR 是前后端唯一契约**。IR 类型、值、指令、基本块、terminator、全局对象全部独立定义。
2. **后端通过 ops 表 + 注册表接入**。`EtccBackendOps` 只要求 `emit_object` / `emit_ir`，驱动层不关心实现。
3. **运行时与编译器解耦**。编译器只生成对 `main`、libc 符号的普通 COFF 引用；运行时是独立静态库。
4. **没有 CRT 也能启动**。`etcc_entry` 直接使用 `GetCommandLineA`、`HeapAlloc`、`WriteFile`、`ExitProcess`。
5. **错误不 `exit(1)` 散落在词法层**。诊断统一带 `SourceFile/line/column`，可打印源码行与 caret；解析器有错误恢复。
6. **Windows 路径与引号是显式处理的**。编译器用 `CreateProcessA` 并实现 MSVC 参数引用规则，能处理 `Program Files` 这类路径。
7. **工具链自动发现 + 全部可覆盖**。适合本机开发，也为未来打包安装布局留出路径。

更多细节见：

- [架构说明](docs/ARCHITECTURE.md)
- [后端开发指南](docs/BACKENDS.md)
- [里程碑 0.1](docs/MILESTONE.md)
- [里程碑 0.2：聚合类型与初始化器](docs/MILESTONE-0.2.md)
- [里程碑 0.3：控制流与枚举](docs/MILESTONE-0.3.md)
- [里程碑 0.4：聚合按值 ABI](docs/MILESTONE-0.4.md)

## 当前限制（有意留在后续里程碑）

- 目标固定为 **Windows x64 / Microsoft ABI**；宿主也按 Windows 构建。
- 不支持浮点类型、函数指针、变参函数**定义**、VLA、位域、`_Atomic`。
- 初始化器不支持指定初始化器（`.field =`、`[index] =`）、flexible array member、嵌套花括号省略。
- 全局聚合初始化暂不支持非空指针重定位（例如 `struct { char *p; } g = { "x" };`）；零指针可以。
- 局部 `static` 变量暂未实现。
- 预处理器暂不支持函数式宏、`#` / `##`、`_Pragma`；`-E` 输出是 include/条件编译后的源码，对象宏仍在词法阶段展开，因此 `-E` 文本中不会展开对象宏。
- 一次只接受一个输入文件。
- 没有优化管线，后端以 `-O0` 汇编 IR。
- 编译器尚未自举；`runtime` 当前由 Clang 构建。未来可以让 ETCC 编译自己的运行时，再逐步自举。
- `printf` 不支持浮点格式；运行时暂不提供文件 I/O、`FILE`、`scanf`。

## 路线图

1. **类型系统继续扩展**：函数指针、位域、`_Alignas`、变参函数定义。
2. **初始化器完整化**：指定初始化器、flexible array member、花括号省略、全局指针重定位。
3. **预处理器完整化**：函数式宏、`#`/`##`、宏展开后的 `-E`、`#line`。
4. **语言完整性**：浮点。
5. **运行时扩展**：文件 I/O、`FILE`、更完整的格式化、环境变量、堆调试。
6. **优化与诊断**：常量折叠、死代码消除、类型检查强化、警告分级。
7. **新后端**：x86-64 原生后端（直接生成 COFF/PE）、C 后端、WASM 后端；用同一套测试矩阵验证。
8. **自举**：用 ETCC 构建 runtime，再构建编译器自身。

## 许可

未指定许可证；在加入正式许可证前，默认保留所有权利。
