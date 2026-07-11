# Rainfall

Windows 桌面屏幕雨幕效果。程序启动后在所有显示器上叠加透明雨滴动画，不拦截鼠标和键盘，适合作为桌面氛围工具。

## 功能

- 全屏透明雨幕，支持多显示器
- 鼠标穿透，不影响正常操作
- 系统托盘控制，右键菜单操作
- 所有设置自动保存，重启后恢复

## 托盘菜单

| 菜单项 | 说明 |
|--------|------|
| 暂停 / 继续 | 暂停或恢复雨滴动画 |
| 浅色背景增强 | 勾选后雨滴变为浅灰色，在白色背景下更易看见 |
| 雨滴长度 | 5 档（1 / 2 / 默认 / 4 / 5） |
| 雨滴密度 | 5 档（1 / 2 / 默认 / 4 / 5） |
| 风力 | 5 档，风力越大雨越倾斜 |
| 雨势 | 5 档，雨势越大雨落得越快 |
| 退出 | 关闭程序 |

## 配置保存

设置保存在：

```
%AppData%\Rainfall\config.ini
```

示例：

```ini
[Rainfall]
LightMode=0
LengthLevel=2
DensityLevel=2
WindLevel=2
SpeedLevel=2
```

`Level` 值为 0~4，对应 1 档到 5 档，默认值为 `2`（第 3 项「默认」）。

## 系统要求

- Windows 10 / 11
- x64

## 下载

在 [Releases](https://github.com/zhongwcool/Rainfall/releases) 页面下载 `Rainfall-vX.X.X-x64.zip`，解压后运行 `Rainfall.exe` 即可。

## 本地编译

需要 Visual Studio 2022 或更高版本，并安装「使用 C++ 的桌面开发」工作负载。

```powershell
msbuild Rainfall\Rainfall.vcxproj /p:Configuration=Release /p:Platform=x64 /m
```

编译产物路径：

```
Rainfall\x64\Release\Rainfall.exe
```

## 自动发布

推送 `v*` 格式的 tag 会触发 GitHub Actions，自动编译并发布到 Releases：

```bash
git tag v1.0.0
git push origin v1.0.0
```

工作流定义见 [.github/workflows/release.yml](.github/workflows/release.yml)。

## 技术栈

- C++20
- Win32 API
- Direct2D（雨滴渲染）
- 分层透明窗口（`WS_EX_LAYERED` + `UpdateLayeredWindow`）

## 许可证

[MIT License](LICENSE)
