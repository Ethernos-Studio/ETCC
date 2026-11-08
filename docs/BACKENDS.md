# ETCC 后端开发指南

ETCC 的核心设计目标之一是：**换后端不碰前端**。本文说明后端接口、Hello World 的 IR 形态、聚合类型的 IR 表示、链接契约，以及新增后端的最小步骤。

## 1. 后端接口

定义在 `include/etcc/backend.h`：

```c
typedef struct EtccBackendOps {
    const char *name;
    const char *description;

    bool (*emit_object)(EtccBackend *b, const IrModule *m,
                        const char *obj_path, EtccDiagnostics *diag);

    bool (*emit_ir)(EtccBackend *b, const IrModule *m,
                    const char *path, EtccDiagnostics *diag);

    void (*destroy)(EtccBackend *b);
} EtccBackendOps;

struct EtccBackend {
    const EtccBackendOps *ops;
    void *impl;
};
```

要求：

- `emit_object` 生成 **Windows x64 COFF 目标文件**，因为驱动固定用 `lld-link` 链接；
- `emit_ir` 输出该后端自己的文本表示，用于 `-S` / 调试；
- `destroy` 释放后端私有资源；arena 分配的资源可忽略；
- 诊断必须通过 `EtccDiagnostics` 返回，不要直接 `exit()`。

驱动只依赖这两个入口，不关心后端是：

- LLVM 文本 IR；
- 直接机器码；
- C 源码；
- WASM；
- 解释执行。

## 2. 当前 LLVM 后端

`src/backend_llvm.c` 的流程：

```text
IrModule
  │
  ├─ 序列化为 .ll（target triple = x86_64-pc-windows-msvc）
  │
  ├─ clang --target=x86_64-pc-windows-msvc -c -x ir ...
  │
  └─ COFF .obj
```

关键选择：

- 文本 IR 而不是 LLVM C++ API：编译器不链接 LLVM 库，构建轻量，LLVM 版本升级成本低；
- `-O0`：不做优化，保持 IR 到机器码的映射清晰，便于调试；
- `-ffreestanding -fno-stack-protector -fno-asynchronous-unwind-tables`：避免引入宿主 CRT 依赖；
- 所有指针统一输出 opaque `ptr`，但 IR 指针保留 pointee 供 GEP/未来后端使用。

## 3. Hello World 的 IR

源程序：

```c
#include <stdio.h>
int main(void) {
    puts("Hello, World!");
    return 0;
}
```

`etcc -S` 输出（节选）：

```llvm
target triple = "x86_64-pc-windows-msvc"

@.str.0 = private unnamed_addr constant [14 x i8] c"Hello, World!\00", align 1

declare i32 @puts(ptr)

define i32 @main() {
entry:
  %t3 = getelementptr inbounds i8, ptr @.str.0, i64 0
  %t4 = call i32 @puts(ptr %t3)
  ret i32 0
}
```

对应 IR 结构：

- `IrGlobal`：`.str.0`，类型 `[14 x i8]`，字符串初始化；
- `IrFunction`：`main`，返回 `i32`，无参数；
- 入口 block：`GEP` 把“指向数组的指针”衰减为“指向首字符的指针”；
- `CALL`：直接调用 `puts`；
- `RET`：`i32 0`。

## 3.1 聚合类型在 IR 中的样子

`struct` / `union` 不在 IR 中保留字段表：

```c
IR_TYPE_STRUCT { size, align }
```

LLVM 后端把它输出为 `[size x i8]`。例如：

```llvm
%p = alloca [8 x i8], align 4
%x = getelementptr inbounds i8, ptr %p, i64 0
store i32 3, ptr %x, align 4
%y = getelementptr inbounds i8, ptr %p, i64 4
store i32 4, ptr %y, align 4
%whole = load [8 x i8], ptr %p, align 4
```

约定：

- 字段偏移由前端算好，后端只做 **字节 GEP**；
- `LOAD` 的结果类型显式给出（不能从字节指针推断）；
- `STORE` 的类型由值决定；
- 聚合整体赋值是 `load [N x i8]` + `store [N x i8]`；
- 全局聚合初始化是 `[N x i8] c"..."` 静态字节镜像，成员访问仍按真实字段类型 GEP/load；
- 后端不需要实现 C 的布局算法，也不要假设 `[N x i8]` 的对齐等于 1（必须使用 IR 的 align）。

如果新后端直接生成机器码，也应该把聚合对象视为“带大小/对齐的连续字节”，并遵守上述 load/store/GEP 类型。

## 4. 链接契约

驱动最终执行：

```bat
lld-link /nologo /entry:etcc_entry /subsystem:console /nodefaultlib ^
         app.obj etccrt.lib kernel32.lib /out:app.exe
```

后端必须保证：

1. 目标文件是 **COFF x86-64**；
2. 用户 `main` 符号被定义（或链接阶段明确报错）；
3. 运行时符号（`puts`、`printf`、`malloc` 等）使用 C 名字、没有额外名称修饰；
4. 不引用 MSVC CRT 符号（除非未来显式更换链接契约）；
5. 遵守 Microsoft x64 ABI，尤其是：
   - 整数参数寄存器 RCX/RDX/R8/R9；
   - 32 字节 shadow space；
   - 16 字节栈对齐；
   - 变参调用约定与 Clang/MSVC 一致。

