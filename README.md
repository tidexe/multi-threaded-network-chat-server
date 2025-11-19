# 多线程局域网聊天服务器（服务器端）

说明：这是一个基于 TCP 的简单多线程聊天服务器实现。服务器接受多个客户端的连接，客户端与服务器使用“4 字节长度前缀 + 消息体”的二进制协议进行通信。服务器负责广播收到的消息给所有在线客户端。

协议：
- 客户端连接后，**首条**消息应当为用户名（字符串），格式：4 字节网络字节序长度 + 用户名 UTF-8 字节流。
- 后续消息同样使用 4 字节长度前缀（big-endian），随后是消息字节流。
- 若客户端发送特定消息 `__quit__`（内容文本），服务器会将其视为断开指令。

构建（Windows / Linux / macOS，要求 CMake + 支持 C++17 的编译器）：

1. 在仓库根目录创建构建目录并生成构建文件：

```powershell
mkdir build
cd build
cmake ..
cmake --build . --config Release
```

2. 生成后可执行文件名为 `chat_server`（Windows 上在 `Release` 或当前目录下的可执行文件）。运行示例：

```powershell
# 默认端口 5555
.\chat_server.exe 5555
```

测试客户端（Python 示例）：

下面给出一个简单的 Python 客户端示例，演示如何按协议发送用户名与消息。保存为 `test_client.py` 并运行。

````markdown
# 多线程局域网聊天服务器（服务器端）

说明：这是一个基于 TCP 的简单多线程聊天服务器实现。服务器接受多个客户端的连接，客户端与服务器使用“4 字节长度前缀 + 消息体”的二进制协议进行通信。服务器负责广播收到的消息给所有在线客户端。

**用法**

- 可执行文件位置：`build/bin/chat_server.exe`（或 `chat_server` 在 POSIX 环境）
- 运行参数：`chat_server [port]`
  - `port`：监听端口，默认 `5555`（若不提供则使用默认端口）。

示例：

```powershell
# 在默认端口运行（5555）
.\build\bin\chat_server.exe

# 指定端口运行
.\build\bin\chat_server.exe 12345
```

协议要点：
- 客户端连接后，**首条**消息应为用户名（4 字节 big-endian 长度 + UTF-8 字节流）。
- 后续每条消息：4 字节 big-endian 长度 + 消息字节流。
- 客户端发送 `__quit__` 文本时，服务器将其视为客户端断开指令并断开该连接。

构建（Windows / Linux / macOS，要求 CMake + 支持 C++17 的编译器）：

```powershell
mkdir build
cd build
cmake -S .. -B .
cmake --build . --config Release
```

简单测试：
- 服务器可与仓库中的客户端互通。也可使用任意遵守上面协议的自定义客户端（例如仓库中的 Python 示例）。

注意事项：
- 当前实现为“每连接一个线程”的模型，适用于小规模局域网场景。要支持大量并发连接，请考虑异步 IO（如 Boost.Asio）或使用线程池。
- 在 Windows 平台上程序会自动初始化 Winsock（WSAStartup），退出时清理（WSACleanup）。

````
    t = threading.Thread(target=recv_loop, daemon=True)
