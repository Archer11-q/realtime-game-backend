/// @file client.ts
/// @brief Gateway HTTP 接口的薄封装。
///
/// 不引 axios：本项目只需要 fetch + JSON，多一个依赖就多一处供应链与版本风险。
///
/// 错误处理有一条硬性约定：**以 `error.reason` 为准，不以 HTTP 状态码为准。**
/// 状态码会变（400/404/409/503 混用同一套 reason），而 reason 是稳定标识。
/// 界面上的提示因此按 reason 分支，而不是按数字分支——后者在服务端调整分类时
/// 会静默失效。

import type {
  ApiError,
  LoginResponse,
  LogoutResponse,
  MatchResponse,
  MatchResultResponse,
  RoomResponse,
} from './types'

/** 服务端返回的业务错误。`reason` 来自响应体的 `error.reason`。 */
export class ApiFailure extends Error {
  readonly status: number
  readonly reason: string

  constructor(status: number, reason: string, message: string) {
    super(message)
    this.name = 'ApiFailure'
    this.status = status
    this.reason = reason
  }
}

/** 网络层失败（连不上、被代理拒绝），与服务端错误区分开，便于给出不同提示。 */
export class NetworkFailure extends Error {
  constructor(message: string) {
    super(message)
    this.name = 'NetworkFailure'
  }
}

/** 生成本次请求的幂等键。后端要求 request_id 非空且不超过 64 字符。 */
function newRequestId(prefix: string): string {
  const random = Math.random().toString(36).slice(2, 10)
  return `${prefix}-${Date.now().toString(36)}-${random}`
}

interface RequestShape {
  error?: ApiError
}

async function request<T extends RequestShape>(
  method: 'GET' | 'POST',
  path: string,
  options: { body?: unknown; token?: string } = {},
): Promise<T> {
  const headers: Record<string, string> = { Accept: 'application/json' }
  if (options.body !== undefined) {
    headers['Content-Type'] = 'application/json'
  }
  if (options.token !== undefined && options.token !== '') {
    headers['Authorization'] = `Bearer ${options.token}`
  }

  let response: Response
  try {
    response = await fetch(path, {
      method,
      headers,
      body: options.body === undefined ? undefined : JSON.stringify(options.body),
    })
  } catch (cause) {
    throw new NetworkFailure(
      `无法连接 Gateway（${path}）：${cause instanceof Error ? cause.message : String(cause)}`,
    )
  }

  const data = (await response.json().catch(() => null)) as T | null
  if (data === null) {
    throw new ApiFailure(response.status, 'invalid_response', '服务端返回的不是合法 JSON')
  }

  const error = data.error
  if (!response.ok || (error?.reason !== undefined && error.reason !== '')) {
    throw new ApiFailure(
      response.status,
      error?.reason ?? 'unknown',
      error?.message ?? `请求失败（HTTP ${response.status}）`,
    )
  }
  return data
}

export interface Credentials {
  account: string
  password: string
}

export function login(credentials: Credentials): Promise<LoginResponse> {
  return request<LoginResponse>('POST', '/api/v1/login', {
    body: {
      account: credentials.account,
      password: credentials.password,
      request_id: newRequestId('web-login'),
      client_type: 'web',
    },
  })
}

export function logout(token: string): Promise<LogoutResponse> {
  return request<LogoutResponse>('POST', '/api/v1/logout', {
    token,
    body: { token, request_id: newRequestId('web-logout') },
  })
}

export function enqueueMatch(token: string): Promise<MatchResponse> {
  return request<MatchResponse>('POST', '/api/v1/matches', {
    token,
    body: { token, request_id: newRequestId('web-enqueue') },
  })
}

export function getMatchStatus(token: string): Promise<MatchResponse> {
  return request<MatchResponse>('GET', '/api/v1/matches/current', { token })
}

export function cancelMatch(token: string): Promise<MatchResponse> {
  return request<MatchResponse>('POST', '/api/v1/matches/current/cancel', {
    token,
    body: { token, request_id: newRequestId('web-cancel') },
  })
}

export function joinRoom(token: string, roomId: string): Promise<RoomResponse> {
  return request<RoomResponse>('POST', '/api/v1/rooms/join', {
    token,
    body: { token, request_id: newRequestId('web-join'), room_id: roomId },
  })
}

export function submitAttack(token: string, roomId: string): Promise<RoomResponse> {
  return request<RoomResponse>('POST', '/api/v1/rooms/input', {
    token,
    body: { token, request_id: newRequestId('web-input'), room_id: roomId },
  })
}

export function getRoomState(token: string, roomId: string): Promise<RoomResponse> {
  return request<RoomResponse>(
    'GET',
    `/api/v1/rooms/state?room_id=${encodeURIComponent(roomId)}`,
    { token },
  )
}

export function getMatchResult(token: string, matchId: string): Promise<MatchResultResponse> {
  return request<MatchResultResponse>(
    'GET',
    `/api/v1/results?match_id=${encodeURIComponent(matchId)}`,
    { token },
  )
}
