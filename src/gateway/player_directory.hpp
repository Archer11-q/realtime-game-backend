/// @file player_directory.hpp
/// @brief 玩家身份目录：把「凭据」与「玩家档案」组合成鉴权结果。
///
/// 为什么是接口而不是具体类：
///   真实实现需要读 MySQL，而单元测试不应该依赖数据库。抽成接口后，
///   测试可以注入内存实现，覆盖「档案不存在 / 依赖不可用 / 账号禁用」等
///   难以在集成环境稳定复现的路径。这与 SessionStore 的处理方式一致。
///
/// 数据来源（依据 docs/07-open-decisions.md 的 D-002）：
///   * 玩家档案：MySQL 的 players 表（Phase 1 期间由 Gateway 只读，见 ADR-0002）。
///   * 账号密码：代码中的测试身份，以 SHA-256 摘要形式比较，不进入数据库。
///
/// 边界（依据 docs/01-architecture.md 服务边界）：
///   本模块只负责「凭据 + 档案 -> 鉴权结果」，不持有会话，不做 Token 管理。

#ifndef RGBT_GATEWAY_PLAYER_DIRECTORY_HPP
#define RGBT_GATEWAY_PLAYER_DIRECTORY_HPP

#include <memory>
#include <optional>
#include <string_view>

#include "gateway.pb.h"

namespace rgbt::gateway {

/// 凭据与档案的校验结果。
enum class CredentialStatus {
    kOk,
    /// 账号或密码不正确。账号不存在与密码错误共用此结果，避免泄露账号是否存在。
    kInvalidCredential,
    /// 账号存在但已被禁用。
    kAccountDisabled,
    /// 依赖（数据库）暂时不可用，调用方可有限重试。
    kUnavailable,
};

class PlayerDirectory {
public:
    virtual ~PlayerDirectory() = default;

    /// 校验凭据。成功时填充 player。
    [[nodiscard]] virtual CredentialStatus Authenticate(std::string_view account,
                                                        std::string_view password,
                                                        rgbt::gateway::v1::PlayerInfo* player) = 0;

    /// 按玩家标识查询档案。用于 GetCurrentPlayer 这类已知身份的路径。
    [[nodiscard]] virtual std::optional<rgbt::gateway::v1::PlayerInfo> FindByPlayerId(
        std::string_view player_id) = 0;

    /// @brief 探测依赖是否可用，供启动日志与健康检查使用。
    ///
    /// 内存实现恒为 true；数据库实现会实际探测连接。
    [[nodiscard]] virtual bool IsHealthy() = 0;

    /// @brief 构建内置测试身份的目录（内存实现，不访问数据库）。
    ///
    /// 用途：单元测试，以及在未配置数据库时的本地快速验证。
    /// 正常服务路径应使用数据库实现。
    [[nodiscard]] static std::unique_ptr<PlayerDirectory> WithBuiltinTestAccounts();
};

}  // namespace rgbt::gateway

#endif  // RGBT_GATEWAY_PLAYER_DIRECTORY_HPP
