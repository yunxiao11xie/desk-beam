"""
Desktop Music Companion - PC-side validation server.

Run this on Windows while QQ Music is playing. It reads the current media
session through Windows SMTC, fetches synced lyrics, and broadcasts JSON updates
over WebSocket for the future ESP32 client.

================================ 用途说明 ================================
在 Windows PC 上运行的媒体信息桥接服务，通过 WebSocket 将当前播放的
歌曲信息、进度、歌词实时推送给 ESP32 客户端（如桌面歌词显示屏）。

工作流程：
  QQ Music → Windows SMTC API → 本脚本 → WebSocket → ESP32 客户端

依赖：
  pip install winsdk websockets requests
=======================================================================
"""

from __future__ import annotations

import argparse
import asyncio
import base64
import contextlib
import json
import re
import socket
import time
import traceback
from dataclasses import dataclass, field
from typing import Any

import requests
import websockets

# -----------------------------------------------------------------------
# DeepSeek 用量数据配置
# 使用 --deepseek-file 指定 WorkBuddy 采集的 JSON 文件路径
# 推荐：D:\claude_code\deepseek_usage_data.json
# -----------------------------------------------------------------------
DEEPSEEK_API_KEY = ""   # 不再使用 API Key 模式，改用文件模式
DEEPSEEK_FILE = "D:\\claude_code\\deepseek_usage_data.json"

# DeepSeek 模型定价 (元/1M tokens) — 用于 usage_detail 回退计算
# 参考: https://api-docs.deepseek.com/zh-cn/quick_start/pricing
# 注: 峰谷时段会有 2x 差异，此处使用标准(非高峰)定价近似
DEEPSEEK_MODEL_PRICING: dict[str, dict[str, float]] = {
    "deepseek-v4-flash": {
        "input_cache_hit":  0.02,
        "input_cache_miss": 1.0,
        "output":           2.0,
    },
    "deepseek-v4-pro": {
        "input_cache_hit":  0.025,
        "input_cache_miss": 3.0,
        "output":           6.0,
    },
}

# -----------------------------------------------------------------------
# Windows SDK 依赖：通过 winsdk 调用 Windows 原生 SMTC API
# SMTC = System Media Transport Controls（系统媒体传输控制）
# 这是 Windows 8+ 提供的标准 API，所有主流播放器（QQ音乐、网易云等）都支持
# -----------------------------------------------------------------------
try:
    from winsdk.windows.media import MediaPlaybackStatus
    from winsdk.windows.media.control import (
        GlobalSystemMediaTransportControlsSession,
        GlobalSystemMediaTransportControlsSessionManager,
    )
except ImportError as exc:  # pragma: no cover - only hit before dependencies install
    raise SystemExit(
        "Missing dependency: winsdk\n"
        "Install with: python -m pip install -r pc_tools/requirements.txt"
    ) from exc


# ======================================================================
# 常量定义
# ======================================================================

# LRC 歌词时间戳正则匹配
# 格式示例: [01:23.456]歌词内容
#           [01:23]歌词内容
#           [1:23:45]歌词内容
LRC_TIME_RE = re.compile(r"\[(\d{1,2}):(\d{2})(?:[.:](\d{1,3}))?\](.*)")


# ======================================================================
# 歌曲信息数据模型
# ======================================================================

@dataclass
class SongInfo:
    """
    当前播放歌曲的完整状态快照。

    属性说明：
      title       — 歌曲标题
      artist      — 歌手/艺术家
      album       — 专辑名称
      duration_ms — 歌曲总时长（毫秒）
      position_ms — 当前播放位置（毫秒，采集瞬间的快照值）
      state       — 播放状态: playing / paused / stopped / no_session
      shuffle     — 是否开启随机播放
      repeat_mode — 重复模式: off / one / all / unknown
      updated_at  — 本快照的采集时间（time.monotonic），用于推算实时进度
    """
    title: str = ""
    artist: str = ""
    album: str = ""
    duration_ms: int = 0
    position_ms: int = 0
    state: str = "unknown"
    shuffle: bool = False
    repeat_mode: str = "unknown"
    track_id: str = ""              # QQ音乐/网易云 Track ID（从 SMTC Genres 提取）
    source: str = ""                # 来源: "qqmusic", "netease", ""
    updated_at: float = field(default_factory=time.monotonic)

    @property
    def key(self) -> tuple[str, str, str, int]:
        """
        歌曲唯一标识键，用于判断是否"换歌"。
        由 (title, artist, album, duration_ms) 四元组构成。
        """
        return (self.title, self.artist, self.album, self.duration_ms)

    def to_payload(self, msg_type: str = "song_info") -> dict[str, Any]:
        """
        将歌曲信息序列化为 JSON 字典，用于 WebSocket 广播。

        参数:
          msg_type — 消息类型标识，默认 "song_info"
                     换歌时也会用此方法构建广播

        返回:
          包含当前状态和实时进度的字典
        """
        return {
            "type": msg_type,
            "title": self.title,
            "artist": self.artist,
            "album": self.album,
            "duration_ms": self.duration_ms,
            "position_ms": self.current_position_ms(),  # 返回实时推算的位置
            "state": self.state,
            "shuffle": self.shuffle,
            "repeat_mode": self.repeat_mode,
        }

    def current_position_ms(self) -> int:
        """
        获取当前播放进度（毫秒）。

        由于 position_ms 是采集时的快照值，调用此方法时已过去一段时间。
        如果正在播放，需要根据经过的时间推算实时位置。

        推算逻辑：
          - 未播放时：直接返回快照值
          - 播放中：快照值 + (当前时间 - 采集时间)
          - 不超过歌曲总时长（防止越界）
          - 不低于 0
        """
        if self.state != "playing":
            return self.position_ms
        elapsed_ms = int((time.monotonic() - self.updated_at) * 1000)
        if self.duration_ms <= 0:
            return max(0, self.position_ms + elapsed_ms)
        return min(self.duration_ms, max(0, self.position_ms + elapsed_ms))


