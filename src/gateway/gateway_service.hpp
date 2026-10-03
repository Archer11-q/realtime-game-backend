/// @file gateway_service.hpp
/// @brief Gateway 的对外服务实现。
///
/// 覆盖接口（依据 docs/05-api-and-data.md 第 2 节的路径）：
///   POST /api/v1/login                  登录并创建会话
///   GET  /api/v1/players/me             查询当前玩家
///   POST /api/v1/logout                 结束会话
///   POST /api/v1/matches                进入匹配（TASK-007）
///   GET  /api/v1/matches/current        查询匹配状态（TASK-007）
///   POST /api/v1/matches/current/cancel 取消匹配（TASK-007）
///   POST /api/v1/rooms/join             加入房间（TASK-008）
///   POST /api/v1/rooms/input            提交攻击输入（TASK-008）
///   GET  /api/v1/rooms/state            查询房间状态（TASK-008）
///   GET  /api/v1/results                查询对局结果（TASK-008）
///
/// 服务边界（docs/01-architecture.md）：
///   Gateway 负责 HTTP 接入、鉴权、路由和限流，不保存战斗状态，也**不保存匹配队列
///   或房间状态**。相关接口只做「鉴权 -> 转给对应服务 -> 整理结果」。
///   Gateway **不直读** match_results 表：该表的所有者是 Room/Battle
///   （见 docs/adr/0003-scope-reduction.md）。
///
/// 安全约定：所有涉及玩家身份的接口，player_id **只能来自会话**，绝不使用请求体
/// 中的字段，否则任何登录用户都能替别人入队、加入房间或代打。

#ifndef RGBT_GATEWAY_GATEWAY_SERVICE_HPP
#define RGBT_GATEWAY_GATEWAY_SERVICE_HPP

#include <cstdint>
#include <string>

#include "gateway.pb.h"
#include "match_client.hpp"
#include "player_directory.hpp"
#include "room_client.hpp"
#include "session_store.hpp"
#include "stream_hub.hpp"

namespace rgbt::gateway {

/// 会话有效期（秒）。默认 7 天，同时是配置项便于测试与后续调整。
inline constexpr std::int32_t kDefaultSessionTtlSeconds = 7 * 24 * 60 * 60;

/// 账号与密码的长度上限。超长输入直接判为输入错误，避免把任意长度字符串
/// 传到底层依赖。
inline constexpr std::size_t kMaxAccountLength = 64;
inline constexpr std::size_t kMaxPasswordLength = 128;
inline constexpr std::size_t kMaxRequestIdLength = 64;
inline constexpr std::size_t kMaxTokenLength = 256;

class GatewayServiceImpl : public rgbt::gateway::v1::GatewayService {
public:
    /// @param room 房间客户端。可以为 nullptr（此时房间接口一律返回 503），
    ///        用于只关心登录与匹配的单元测试。
    /// @param stream SSE 订阅表。可以为 nullptr（此时推送接口一律返回 503）；
    ///        成功建立流式响应还需要真实的 HTTP 上下文，因此正向路径只能在
    ///        端到端脚本里验证（见 scripts/verify-stream.sh）。
    GatewayServiceImpl(SessionStore* sessions, PlayerDirectory* players, MatchClient* match,
                       RoomClient* room, StreamHub* stream = nullptr,
                       std::int32_t session_ttl_seconds = kDefaultSessionTtlSeconds);

    void Login(::google::protobuf::RpcController* controller,
               const rgbt::gateway::v1::LoginRequest* request,
               rgbt::gateway::v1::LoginResponse* response,
               ::google::protobuf::Closure* done) override;

    void GetCurrentPlayer(::google::protobuf::RpcController* controller,
                          const rgbt::gateway::v1::GetCurrentPlayerRequest* request,
                          rgbt::gateway::v1::GetCurrentPlayerResponse* response,
                          ::google::protobuf::Closure* done) override;

    void Logout(::google::protobuf::RpcController* controller,
                const rgbt::gateway::v1::LogoutRequest* request,
                rgbt::gateway::v1::LogoutResponse* response,
                ::google::protobuf::Closure* done) override;

    void EnqueueMatch(::google::protobuf::RpcController* controller,
                      const rgbt::gateway::v1::EnqueueMatchRequest* request,
                      rgbt::gateway::v1::EnqueueMatchResponse* response,
                      ::google::protobuf::Closure* done) override;

    void GetMatchStatus(::google::protobuf::RpcController* controller,
                        const rgbt::gateway::v1::GetMatchStatusRequest* request,
                        rgbt::gateway::v1::GetMatchStatusResponse* response,
                        ::google::protobuf::Closure* done) override;

    void CancelMatch(::google::protobuf::RpcController* controller,
                     const rgbt::gateway::v1::CancelMatchRequest* request,
                     rgbt::gateway::v1::CancelMatchResponse* response,
                     ::google::protobuf::Closure* done) override;

    void JoinRoom(::google::protobuf::RpcController* controller,
                  const rgbt::gateway::v1::JoinRoomRequest* request,
                  rgbt::gateway::v1::JoinRoomResponse* response,
                  ::google::protobuf::Closure* done) override;

    void SubmitInput(::google::protobuf::RpcController* controller,
                     const rgbt::gateway::v1::SubmitInputRequest* request,
                     rgbt::gateway::v1::SubmitInputResponse* response,
                     ::google::protobuf::Closure* done) override;

    void GetRoomState(::google::protobuf::RpcController* controller,
                      const rgbt::gateway::v1::GetRoomStateRequest* request,
                      rgbt::gateway::v1::GetRoomStateResponse* response,
                      ::google::protobuf::Closure* done) override;

