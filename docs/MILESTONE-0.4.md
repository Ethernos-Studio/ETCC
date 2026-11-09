# 里程碑 0.4：聚合按值传参/返回

## 目标

补齐 0.2 留下的最大限制：`struct` / `union` 不再只能通过指针传递，而是支持 C 的按值语义，并遵循 **Windows x64 MSVC ABI**。

支持范围：

- `struct` / `union` 作为函数参数；
- `struct` / `union` 作为函数返回值；
- 小聚合（1/2/4/8 字节）走整数寄存器；
- 大聚合走隐藏指针 / `sret`；
- 按值复制语义：被调函数修改参数不影响调用者；
- 与 Clang 编译的 C 代码双向 ABI 互操作。

## Windows x64 聚合 ABI

ETCC 在**前端/sema**完成 ABI lowering，IR 和后端只看到标量或指针参数：

| 聚合大小 | 参数 | 返回 |
|---|---|---|
| 1 字节 | `i8` 寄存器 | `i8` |
| 2 字节 | `i16` 寄存器 | `i16` |
| 4 字节 | `i32` 寄存器 | `i32` |
| 8 字节 | `i64` 寄存器 | `i64` |
| 其他（3、12、16、...） | `ptr` 指向调用者副本 | 隐藏 `sret` 指针，函数返回 `void` |

例如：

```c
struct Pair { int a, b; };          /* size 8  -> i64 */
struct Big  { int a, b, c; };       /* size 12 -> ptr / sret */

struct Pair make_pair(int a, int b);
struct Big  make_big(int x);
```

lowering 后：

```llvm
define i64 @make_pair(i32 %a, i32 %b)
define void @make_big(ptr %sret, i32 %x)
```

调用点：

```llvm
%p = call i64 @make_pair(i32 1, i32 2)
%s = alloca [12 x i8], align 4
call void @make_big(ptr %s, i32 10)
```

## 实现方式

关键决定：**ABI 转换放在 sema，而不是 LLVM backend**。这样 IR 仍然保持简单的标量/指针函数签名，任何后端（LLVM、未来的 Whippet/原生后端）都能直接消费。

- `IrFunction.has_sret` 标记大聚合返回；
- 大聚合参数在 IR 中是 `ptr`，调用者先复制到临时对象；
- 小聚合参数在 IR 中是整数，函数入口把它存入本地槽；
- 大聚合返回在 IR 中是 `void` + 首个隐藏 `ptr` 参数；
- 小聚合返回在 IR 中是标量，调用点再展开成聚合临时对象；
- 成员访问支持聚合右值（例如 `make_pair(1, 2).a`）。

## 与 C/Clang 的互操作验证

除了 ETCC 内部测试，还做了双向交叉验证：

1. **C callee → ETCC caller**：Clang `-c` 编译带聚合参数/返回的 C 函数，ETCC 编译调用方，`lld-link` + ETCC runtime 链接运行；
2. **ETCC callee → C caller**：ETCC `-c` 编译聚合函数，Clang 编译 C 调用方并链接运行。

覆盖 1/8/12 字节聚合、`sret`、小聚合寄存器、参数按值修改不影响调用者。

## 测试

新增：

- `tests/aggregate_abi.c`：1/8/12 字节参数与返回；
- `tests/aggregate_abi2.c`：2/4/8/12/16 字节、3 字节、union、按值复制；
- `tests/aggregate_abi3.c`：多聚合参数、聚合原型、右值成员访问；
- 错误诊断：聚合参数类型不匹配、聚合返回类型不匹配。

当前总结果：

```text
104 passed, 0 failed
```

## 后续正确性修正

在 0.4 基础上补了以下 sema/常量的回归测试：

- `tests/type_inference.c`：二元/条件表达式、`sizeof`、十进制/十六进制字面量与 `U`/`L`/`LL` 后缀的类型推断；
- `tests/conversions.c`：`_Bool` 与指针、`void *` 与对象指针转换；
- `tests/const_short.c`：`&&`/`||`/`?:` 常量短路，`_Bool` 常量转换；
- `tests/compound_shift.c`：复合移位赋值的左操作数类型规则；
- 错误用例：隐式不兼容指针、void 到值、常量有符号溢出。

## 仍然不支持

- 聚合通过可变参数传递（`printf("%d", pair)` 会明确报错）；
- 变参函数定义；
- 函数指针；
- 浮点聚合字段（浮点类型整体尚未支持）；
- `__m128` / 向量 ABI；
- 位域、flexible array member。

这些不改变本次 ABI lowering 的结构；后续可以在同一层扩展。