# ======================================================================
# 核心服务器类
# ======================================================================

class MediaServer:
    """
    Windows SMTC → WebSocket 媒体信息桥接服务器。

    职责：
      1. 通过 winsdk 连接 Windows SMTC，获取当前媒体会话
      2. 定期轮询歌曲信息（标题、歌手、进度、状态）
      3. 自动检测"换歌"事件，触发歌词查询
      4. 通过 LRCLIB API 获取同步歌词（LRC 格式）
      5. 通过 WebSocket 将信息广播给所有连接的 ESP32 客户端
      6. 接收客户端发来的控制命令（播放/暂停、上一首、下一首）

    运行方式：
      python windows_media_server.py
      → 监听 ws://0.0.0.0:8765
      → ESP32 客户端连接后自动接收歌曲信息和歌词
    """

    def __init__(self, host: str, port: int, lyric_timeout: float,
                 deepseek_key: str | None = None,
                 deepseek_file: str | None = None) -> None:
        """
        初始化媒体服务器。

        参数:
          host          — WebSocket 绑定地址 ("0.0.0.0" 表示接受所有网卡连接)
          port          — WebSocket 端口号
          lyric_timeout — 歌词 API HTTP 请求超时时间（秒）
          deepseek_key  — （已弃用）DeepSeek API Key
          deepseek_file — WorkBuddy DeepSeek 用量 JSON 文件路径（默认模式）
        """
        self.host = host
        self.port = port
        self.lyric_timeout = lyric_timeout
        self.deepseek_key = deepseek_key
        self.deepseek_file = deepseek_file
        self.deepseek_cache: dict[str, Any] | None = None  # 缓存用量数据，断线重连时推送
        self.manager: GlobalSystemMediaTransportControlsSessionManager | None = None
        self.session: GlobalSystemMediaTransportControlsSession | None = None
        self.clients: set[Any] = set()           # 已连接的 WebSocket 客户端集合
        self.song = SongInfo()                    # 当前歌曲信息
        self.last_song_key: tuple[str, str, str, int] | None = None  # 上一次的歌曲 key，用于检测换歌
        self.lyrics_cache: dict[tuple[str, str], list[dict[str, Any]]] = {}  # 歌词缓存 { (title, artist): [...] }
        self.paused: bool = False                  # 暂停广播标志：暂停时仍监听 SMTC，但不推送数据

    # ------------------------------------------------------------------
    # 生命周期管理
    # ------------------------------------------------------------------

    async def start(self) -> None:
        """
        启动服务器。

        步骤：
          1. 请求 Windows SMTC 会话管理器（需要用户授权）
          2. 启动 WebSocket 服务器，接受客户端连接
          3. 同时运行两个后台协程：
             - poll_media_loop()  — 每秒轮询媒体状态
             - position_loop()    — 每秒广播播放进度
        """
        print("Requesting Windows SMTC session manager...")
        self.manager = await asyncio.wait_for(
            GlobalSystemMediaTransportControlsSessionManager.request_async(),
            timeout=8.0,
        )
        print("SMTC session manager ready.")
        async with websockets.serve(self.handle_client, self.host, self.port):
            ip = get_lan_ip()
            print(f"WebSocket server: ws://{self.host}:{self.port}")
            print(f"ESP32 same-LAN address hint: ws://{ip}:{self.port}")
            print("Open QQ Music and play a song. Press Ctrl+C to stop.")
            print("Press [P] to pause/resume broadcasting to clients.")
            await asyncio.gather(
                self.poll_media_loop(),
                self.position_loop(),
                self.deepseek_loop(),
                self.keyboard_listener(),
            )

    # ------------------------------------------------------------------
    # WebSocket 客户端管理
    # ------------------------------------------------------------------

    async def handle_client(self, websocket: Any) -> None:
        """
        处理单个 WebSocket 客户端连接。

        当 ESP32 客户端连接时：
          1. 将客户端加入广播列表
          2. 立即发送当前歌曲信息（让客户端马上显示）
          3. 立即发送当前歌词数据
          4. 持续监听客户端发来的消息（控制命令）
          5. 断开连接时自动清理

        参数:
          websocket — WebSocket 连接对象
        """
        self.clients.add(websocket)
        print(f"Client connected: {websocket.remote_address}")
        try:
            # 客户端刚连上，立即推送当前状态，避免等待下一次轮询
            await websocket.send(json.dumps(self.song.to_payload(), ensure_ascii=False))
            await self.send_lyrics(websocket, self.song)
            await self.send_deepseek_to_client(websocket)  # DeepSeek 用量缓存
            # 告知客户端当前的暂停状态
            if self.paused:
                await websocket.send(json.dumps({
                    "type": "server_status",
                    "paused": True,
                    "message": "服务器已暂停广播",
                }, ensure_ascii=False))
            # 持续监听客户端的控制命令
            async for raw in websocket:
                await self.handle_command(raw)
        finally:
            self.clients.discard(websocket)
            print(f"Client disconnected: {websocket.remote_address}")

    # ------------------------------------------------------------------
    # 轮询循环
    # ------------------------------------------------------------------

    async def poll_media_loop(self) -> None:
        """
        媒体信息轮询主循环（每秒执行一次）。

        每次调用 poll_once() 检查：
          - 当前是否有活动的媒体会话
          - 歌曲是否发生变化（触发换歌事件）
          - 播放进度是否被用户拖拽（position jump）
          - 播放状态是否变化（播放/暂停/停止）
        """
        while True:
            try:
                await self.poll_once()
            except Exception:
                print("Media poll failed:")
                traceback.print_exc()
            await asyncio.sleep(1.0)

    async def position_loop(self) -> None:
        """
        播放进度广播循环（每秒执行一次）。

        即使歌曲没变，也需要持续推送播放进度变化，
        让 ESP32 端的进度条/歌词高亮行能够实时更新。

        暂停时跳过广播，但 resume 后会自动推送最新状态。
        """
        while True:
            if not self.paused:
                await self.broadcast(
                    {
                        "type": "position",
                        "position_ms": self.song.current_position_ms(),
                        "duration_ms": self.song.duration_ms,
                    }
                )
            await asyncio.sleep(1.0)

    async def poll_once(self) -> None:
        """
        单次媒体状态采集。

        完整流程：
          1. 获取当前媒体会话（可能为 None，如无播放器在运行）
          2. 读取媒体属性：标题、歌手、专辑
          3. 读取时间线：总时长、当前进度
          4. 读取播放状态：播放/暂停/停止
          5. 与上一次状态比较，决定是否触发换歌事件
          6. 换歌 → 广播新歌曲信息 + 查询并广播歌词
          7. 进度跳转/状态变化 → 广播更新
        """
        if self.manager is None:
            return

        self.session = self.manager.get_current_session()
        if self.session is None:
            # ---- 没有活动的媒体会话 ----
            if self.song.state != "no_session":
                self.song = SongInfo(state="no_session")
                self.last_song_key = None
                print("No active media session. Is QQ Music playing?")
                await self.broadcast(self.song.to_payload())
            return

        # ---- 读取当前会话的完整状态 ----
        media = await self.session.try_get_media_properties_async()

        # 从 SMTC Genres 字段提取播放器专属 Track ID（QQ音乐/网易云会嵌入）
        track_id, source = "", ""
        try:
            if hasattr(media, 'genres') and media.genres:
                for genre in media.genres:
                    g = str(genre).strip() if genre else ""
                    if g.startswith("BetterLyrics.QQMusicTrackID:"):
                        track_id = g.split(":", 1)[1].strip()
                        source = "qqmusic"
                        break
                    elif g.startswith("BetterLyrics.NetEaseCloudMusicTrackID:"):
                        track_id = g.split(":", 1)[1].strip()
                        source = "netease"
                        break
        except Exception:
            pass  # winsdk 版本兼容性，忽略 genres 读取失败

        timeline = self.session.get_timeline_properties()
        playback = self.session.get_playback_info()
        controls = playback.controls

        if track_id:
            print(f"  SMTC Genres: source={source}, track_id={track_id}")

        song = SongInfo(
            title=str(media.title or ""),
            artist=str(media.artist or ""),
            album=str(media.album_title or ""),
            duration_ms=timespan_to_ms(timeline.end_time),
            position_ms=timespan_to_ms(timeline.position),
            state=playback_status_name(playback.playback_status),
            shuffle=bool(getattr(playback, "is_shuffle_active", False)),
            repeat_mode=str(getattr(playback, "auto_repeat_mode", "unknown")).split(".")[-1].lower(),
            track_id=track_id,
            source=source,
        )

        # ---- 状态变化检测 ----
        changed = song.key != self.last_song_key           # 歌曲切换
        position_jump = abs(song.position_ms - self.song.current_position_ms()) > 2500  # 手动拖动进度 > 2.5s
        state_changed = song.state != self.song.state      # 播放/暂停状态切换
        self.song = song

        if changed:
            # ---- 换歌了！需要更新内部状态 + 查歌词（广播受暂停控制） ----
            self.last_song_key = song.key
            print(f"Now: {song.title or '(unknown title)'} - {song.artist or '(unknown artist)'}")
            # 歌词查询是 HTTP 请求，用 asyncio.to_thread 避免阻塞事件循环
            lyrics = await asyncio.to_thread(self.fetch_lyrics, song)
            print(f"Lyrics lines: {len(lyrics)}")
            if not self.paused:
                await self.broadcast(song.to_payload())
                await self.broadcast({"type": "lyrics", "lines": lyrics})
        elif position_jump or state_changed:
            # ---- 进度跳转或状态变化，暂停时也暂不广播 ----
            if not self.paused:
                await self.broadcast(song.to_payload())

        # ---- 提醒：如果媒体控制按钮全部禁用，说明此会话只读 ----
        # 非关键检查：探测播放器是否支持远程控制
        # winsdk 不同版本中检查属性名可能不同，用 getattr 安全访问
        has_play = getattr(controls, "is_play_pause_enabled", None)
        has_next = getattr(controls, "is_next_enabled", None)
        has_prev = getattr(controls, "is_previous_enabled", None)
        if has_play is not None and has_next is not None and has_prev is not None:
            if not any([has_play, has_next, has_prev]):
                print("Warning: this session exposes metadata, but media control may be limited.")

    # ------------------------------------------------------------------
    # 客户端控制命令处理
    # ------------------------------------------------------------------

    async def handle_command(self, raw: str) -> None:
        """
        处理 WebSocket 客户端发来的控制命令。

        客户端发送 JSON:
          { "type": "command", "action": "play_pause" | "next" | "prev" }

        服务器通过 SMTC API 控制播放器（如 QQ Music）执行对应操作。
        ESP32 端可以通过触摸按钮发送这些命令，实现远程控制。

        参数:
          raw — 客户端发送的原始字符串（应为 JSON 格式）
        """
        try:
            msg = json.loads(raw)
        except json.JSONDecodeError:
            print(f"Ignored non-JSON message: {raw!r}")
            return

        if msg.get("type") != "command":
            return

        action = msg.get("action")
        print(f"Command from client: {action}")
        if self.session is None:
            return

        if action == "play_pause":
            await self.session.try_toggle_play_pause_async()
        elif action == "next":
            await self.session.try_skip_next_async()
        elif action in {"prev", "previous"}:
            await self.session.try_skip_previous_async()
        elif action == "pause_server":
            if not self.paused:
                await self.toggle_pause()
        elif action == "resume_server":
            if self.paused:
                await self.toggle_pause()
        elif action == "toggle_shuffle":
            await self._toggle_shuffle()
        elif action == "toggle_repeat":
            await self._toggle_repeat()
        elif action == "seek":
            await self._seek(msg.get("position_ms", 0))
        elif action == "deepseek_refresh":
            print("DeepSeek: 手动刷新用量数据")
            if self.deepseek_file:
                await self._broadcast_deepseek_from_file()
            elif self.deepseek_key:
                await self._fetch_and_broadcast_deepseek()
            else:
                print("  DeepSeek: 未配置数据源")
        else:
            print(f"Unsupported command for this validation script: {action}")

    # ------------------------------------------------------------------
    # 扩展控制命令
    # ------------------------------------------------------------------

    async def _toggle_shuffle(self) -> None:
        """切换随机播放模式（通过 SMTC）。"""
        try:
            if self.session is None:
                return
            playback = self.session.get_playback_info()
            current = bool(getattr(playback, "is_shuffle_active", False))
            # SMTC 提供了 try_change_shuffle_active_async 切换
            await self.session.try_change_shuffle_active_async(not current)
            print(f"  Shuffle: {current} → {not current}")
        except Exception as exc:
            print(f"  toggle_shuffle failed: {exc}")

    async def _toggle_repeat(self) -> None:
        """切换循环模式：off → all → one → off（通过 SMTC）。"""
        try:
            if self.session is None:
                return
            playback = self.session.get_playback_info()
            current = str(getattr(playback, "auto_repeat_mode", "unknown")).split(".")[-1].lower()

            # 循环顺序: off → all → one → off
            from winsdk.windows.media import MediaPlaybackAutoRepeatMode
            mode_map = {
                "none": MediaPlaybackAutoRepeatMode.LIST,
                "off":  MediaPlaybackAutoRepeatMode.LIST,
                "list": MediaPlaybackAutoRepeatMode.TRACK,
                "all":  MediaPlaybackAutoRepeatMode.TRACK,
                "track": MediaPlaybackAutoRepeatMode.NONE,
                "one":  MediaPlaybackAutoRepeatMode.NONE,
            }
            next_mode = mode_map.get(current, MediaPlaybackAutoRepeatMode.LIST)
            await self.session.try_change_auto_repeat_mode_async(next_mode)
            mode_name = str(next_mode).split(".")[-1].lower()
            print(f"  Repeat: {current} → {mode_name}")
        except Exception as exc:
            print(f"  toggle_repeat failed: {exc}")

    async def _seek(self, position_ms: int) -> None:
        """跳转到指定播放位置（通过 SMTC）。"""
        try:
            if self.session is None or position_ms < 0:
                return
            # WinRT TimeSpan 以 100ns tick 为单位，直接传整数
            ticks = position_ms * 10000  # ms → 100ns ticks
            await self.session.try_change_playback_position_async(ticks)
            total_sec = position_ms // 1000
            print(f"  Seek → {total_sec // 60}:{total_sec % 60:02d}")
        except Exception as exc:
            print(f"  seek failed: {exc}")

    # ------------------------------------------------------------------
    # WebSocket 广播
    # ------------------------------------------------------------------

    async def broadcast(self, payload: dict[str, Any]) -> None:
        """
        向所有已连接的 WebSocket 客户端广播消息。

        如果某个客户端已断开，自动从列表中移除（惰性清理）。

        参数:
          payload — 要广播的字典，会被序列化为 JSON
        """
        if not self.clients:
            return
        data = json.dumps(payload, ensure_ascii=False)
        stale = []
        for client in self.clients:
            try:
                await client.send(data)
            except websockets.ConnectionClosed:
                stale.append(client)
        for client in stale:
            self.clients.discard(client)

    async def send_lyrics(self, websocket: Any, song: SongInfo) -> None:
        """
        向指定客户端发送缓存的歌词数据。

        用于新客户端刚连接时，立即推送当前歌曲的歌词，
        不需要等待下一次轮询。

        参数:
          websocket — WebSocket 连接对象
          song      — 当前歌曲信息（用于查找缓存 key）
        """
        lyrics = self.lyrics_cache.get((song.title, song.artist), [])
        await websocket.send(json.dumps({"type": "lyrics", "lines": lyrics}, ensure_ascii=False))

    # ------------------------------------------------------------------
    # 暂停/恢复广播
    # ------------------------------------------------------------------

    async def toggle_pause(self) -> None:
        """
        切换暂停/恢复广播状态。

        暂停时：
          - PC 端仍在后台轮询 SMTC（保持数据最新）
          - 停止向所有 WebSocket 客户端推送 song_info / lyrics / position
          - 断线重连的客户端仍会收到当前状态（不受暂停影响）

        恢复时：
          - 立即向所有客户端推送最新的完整状态
          - 继续正常广播
        """
        self.paused = not self.paused
        if self.paused:
            print("\n⏸️  PAUSED — 已暂停广播到客户端")
            await self.notify_pause_state()
        else:
            print("\n▶️  RESUMED — 已恢复广播")
            # 恢复时立即推送最新状态，让客户端快速追上
            await self.broadcast(self.song.to_payload("song_info"))
            await self.send_lyrics_to_all()
            await self.notify_pause_state()

    async def notify_pause_state(self) -> None:
        """向所有客户端广播暂停状态变化。"""
        await self.broadcast({
            "type": "server_status",
            "paused": self.paused,
            "message": "广播已暂停" if self.paused else "广播已恢复",
        })

    async def send_lyrics_to_all(self) -> None:
        """向所有客户端发送当前缓存的歌词。"""
        lyrics = self.lyrics_cache.get((self.song.title, self.song.artist), [])
        if lyrics:
            await self.broadcast({"type": "lyrics", "lines": lyrics})

    async def keyboard_listener(self) -> None:
        """
        键盘监听协程（Windows 专用）。

        在后台线程中调用 msvcrt.getwch() 阻塞等待按键，
        通过 asyncio.get_running_loop().run_in_executor() 桥接到事件循环，
        既不影响主协程，又避免轮询开销。

        快捷键：
          P — 切换暂停/恢复广播
          Q — 退出服务器
        """
        import msvcrt
        loop = asyncio.get_running_loop()
        while True:
            # getwch() 在后台线程阻塞，按任意键立即返回
            ch = await loop.run_in_executor(None, msvcrt.getwch)
            if ch.lower() == "p":
                await self.toggle_pause()
            elif ch.lower() == "q":
                print("\n👋 用户请求退出，正在停止服务器...")
                # 取消所有 asyncio 任务，触发 shutdown
                for task in asyncio.all_tasks(loop):
                    if task is not asyncio.current_task():
                        task.cancel()
                break

    # ------------------------------------------------------------------
    # DeepSeek API 用量统计
    # ------------------------------------------------------------------

    DEEPSEEK_BALANCE_URL = "https://api.deepseek.com/user/balance"

    async def deepseek_loop(self) -> None:
        """
        DeepSeek 用量数据采集循环。

        两种模式（文件模式优先）：
          1. 文件模式 (--deepseek-file):
             - 直接从 WorkBuddy 生成的 JSON 文件读取
             - 每 300s 检查一次
             - 不依赖 API Key
          2. API 模式 (--deepseek-key):
             - 调用 DeepSeek /user/balance API
             - 每 60s 轮询一次
             - 仅获取余额（无用量数据）
        """
        if self.deepseek_file:
            print("DeepSeek: 文件模式已启用")
            print(f"          文件: {self.deepseek_file}")
            print("          WorkBuddy 每 1h 更新数据，本循环每 300s 检查")
            # 先立即读取一次
            try:
                await self._broadcast_deepseek_from_file()
            except Exception:
                traceback.print_exc()
            while True:
                await asyncio.sleep(300.0)
                try:
                    await self._broadcast_deepseek_from_file()
                except Exception:
                    traceback.print_exc()
        elif self.deepseek_key:
            print("DeepSeek: API 模式已启动（每 60s）")
            while True:
                try:
                    await self._fetch_and_broadcast_deepseek()
                except Exception:
                    traceback.print_exc()
                await asyncio.sleep(60.0)
        else:
            print("DeepSeek: 未配置数据源，跳过用量采集")
            print("          文件模式: --deepseek-file <path>")

    async def _fetch_and_broadcast_deepseek(self) -> None:
        """获取 DeepSeek 余额数据并广播。"""
        payload = await asyncio.to_thread(self._fetch_deepseek_data)
        if payload:
            self.deepseek_cache = payload
            await self.broadcast(payload)

    def _fetch_deepseek_data(self) -> dict[str, Any] | None:
        """
        同步获取 DeepSeek 余额数据（在后台线程中执行）。

        调用 /user/balance API 获取账户余额。

        返回:
          deepseek_usage 字典，失败时返回 None

        注意：
          DeepSeek 未提供公开的用量查询 API，Token 用量/每日用量
          等数据仅在网页端可见 (https://platform.deepseek.com/usage)。
          ESP32 端页面已适配此情况，用量卡片会显示"仅网页端可查"。
        """
        headers = {
            "Authorization": f"Bearer {self.deepseek_key}",
            "Accept": "application/json",
        }

        result: dict[str, Any] = {
            "type": "deepseek_usage",
            "balance": "0.00",
            "currency": "CNY",
            "monthly_cost": "N/A",
            "total_requests": 0,
            "total_tokens": 0,
            "models": [],
            "last_sync": "",
        }

        try:
            # ---- 查询余额 ----
            bal_resp = requests.get(
                self.DEEPSEEK_BALANCE_URL,
                headers=headers,
                timeout=10.0,
            )
            if bal_resp.status_code == 200:
                bal_data = bal_resp.json()
                result["balance"] = str(bal_data.get("total_balance", "0.00"))
                result["currency"] = bal_data.get("currency", "CNY")
                print(f"  DeepSeek 余额: ¥{result['balance']}")
            else:
                print(f"  DeepSeek balance API: HTTP {bal_resp.status_code}")

            # ---- 同步时间戳 ----
            result["last_sync"] = time.strftime("%H:%M:%S")

            return result

        except requests.RequestException as exc:
            print(f"  DeepSeek API request failed: {exc}")
            return None

    async def _broadcast_deepseek_from_file(self) -> None:
        """从 WorkBuddy 文件读取 DeepSeek 数据并广播。"""
        payload = await asyncio.to_thread(self._read_deepseek_from_file)
        if payload:
            self.deepseek_cache = payload
            await self.broadcast(payload)

    def _read_deepseek_from_file(self) -> dict[str, Any] | None:
        """
        读取 WorkBuddy 生成的 DeepSeek 用量 JSON 文件。

        文件格式 (deepseek_usage_data.json):
        ```json
        {
          "balance": "0.77",
          "currency": "CNY",
          "monthly_cost": "6.60",
          "month": "7",
          "year": "2026",
          "models": [
            {"name": "deepseek-v4-pro", "requests": 6, "tokens": 115395},
            {"name": "deepseek-v4-flash", "requests": 1143, "tokens": 90892968}
          ],
          "cost_detail": {
            "daily_cost": [
              {"date": "2026-07-01", "total_cost": "1.040004", "models": [...]},
              ...
            ]
          },
          "updated_at": "2026-07-06 16:16:35"
        }
        ```

        转为 ESP32 协议格式:
          daily_usage → 31 天 float 数组（从 cost_detail.daily_cost 提取）

        返回:
          符合 ESP32 deepseek_usage 消息格式的字典，失败时返回 None
        """
        try:
            with open(self.deepseek_file, "r", encoding="utf-8") as f:
                data = json.load(f)
        except FileNotFoundError:
            print(f"  DeepSeek 文件未找到: {self.deepseek_file}")
            return None
        except json.JSONDecodeError as exc:
            print(f"  DeepSeek 文件 JSON 解析失败: {exc}")
            return None
        except OSError as exc:
            print(f"  DeepSeek 文件读取失败: {exc}")
            return None

        result: dict[str, Any] = {
            "type": "deepseek_usage",
        }

        # ---- 余额 ----
        result["balance"] = str(data.get("balance", "0.00"))
        result["currency"] = str(data.get("currency", "CNY"))

        # ---- 月消费 ----
        result["monthly_cost"] = str(data.get("monthly_cost", "0.00"))

        # ---- 模型数组 ----
        models = data.get("models", [])
        total_requests = 0
        total_tokens = 0

        result["models"] = []
        for m in models:
            entry: dict[str, Any] = {
                "name": str(m.get("name", "unknown")),
                "requests": int(m.get("requests", 0)),
                "tokens": int(m.get("tokens", 0)),
            }
            total_requests += entry["requests"]
            total_tokens += entry["tokens"]
            result["models"].append(entry)

        result["total_requests"] = total_requests
        result["total_tokens"] = total_tokens

        # ---- 每日消费 (daily_usage: 31 天 float 数组) ----
        # 优先从 cost_detail.daily_cost 读取（有 total_cost 字段）
        # 回退：从 usage_detail.daily_usage（token 数据 × 模型定价计算）
        daily_usage = [0.0] * 31
        cost_detail = data.get("cost_detail", {})
        daily_cost = cost_detail.get("daily_cost", [])

        if daily_cost:
            # 主方案：cost_detail.daily_cost 有数据
            for entry in daily_cost:
                date_str = entry.get("date", "")
                total_cost_str = entry.get("total_cost", "0")
                try:
                    day = int(date_str.split("-")[2])  # "2026-07-01" → 1
                    if 1 <= day <= 31:
                        daily_usage[day - 1] = float(total_cost_str)
                except (IndexError, ValueError):
                    continue
        else:
            # 回退方案：从 usage_detail.daily_usage 计算
            usage_detail = data.get("usage_detail", {})
            daily_usage_list = usage_detail.get("daily_usage", [])
            for entry in daily_usage_list:
                date_str = entry.get("date", "")
                models = entry.get("models", [])
                try:
                    day = int(date_str.split("-")[2])
                except (IndexError, ValueError):
                    continue
                if day < 1 or day > 31:
                    continue
                # 累加所有模型的费用
                day_cost = 0.0
                for m in models:
                    model_name = m.get("model", "")
                    pricing = DEEPSEEK_MODEL_PRICING.get(model_name)
                    if pricing is None:
                        continue
                    cache_hit  = float(m.get("prompt_cache_hit_tokens", 0))
                    cache_miss = float(m.get("prompt_cache_miss_tokens", 0))
                    out_tok    = float(m.get("response_tokens", 0))
                    day_cost += (
                        cache_hit  * pricing["input_cache_hit"]  / 1_000_000
                        + cache_miss * pricing["input_cache_miss"] / 1_000_000
                        + out_tok    * pricing["output"]          / 1_000_000
                    )
                daily_usage[day - 1] = day_cost
        result["daily_usage"] = daily_usage

        # ---- 更新时间 ----
        result["last_sync"] = str(data.get("updated_at", ""))

        if daily_cost:
            source_note = "cost_detail.daily_cost"
        else:
            source_note = "usage_detail.daily_usage (token×定价计算)"
        non_zero_days = sum(1 for v in daily_usage if v > 0.001)
        total_daily_cost = sum(daily_usage)
        print(f"  DeepSeek 文件: 余额 ¥{result['balance']}, "
              f"请求 {total_requests}, Tokens {total_tokens}, "
              f"月费 ¥{total_daily_cost:.2f}")
        print(f"    每日消费来源: {source_note}")
        if non_zero_days > 0:
            nonzero_vals = [v for v in daily_usage if v > 0.001]
            cost_min = min(nonzero_vals)
            cost_max = max(daily_usage)
            print(f"    有效天数: {non_zero_days}/31, "
                  f"日费用范围: {cost_min:.3f}~{cost_max:.3f} 元")
        else:
            print("    日费用: 全部为零")
        return result

    async def send_deepseek_to_client(self, websocket: Any) -> None:
        """
        向指定客户端推送 DeepSeek 用量数据。

        优先使用缓存；若缓存为空（客户端在 deepseek_loop 首次运行前已连上），
        立即从文件读取一次确保新客户端总能拿到数据。
        """
        payload = self.deepseek_cache
        if payload is None and self.deepseek_file:
            # 缓存为空：客户端连接早于 deepseek_loop 首次读取，现场加载
            payload = await asyncio.to_thread(self._read_deepseek_from_file)
            if payload:
                self.deepseek_cache = payload
        if payload:
            await websocket.send(json.dumps(payload, ensure_ascii=False))

    # ------------------------------------------------------------------
    # 歌词获取（LRCLIB API）
    # ------------------------------------------------------------------

    def fetch_lyrics(self, song: SongInfo) -> list[dict[str, Any]]:
        """
        多源歌词查询，按优先级尝试：

          1. QQ Music API（主源 — SMTC Track ID 直达）
          2. NetEase Music API（通用 fallback）
          3. LRCLIB 公共 API（海外 fallback）

        参数:
          song — 当前歌曲信息

        返回:
          解析后的歌词行列表:
          [ { "time_ms": 12345, "text": "歌词内容" }, ... ]
          如果全部失败或没有歌词，返回空列表
        """
        if not song.title:
            return []

        cache_key = (song.title, song.artist)
        if cache_key in self.lyrics_cache:
            return self.lyrics_cache[cache_key]

        lines: list[dict[str, Any]] = []

        # ---- 1) QQ Music API（主源，SMTC 直取 Track ID）----
        if song.source == "qqmusic" and song.track_id:
            lines = self._fetch_qqmusic_lyrics(song)
            if lines:
                print(f"  → QQ Music: {len(lines)} lyrics lines")

        # ---- 2) NetEase Music API（通用 fallback） ----
        if not lines:
            lines = self._fetch_netease_lyrics(song)
            if lines:
                print(f"  → NetEase: {len(lines)} lyrics lines")

        # ---- 3) LRCLIB fallback ----
        if not lines:
            lines = self._fetch_lrclib_lyrics(song)
            if lines:
                print(f"  → LRCLIB: {len(lines)} lyrics lines")

        if not lines:
            print("  → No lyrics found from any source")

        if lines:
            self._print_lyrics_preview(lines)

        self.lyrics_cache[cache_key] = lines
        return lines

    @staticmethod
    def _print_lyrics_preview(lines: list[dict[str, Any]]) -> None:
        """在终端打印前几行歌词，方便肉眼确认是否获取正确。"""
        preview_count = min(6, len(lines))
        print(f"  ── Lyrics preview ({preview_count} of {len(lines)} lines) ──")
        for i in range(preview_count):
            ts = lines[i]["time_ms"]
            total_sec = ts / 1000
            m, s = int(total_sec // 60), int(total_sec % 60)
            text = lines[i]["text"]
            print(f"     [{m:02d}:{s:02d}] {text}")
        if len(lines) > preview_count:
            print(f"     ... ({len(lines) - preview_count} more lines)")

    def _fetch_qqmusic_lyrics(self, song: SongInfo) -> list[dict[str, Any]]:
        """
        通过 QQ音乐 API 获取同步歌词。

        利用 SMTC Genres 中提取的 BetterLyrics.QQMusicTrackID，
        直接调 QQ音乐内部接口获取 LRC 歌词（base64 编码）。
        """
        try:
            song_id = int(song.track_id)
        except ValueError:
            return []

        url = "https://u.y.qq.com/cgi-bin/musicu.fcg"
        payload = {
            "req": {
                "module": "lyric.server.SongLyricModule",
                "method": "Lyric_GetLyric",
                "param": {
                    "songid": song_id,
                    "songtype": 1,
                    "nocache": 0,
                }
            }
        }
        headers = {
            "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36",
            "Referer": "https://y.qq.com",
        }

        try:
            resp = requests.post(url, json=payload, timeout=self.lyric_timeout, headers=headers)
            resp.raise_for_status()
            data = resp.json()

            lyric_data = data.get("req", {}).get("data", {})
            lrc_b64 = lyric_data.get("lyric", "")
            if not lrc_b64:
                return []

            lrc_text = base64.b64decode(lrc_b64).decode("utf-8", errors="replace")
            return parse_lrc(lrc_text)
        except Exception as exc:
            print(f"  QQ Music lookup failed: {exc}")
            return []

    def _fetch_netease_lyrics(self, song: SongInfo) -> list[dict[str, Any]]:
        """
        通过网易云音乐 API 获取同步歌词。

        两步：
          1. 搜索歌曲（title + artist）→ 获取 song ID
          2. 用 song ID 获取 LRC 歌词

        如果 SMTC Genres 提供了 NetEase track_id，直接走 ID 查询跳过搜索。
        """
        headers = {"User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"}

        try:
            song_id = 0

            # ---- 如果有 NetEase track ID，直接跳过搜索 ----
            if song.source == "netease" and song.track_id:
                try:
                    song_id = int(song.track_id)
                except ValueError:
                    song_id = 0

            # ---- 否则按 title + artist 搜索 ----
            if not song_id:
                search_url = "https://music.163.com/api/search/get"
                params = {
                    "s": f"{song.title} {song.artist}",
                    "type": 1,       # 1 = 歌曲
                    "limit": 5,
                }
                resp = requests.get(
                    search_url, params=params,
                    timeout=self.lyric_timeout, headers=headers,
                )
                resp.raise_for_status()
                data = resp.json()

                if data.get("code") != 200:
                    return []
                songs = data.get("result", {}).get("songs", [])
                if not songs:
                    return []

                # 找歌手匹配度最高的结果
                artist_lower = song.artist.lower()
                best_id = 0
                for s in songs:
                    artists = ", ".join(
                        a.get("name", "") for a in s.get("artists", [])
                    )
                    if artist_lower and artist_lower in artists.lower():
                        best_id = s["id"]
                        break
                if not best_id and songs:
                    best_id = songs[0]["id"]
                if not best_id:
                    return []
                song_id = best_id

            # ---- 获取 LRC 歌词 ----
            lyric_url = (
                f"https://music.163.com/api/song/lyric"
                f"?id={song_id}&lv=-1&kv=-1&tv=-1"
            )
            resp = requests.get(
                lyric_url, timeout=self.lyric_timeout, headers=headers,
            )
            resp.raise_for_status()
            data = resp.json()

            lrc_text = data.get("lrc", {}).get("lyric", "")
            if not lrc_text:
                return []

            return parse_lrc(lrc_text)

        except Exception as exc:
            print(f"  NetEase lookup failed: {exc}")
            return []

    def _fetch_lrclib_lyrics(self, song: SongInfo) -> list[dict[str, Any]]:
        """
        从 LRCLIB 公共 API 查询同步歌词（海外 fallback）。
        """
        params = {
            "track_name": song.title,
            "artist_name": song.artist,
        }
        if song.album:
            params["album_name"] = song.album
        if song.duration_ms:
            params["duration"] = str(round(song.duration_ms / 1000))

        try:
            resp = requests.get(
                "https://lrclib.net/api/get",
                params=params,
                timeout=self.lyric_timeout,
                headers={"User-Agent": "DesktopMusicCompanion/0.1"},
            )
            if resp.status_code == 404:
                return []
            resp.raise_for_status()
            data = resp.json()
            lrc_text = data.get("syncedLyrics") or data.get("plainLyrics") or ""
            return parse_lrc(lrc_text)
        except Exception as exc:
            print(f"  LRCLIB lookup failed: {exc}")
            return []


# ======================================================================
# 工具函数
# ======================================================================

def parse_lrc(text: str) -> list[dict[str, Any]]:
    """
    解析 LRC 格式歌词文本。

    LRC 格式示例：
      [00:12.345]第一句歌词
      [00:25.000]第二句歌词
      [01:45.678]第三句歌词

    时间戳格式支持：
      [mm:ss.xxx]     — 毫秒（3位）
      [mm:ss.xx]      — 厘秒（2位）
      [mm:ss.x]       — 分秒（1位）
      [mm:ss]         — 无小数
      [m:ss.xxx]      — 分钟不补零

    参数:
      text — 原始 LRC 文本（可能含多行）

    返回:
      按时间排序的歌词行列表:
      [ { "time_ms": 12345, "text": "第一句歌词" }, ... ]
    """
    lines: list[dict[str, Any]] = []
    for raw_line in text.splitlines():
        match = LRC_TIME_RE.match(raw_line.strip())
        if not match:
            continue
        minutes, seconds, fraction, lyric = match.groups()
        # 解析毫秒部分（兼容 1~3 位小数）
        frac = fraction or "0"
        if len(frac) == 1:
            frac_ms = int(frac) * 100
        elif len(frac) == 2:
            frac_ms = int(frac) * 10
        else:
            frac_ms = int(frac[:3])
        time_ms = (int(minutes) * 60 + int(seconds)) * 1000 + frac_ms
        lines.append({"time_ms": time_ms, "text": lyric.strip()})
    return lines


def timespan_to_ms(value: Any) -> int:
    """
    将 Windows SMTC 的时间跨度转换为毫秒。

    Windows 中时间跨度有两种表示方式：
      - datetime.timedelta（有 total_seconds 方法）
      - WinRT TimeSpan（以 100ns tick 为单位的 duration 属性）

    参数:
      value — 时间跨度对象

    返回:
      对应的毫秒数（最小为 0）
    """
    if hasattr(value, "total_seconds"):
        return max(0, int(value.total_seconds() * 1000))
    # WinRT TimeSpan is exposed as 100 ns ticks by winsdk.
    try:
        return max(0, int(value.duration / 10_000))  # 100ns → 1ms = 10_000 ticks
    except AttributeError:
        return max(0, int(value / 10_000))
    except Exception:
        return 0


def playback_status_name(status: Any) -> str:
    """
    将 MediaPlaybackStatus 枚举转换为简短的字符串。

    参数:
      status — winsdk 的 MediaPlaybackStatus 枚举值

    返回:
      "playing" | "paused" | "stopped" | "unknown"
    """
    if status == MediaPlaybackStatus.PLAYING:
        return "playing"
    if status == MediaPlaybackStatus.PAUSED:
        return "paused"
    if status == MediaPlaybackStatus.STOPPED:
        return "stopped"
    return str(status).split(".")[-1].lower()


def get_lan_ip() -> str:
    """
    获取本机局域网 IP 地址。

    通过创建一个 UDP socket 并"连接"到外部地址（8.8.8.8），
    操作系统会自动选择本机最合适的网卡 IP 作为源地址。
    这个 socket 不会真的发送数据包，只是用来探测本机 IP。

    返回:
      局域网 IP 字符串，如 "192.168.1.100"
      如果探测失败，返回 "127.0.0.1"
    """
    with contextlib.closing(socket.socket(socket.AF_INET, socket.SOCK_DGRAM)) as sock:
        try:
            sock.connect(("8.8.8.8", 80))
            return sock.getsockname()[0]
        except OSError:
            return "127.0.0.1"


# ======================================================================
# 入口
# ======================================================================

def main() -> None:
    """
    命令行入口。

    用法：
      python windows_media_server.py
      python windows_media_server.py --host 0.0.0.0 --port 8765
      python windows_media_server.py --lyric-timeout 10.0

    参数：
      --host          WebSocket 绑定地址（默认 0.0.0.0）
      --port          WebSocket 端口号（默认 8765）
      --lyric-timeout 歌词 API 超时时间（默认 5 秒）
    """
    parser = argparse.ArgumentParser(description="Windows SMTC to ESP32 WebSocket bridge.")
    parser.add_argument("--host", default="0.0.0.0", help="WebSocket bind host.")
    parser.add_argument("--port", type=int, default=8765, help="WebSocket bind port.")
    parser.add_argument("--lyric-timeout", type=float, default=5.0, help="Lyrics HTTP timeout in seconds.")
    parser.add_argument("--deepseek-key", default=None, help="(已弃用) 改用 --deepseek-file 文件模式.")
    parser.add_argument("--deepseek-file", default=DEEPSEEK_FILE,
                        help="WorkBuddy DeepSeek usage data JSON file path (default: %(default)s)")
    args = parser.parse_args()

    server = MediaServer(args.host, args.port, args.lyric_timeout,
                         deepseek_key=args.deepseek_key,
                         deepseek_file=args.deepseek_file)
    try:
        asyncio.run(server.start())
    except KeyboardInterrupt:
        print("\nStopped.")


if __name__ == "__main__":
    main()
