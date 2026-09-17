#include "mysql_player_reader.hpp"

#include <vector>

namespace rgbt::gateway {
namespace {

/// 只选择需要的列，避免 SELECT *。
/// 注意：表中不含密码列，因此这里也不可能把密码读进内存。
constexpr const char* kSelectByAccount =
    "SELECT player_id, account, display_name, status FROM players WHERE account = ? LIMIT 1";

constexpr const char* kSelectByPlayerId =
    "SELECT player_id, account, display_name, status FROM players WHERE player_id = ? LIMIT 1";

}  // namespace

MysqlPlayerReader::MysqlPlayerReader(MysqlConnection* connection) : connection_(connection) {}

ReaderStatus MysqlPlayerReader::QueryOne(const std::string& sql, std::string_view param,
                                         std::optional<PlayerRecord>* out_record) {
    if (connection_ == nullptr || out_record == nullptr) {
        return ReaderStatus::kUnavailable;
    }
    out_record->reset();

    std::vector<SqlRow> rows;
    if (!connection_->Query(sql, {std::string(param)}, &rows)) {
        return ReaderStatus::kUnavailable;
    }
    if (rows.empty()) {
        return ReaderStatus::kNotFound;
    }

    const SqlRow& row = rows.front();
    // 期望 4 列；列数不符说明查询与实现不一致，按依赖故障处理而不是猜测。
    if (row.size() != 4) {
        return ReaderStatus::kUnavailable;
    }
    // 这 4 列在建表语句中都是 NOT NULL，出现 NULL 说明数据异常。
    if (!row[0].has_value() || !row[1].has_value() || !row[2].has_value() || !row[3].has_value()) {
        return ReaderStatus::kUnavailable;
    }

    PlayerRecord record;
    record.player_id = *row[0];
    record.account = *row[1];
    record.display_name = *row[2];
    record.status = *row[3];
    *out_record = std::move(record);
    return ReaderStatus::kOk;
}

ReaderStatus MysqlPlayerReader::FindByAccount(std::string_view account,
                                              std::optional<PlayerRecord>* out_record) {
    return QueryOne(kSelectByAccount, account, out_record);
}

ReaderStatus MysqlPlayerReader::FindByPlayerId(std::string_view player_id,
                                               std::optional<PlayerRecord>* out_record) {
    return QueryOne(kSelectByPlayerId, player_id, out_record);
}

bool MysqlPlayerReader::IsHealthy() {
    return connection_ != nullptr && connection_->Ping();
}

}  // namespace rgbt::gateway
