/// @file player_reader.hpp
/// @brief 玩家档案读取接口。
///
/// 为什么需要这一层：
///   账号密码保留在代码中作为测试数据（依据 docs/07-open-decisions.md 的 D-002），
///   而玩家档案存在 MySQL。两者来源不同，若直接写在 PlayerDirectory 里，
///   「读数据库」与「校验凭据」两种职责会耦合在一起，也无法在没有 MySQL 的
///   情况下做单元测试。
///
///   因此拆成两层：
///     PlayerReader        —— 只负责按 account / player_id 读档案
///     PlayerDirectory     —— 组合档案与凭据，产出凭据校验结果
///
/// 所有者说明（依据 docs/01-architecture.md 与 docs/adr/0003-scope-reduction.md）：
///   players 表的所有者是 **Gateway**。原定的 Player/State 服务已列为非目标、
///   不实现，因此该归属是正式安排，不再是临时例外。本接口只读，不提供写入方法。

#ifndef RGBT_GATEWAY_PLAYER_READER_HPP
#define RGBT_GATEWAY_PLAYER_READER_HPP

#include <optional>
#include <string>
#include <string_view>

namespace rgbt::gateway {

/// 玩家档案记录，对应 players 表的一行。
struct PlayerRecord {
    std::string player_id;
    std::string account;
    std::string display_name;
    std::string status;
};

/// 读取结果。
///
/// 区分 kNotFound 与 kUnavailable 很重要：前者是业务上的「档案不存在」，
/// 后者是依赖故障，对外返回的错误码与可重试语义完全不同。
enum class ReaderStatus {
    kOk,
    kNotFound,
    kUnavailable,
};

class PlayerReader {
public:
    virtual ~PlayerReader() = default;

    /// 按登录账号读取档案。
    virtual ReaderStatus FindByAccount(std::string_view account,
                                       std::optional<PlayerRecord>* out_record) = 0;

    /// 按玩家标识读取档案。
    virtual ReaderStatus FindByPlayerId(std::string_view player_id,
                                        std::optional<PlayerRecord>* out_record) = 0;
};

}  // namespace rgbt::gateway

#endif  // RGBT_GATEWAY_PLAYER_READER_HPP
