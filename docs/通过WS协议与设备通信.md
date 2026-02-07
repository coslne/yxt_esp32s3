# 设备与服务器端通信协议详解 (WebSocket)

本文档详细描述了小智设备在与服务器建立 WebSocket 连接后的通信细节。只要遵循本协议，任何编程语言实现的服务器都可以与小智设备对接。

## 1. 建立 WebSocket 连接

在设备完成 HTTP 初始化握手获取到 WebSocket URL 和 Token 后，会发起 WebSocket 连接。

**连接参数:**
*   **URL**: `wss://your-domain.com/ws` (具体地址由 HTTP 握手下发)
*   **Headers**:
    *   `Authorization`: `Bearer <token>` (鉴权令牌)
    *   `Device-Id`: `xx:xx:xx:xx:xx:xx` (设备 MAC 地址)
    *   `Protocol-Version`: `1` (当前使用的协议版本)
    *   `Client-Id`: `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx` (设备 UUID)

---

## 2. 协议握手 (Handshake)

连接建立后的第一步必须是双方交换 `hello` 消息，确认参数。

### 2.1 设备发送 Hello (Device -> Server)
连接一旦打开，设备会立即发送：
```json
{
  "type": "hello",
  "version": 1,
  "transport": "websocket",
  "audio_params": {
    "format": "opus",
    "sample_rate": 16000,
    "channels": 1,
    "frame_duration": 60
  }
}
```
*   `frame_duration`: 60ms 是 Opus 的帧长。

### 2.2 服务器回复 Hello (Server -> Device)
服务器收到后必须回复：
```json
{
    "type": "hello",
    "transport": "websocket",
    "audio_params": {
        "sample_rate": 24000, 
        "frame_duration": 60
    }
}
```
*   `sample_rate`: 服务器即将发送的 TTS 音频采样率（通常 24000 或 16000）。
*   **注意**: 如果设备 10 秒内未收到此回复，会断开连接。

---

## 3. 消息类型

通信分为 **文本帧 (Text Frame)** 和 **二进制帧 (Binary Frame)**。
*   **文本帧**: JSON 格式，用于控制信令、状态同步、字幕显示。
*   **二进制帧**: Opus 编码的音频数据。

---

## 4. 核心交互流程详解

以下是标准的一问一答流程。

### 4.1 阶段一：用户说话 (Listening Phase)

#### A. 设备处理
1.  **发送音频**: 用户按住对讲键或唤醒后，设备会持续录音，编码为 Opus，通过 **二进制帧** 发送给服务器。
2.  **发送唤醒事件** (如果是语音唤醒):
    ```json
    {
        "type": "listen",
        "state": "detect",
        "text": "你好小智"
    }
    ```

#### B. 服务器处理 (STT)
服务器接收音频流进行语音识别 (STT)。当识别出中间结果或最终结果时，下发字幕：
```json
{
    "type": "stt",
    "text": "今天天气怎么样"
}
```
*   **效果**: 设备屏幕上会显示 "user: 今天天气怎么样"。

### 4.2 阶段二：大模型思考 (Thinking Phase)
服务器将识别的文本送入 LLM 处理。此时可以下发表情让设备“思考”。
```json
{
    "type": "llm",
    "emotion": "happy"
}
```
*   **支持表情**: `neutral` (默认), `happy` (开心), `sad` (悲伤), `angry` (生气), `fear` (恐惧), `surprise` (惊讶)。

### 4.3 阶段三：机器人回复 (Speaking Phase)

#### A. 开始播放 (Start)
服务器准备好 TTS 音频流之前，先发送开始指令：
```json
{
    "type": "tts",
    "state": "start"
}
```
*   **效果**: 设备状态切换为 `Speaking`，准备播放音频缓冲区的数据。

#### B. 传输音频与字幕
服务器并行做两件事：
1.  **发送音频**: 将 TTS 生成的音频特定为 Opus 格式，切分为二进制帧发送。
2.  **发送字幕**: 每句话开始时发送文本。
    ```json
    {
        "type": "tts",
        "state": "sentence_start",
        "text": "今天天气非常晴朗，适合出去游玩。"
    }
    ```
    *   **效果**: 设备屏幕显示 "assistant: 今天天气非常..."。

