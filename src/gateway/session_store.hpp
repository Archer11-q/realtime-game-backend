/// @file session_store.hpp
/// @brief 登录会话存储（Redis）。
///
/// 依据 docs/05-api-and-data.md 第 4 节：
///   * Redis 用于「登录 Session 和连接映射」。
///   * Key 必须带环境和服务前缀。
///   * TTL 必须明确，禁止无期限堆叠临时 Key。
///
/// Key 规则（实现见 session_store.cpp）：
///   <env>:<service>:session:<token>        -> Hash，会话内容
///   <env>:<service>:idem:login:<request_id> -> String，登录幂等映射
///
/// 幂等（依据第 5 节「登录会话创建必须定义幂等键」）：
///   同一 request_id 重复登录返回同一 Token，不创建新会话，也不延长既有 TTL。

#ifndef RGBT_GATEWAY_SESSION_STORE_HPP
#define RGBT_GATEWAY_SESSION_STORE_HPP

#include <cstdint>
#include <string>
#include <string_view>

namespace rgbt::gateway {

/// 会话内容。
struct SessionRecord {
    std::string token;
    std::string player_id;
    std::string client_type;
    std::int64_t created_at_unix = 0;
};

/// 存储操作结果。
///
/// 区分 kNotFound 与 kUnavailable 很重要：前者是业务上的「会话不存在」，
/// 后者是依赖故障，两者对外返回的错误码和可重试语义完全不同。
enum class StoreStatus {
    kOk,
    kNotFound,
    kUnavailable,
};

/// 会话存储接口。
///
/// 抽成接口是为了让服务层可以在没有 Redis 的情况下被单元测试，
/// 也便于后续替换实现。
class SessionStore {
public:
    virtual ~SessionStore() = default;

    /// 创建会话。同一 request_id 已存在时返回既有 Token。
    /// @param ttl_seconds 会话有效期，必须为正数。
    /// @param out_token 输出参数，成功时写入 Token。
    virtual StoreStatus CreateSession(const std::string& request_id, const std::string& player_id,
                                      const std::string& client_type, std::int32_t ttl_seconds,
                                      std::string* out_token) = 0;

    /// 读取会话。
    virtual StoreStatus GetSession(const std::string& token, SessionRecord* out_session) = 0;

    /// 删除会话。会话本就不存在时返回 kNotFound，由调用方决定是否视为成功。
    virtual StoreStatus DeleteSession(const std::string& token) = 0;
};

}  // namespace rgbt::gateway

#endif  // RGBT_GATEWAY_SESSION_STORE_HPP