    void GetMatchResult(::google::protobuf::RpcController* controller,
                        const rgbt::gateway::v1::GetMatchResultRequest* request,
                        rgbt::gateway::v1::GetMatchResultResponse* response,
                        ::google::protobuf::Closure* done) override;

    void StreamEvents(::google::protobuf::RpcController* controller,
                      const rgbt::gateway::v1::StreamEventsRequest* request,
                      rgbt::gateway::v1::StreamEventsResponse* response,
                      ::google::protobuf::Closure* done) override;

    /// @brief 当前是否所有依赖都可用。供健康检查使用。
    [[nodiscard]] bool DependenciesHealthy();

private:
    /// 写统一的错误体，并返回对应的 HTTP 状态码。
    static std::int32_t FillError(rgbt::gateway::v1::Error* error,
                                  rgbt::gateway::v1::ErrorCode code, const std::string& reason,
                                  const std::string& message, const std::string& request_id);

    /// @brief 由 Token 解析出会话所属的 player_id。
    /// @return 200 表示成功；其他值为已写入 error 的 HTTP 状态码，调用方直接返回即可。
    ///
    /// 抽成独立函数的原因：三个匹配接口都需要同一段「校验 Token 格式 -> 查会话 ->
    /// 取 player_id」逻辑，而这段逻辑里的错误码区分（400 / 401 / 503）是最容易写错
    /// 也最需要保持一致的地方。
    std::int32_t ResolvePlayerId(const std::string& token, const std::string& request_id,
                                 std::string* out_player_id, rgbt::gateway::v1::Error* error);

    /// 把 Match 返回的快照写入响应体。
    static void FillMatchStatus(const MatchSnapshot& snapshot,
                                rgbt::gateway::v1::MatchStatusInfo* out);

    /// @brief 把 Match 的调用结果转成错误体 + HTTP 状态码。
    /// @return 200 表示调用成功，调用方应继续填充 match 字段。
    std::int32_t HandleMatchFailure(MatchCallStatus status, const std::string& request_id,
                                    rgbt::gateway::v1::Error* error);

    /// 把 Room 返回的快照写入响应体。
    static void FillRoomState(const RoomSnapshot& snapshot, rgbt::gateway::v1::RoomStateInfo* out);

    /// @brief 把 Room 的调用结果转成错误体 + HTTP 状态码。
    /// @return 200 表示调用成功，调用方应继续填充业务字段。
    /// @param not_found_reason kNotFound 时使用的 reason。房间查询用 room_not_found、
    ///        结果查询用 result_not_found：两者对外都是 404，但排障时需要一眼看出
    ///        是「房间没了」还是「这一局没有结果」。
    std::int32_t HandleRoomFailure(RoomCallStatus status, const std::string& request_id,
                                   rgbt::gateway::v1::Error* error,
                                   const char* not_found_reason = "room_not_found");

    /// 设置 HTTP 状态码、记一次指标、并输出一条 `request_done`（TASK-021）。
    ///
    /// 处理函数一律调用本函数而不是 `ApplyHttpStatus`，这样"漏记账"就不可能发生
    /// ——这是选它做收敛点的理由：新增接口时忘了写日志或指标不会有任何报错，
    /// 只会让面板数字悄悄偏低、让这条路径在日志里完全消失。
    ///
    /// TASK-021 之所以把 `request_done` 放在这里：它是**所有** HTTP 响应（含成功
    /// 路径）的唯一收敛点，因此"每个请求都留下一条可按 trace 检索的记录"这件事
    /// 不依赖谁来记得加日志。此前 15 个接口里只有 2 个（匹配入队、SSE 订阅）
    /// 有结构化日志，登录、进房、结算三条关键路径在 Gateway 侧一条都没有。
    ///
    /// @param request_id 这次请求的关联 id（trace）。为空时不输出 `trace=` 字段
    ///        （宁缺勿假，见 logging.hpp）——**不生成一个伪 id**。
    void ApplyHttpStatusAndRecord(::google::protobuf::RpcController* controller,
                                  std::int32_t status_code, const std::string& request_id);

    /// 真正的记账逻辑。参数用 `google::protobuf::RpcController`（而不是
    /// `brpc::Controller`）是为了让本头文件**不依赖 brpc**：brpc 的头文件对
    /// include 顺序有要求（glog 的导出宏必须在它之前定义），一旦泄漏进这个被广泛
    /// 包含的头，就会以"glog 没有被正确包含"这种看不出根因的报错出现（实测踩到）。
    /// 实现里再做一次 downcast。
    void RecordHttpRequest(::google::protobuf::RpcController* controller,
                           std::int32_t status_code) noexcept;

    /// 写一条 `request_done`。与 `RecordHttpRequest` 分开两个函数、而不是塞进一个：
    /// 指标只对 `/api/v1/` 前缀的路径记账（brpc 内置端点不该混进业务 QPS），
    /// 而"这次请求结束了"这件事对所有接口都要记。
    void RecordRequestDone(::google::protobuf::RpcController* controller, std::int32_t status_code,
                           const std::string& request_id) noexcept;

    /// 构造完成标志。构造过程中不该记账，否则会用到尚未就绪的登记表。
    bool metrics_ready_ = false;

    SessionStore* sessions_;
    // 不加 const：接口方法本身不是 const（实现需要查询外部依赖），
    // 与 sessions_ 的写法保持一致。
    PlayerDirectory* players_;
    MatchClient* match_;
    RoomClient* room_;
    StreamHub* stream_;
    std::int32_t session_ttl_seconds_;
};

}  // namespace rgbt::gateway

#endif  // RGBT_GATEWAY_GATEWAY_SERVICE_HPP
