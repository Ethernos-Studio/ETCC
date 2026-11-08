# 里程碑 0.2：聚合类型与初始化器

## 目标

在 0.1（Windows x64 Hello World + 自有 C 运行时）之上，让 ETCC 能处理真实 C 程序里最常见的聚合数据：

- 具名/匿名 `struct`、`union`；
- 前置声明、自引用指针、嵌套结构体；
- Windows x64 自然对齐布局；
- `.` / `->` 成员访问；
- 结构体/联合体整体赋值；
- 结构体数组、结构体指针算术；
- 局部/全局 `{ ... }` 初始化；
- 字符串数组长度推导与补零；
- 部分初始化补零；
- 全局聚合初始化的静态字节镜像。

仍然保持：

- 后端无关 IR；
- LLVM 后端可替换；
- `/nodefaultlib` + 自有 C 运行时；
- 现有 Hello World 和 49 个测试不回归。

## 示例

```c
#include <stdio.h>

struct Point { int x; int y; };
struct Rect  { struct Point a; struct Point b; };

struct Rect g = {{1, 2}, {3, 4}};
char gs[] = "hello";

int main(void) {
    struct Rect r = g;
    struct Rect *p = &r;
    printf("%d %d %s\n", p->b.y, (int)sizeof(struct Rect), gs);
    return 0;
}
```

```bat
build\etcc.exe tests\structs3.c -o build\structs3.exe
build\structs3.exe
```

## 设计

### 类型系统

`EtccCType` 增加：

```c
const char *tag;
EtccField *fields;
bool is_complete;
const void *identity;
```

- `identity` 用于 tag 类型兼容性；同一 tag 的完整/不完整/const 副本保持同一 identity；
- 布局在 parser 定义完字段后一次计算：结构体按字段顺序对齐、填充、末尾补齐；联合体所有字段偏移 0、大小取最大、对齐取最大；
- 不完整类型允许出现在指针中，但不能作为对象、字段（除非指针）、数组元素或 `sizeof`。

### 成员访问

sema 把成员访问解析成 **字节偏移 GEP**：

```c
addr = gep i8, base, field_offset
```

字段类型由 C 类型系统给出，load/store 使用显式类型。这样：

- LLVM 后端不需要理解 `struct` 字段；
- union 与 padding 天然支持；
- 新后端只需要实现“按字节偏移取地址 + 显式类型读写”。

### 聚合赋值

```c
struct S a = b;
a = c;
```

生成：

```llvm
%v = load [N x i8], ptr %src, align A
store [N x i8] %v, ptr %dst, align A
```

### 初始化

- 局部聚合：按字段/元素递归生成 store；先对整体补零，再写入给定项；
- 全局聚合：在编译期生成 `size` 字节镜像，LLVM 输出为 `@g = global [N x i8] c"..."`；
- 字符串数组：`char s[] = "abc"` 推导为长度 4；`char buf[8] = "hi"` 其余补零；
- 数组长度推导：`int a[] = {1,2,3}` → 长度 3；
- 部分初始化：缺失字段/元素补零；
- 非空指针重定位（聚合中的 `char *p = "x"`）暂不支持，会给出明确诊断。

## 测试

新增 19 个测试项，覆盖：

- 结构体字段、嵌套、`sizeof`、填充；
- `->`、成员链、结构体指针、结构体数组、指针算术；
- 自引用链表、前置声明、`typedef struct`；
- 局部/全局 `struct`/`union` 初始化；
- 部分初始化补零；
- 字符串数组与长度推导；
- 结构体/联合体整体赋值；
- 局部 tag 作用域；
- 聚合错误诊断：按值传参、不完整类型、字符串过长、重复定义、成员不存在。

当前总结果：

```text
68 passed, 0 failed
```

## 仍然不支持

- `enum`；
- 结构体/联合体按值传参、返回（会明确报错）；
- 位域、flexible array member；
- 指定初始化器 `.field =`、`[index] =`；
- 嵌套花括号省略（必须显式嵌套）；
- 全局聚合中的非空指针重定位；
- 聚合条件表达式（`cond ? s1 : s2`）。

这些进入下一阶段，不影响现有 IR/后端契约。
