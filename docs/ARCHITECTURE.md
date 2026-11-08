# ETCC 架构说明

本文描述 ETCC 0.2 的模块边界、数据流、IR 契约和后端扩展方式。

## 1. 总览

```text
┌────────────┐   ┌──────────────┐   ┌────────────┐   ┌──────────────┐
│ .c source  │──►│ preprocessor │──►│   lexer    │──►│    parser    │
└────────────┘   └──────────────┘   └────────────┘   └──────┬───────┘
                                                            │ AST
                                                            ▼
┌────────────┐   ┌──────────────┐   ┌────────────┐   ┌──────────────┐
│ lld-link   │◄──│ COFF object  │◄──│  backend   │◄──│ sema + IR gen│
└─────┬──────┘   └──────────────┘   └────────────┘   └──────────────┘
      │                                      ▲
      ▼                                      │
┌────────────┐                        ┌──────┴───────┐
│   .exe     │◄───────────────────────│ ETCC runtime │
└────────────┘                        │ own libc/crt │
                                      └──────────────┘
```

前端、IR、后端之间没有 LLVM 类型泄漏：

- `include/etcc/ir.h` 不包含任何 LLVM 头；
- `sema.c` 只依赖 `ir.h`；
- `backend_llvm.c` 是唯一包含 LLVM 文本格式知识的模块；
- `driver.c` 只通过 `EtccBackendOps` 调用后端。

## 2. 编译阶段

### 2.1 Preprocessor (`pp.c`)

输入是磁盘文件，输出是“可编译的源码文本”加 `EtccSourceMap`。

- 逐行处理 `#include`、条件编译、`#pragma once`、对象宏定义。
- 保留原始注释和空白，交给 lexer 处理。
- 每个输出行在 `EtccSourceMap` 中登记 `(offset → SourceFile, line)`。
- 词法/语法/语义错误定位时，通过 source map 映射回真实文件与行号。
- 条件编译使用独立栈，支持嵌套、`#elif`、`#else`；文件结束时检查未闭合条件。
- include 搜索顺序：当前文件目录（仅引号形式）→ `-I` 目录 → 运行时头文件目录。
- 对象宏在 **lexer** 中展开（见下），而不是在 PP 文本阶段替换，这样不会破坏字符串、注释和 token 间距。

### 2.2 Lexer (`lexer.c`)

- 输入是 PP 文本 + `EtccMacroTable`。
- 支持完整 C 运算符最大匹配、注释、十进制/十六进制/八进制/二进制整数、字符常量、字符串转义。
- 对象宏通过宏帧（macro frame）在 token 层递归展开，带 active set 防止自递归。
- token 的位置由 source map 计算；宏展开出的 token 使用宏调用点的位置。
- 遇到函数式宏会给出明确错误，而不是静默生成错误代码。

### 2.3 Parser (`parser.c`)

递归下降解析器，直接生成 AST：

- 声明说明符（存储类、限定符、基础类型）与声明符（指针、数组、函数参数）。
- 表达式优先级：赋值 → 条件 → `||` → `&&` → 位或 → 异或 → 位与 → 相等 → 关系 → 移位 → 加减 → 乘除 → cast → unary → postfix → primary。
- 语句：复合、`if`、`while`、`do`、`for`、`break`、`continue`、`return`。
- 解析期维护 typedef 名称表和 tag 名称表（struct/union，按作用域深度）；
- `struct` / `union` 定义、字段声明、前置声明、自引用指针、匿名聚合；
- `enum` 定义与枚举常量、`switch`/`case`/`default`、`goto`/标签；
- 初始化器递归解析为 `AST_INIT_LIST`，支持嵌套、尾随逗号；
- 错误恢复：语句级同步到 `;` / `}`；顶层同步到声明起点；错误过多时停止。
- AST 节点、名称、字符串全部落在 arena 中。

### 2.4 Semantic analysis + IR lowering (`sema.c`)

