/// @file match_queue_store.hpp
/// @brief 匹配队列的旁路快照：格式定义、编解码与存储接口（TASK-015）。
///
/// 定位（见 docs/TASKS.md 的 TASK-015）：
///   队列的**权威状态在 Match 进程内存里**，本模块只是它的旁路快照，
///   用于"进程重启后排队状态不丢"。因此这里的失败策略与房间快照一致：
///   **可以丢弃、不重试、不阻塞业务**，绝不能让一次 Redis 超时影响入队与配对。
///
/// 为什么把编解码做成不依赖 Redis 的纯函数：
///   「快照格式对不对」是本任务最容易出错、也最需要反复构造边界情况的部分
///   （未知版本、截断的字段、离谱的计数、行尾多余字节）。做成纯函数后，
///   这些情况可以在单元测试里逐条构造，而不是只能靠往 Redis 里塞脏数据碰运气。
///   存储实现（redis_match_queue_store）只负责"把行放进去/取出来"。
///
/// 格式（显式带版本号，见下）：
/// @code
///   queued  := "1Q" str(player_id) int(queued_at_ms)
///   matched := "1M" str(match_id) str(room_id) int(stamp_ms) int(玩家数) str(player_id)...
///   str(s)  := <十进制长度> ':' <原始字节>
///   int(v)  := <十进制整数> ';'
/// @endcode
///
/// 三个格式决定及理由：
///   1. **长度前缀而不是分隔符**。字符串字段全部由服务端生成
///      （`p-0001`、`m-…`、`r-…`），但依赖"它们不含某个分隔符"是一个
///      迟早会破的假设；长度前缀对内容完全无要求，也不需要转义。
///   2. **带版本号**。以后改字段时，旧进程读到新格式必须能**识别出来并拒绝**，
///      而不是解析出一支乱七八糟的队列。版本不认识的行会被当作损坏行跳过。
///   3. **存的是"整份队列"而不是增量**：条目之间有 FIFO 顺序，增量更新要自己
///      维护顺序，整份重写天然给出"要么旧的完整队列、要么新的完整队列"。

#ifndef RGBT_MATCH_MATCH_QUEUE_STORE_HPP
#define RGBT_MATCH_MATCH_QUEUE_STORE_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rgbt::match {

/// 快照格式版本。改动字段布局时必须递增，并让旧版本被拒绝。
inline constexpr char kSnapshotFormatVersion = '1';

/// 解析时允许的单组玩家数上限。
///
/// 当前一局固定两人（`kPlayersPerMatch`），这里给到 8 只是**防御性上界**：
/// 一条损坏或恶意构造的行不应该让解析器去循环一个天文数字。
inline constexpr std::size_t kMaxPlayersPerSnapshotGroup = 8;

/// 一条快照条目的类型。
enum class SnapshotEntryKind {
    /// 排队中（含"正在分配房间"的条目——重启后它们退回队列，见 TASK-015 决策 4）。
    kQueued,
    /// 已配对成功，客户端可能还没领取结果。
    kMatched,
};

/// 一条快照条目。
///
/// 为什么两类条目共用一个结构而不是两个类型：它们在 Redis 里是同一个 LIST 的
/// 不同行，解析后要按**写入顺序**一起交给调用方；拆成两个类型会立刻需要一层
/// "带标签的联合"，而 `kind` 字段就是那个标签。
struct MatchQueueEntry {
    SnapshotEntryKind kind = SnapshotEntryKind::kQueued;

    /// kind == kQueued 时有效：排队中的玩家与入队时刻。
    std::string player_id;
    std::int64_t queued_at_ms = 0;

    /// kind == kMatched 时有效：这一局的信息与结果生成时刻。
    std::string match_id;
    std::string room_id;
    std::vector<std::string> player_ids;
    std::int64_t stamp_ms = 0;
};

/// 解码一行的结果。
struct MatchQueueSnapshotRow {
    MatchQueueEntry entry;

    /// 这一行**不可用**的原因；空字符串表示可用。
    /// 沿用 room_snapshot_reader.hpp 里 `RoomSnapshotRow::problem` 的同一套做法：
    /// 解析层负责"能不能用"，调用方只判断"problem 非空即跳过并记日志"。
    std::string problem;
};

/// 把一条条目编码成一行。返回 false 表示字段不合法（调用方跳过该条并记日志）。
///
/// 不合法的情况包括：必填标识为空或超长、时间戳非正、玩家数为 0 或超过上界。
/// **宁可少写一条，也不写一条格式可疑的记录**——读取端无法区分"格式可疑"与
/// "队列本来就这样"。
[[nodiscard]] bool EncodeMatchQueueEntry(const MatchQueueEntry& entry, std::string* out_line);

/// 把一行解码成条目。`problem` 非空表示这一行不可用（未知版本、字段截断、
/// 行尾有多余字节、标识超长、计数越界等）。
[[nodiscard]] MatchQueueSnapshotRow DecodeMatchQueueEntry(std::string_view line);

/// 整份队列快照。
struct MatchQueueSnapshot {
    /// 排队中的条目，**顺序即 FIFO 顺序**。
    std::vector<MatchQueueEntry> queued;
    /// 已配对、可能尚未被客户端领取的结果。
    std::vector<MatchQueueEntry> matched;
};

/// 队列快照的存储接口。实现只需负责"整份放进去 / 整份取出来"。
class MatchQueueStore {
public:
    MatchQueueStore() = default;
    MatchQueueStore(const MatchQueueStore&) = delete;
    MatchQueueStore& operator=(const MatchQueueStore&) = delete;
    virtual ~MatchQueueStore() = default;

    /// @brief 整份覆盖写入。
    /// @return false 表示存储不可用或写入失败。**调用方只记日志、不重试、不阻塞**。
    [[nodiscard]] virtual bool Save(const MatchQueueSnapshot& snapshot) = 0;

    /// @brief 读出全部行。
    ///
    /// @return false 表示存储不可用（此时 `out_rows` 无意义）。
    ///         成功但 `out_rows` 为空表示"没有快照"——这与"读失败"必须区分开：
    ///         前者是正常的首次启动，后者意味着**可能有队列没被恢复**，
    ///         必须在启动日志里说清楚，而不是当成空队列。
    ///
    /// **损坏的行也要返回**（`problem` 非空）：读取层自行丢弃等于静默丢数据。
    [[nodiscard]] virtual bool Load(std::vector<MatchQueueSnapshotRow>* out_rows) = 0;

    /// @brief 存储当前是否可用。用于启动日志与降级判断。
    [[nodiscard]] virtual bool IsHealthy() = 0;
};

}  // namespace rgbt::match

#endif  // RGBT_MATCH_MATCH_QUEUE_STORE_HPP
