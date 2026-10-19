# Niri 26.04 / QQ 3.2.34 DMA-BUF 兼容候选

状态：已实现默认关闭的 LINEAR DMA-BUF 路径，完成 34 项离线检查、ASan/UBSan 检查和本机 EGL/GBM 探针。2026-10-19 用户实测后确认共享正常；本地日志也确认实际取到 2560×1440、单平面、modifier=0 的 DMA-BUF 帧。本机首次共享验证通过；跨驱动兼容性、重复启停、窗口缩放及长时间稳定性仍需分别验证。

## 已确认的问题

2026-10-19 17:46:14，Portal 成功返回 PipeWire node 100。QQ 随后连接该节点，但 PipeWire link `(100.0.0 -> 103.0.0)` 报：

```text
negotiating -> error no more input formats (-22) (configure-configure)
```

QQ 的 EnumFormat 已包含 BGRx，尺寸范围也覆盖实际的 2560×1440，但没有 modifier 属性。Niri 提供 BGRx，并要求 `MANDATORY | DONT_FIXATE` 的 modifier 属性。双方缺少可兼容的缓冲区格式声明。

本机保存的完整日志和上轮分析位于：

```text
~/.local/state/linuxqq-wayland-fix/share-failure-20261019-174818/
```

## 当前版本的能力

核对 Niri 官方 v26.04 的 `src/screencasting/pw_utils.rs`：

- `make_video_params()` 总是提供 mandatory modifier。
- 收到的格式缺少 modifier 时会停止采集。
- Buffers 的 dataType 仅包含 DmaBuf，实际分配使用 GBM。

这是当前发行版路径的结论，不代表所有新版本或开发分支都没有 SHM 支持。

核对本机 `/opt/QQ/resources/app/avsdk/broadcast-core.so`，版本 3.2.34-53644：

- `OnStreamProcess`（0x52af0）已有 `SPA_DATA_DmaBuf` 分支。
- 该分支从第 0 平面读取 fd、stride、offset，以及协商出的 modifier，调用 `GL_OpenSharedTexture`（0x4fb20）。
- `GL_OpenSharedTexture` 通过 `eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT)` 和 `glEGLImageTargetTexture2DOES(GL_TEXTURE_2D)` 导入。
- 该导入器只传第 0 平面，不能把驱动支持的所有 modifier 直接当成 QQ 支持的 modifier。
- EGLDisplay 位于 capture 对象 +0x10；上次导入的 fd 缓存位于 +0x1f4。候选只在对应函数机器码特征匹配时使用这些偏移。

因此优先补齐 QQ 的 DMA-BUF 协商声明，复用已有导入路径；当前不新增 GPU 到 CPU 的中转层。

## 本机 GPU 验证

独立进程在 X11 EGLDisplay 上查询并分配 2560×1440 XRGB8888 缓冲区，再尝试导入 EGLImage 和 GL_TEXTURE_2D。未读取屏幕或注入运行中的 QQ。

环境：Mesa Intel Iris Xe (RPL-P)，`/dev/dri/renderD128`。

| modifier | 实际平面数 | EGL / GL_TEXTURE_2D 导入 | 首个候选 |
| --- | ---: | --- | --- |
| LINEAR，0 | 1 | 成功 | 使用 |
| Intel X tiled，0x0100000000000001 | 1 | 成功 | 暂不使用 |
| Intel Y tiled，0x0100000000000002 | 1 | 成功 | 暂不使用 |
| 0x0100000000000006 | 2 | 未尝试单平面导入 | 排除 |
| 0x0100000000000008 | 3 | 未尝试单平面导入 | 排除 |

该探针验证了独立进程中的分配与导入，不等于验证了 QQ 的编码、跨 GPU 路径或对端画面。

## 候选实现

显式设置 `QQ_WL_DMABUF=linear` 才启用，默认行为保持原样。

1. `wrap_add_listener` 保存已识别 QQ 版本的 capture 对象，与可单独关闭的纹理复用逻辑分开。
2. `wrap_stream_connect` 查询该对象实际使用的 EGLDisplay。只有 XRGB8888 LINEAR 可作为 GL_TEXTURE_2D 导入时才提供 DMA-BUF 候选；不创建或切换 QQ 的 GL context。
3. 在 QQ 原有格式之前增加 `BGRx + mandatory modifier=0` 的 EnumFormat，保留原来的尺寸、帧率和其他属性，也保留原有格式作为其他生产端的回退选项。不声明 DONT_FIXATE，因为本候选只有一个已固定的 modifier。
4. 格式回调记录实际尺寸、modifier 和就绪状态。DMA-BUF 不经普通内存 memcpy，交由 QQ 的已有导入器处理。
5. 取帧时验证已经协商 LINEAR BGRx、只有一个平面、fd 有效、chunk 存在且 stride 至少为 width×4。非法帧立即归还 PipeWire，不能让 QQ 把多平面缓冲区误作单平面使用。
6. DMA-BUF 的 maxsize 不作为普通像素内存大小检查。当前 Niri 可以将其设置为 1，不能据此判定缓冲区太小。
7. 重新协商格式或尺寸后，令 QQ 的上次导入 fd 缓存失效，避免 fd 号码相同但底层缓冲区已经改变时继续使用旧纹理。

