# LiteMon 2.0.0 — Agent 协作规范

## 已知陷阱

### clangd LSP 误报 Qt6 类型（反复出现，禁止因 LSP 报错改代码）

clangd（opencode 内置 LSP）在 Qt6/CMake 项目中持续报大量假阳性错误：
```
ERROR [35:33] Unknown type name 'QString'
ERROR [35:59] Allocation of incomplete type 'QLabel'
note: forward declaration of 'QLabel'
```
**根因：** clangd 未继承 CMake 的 Qt6 include 路径（`find_package(Qt6)` 设置的路径不自动传递给 clangd）。
**结论：** 纯误报，`cmake --build --preset dev` 编译 clean，4/4 test pass。
**规则：** 遇到 Qt 类型 LSP 报错时，**直接忽略**，只用 `cmake --build` 验证真实编译结果。不要因为 LSP 报错就添加额外的 `#include` 或修改代码结构。

## 构建命令

```bash
cmake --build --preset dev          # 开发版编译
cmake --build --preset release      # 发布版编译
ctest --preset dev                  # 运行测试（4/4）
```

## 部署

- 二进制：`~/.local/bin/litemon`
- 桌面文件：`~/.local/share/applications/io.github.litemon.LiteMon.desktop`
- Collector 服务：`litemon-collector.service`（systemd --user）
- GUI：`litemon-gui` transient unit（`systemd-run --user`，Restart=on-failure）
