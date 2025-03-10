#include "source/server/admin/logs_handler.h"

#include <string>

#include "source/common/common/fine_grain_logger.h"
#include "source/common/common/logger.h"
#include "source/server/admin/utils.h"

#include "absl/strings/str_split.h"

namespace Envoy {
namespace Server {

namespace {
// Build the level string to level enum map.
absl::flat_hash_map<absl::string_view, spdlog::level::level_enum> buildLevelMap() {
  absl::flat_hash_map<absl::string_view, spdlog::level::level_enum> levels;

  uint32_t i = 0;
  for (absl::string_view level_string : LogsHandler::levelStrings()) {
    levels[level_string] = static_cast<spdlog::level::level_enum>(i++);
  }

  return levels;
}
} // namespace

LogsHandler::LogsHandler(Server::Instance& server)
    : HandlerContextBase(server), log_levels_(buildLevelMap()) {}

std::vector<absl::string_view> LogsHandler::levelStrings() {
  std::vector<absl::string_view> strings;
  strings.reserve(ARRAY_SIZE(spdlog::level::level_string_views));
  for (spdlog::string_view_t level : spdlog::level::level_string_views) {
    strings.emplace_back(absl::string_view{level.data(), level.size()});
  }
  return strings;
}

Http::Code LogsHandler::handlerLogging(absl::string_view, Http::ResponseHeaderMap&,
                                       Buffer::Instance& response, AdminStream& admin_stream) {
  Http::Utility::QueryParams query_params = admin_stream.queryParams();

  Http::Code rc = Http::Code::OK;
  const absl::Status status = changeLogLevel(query_params);
  if (!status.ok()) {
    rc = Http::Code::BadRequest;
    response.add(fmt::format("error: {}\n\n", status.message()));

    response.add("usage: /logging?<name>=<level> (change single level)\n");
    response.add("usage: /logging?paths=name1:level1,name2:level2,... (change multiple levels)\n");
    response.add("usage: /logging?level=<level> (change all levels)\n");
    response.add("levels: ");
    for (auto level_string_view : spdlog::level::level_string_views) {
      response.add(fmt::format("{} ", level_string_view));
    }

    response.add("\n");
  }

  if (!Logger::Context::useFineGrainLogger()) {
    response.add("active loggers:\n");
    for (const Logger::Logger& logger : Logger::Registry::loggers()) {
      response.add(fmt::format("  {}: {}\n", logger.name(), logger.levelString()));
    }

    response.add("\n");
  } else {
    response.add("active loggers:\n");
    std::string logger_info = getFineGrainLogContext().listFineGrainLoggers();
    response.add(logger_info);
    response.add("\n");
  }

  return rc;
}

Http::Code LogsHandler::handlerReopenLogs(absl::string_view, Http::ResponseHeaderMap&,
                                          Buffer::Instance& response, AdminStream&) {
  server_.accessLogManager().reopen();
  response.add("OK\n");
  return Http::Code::OK;
}

absl::Status LogsHandler::changeLogLevel(Http::Utility::QueryParams& params) {
  // "level" and "paths" will be set to the empty string when this is invoked
  // from HTML without setting them, so clean out empty values.
  auto level = params.find("level");
  if (level != params.end() && level->second.empty()) {
    params.erase(level);
    level = params.end();
  }
  auto paths = params.find("paths");
  if (paths != params.end() && paths->second.empty()) {
    params.erase(paths);
    paths = params.end();
  }

  if (params.empty()) {
    return absl::OkStatus();
  }

  if (params.size() != 1) {
    return absl::InvalidArgumentError("invalid number of parameters");
  }

  if (level != params.end()) {
    // Change all log levels.
    const absl::StatusOr<spdlog::level::level_enum> level_to_use = parseLogLevel(level->second);
    if (!level_to_use.ok()) {
      return level_to_use.status();
    }

    changeAllLogLevels(*level_to_use);
    return absl::OkStatus();
  }

  // Build a map of name:level pairs, a few allocations is ok here since it's
  // not common to call this function at a high rate.
  absl::flat_hash_map<absl::string_view, spdlog::level::level_enum> name_levels;

  if (paths != params.end()) {
    // Bulk change log level by name:level pairs, separated by comma.
    std::vector<absl::string_view> pairs =
        absl::StrSplit(paths->second, ',', absl::SkipWhitespace());
    for (const auto& name_level : pairs) {
      const std::pair<absl::string_view, absl::string_view> name_level_pair =
          absl::StrSplit(name_level, absl::MaxSplits(':', 1), absl::SkipWhitespace());
      auto [name, level] = name_level_pair;
      if (name.empty() || level.empty()) {
        return absl::InvalidArgumentError("empty logger name or empty logger level");
      }

      const absl::StatusOr<spdlog::level::level_enum> level_to_use = parseLogLevel(level);
      if (!level_to_use.ok()) {
        return level_to_use.status();
      }

      name_levels[name] = *level_to_use;
    }
  } else {
    // The HTML admin interface will always populate "level" and "paths" though
    // they may be empty. There's a legacy non-HTML-accessible mechanism to
    // set a single logger to a level, which we'll handle now. In this scenario,
    // "level" and "paths" will not be populated.
    if (params.size() != 1) {
      return absl::InvalidArgumentError("invalid number of parameters");
    }

    // Change particular log level by name.
    const auto it = params.begin();
    const std::string& key = it->first;
    const std::string& value = it->second;

    const absl::StatusOr<spdlog::level::level_enum> level_to_use = parseLogLevel(value);
    if (!level_to_use.ok()) {
      return level_to_use.status();
    }

    name_levels[key] = *level_to_use;
  }

  return changeLogLevels(name_levels);
}

void LogsHandler::changeAllLogLevels(spdlog::level::level_enum level) {
  if (!Logger::Context::useFineGrainLogger()) {
    ENVOY_LOG(info, "change all log levels: level='{}'", spdlog::level::level_string_views[level]);
    Logger::Registry::setLogLevel(level);
  } else {
    // Level setting with Fine-Grain Logger.
    FINE_GRAIN_LOG(info, "change all log levels: level='{}'",
                   spdlog::level::level_string_views[level]);
    getFineGrainLogContext().setAllFineGrainLoggers(level);
  }
}

absl::Status LogsHandler::changeLogLevels(
    const absl::flat_hash_map<absl::string_view, spdlog::level::level_enum>& changes) {
  if (!Logger::Context::useFineGrainLogger()) {
    std::vector<std::pair<Logger::Logger*, spdlog::level::level_enum>> loggers_to_change;
    for (Logger::Logger& logger : Logger::Registry::loggers()) {
      auto name_level_itr = changes.find(logger.name());
      if (name_level_itr == changes.end()) {
        continue;
      }

      loggers_to_change.emplace_back(std::make_pair(&logger, name_level_itr->second));
    }

    // Check if we have any invalid logger in changes.
    if (loggers_to_change.size() != changes.size()) {
      return absl::InvalidArgumentError("unknown logger name");
    }

    for (auto& it : loggers_to_change) {
      Logger::Logger* logger = it.first;
      spdlog::level::level_enum level = it.second;

      ENVOY_LOG(info, "change log level: name='{}' level='{}'", logger->name(),
                spdlog::level::level_string_views[level]);
      logger->setLevel(level);
    }
  } else {
    std::vector<std::pair<SpdLoggerSharedPtr, spdlog::level::level_enum>> loggers_to_change;
    for (auto& it : changes) {
      SpdLoggerSharedPtr logger = getFineGrainLogContext().getFineGrainLogEntry(it.first);
      if (!logger) {
        return absl::InvalidArgumentError("unknown logger name");
      }

      loggers_to_change.emplace_back(std::make_pair(logger, it.second));
    }

    for (auto& it : loggers_to_change) {
      SpdLoggerSharedPtr logger = it.first;
      spdlog::level::level_enum level = it.second;

      FINE_GRAIN_LOG(info, "change log level: name='{}' level='{}'", logger->name(),
                     spdlog::level::level_string_views[level]);
      logger->set_level(level);
    }
  }

  return absl::OkStatus();
}

} // namespace Server
} // namespace Envoy