如果新后端生成的目标文件格式不同（ELF/Mach-O），需要同时扩展驱动链接层；当前 `EtccBackendOps` 的隐含契约是 COFF。

## 5. 新增后端的最小步骤

### 第一步：实现 ops

新建 `src/backend_native.c`：

```c
typedef struct {
    EtccArena *arena;
    EtccDiagnostics *diag;
} NativeBackend;

static bool native_emit_object(EtccBackend *b, const IrModule *m,
                               const char *obj_path, EtccDiagnostics *diag) {
    NativeBackend *nb = b->impl;
    (void)nb;
    /* 遍历 m->globals / m->functions / blocks / instructions，
       生成 COFF 或先写汇编再调用汇编器。 */
    return true;
}

static bool native_emit_ir(EtccBackend *b, const IrModule *m,
                           const char *path, EtccDiagnostics *diag) {
    /* 可选：输出自定义汇编/文本 IR。 */
    return true;
}

static void native_destroy(EtccBackend *b) { (void)b; }

static const EtccBackendOps native_ops = {
    .name = "native",
    .description = "direct x86-64 COFF backend",
    .emit_object = native_emit_object,
    .emit_ir = native_emit_ir,
    .destroy = native_destroy,
};

EtccBackend *etcc_backend_native_create(EtccArena *arena,
                                        EtccDiagnostics *diag,
                                        const EtccBackendOptions *opt) {
    NativeBackend *nb = etcc_arena_alloc(arena, sizeof(*nb), _Alignof(NativeBackend));
    nb->arena = arena;
    nb->diag = diag;
    EtccBackend *b = etcc_arena_alloc(arena, sizeof(*b), _Alignof(EtccBackend));
    b->ops = &native_ops;
    b->impl = nb;
    return b;
}
```

### 第二步：注册

在 `src/backend.c` 的表中加入：

```c
static const EtccBackendEntry backends[] = {
    {"llvm", etcc_backend_llvm_create},
    {"native", etcc_backend_native_create},
};
```

并在 `include/etcc/backend.h` 声明工厂。

### 第三步：使用

```bat
etcc --backend native tests\hello.c -o build\hello.exe
```

前端、IR、运行时、驱动链接逻辑都不需要改。

## 6. 后端实现检查清单

遍历 `IrModule` 时至少处理：

- [ ] `IrGlobal`：internal/external/const、字符串、整数初始化、指针初始化、零初始化、对齐；
- [ ] `IrFunction`：声明/定义、internal、参数、变参；
- [ ] `IrBlock`：顺序、终止符；
- [ ] `ALLOCA`：类型、对齐；
- [ ] `LOAD/STORE`：显式结果类型、对齐、聚合按字节复制；
- [ ] `GEP`：数组元素缩放、`i8` 字节偏移、指针衰减；
- [ ] 聚合类型：`IR_TYPE_STRUCT` 的 size/align，不要重新实现 C 字段布局；
- [ ] `BIN`：有符号/无符号除法、余数、算术右移/逻辑右移；
- [ ] `CMP`：有符号/无符号比较，结果为 C 的 `int` 0/1；
- [ ] `CAST`：trunc/zext/sext/ptrtoint/inttoptr；
- [ ] `CALL`：直接调用、返回 void、变参调用；
- [ ] `RET/BR/COND_BR`：条件值 `i32` 与 0 比较；
- [ ] 字符串常量中的 NUL 与任意字节转义。

## 7. 测试新后端

现有测试是端到端黑盒，只要新后端能生成 COFF 并链接运行，直接跑：

```bat
cmake --build --preset mingw
ctest --preset mingw
```

或用 `cmake --build --preset mingw --target check` 一次完成构建 + 测试。

额外建议：

1. 用 `-S` 输出对照检查；
2. 用 `llvm-objdump -d` 检查调用约定；
3. 增加 ABI 边界测试：多参数、64 位整数、指针、变参 `printf`、递归；
4. 用 `--backend` 跑同一批测试，确保前端没有隐藏 LLVM 假设；
5. 增加 `dump IR` 的 golden test，避免 backend 依赖未定义行为。

## 8. 常见坑

- **不要把 IR 指针当作 LLVM typed pointer**：IR 的 `elem` 只用于语义/GEP，LLVM 输出必须是 `ptr`。
- **不要根据指针类型推断 LOAD 类型**：字节 GEP 后的指针可能是 `i8*`，但实际字段可能是 `i32`/`ptr`/聚合；必须使用指令的显式结果类型。
- **不要假设 C 的 `int` 是 64 位**：Windows LLP64 下 `long` 是 32 位。
- **不要忽略全局聚合的字节镜像**：`has_bytes_init` 的全局对象定义类型是 `[N x i8]`，但成员访问可能使用任意字段类型；opaque pointer 下这是合法的。
- **不要在 `CMP` 后端直接返回 `i1`**：IR 结果是 `i32`，要 `zext` 或直接生成 0/1。
- **不要忽略 `IR_TERM_NONE`**：`ir_validate` 会报错；新后端也应拒绝未终止 block。
- **不要从后端调用前端**：后端只消费 IR；需要额外信息时扩展 IR，而不是反向依赖 AST。
- **不要破坏 `/nodefaultlib`**：一旦引入 CRT 符号，Hello World 就不再是“自己的 C 库”里程碑。
