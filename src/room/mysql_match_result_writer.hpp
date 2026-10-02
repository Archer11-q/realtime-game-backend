/// @file mysql_match_result_writer.hpp
/// @brief 基于 MySQL 的对局结果读写实现。

#ifndef RGBT_ROOM_MYSQL_MATCH_RESULT_WRITER_HPP
#define RGBT_ROOM_MYSQL_MATCH_RESULT_WRITER_HPP

#include <string>

#include "common/mysql_connection.hpp"
#include "match_result_writer.hpp"

namespace rgbt::room {

/// 对局结果的 MySQL 实现。
///
/// 表结构由 migrations/003_create_match_results.sql 建立：
///   match_id(PK) / room_id / winner_id(可空) / player_count / started_at / finished_at
class MysqlMatchResultWriter final : public MatchResultWriter {
public:
    /// 不接管 connection 的所有权，调用方需保证其生命周期覆盖本对象。
    explicit MysqlMatchResultWriter(rgbt::common::MysqlConnection* connection);

    [[nodiscard]] WriteStatus Write(const MatchResultRecord& record) override;

    [[nodiscard]] ReadStatus Read(const std::string& match_id,
                                  MatchResultRecord* out_record) override;

    [[nodiscard]] bool IsHealthy() override;

private:
    rgbt::common::MysqlConnection* connection_;
};

}  // namespace rgbt::room

#endif  // RGBT_ROOM_MYSQL_MATCH_RESULT_WRITER_HPP