当前把语义检查与 IR 生成放在同一遍，原因是：

- 这个阶段的类型检查必须知道符号的存储位置（alloca / global）；
- 保持单遍可以更快达到可运行里程碑；
- IR 仍然是独立、完整、可验证的输出，未来可以拆成 HIR + IRGen 两遍而不影响后端。

主要职责：

- 建立全局符号表（函数、全局变量），再分析函数体；
- 局部作用域、块作用域、`for` 作用域；
- 整数提升、常用算术转换、指针算术、赋值兼容性；
- `struct` / `union` 类型完整性、字段查找、Windows x64 布局（偏移/填充/大小/对齐）；
- 生成 typed IR：alloca/load/store/gep/bin/cmp/cast/call；
- 控制流 lowering：`if`、`while`、`do`、`for`、`switch`、`goto`、`&&`/`||` 短路、`?:` 都转换为基本块；
- `switch` 先收集 case/default 并创建目标块，再生成 case 比较链，fallthrough 与 `break` 由块间 `br` 表达；
- 函数体分析前预扫描标签并为每个标签创建块，`goto` 直接跳转；不可达代码继续生成无前驱死块，保证跳入其内部标签仍然有合法 CFG；
- 枚举常量作为 `SYM_ENUM_CONST` 进入普通标识符作用域，表达式上下文直接生成 int 常量；
- 变量声明统一在函数 entry block 分配栈槽，避免循环内重复 alloca；
- 聚合初始化：局部对象按字段/元素递归写入并补零；全局对象在编译期生成字节镜像；
- 生成字符串全局对象、全局变量初始化器；
- 调用 `ir_validate()` 检查每个 block 是否终止。

聚合类型的关键约定：

- `struct` / `union` 的字段偏移由 `ctype.c` 的布局算法一次算好；
- sema 对成员访问统一生成 **字节偏移 GEP**（`getelementptr i8, ptr base, i64 offset`），因此后端不需要理解 C 字段；
- 聚合整体赋值生成 `load <struct> / store <struct>`；
- 聚合按值传参/返回尚未实现，会给出明确诊断；指针传参完全支持。

### 2.5 Backend (`backend.c`, `backend_llvm.c`)

驱动通过注册表选择后端：

```c
const EtccBackendEntry *entry = etcc_backend_find("llvm");
EtccBackend *b = entry->create(arena, diag, &options);
b->ops->emit_object(b, module, obj_path, diag);
```

LLVM 后端的职责非常窄：

1. 把 `IrModule` 序列化为带 `x86_64-pc-windows-msvc` triple 的 LLVM 文本 IR；
2. 优先调用 `llc -filetype=obj` 生成 COFF，其次回退到 `clang -cc1 -emit-obj`，最后回退到公开的 `clang -c -x ir`；
3. 失败时把诊断交回驱动。

性能上，ETCC 前端处理 Hello World 只需约 20–25 ms；完整编译的固定成本主要来自 `llc.exe`（约 66 MB）、`clang.exe`（约 108 MB）和 `lld-link.exe`（约 72 MB）的进程启动。后端默认走 llc 已减少一层 clang driver 开销；驱动也支持 `--linker`/`ETCC_LINK` 切换到更轻的 MSVC `link.exe`。

它不负责：

- include/宏/语法；
- 类型检查；
- 运行时链接参数；
- 可执行文件入口点选择。

### 2.6 Driver (`driver.c`)

- 参数解析与默认输出名；
- 工具链发现（clang / lld-link / kernel32.lib / runtime）；
- 调用 PP → Parser → Sema → Backend；
- 链接：
  ```text
  lld-link /entry:etcc_entry /subsystem:console /nodefaultlib
          app.obj etccrt.lib kernel32.lib /out:app.exe
  ```
- 临时文件位于当前工作目录（`.etcc-<pid>-<n>.*`），成功/失败后清理；`--save-temps` 保留目标文件。
- 进程调用不经过 `system()`，而是自行按 MSVC 规则引用参数后调用 `CreateProcessA`，因此能安全处理 `C:\Program Files (x86)\...`。