#### C. 结束播放 (Stop)
当所有音频发送完毕，且 LLM 回复结束：
```json
{
    "type": "tts",
    "state": "stop"
}
```
*   **效果**: 设备状态回到 `Idle` 或 `Listening`（取决于是否开启连续对话）。

---

## 5. 其他控制指令

### 5.1 用于打断 (Abort)
如果用户在机器人说话时打断（例如按了按钮）：
**Device -> Server**:
```json
{
    "type": "abort",
    "reason": "wake_word_detected" 
}
```
服务器收到后应立即停止 TTS 生成和发送。

### 5.2 物联网控制 (IoT)
**Server -> Device**:
```json
{
    "type": "iot",
    "commands": [
        {
            "id": "light_1",
            "action": "turn_on",
            "params": { "brightness": 100 }
        }
    ]
}
```
设备收到后会调用本地注册的 IoT 设备驱动。

---

## 6. Python 服务器参考代码

以下是一段最小可运行的 Python 服务器代码 (使用 `aiohttp`)，展示了通过 WebSocket 对接的核心逻辑。

```python
import logging
import json
import asyncio
from aiohttp import web, WSMsgType

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger("server")

async def websocket_handler(request):
    ws = web.WebSocketResponse()
    await ws.prepare(request)
    
    # 1. 打印连接信息
    device_id = request.headers.get('Device-Id')
    logger.info(f"设备连接: {device_id}")

    try:
        async for msg in ws:
            # 2. 处理文本消息 (JSON)
            if msg.type == WSMsgType.TEXT:
                data = json.loads(msg.data)
                logger.info(f"收到消息: {data}")

                # 3. 处理握手
                if data.get('type') == 'hello':
                    # 回复 hello
                    response = {
                        "type": "hello",
                        "transport": "websocket",
                        "audio_params": {
                            "sample_rate": 24000,
                            "frame_duration": 60
                        }
                    }
                    await ws.send_json(response)
                    logger.info("握手完成")

                # 4. 模拟处理语音指令 (示例逻辑)
                # 在实际场景中，这里应该是在收到 Binary 音频流并 VAD 静音检测后触发
                # 这里为了演示，假设收到 'listen' stop 状态或 detect 状态进行回复
                elif data.get('type') == 'listen' and data.get('state') == 'detect':
                    user_text = "你好"
                    
                    # A. 下发识别结果 (STT)
                    await ws.send_json({"type": "stt", "text": user_text})
                    
                    # B. 下发表情
                    await ws.send_json({"type": "llm", "emotion": "happy"})
                    
                    # C. 开始说话 (TTS Start)
                    await ws.send_json({"type": "tts", "state": "start"})
                    
                    # D. 下发字幕
                    reply_text = "你好！我是小智，很高兴为你服务。"
                    await ws.send_json({
                        "type": "tts", 
                        "state": "sentence_start", 
                        "text": reply_text
                    })

                    # E. 下发音频 (模拟发送一些空数据或音频文件)
                    # await ws.send_bytes(opus_encoded_audio_bytes)

                    # F. 结束说话
                    await asyncio.sleep(1) # 模拟说完的时间
                    await ws.send_json({"type": "tts", "state": "stop"})

            # 5. 处理音频消息 (Binary)
            elif msg.type == WSMsgType.BINARY:
                audio_data = msg.data
                #在此处将 audio_data 送入 VAD 或 STT 引擎
                # logger.debug(f"收到音频数据: {len(audio_data)} bytes")
                pass

            elif msg.type == WSMsgType.ERROR:
                logger.error(f"连接异常: {ws.exception()}")

    finally:
        logger.info("连接断开")
    return ws

app = web.Application()
app.router.add_get('/ws', websocket_handler) # 路径需与 HTTP 握手下发的一致

if __name__ == '__main__':
    web.run_app(app, port=8000)
```

## 7. 完整多轮对话时序范例

以下通过一个时序图的描述，展示从“语音唤醒”到“对答”再到“自动聆听下一句”的完整流程。假设用户开启了语音唤醒功能，且设备已连接 WS。

**场景**:
1. 用户说："你好小智" (唤醒)
2. 用户说："北京今天天气怎么样？"
3. 小智回复："北京今天晴天，气温20度。"
4. (自动进入聆听)
5. 用户追问："那明天呢？"
6. 小智回复："明天也是晴天。"
7. (自动进入聆听，但用户不再说话)
8. 小智超时，停止聆听。

