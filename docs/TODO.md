# MimiClaw vs Nanobot — Feature Gap Tracker

> Comparing against `nanobot/` reference implementation. Tracks features MimiClaw has not yet aligned with.
> Priority: P0 = Core missing, P1 = Important enhancement, P2 = Nice to have

---

## P0 — Core Agent Capabilities

### [ ] Tool Use Loop (multi-turn agent iteration)
- **nanobot**: `loop.py` L167-210 — while loop calls LLM, checks `response.has_tool_calls`, executes tools, feeds results back into messages, repeats until LLM stops calling tools (max 20 iterations)
- **MimiClaw**: `agent_loop.c` only makes a single LLM call (one-shot), cannot use any tools
- **Scope**: Need to parse Anthropic API `tool_use` content blocks, implement tool execution loop
- **Note**: Anthropic tool_use format differs from OpenAI — uses content blocks, not function_call

### [ ] Memory Write via Tool Use (agent-driven memory persistence)
- **openclaw**: Agent uses standard `write`/`edit` tools to write `MEMORY.md` and `memory/YYYY-MM-DD.md`; system prompt instructs agent to persist important information; pre-compaction memory flush triggers a silent agent turn to save durable memories before context window limit
- **MimiClaw**: `memory_write_long_term` and `memory_append_today` exist but are only called from CLI; agent loop never writes memory
- **Scope**: Expose `memory_write` and `memory_append_today` as tool_use tools for Claude; add system prompt guidance on when to persist memory; optionally add pre-compaction flush (trigger memory save when session history nears `MIMI_SESSION_MAX_MSGS`)
- **Depends on**: Tool Use Loop

### [ ] Tool Registry + Built-in Tools
- **nanobot**: `tools/registry.py` — dynamic tool registration/execution, `tools/base.py` defines abstract Tool base class
- **nanobot built-in tools**:
  - `read_file` — read files (`tools/filesystem.py`)
  - `write_file` — write files
  - `edit_file` — edit files
  - `list_dir` — list directory
  - `exec` — execute shell commands (`tools/shell.py`)
  - `web_search` — web search (`tools/web.py`)
  - `web_fetch` — fetch web pages
  - `message` — send message to user (`tools/message.py`)
  - `spawn` — launch subagent (`tools/spawn.py`)
- **MimiClaw**: No tool system at all
- **Recommendation**: Reasonable tool subset for ESP32: `read_file`, `write_file`, `list_dir` (SPIFFS), `message`. Shell/web not suitable for MCU

### [ ] Subagent / Spawn Background Tasks
- **nanobot**: `subagent.py` — SubagentManager spawns independent agent instances with isolated tool sets and system prompts, announces results back to main agent via system channel
- **MimiClaw**: Not implemented
- **Recommendation**: ESP32 memory is limited; simplify to a single background FreeRTOS task for long-running work, inject result into inbound queue on completion

---

## P1 — Important Features

### [ ] Telegram User Allowlist (allow_from)
- **nanobot**: `channels/base.py` L59-82 — `is_allowed()` checks sender_id against allow_list
- **MimiClaw**: No authentication; anyone can message the bot and consume API credits
- **Recommendation**: Store allow_from list in NVS, filter in `process_updates()`

### [ ] Telegram Markdown to HTML Conversion
- **nanobot**: `channels/telegram.py` L16-76 — `_markdown_to_telegram_html()` full converter: code blocks, inline code, bold, italic, links, strikethrough, lists
- **MimiClaw**: Uses `parse_mode: Markdown` directly; special characters can cause send failures (has fallback to plain text)
- **Recommendation**: Implement simplified Markdown-to-HTML converter, or switch to `parse_mode: HTML`

### [ ] Telegram /start Command
- **nanobot**: `telegram.py` L183-192 — handles `/start` command, replies with welcome message
- **MimiClaw**: Not handled; /start is sent to Claude as a regular message

### [ ] Telegram Media Handling (photos/voice/files)
- **nanobot**: `telegram.py` L194-289 — handles photo, voice, audio, document; downloads files; transcribes voice
- **MimiClaw**: Only processes `message.text`, ignores all media messages
- **Recommendation**: Images can be base64-encoded for Claude Vision; voice requires Whisper API (extra HTTPS request)

### [ ] Skills System (pluggable capabilities)
- **nanobot**: `agent/skills.py` — loads skills from SKILL.md files, supports always-loaded and on-demand, frontmatter metadata, requirements checking
- **MimiClaw**: Not implemented
- **Recommendation**: Simplified version: store SKILL.md files on SPIFFS, load into system prompt via context_builder

### [ ] Full Bootstrap File Alignment
- **nanobot**: Loads `AGENTS.md`, `SOUL.md`, `USER.md`, `TOOLS.md`, `IDENTITY.md` (5 files)
- **MimiClaw**: Only loads `SOUL.md` and `USER.md`
- **Recommendation**: Add AGENTS.md (behavior guidelines) and TOOLS.md (tool documentation)

### [ ] Longer Memory Lookback
- **nanobot**: `memory.py` L56-80 — `get_recent_memories(days=7)` defaults to 7 days
- **MimiClaw**: `context_builder.c` only reads last 3 days
- **Recommendation**: Make configurable, but mind token budget

