/// @file mysql_player_reader.hpp
/// @brief 基于 MySQL 的玩家档案读取实现。

#ifndef RGBT_GATEWAY_MYSQL_PLAYER_READER_HPP
#define RGBT_GATEWAY_MYSQL_PLAYER_READER_HPP

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "mysql_connection.hpp"
#include "player_reader.hpp"

namespace rgbt::gateway {

/// 只读玩家档案。
///
/// 只提供读取方法，没有写入方法——这是刻意的：本表在 Phase 1 期间由 Gateway
/// 只读，写入由迁移脚本负责（见 ADR-0002）。
class MysqlPlayerReader : public PlayerReader {
public:
    /// 不接管 connection 的所有权，调用方需保证其生命周期覆盖本对象。
    explicit MysqlPlayerReader(MysqlConnection* connection);

    ReaderStatus FindByAccount(std::string_view account,
                               std::optional<PlayerRecord>* out_record) override;

    ReaderStatus FindByPlayerId(std::string_view player_id,
                                std::optional<PlayerRecord>* out_record) override;

    /// @brief 探测底层连接是否可用，供启动日志使用。
    [[nodiscard]] bool IsHealthy();

private:
    /// 执行查询并转换为 PlayerRecord。
    ReaderStatus QueryOne(const std::string& sql, std::string_view param,
                          std::optional<PlayerRecord>* out_record);

    MysqlConnection* connection_;
};

}  // namespace rgbt::gateway

#endif  // RGBT_GATEWAY_MYSQL_PLAYER_READER_HPP