新增的是格式声明和已有导入路径的边界检查，不是把 DMA-BUF 类型伪装成 MemPtr，也不是删除生产端的 mandatory 标志。

EGL 通过 QQ 已加载的 libEGL 动态查询，没有增加项目的 EGL/GBM 生产构建依赖。上面的独立 GPU 探针使用 EGL/GBM 开发库，仅用于本机调查。

## 仍需验证的限制

- QQ 在 modifier 为 0 时省略 EGL modifier 属性。Khronos 规范规定省略属性时布局解释由实现决定，并不普遍等价于显式 LINEAR。本机对实际分配的 LINEAR 缓冲区测试了两种导入方式，均成功；候选不能据此保证所有驱动都兼容。
- 当前不承诺显式同步、跨 GPU 导入或多平面压缩 modifier 支持，不声明新的 SyncTimeline 元数据能力。Niri v26.04 的当前路径在渲染完成后提交帧，后续版本需要重新检查同步约定。
- QQ 自己的 EGL 导入错误、颜色、方向和编码行为仍需真实测试；仅消除 -22 不能替代对端确认。
- 不在此候选中增加帧率限制，不修改 Niri，不安装或重启系统组件。

## 本地验证与构建

```bash
make check-dmabuf
```

离线测试直接使用候选的格式构造和取帧函数，覆盖原始 -22、双向格式过滤、新格式回调、保持原参数、拒绝未知格式、多平面和非法 stride、fd 缓存失效、原路径保持等行为。测试不连接 Portal 或实际 PipeWire 流。

本轮独立构建目录为 `build/dmabuf/`，版本 `0.2.21+dmabuf-linear`。其 Makefile、源码和模板通过软链接引用工作区，构建产物及安装前缀均在该目录内；原来的 `build/local` 保持不变。

完全退出旧 QQ 后，在 Niri 桌面的终端执行：

```bash
QQ_WL_DMABUF=linear \
QQ_WAYLAND_FIX_ANGLE=off \
QQ_WAYLAND_FIX_LOG="/run/user/$(id -u)/linuxqq-wayland-fix-dmabuf.log" \
/home/bao/projects/linuxqq-wayland-fix/build/dmabuf/local/bin/linuxqq-wayland-fix
```

ANGLE 设为 off 是为了避开上次启动时已观察到的 Vulkan 初始化失败，与本次 DMA-BUF 格式声明是独立设置。

观察日志：

```bash
tail -n 100 -F "/run/user/$(id -u)/linuxqq-wayland-fix-dmabuf.log"
```

关键里程碑：

```text
DMA-BUF offer: BGRx modifier=0 (LINEAR) ...
DMA-BUF format: ... modifier=0 ready=1
DMA-BUF frame: fd=... planes=1 stride=... offset=... modifier=0
```

第一个只证明已提供候选；第二个证明选择的格式；第三个证明已经从 PipeWire 取到 DMA-BUF。仍须结合 Niri/QQ 进入 Streaming，以及对端看到持续变化的画面，才能判定共享成功。

2026-10-19 首次实测已由用户确认正常，对应日志包括：

```text
DMA-BUF offer: BGRx modifier=0 (LINEAR), QQ native single-plane import; 1 fallback params
DMA-BUF format: 2560x1440 format=8 modifier=0 ready=1
DMA-BUF frame: fd=89 planes=1 stride=10240 offset=0 modifier=0
reuse capture texture: 2560x1440 (skip per-frame glTexImage2D/glBufferData)
```

同次 Niri 日志在 18:09:17 确认 `allocation successful (modifier=Linear, plane_count=1)`，随后 `Paused -> Streaming`。这与用户的实际共享结果共同支持本机首次验证通过。

首次共享通过后，后续分别验证停止后重启、窗口共享与改变窗口尺寸，以及长时间共享的资源趋势。没有把首次实测成功扩展为这些场景均已通过。

回退：完全退出候选版后，使用原来的 `build/local/bin/linuxqq-wayland-fix`。本轮没有覆盖该入口及其库。

## 官方依据

- [Niri v26.04 的录屏实现](https://github.com/niri-wm/niri/blob/v26.04/src/screencasting/pw_utils.rs)
- [PipeWire DMA-BUF sharing](https://docs.pipewire.org/devel/page_dma_buf.html)
- [EGL_EXT_image_dma_buf_import_modifiers](https://registry.khronos.org/EGL/extensions/EXT/EGL_EXT_image_dma_buf_import_modifiers.txt)：查询 external_only、额外平面及省略 modifier 的语义。
