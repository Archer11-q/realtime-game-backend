/// @file types.ts
/// @brief 与 api/proto/gateway.proto 一一对应的响应类型。
///
/// 字段名保持 **snake_case**，不做 camelCase 转换。
/// 理由：brpc 的 HTTP 映射直接用 proto 字段名序列化 JSON。在前端加一层命名转换
/// 只会多出一处容易写错、且出错时表现为「字段读出来是 undefined」的映射——
/// 这类错误在 TypeScript 里也无法被静态发现（转换函数通常是 any 进出）。
///
/// 所有字段都声明为可选：brpc 不输出取默认值的字段。例如成功响应里没有 `error`，
/// 平局的 `winner_id` 会是空字符串而不是缺失。前端必须对两种形态都健壮。

/** 服务端统一错误体。`reason` 是稳定可判定的标识，`message` 只用于展示。 */
export interface ApiError {
  code?: string
  reason?: string
  message?: string
  request_id?: string
}

export interface PlayerInfo {
  player_id?: string
  display_name?: string
  status?: string
}

export interface LoginResponse {
  status_code?: number
  token?: string
  expires_in_seconds?: number
  player?: PlayerInfo
  error?: ApiError
}

export interface LogoutResponse {
  status_code?: number
  error?: ApiError
}

/** state 取值：idle / queued / matched / timeout。 */
export interface MatchStatusInfo {
  state?: string
  match_id?: string
  room_id?: string
  player_ids?: string[]
  queued_at_ms?: number
  queue_size?: number
}

export interface MatchResponse {
  status_code?: number
  match?: MatchStatusInfo
  error?: ApiError
}

export interface RoomPlayerInfo {
  player_id?: string
  hp?: number
  /** 表示「已在房间内」，不是「网络是否连通」。见 src/room/room_types.hpp。 */
  connected?: boolean
}

/** state 取值：created / waiting / playing / finishing / finished / aborted。 */
export interface RoomStateInfo {
  room_id?: string
  match_id?: string
  state?: string
  frame?: number
  players?: RoomPlayerInfo[]
  finish_reason?: string
  winner_id?: string
  started_at_ms?: number
  finished_at_ms?: number
}

export interface RoomResponse {
  status_code?: number
  room?: RoomStateInfo
  error?: ApiError
}

export interface MatchResultInfo {
  match_id?: string
  room_id?: string
  winner_id?: string
  player_count?: number
  started_at_ms?: number
  finished_at_ms?: number
}

export interface MatchResultResponse {
  status_code?: number
  result?: MatchResultInfo
  error?: ApiError
}

/** SSE 事件的版本化信封，与 docs/05-api-and-data.md 第 2 节一致。 */
export interface StreamEnvelope<TPayload = unknown> {
  version?: number
  type?: string
  sequence?: number
  timestamp_ms?: number
  payload?: TPayload
}

export type RoomStateEnvelope = StreamEnvelope<RoomStateInfo>

export interface SessionReadyPayload {
  player_id?: string
  room_id?: string
  server_time_ms?: number
}
