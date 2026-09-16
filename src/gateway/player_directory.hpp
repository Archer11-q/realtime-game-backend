/// @file player_directory.hpp
/// @brief 测试身份目录。
///
/// 范围说明（对应 docs/07-open-decisions.md D-002）：
///   第一版使用固定的测试身份，不实现注册系统，也**不把账号密码存入 MySQL**。
///   因此本模块是「测试数据」，不是正式的玩家档案服务。
///   正式玩家档案属于 Player/State 服务，留到后续阶段，届时本接口的调用方
///   （服务层）不需要改动。
///
/// 边界（对应 docs/01-architecture.md 服务边界）：
///   本模块只负责「凭据 -> 玩家标识」的校验，不持有会话，不做 Token 管理。

#ifndef RGBT_GATEWAY_PLAYER_DIRECTORY_HPP
#define RGBT_GATEWAY_PLAYER_DIRECTORY_HPP

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "gateway.pb.h"

namespace rgbt::gateway {

/// 玩家账号记录。仅测试数据使用，真实实现会来自 Player/State 服务。
struct PlayerAccount {
    std::string account;
    std::string password;
    std::string player_id;
    std::string display_name;
    std::string status;
};

/// 凭据校验结果。
enum class CredentialStatus {
    kOk,
    kInvalidCredential,
    kAccountDisabled,
};

class PlayerDirectory {
public:
    /// 使用内置的测试账号构造。
    ///
    /// 测试账号是刻意写成代码内数据的：真实实现会替换为对 Player/State 的调用，
    /// 而当前阶段不引入数据库，避免把「注册与档案」这类未确认的范围提前拉进来。
    static PlayerDirectory WithBuiltinTestAccounts();

    /// 使用显式提供的账号构造，便于测试。
    explicit PlayerDirectory(std::vector<PlayerAccount> accounts);

    /// 空目录。提供默认构造是为了让本类可以安全地作为成员变量声明，
    /// 避免使用方因为缺少默认构造而在类定义处产生一连串级联编译错误。
    PlayerDirectory() = default;

    /// 校验凭据。
    /// @return kOk 且 player 被填充；否则返回对应失败原因，player 不被修改。
    [[nodiscard]] CredentialStatus Authenticate(std::string_view account, std::string_view password,
                                                rgbt::gateway::v1::PlayerInfo* player) const;

    /// 按玩家标识查询。用于 GetCurrentPlayer 这类已知身份的路径。
    [[nodiscard]] std::optional<rgbt::gateway::v1::PlayerInfo> FindByPlayerId(
        std::string_view player_id) const;

private:
    std::vector<PlayerAccount> accounts_;
};

}  // namespace rgbt::gateway

#endif  // RGBT_GATEWAY_PLAYER_DIRECTORY_HPP
