#pragma once

#include <raftpp/logging.h>

#define PULPFS_LOG_TRACE(...)                                                                    \
    RAFTPP_LOGGER_CALL("pulpfs", ::raftpp::logging::LogLevel::kTrace, __VA_ARGS__)
#define PULPFS_LOG_DEBUG(...)                                                                    \
    RAFTPP_LOGGER_CALL("pulpfs", ::raftpp::logging::LogLevel::kDebug, __VA_ARGS__)
#define PULPFS_LOG_INFO(...)                                                                     \
    RAFTPP_LOGGER_CALL("pulpfs", ::raftpp::logging::LogLevel::kInfo, __VA_ARGS__)
#define PULPFS_LOG_WARN(...)                                                                     \
    RAFTPP_LOGGER_CALL("pulpfs", ::raftpp::logging::LogLevel::kWarn, __VA_ARGS__)
#define PULPFS_LOG_ERROR(...)                                                                    \
    RAFTPP_LOGGER_CALL("pulpfs", ::raftpp::logging::LogLevel::kError, __VA_ARGS__)
#define PULPFS_LOG_CRITICAL(...)                                                                 \
    RAFTPP_LOGGER_CALL("pulpfs", ::raftpp::logging::LogLevel::kCritical, __VA_ARGS__)