## 3. IR 契约

### 3.1 类型

```c
IR_TYPE_VOID
IR_TYPE_I8 / I16 / I32 / I64
IR_TYPE_PTR       // 带 pointee 信息，LLVM 输出为 opaque ptr
IR_TYPE_ARRAY
IR_TYPE_STRUCT    // 只携带 size/align；LLVM 输出为 [size x i8]
```

IR 指针保留 `elem` 是为了：

- 前端/GEP 表达元素类型；
- 未来 native 后端计算步长；
- LLVM 后端仍统一输出 `ptr`，不依赖 typed pointer。

`IR_TYPE_STRUCT` 不携带字段列表。C 的字段偏移在 sema/ctype 中已经解析为字节偏移，IR 只需知道对象大小和对齐。这样：

- LLVM 后端把结构体当作 `[N x i8]`，天然支持任意布局、联合体和填充；
- 成员访问是字节 GEP + 显式类型的 load/store；
- 新后端不需要复制 C 的字段布局算法。

`LOAD` 指令显式携带结果类型（`ir_build_load(..., type)`），因为字节 GEP 得到的指针 IR 类型可能是 `i8*`，而实际读取的字段可能是 `i32`/`ptr`/结构体。

### 3.2 值

- `IR_VALUE_CONST_INT`：整数常量；
- `IR_VALUE_CONST_NULL`：空指针；
- `IR_VALUE_GLOBAL`：全局对象地址；
- `IR_VALUE_FUNCTION`：函数地址；
- `IR_VALUE_PARAM`：函数参数；
- `IR_VALUE_INSTRUCTION`：指令结果。

每个值有模块内唯一 `id`。LLVM 后端把参数命名为 `%p<id>`，指令结果命名为 `%t<id>`。

### 3.3 指令

| IR | 语义 | LLVM 输出 |
|---|---|---|
| `ALLOCA` | 栈槽 | `alloca` |
| `LOAD` | 读取 | `load` |
| `STORE` | 写入 | `store` |
| `GEP` | 指针/数组下标/结构体字节偏移 | `getelementptr` |
| `BIN` | 整数二元运算 | `add/sub/mul/sdiv/...` |
| `CMP` | 比较并产生 C 的 `int` 0/1 | `icmp` + `zext` |
| `CAST` | 截断/扩展/指针转换 | `trunc/zext/sext/ptrtoint/inttoptr` |
| `CALL` | 直接函数调用 | `call` |

`LOAD`/`STORE` 的聚合类型在 LLVM 中表现为 `[N x i8]`；`GEP` 既可以按数组元素类型缩放，也可以用 `i8` 做字节偏移。聚合初始化在 sema 中已经被拆成语义明确的逐字段/逐元素存储，因此 IR 不需要“初始化器”指令。

`CMP` 在 IR 层直接产生 `i32` 的 0/1，而不是 `i1`。这样 C 语义（比较结果是 `int`）在前端就固定下来，后端不需要知道 C 的布尔表示。LLVM 后端用两条 LLVM 指令实现。

### 3.4 基本块与终止符

- `IR_TERM_RET`：`ret`，值可为空表示 `void`；
- `IR_TERM_BR`：无条件跳转；
- `IR_TERM_COND_BR`：条件跳转，条件值是 `i32`，后端负责与 0 比较。

没有 PHI。局部变量统一用 alloca/load/store，短路与三元表达式用临时槽。这样 IR 更容易被多个后端消费，也避免前端实现 SSA 构造。

### 3.5 全局对象与函数

