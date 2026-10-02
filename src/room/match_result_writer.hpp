/// @file match_result_writer.hpp
/// @brief 对局结果持久化接口。
///
/// 为什么抽成接口（与 SessionStore、PlayerDirectory、MatchClient 的做法一致）：
///   RoomManager 需要覆盖「写入成功」「MySQL 不可用」「结果尚不存在」三类分支，
///   而这些分支如果只能在有真实 MySQL 时验证，错误路径就永远测不到。
///   抽成接口后，单元测试可以注入假实现精确控制返回。
///
/// 存储归属：match_results 表由 Room/Battle 拥有
/// （见 docs/adr/0003-scope-reduction.md）。写入口径是**同步幂等**：
/// 以 match_id 为幂等业务键，重复写入不产生第二行。

#ifndef RGBT_ROOM_MATCH_RESULT_WRITER_HPP
#define RGBT_ROOM_MATCH_RESULT_WRITER_HPP

#include <string>

#include "room_types.hpp"

namespace rgbt::room {

/// 写入结果。
///
/// 只区分成功与「依赖不可用」：调用方需要的决策是「能不能把房间标记为已完成」，
/// 而不是具体是连接失败还是语句失败。细节在实现里通过日志与 last_error 暴露。
enum class WriteStatus {
    kOk,
    kUnavailable,
};

/// 读取结果。
///
/// kNotFound 与 kUnavailable 必须分开：前者是「确实没有这条结果」，
/// 对外是 404 类语义；后者是依赖故障，对外是 503 且可重试。
/// 混在一起会让调用方把「数据库挂了」误报成「对局结果不存在」。
enum class ReadStatus {
    kOk,
    kNotFound,
    kUnavailable,
};

class MatchResultWriter {
public:
    MatchResultWriter() = default;
    MatchResultWriter(const MatchResultWriter&) = delete;
    MatchResultWriter& operator=(const MatchResultWriter&) = delete;
    MatchResultWriter(MatchResultWriter&&) = delete;
    MatchResultWriter& operator=(MatchResultWriter&&) = delete;
    virtual ~MatchResultWriter() = default;

    /// @brief 幂等写入一条对局结果。同一 match_id 重复调用不产生第二行。
    [[nodiscard]] virtual WriteStatus Write(const MatchResultRecord& record) = 0;

    /// @brief 按 match_id 读取对局结果。
    [[nodiscard]] virtual ReadStatus Read(const std::string& match_id,
                                          MatchResultRecord* out_record) = 0;

    /// @brief 探测存储是否可用，供启动日志使用。
    [[nodiscard]] virtual bool IsHealthy() = 0;
};

}  // namespace rgbt::room

#endif  // RGBT_ROOM_MATCH_RESULT_WRITER_HPP
