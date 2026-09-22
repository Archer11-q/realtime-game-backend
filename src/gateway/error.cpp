#include "error.hpp"

namespace rgbt::gateway {

std::int32_t HttpStatusOf(rgbt::gateway::v1::ErrorCode code) noexcept {
    using rgbt::gateway::v1::ErrorCode;
    switch (code) {
        case ErrorCode::INVALID_ARGUMENT:
            return 400;
        case ErrorCode::UNAUTHENTICATED:
            return 401;
        case ErrorCode::NOT_FOUND:
            return 404;
        case ErrorCode::RESOURCE_EXHAUSTED:
            // 限流（例如匹配队列已满）。429 而不是 503：请求本身没错，
            // 是当前容量不足，调用方应退避后重试。
            return 429;
        case ErrorCode::UNAVAILABLE:
            return 503;
        case ErrorCode::INTERNAL:
            return 500;
        case ErrorCode::ERROR_CODE_UNSPECIFIED:
            break;
        // protobuf 为枚举生成的哨兵值。它们不应出现在正常流量中，与
        // ERROR_CODE_UNSPECIFIED 同样按内部错误处理。
        case ErrorCode::ErrorCode_INT_MIN_SENTINEL_DO_NOT_USE_:
        case ErrorCode::ErrorCode_INT_MAX_SENTINEL_DO_NOT_USE_:
            break;
    }
    // 未指定与未知错误码都按内部错误处理，避免默认返回成功语义的 200。
    return 500;
}

std::string_view ErrorCodeName(rgbt::gateway::v1::ErrorCode code) noexcept {
    using rgbt::gateway::v1::ErrorCode;
    switch (code) {
        case ErrorCode::ERROR_CODE_UNSPECIFIED:
            return "ERROR_CODE_UNSPECIFIED";
        case ErrorCode::INVALID_ARGUMENT:
            return "INVALID_ARGUMENT";
        case ErrorCode::UNAUTHENTICATED:
            return "UNAUTHENTICATED";
        case ErrorCode::NOT_FOUND:
            return "NOT_FOUND";
        case ErrorCode::RESOURCE_EXHAUSTED:
            return "RESOURCE_EXHAUSTED";
        case ErrorCode::UNAVAILABLE:
            return "UNAVAILABLE";
        case ErrorCode::INTERNAL:
            return "INTERNAL";
        case ErrorCode::ErrorCode_INT_MIN_SENTINEL_DO_NOT_USE_:
        case ErrorCode::ErrorCode_INT_MAX_SENTINEL_DO_NOT_USE_:
            return "UNKNOWN";
    }
    return "UNKNOWN";
}

bool IsRetryable(rgbt::gateway::v1::ErrorCode code) noexcept {
    using rgbt::gateway::v1::ErrorCode;
    // 依赖暂时失败与限流都允许调用方重试（限流需退避）；输入错误和未认证重试无意义。
    return code == ErrorCode::UNAVAILABLE || code == ErrorCode::RESOURCE_EXHAUSTED;
}

}  // namespace rgbt::gateway