- `IrGlobal`：名字、类型、是否 internal/external/const、字符串/字节初始化数据、整数或指针初始化器；
- `IrFunction`：返回类型、参数类型数组、是否变参、是否定义、参数值、基本块链表；
- 函数声明与定义共用同一个 `IrFunction`，`is_definition` 区分；
- 字符串字面量生成 private unnamed_addr constant；
- 全局聚合初始化生成静态字节镜像，LLVM 声明为 `[N x i8]` + `c"..."`，成员/元素访问仍按真实 C 类型做 GEP/load；
- 非空指针重定位（聚合里的 `char *p = "x"`）尚未支持，避免把地址写死进字节数据。

## 4. 运行时与 ABI

### 4.1 入口

`runtime/src/crt0.c`：

```c
extern int main(int argc, char **argv);
__declspec(noreturn) void etcc_entry(void) {
    int argc;
    char **argv;
    build_args(&argc, &argv);       // GetCommandLineA + 自写解析
    ExitProcess((unsigned)main(argc, argv));
}
```

- 链接器入口：`/entry:etcc_entry`；
- `/nodefaultlib`：不链接任何系统 CRT；
- 只导入 `kernel32.lib`；
- `main(void)` 与 `main(int, char**)` 都可用；x64 ABI 下给 `main(void)` 多传两个参数是安全的；
- `argc/argv` 由 `GetCommandLineA` 自行切分，支持引号和空格。

### 4.2 Windows ABI 约定

- `x86_64-pc-windows-msvc`：Microsoft x64 ABI；
- `long` 为 32 位（LLP64），`long long` 为 64 位；
- `char` 默认有符号；
- 指针对齐 8；
- 参数/返回使用 RCX/RDX/R8/R9 与 XMM 寄存器；寄存器溢出使用 shadow space；
- 变参调用由 Clang/MSVC ABI 一致处理。

### 4.3 自有限制

运行时是“可运行的 libc 子集”，不是完整 ISO C：

- 没有 `FILE`、`fopen`、`fread`、`fwrite`；
- `printf` 仅支持整数/字符/字符串/指针格式；
- 没有浮点格式化、locale、宽字符；
- `malloc` 直接使用进程堆；
- 没有 atexit、线程、TLS、异常、SEH。

## 5. 诊断与错误模型

- `EtccLoc { SourceFile*, line, column }`；
- 所有诊断统一走 `diag.c`，格式：
  ```text
  path:line:col: error: message
      source line
      ^
  ```
- 预处理器、词法、语法、语义、IR 校验、后端和链接失败都能回到统一的错误计数；
- 默认最多 25 个错误后停止，避免错误级联淹没输出；
- 解析器错误恢复后继续分析，尽量一次给出多个错误。

## 6. 内存与生命周期

- 编译器是短生命周期进程，所有 AST、IR、类型、名称、宏体使用 arena；
- PP 的 include 目录、once 集合、source map 段使用 malloc，并显式释放；
- IR 只在编译期间存在；对象文件生成后模块即可丢弃；
- 不追求编译器进程内的细粒度释放，优先保证正确性和实现清晰。

## 7. 已知架构债

1. 语义分析与 IR 生成在同一遍，未来可拆为 typed AST/HIR + IRGen。
2. 预处理器是行级的，函数式宏、`#`/`##`、跨行宏续行尚未实现。
3. `-E` 不展开对象宏（对象宏在 lexer 阶段才展开）。
4. typedef/tag 名称仅由 parser 维护，语义阶段没有独立的 tag/typedef 命名空间。
5. 全局 tentative definition 的重复声明规则还不完整。
6. 临时文件放在当前工作目录，若在只读目录运行需要显式指定可写目录（后续可加 `TMPDIR`/输出目录策略）。
7. 聚合按值传参/返回、位域、指定初始化器、flexible array member 尚未实现。
8. 全局聚合初始化暂不支持非空指针重定位。
9. `switch` 目前降级为线性 case 比较链，尚无 jump table 优化；`goto` 的死块策略会生成少量不可达块。
10. 枚举类型统一按 `int` 表示，尚无底层类型选择/`_Generic` 等机制。

这些债不会阻碍新增后端：后端只消费 IR，不依赖前端内部结构。
