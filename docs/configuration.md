# Nano AI Search 配置

## 最小运行配置

纯网页 AI Search 只需要以下配置，其中前三项必填：

| 环境变量 | 必需 | 说明 |
|---|---:|---|
| `RAG_CHAT_ENDPOINT_URL` | 是 | OpenAI-compatible Responses endpoint |
| `RAG_CHAT_API_KEY` | 是 | 模型服务 Key；不要提交到 Git |
| `RAG_CHAT_MODEL_ID` | 是 | Planner 与最终回答使用的模型 |
| `RAG_WEB_SEARCH_MODEL_ID` | 否 | 默认复用 Chat 模型 |

`RAG_WEB_SEARCH_PROVIDER=openai` 会通过 Responses API 的 `web_search`
工具搜索网页。`.env.example` 已给出可直接复制的配置模板。

## 可选能力

- 本地文档、图片、视频查询：需要 Embedding endpoint，并先完成入库。
- 上传：需要 MySQL 和 S3-compatible 对象存储。
- 异步入库：在上传配置之外还需要 Kafka。
- 图片入库：需要 Vision endpoint。
- 视频入库：需要 Vision、Speech-to-Text、FFmpeg 和 FFprobe。
- 遥测：启用时需要 OTLP/HTTP Collector。

Nano 默认使用 C++ 进程内存索引，进程退出后索引不会保留。这是为了让最小版无需
Milvus 即可启动；需要持久化向量库时再单独增加存储适配器。

## 服务端口

| 服务 | 默认地址 |
|---|---|
| React | `http://127.0.0.1:5173` |
| Python API | `http://127.0.0.1:8000` |
| C++ HTTP Core | `http://127.0.0.1:8081` |

一键启动脚本支持用 `NANO_WEB_HOST`、`NANO_WEB_PORT`、`NANO_API_HOST`、
`NANO_API_PORT`、`NANO_CORE_HOST`、`NANO_CORE_PORT` 覆盖这些默认值。

Python 与 C++ 之间只有三个 JSON endpoint：

- `GET /health`
- `POST /v1/execute-plan`
- `POST /v1/index-asset`
