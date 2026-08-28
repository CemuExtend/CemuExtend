#pragma once

#include <string_view>

class LoggingCallbacks
{
  public:
	virtual void Log(std::string_view filter, std::string_view message) {}
	virtual void Log(std::string_view filter, std::wstring_view message) {}
	virtual ~LoggingCallbacks() = default;
};

void cemuLog_setCallbacks(LoggingCallbacks* loggingCallbacks);
void cemuLog_clearCallbacks();