| 时间轴 | 发送方 | 消息类型 | 消息内容 / 动作描述 | 设备状态 (State) |
| :--- | :--- | :--- | :--- | :--- |
| **00:00** | **Device** | `Binary` | [发送包含"你好小智"的Opus音频流...] | **Idle** |
| | **Device** | (VAD) | 本地 VAD 模块检测到唤醒词 | |
| **00:01** | **Device** | `Text` | `{"type": "listen", "state": "detect", "text": "你好小智"}` | -> **Listening** (AutoStop) |
| **00:02** | **Device** | `Binary` | [持续发送"北京今天天气怎么样？"的音频流...] | **Listening** |
| | **Server** | (VAD) | 服务器检测到用户说完一句话 (VAD End) | |
| **00:03** | **Server** | `Text` | `{"type": "stt", "text": "北京今天天气怎么样？"}` | **Listening** (屏幕显示文字) |
| **00:04** | **Server** | `Text` | `{"type": "llm", "emotion": "thinking"}` | **Listening** |
| | **Server** | (Process)| LLM 生成回复文本，TTS 生成音频 | |
| **00:05** | **Server** | `Text` | `{"type": "tts", "state": "start"}` | -> **Speaking** |
| **00:05** | **Server** | `Binary` | [发送 TTS 音频帧 1...] | **Speaking** (开始播放) |
| **00:06** | **Server** | `Binary` | [发送 TTS 音频帧 2...] | **Speaking** |
| **00:06** | **Server** | `Text` | `{"type": "tts", "state": "sentence_start", "text": "北京今天晴天..."}` | **Speaking** (屏幕显示字幕) |
| **00:07** | **Server** | `Binary` | [发送 TTS 音频帧 N...] | **Speaking** |
| **00:08** | **Server** | `Text` | `{"type": "tts", "state": "stop"}` | -> **Listening** |
| **说明** | | | 因为是唤醒触发 (AutoStop模式)，TTS结束后自动回到Listening | |
| **00:09** | **Device** | `Binary` | [发送静音或环境噪音...] | **Listening** |
| **00:10** | **Device** | `Binary` | [发送"那明天呢？"的音频流...] | **Listening** |
| **00:12** | **Server** | `Text` | `{"type": "stt", "text": "那明天呢？"}` | **Listening** |
| **00:13** | **Server** | `Text` | `{"type": "tts", "state": "start"}` | -> **Speaking** |
| **00:14** | **Server** | `Binary` | [发送 TTS 音频帧...] | **Speaking** |
| **00:15** | **Server** | `Text` | `{"type": "tts", "state": "stop"}` | -> **Listening** |
| **00:16** | **Device** | `Binary` | [发送环境噪音...] | **Listening** |
| **00:20** | **Device** | (VAD) | 此时若开启本地 VAD 且长时间无人说话，或者服务器端判断超时 | |
| **00:20** | **Device** | (Logic) | 调用 StopListening (通常由 VAD 超时或服务器下发停止指令触发) | -> **Idle** |

**关键点总结**:
1. **多轮对话的核心**：在于 `tts: stop` 消息。如果设备处于自动模式 (`kListeningModeAutoStop` / `kListeningModeRealtime`)，收到 `tts: stop` 后会自动切回 `Listening` 状态，并重新打开音频采集，等待用户下一句话。
2. **打断**：如果在 **Speaking** 状态下用户突然说话（基于本地/云端唤醒词检测），设备会发送 `abort` 消息，服务器应立即停止当前 TTS 推送，并将上下文切换到新的对话中。

---

## 8. 常见问题 (FAQ)

### Q1: 如果设备没有屏幕，还需要发送 `llm` (表情) 或 `stt` (字幕) 消息吗？
**不需要。**
*   **协议层面**：`llm` 和 `stt` 消息是完全可选的附加信息。如果不发送，设备端的解析逻辑会直接跳过，**不会导致通讯异常**或断开连接。
*   **应用层面**：对于无屏设备 (Headless Device)，接收到这些消息通常会被底层 Display 驱动忽略，或者开发者可以在固件编译时选择不包含 Display 模块。
*   **建议**：为节省带宽，如果明确知道设备无屏幕，建议服务器不下发 `type: llm` 和 `type: stt` 消息。