### [ ] System Prompt Tool Guidance
- **nanobot**: `context.py` L74-101 — includes current time, workspace path, tool usage instructions
- **MimiClaw**: Has current time, but lacks tool usage guide and workspace description
- **Depends on**: Tool Use implementation

### [ ] Message Metadata (media, reply_to, metadata)
- **nanobot**: `bus/events.py` — InboundMessage has media, metadata fields; OutboundMessage has reply_to
- **MimiClaw**: `mimi_msg_t` only has channel + chat_id + content
- **Recommendation**: Extend msg struct, add media_path and metadata fields

### [ ] Outbound Subscription Pattern
- **nanobot**: `bus/queue.py` L41-49 — supports `subscribe_outbound(channel, callback)` subscription model
- **MimiClaw**: Hardcoded if-else dispatch
- **Recommendation**: Current approach is simple and reliable; not worth changing with few channels

---

## P2 — Advanced Features

### [ ] Cron Scheduled Task Service
- **nanobot**: `cron/service.py` — full cron scheduler supporting at/every/cron expressions, persistent storage, timed agent triggers
- **MimiClaw**: Not implemented
- **Recommendation**: Use FreeRTOS timer for simplified version, support "every N minutes" only

### [ ] Heartbeat Service
- **nanobot**: `heartbeat/service.py` — reads HEARTBEAT.md every 30 minutes, triggers agent if tasks are found
- **MimiClaw**: Not implemented
- **Recommendation**: Simple FreeRTOS timer that periodically checks HEARTBEAT.md

### [ ] Multi-LLM Provider Support
- **nanobot**: `providers/litellm_provider.py` — supports OpenRouter, Anthropic, OpenAI, Gemini, DeepSeek, Groq, Zhipu, vLLM via LiteLLM
- **MimiClaw**: Hardcoded to Anthropic Messages API
- **Recommendation**: Abstract LLM interface, support OpenAI-compatible API (most providers are compatible)

### [ ] Voice Transcription
- **nanobot**: `providers/transcription.py` — Groq Whisper API
- **MimiClaw**: Not implemented
- **Recommendation**: Requires extra HTTPS request to Whisper API: download Telegram voice -> forward -> get text

### [ ] YAML Config File System
- **nanobot**: `config/loader.py` + `config/schema.py` — Pydantic config validation, YAML config support
- **MimiClaw**: All configuration via NVS key-value storage
- **Recommendation**: Current NVS approach is suitable for MCU, no change needed

### [ ] WebSocket Gateway Protocol Enhancement
- **nanobot**: Gateway port 18790 + richer protocol
- **MimiClaw**: Basic JSON protocol, lacks streaming token push
- **Recommendation**: Add `{"type":"token","content":"..."}` streaming push

### [ ] Multi-Channel Manager
- **nanobot**: `channels/manager.py` — unified lifecycle management for multiple channels
- **MimiClaw**: Hardcoded in app_main()
- **Recommendation**: Not worth abstracting with few channels

### [ ] WhatsApp / Feishu Channels
- **nanobot**: `channels/whatsapp.py`, `channels/feishu.py`
- **MimiClaw**: Only Telegram + WebSocket
- **Recommendation**: Low priority, Telegram is sufficient

### [x] ~~Telegram Proxy Support (HTTP CONNECT)~~
- Implemented: HTTP CONNECT tunnel via `proxy/http_proxy.c`, configurable via NVS + CLI (`set_proxy`/`clear_proxy`)

### [ ] Session Metadata Persistence
- **nanobot**: `session/manager.py` L136-153 — session file includes metadata line (created_at, updated_at)
- **MimiClaw**: JSONL only stores role/content/ts, no metadata header
- **Recommendation**: Low priority

---

## Completed Alignment

- [x] Telegram Bot long polling (getUpdates)
- [x] Message Bus (inbound/outbound queues)
- [x] Agent Loop basic flow (single LLM call)
- [x] Claude API (Anthropic Messages API + SSE streaming)
- [x] Context Builder (system prompt + bootstrap files + memory)
- [x] Memory Store (MEMORY.md + daily notes)
- [x] Session Manager (JSONL per chat_id, ring buffer history)
- [x] WebSocket Gateway (port 18789, JSON protocol)
- [x] Serial CLI (esp_console, 14 commands)
- [x] HTTP CONNECT Proxy (Telegram + Claude API via proxy tunnel)
- [x] OTA Update
- [x] WiFi Manager (NVS credentials, exponential backoff)
- [x] SPIFFS storage
- [x] NVS configuration (token, API key, model)

---

## Suggested Implementation Order

```
1. Tool Use Loop + Tool Registry    <- this determines whether the agent is truly "intelligent"
2. Memory Write via Tool Use         <- makes the agent actually remember
3. Built-in Tools (read_file, write_file, message)
3. Telegram Allowlist (allow_from)   <- security essential
4. Bootstrap File Completion (AGENTS.md, TOOLS.md)
5. Subagent (simplified)
6. Telegram Markdown -> HTML
7. Media Handling
8. Cron / Heartbeat
9. Other enhancements
```
