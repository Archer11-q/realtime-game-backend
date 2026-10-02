/// @file event_sink.hpp
/// @brief 事件出口：把一段已格式化好的 SSE 文本写给一个下游连接。
///
/// 为什么抽成接口（与 SessionStore、MatchClient、RoomClient 的做法一致）：
///   StreamHub 里最容易写错、也最需要验证的三条分支是「写失败即清理订阅」
///   「无订阅者不轮询」「房间结束后关闭流」。如果它们只能靠端到端脚本碰运气，
///   就永远测不充分。抽象出接口后，单元测试可以注入一个可控的假出口，
///   精确制造写失败与连接断开。
///
/// 实现方（见 gateway_service.cpp 的 ProgressiveAttachmentSink）把数据交给
/// brpc 的 ProgressiveAttachment，从而以 chunked 方式持续写出。

#ifndef RGBT_GATEWAY_EVENT_SINK_HPP
#define RGBT_GATEWAY_EVENT_SINK_HPP

#include <string>

namespace rgbt::gateway {

class EventSink {
public:
    EventSink() = default;
    EventSink(const EventSink&) = delete;
    EventSink& operator=(const EventSink&) = delete;
    EventSink(EventSink&&) = delete;
    EventSink& operator=(EventSink&&) = delete;
    virtual ~EventSink() = default;

    /// @brief 写一段数据。
    /// @return false 表示连接已断，调用方应当清理该订阅。
    ///
    /// 约定：本方法可能阻塞（对端读取慢时会受到背压），因此调用方**不得在持锁
    /// 状态下调用它**——一次慢客户端不应该拖住整个订阅表。
    [[nodiscard]] virtual bool Write(const std::string& data) = 0;

    /// @brief 显式结束这条流。析构同样会结束，这里给出可言说的语义。
    virtual void Close() = 0;
};

}  // namespace rgbt::gateway

#endif  // RGBT_GATEWAY_EVENT_SINK_HPP
