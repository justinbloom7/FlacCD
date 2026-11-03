#include "flaccd/mount/AgentProtocol.h"

#include <charconv>
#include <stdexcept>

namespace flaccd {
namespace {

void appendNumber(std::string& text, std::size_t number) {
   if (number > MAX_AGENT_MESSAGE) {
      throw std::invalid_argument("IPC field too large");
   }
   for (unsigned shift = 0; shift < 32; shift += 8) {
      text.push_back(static_cast<char>((number >> shift) & 255));
   }
}

std::size_t readNumber(std::string_view& text) {
   if (text.size() < 4) {
      throw std::invalid_argument("truncated IPC number");
   }
   std::size_t number = 0;
   for (unsigned position = 0; position < 4; ++position) {
      number |= static_cast<std::size_t>(static_cast<unsigned char>(text[position])) << (position * 8);
   }
   text.remove_prefix(4);
   return number;
}

std::string encode(const std::vector<std::string>& fields) {
   std::string result;
   appendNumber(result, fields.size());
   for (const auto& field : fields) {
      appendNumber(result, field.size());
      if (result.size() + field.size() > MAX_AGENT_MESSAGE) {
         throw std::invalid_argument("IPC message too large");
      }
      result += field;
   }
   return result;
}

std::vector<std::string> decode(std::string_view text) {
   if (text.size() > MAX_AGENT_MESSAGE) {
      throw std::invalid_argument("IPC message too large");
   }
   const auto count = readNumber(text);
   if (count > 4096) {
      throw std::invalid_argument("too many IPC fields");
   }
   std::vector<std::string> result;
   for (std::size_t position = 0; position < count; ++position) {
      const auto length = readNumber(text);
      if (length > text.size()) {
         throw std::invalid_argument("truncated IPC field");
      }
      result.emplace_back(text.substr(0, length));
      text.remove_prefix(length);
   }
   if (!text.empty()) {
      throw std::invalid_argument("trailing IPC bytes");
   }
   return result;
}

}

std::string encodeAgentRequest(const std::vector<std::string>& arguments) {
   auto fields = arguments;
   fields.insert(fields.begin(), "FlacCD request 1");
   return encode(fields);
}

std::vector<std::string> decodeAgentRequest(std::string_view message) {
   auto fields = decode(message);
   if (fields.empty() || fields[0] != "FlacCD request 1" || fields.size() > 257) {
      throw std::invalid_argument("unsupported IPC request version/count");
   }
   fields.erase(fields.begin());
   for (const auto& field : fields) {
      if (field.find('\0') != std::string::npos) {
         throw std::invalid_argument("NUL in IPC argument");
      }
   }
   return fields;
}

std::string encodeAgentReply(const AgentReply& reply) {
   std::vector<std::string> fields{"FlacCD reply 1", std::to_string(reply.exitCode), reply.output, reply.diagnostics};
   for (const auto& record : reply.records) {
      fields.push_back(encode(record));
   }
   return encode(fields);
}

AgentReply decodeAgentReply(std::string_view message) {
   const auto fields = decode(message);
   if (fields.size() < 4 || fields[0] != "FlacCD reply 1") {
      throw std::invalid_argument("unsupported IPC reply version");
   }
   AgentReply reply;
   const auto& code = fields[1];
   const auto parsed = std::from_chars(code.data(), code.data() + code.size(), reply.exitCode);
   if (parsed.ec != std::errc{} || parsed.ptr != code.data() + code.size()) {
      throw std::invalid_argument("invalid IPC reply status");
   }
   reply.output = fields[2];
   reply.diagnostics = fields[3];
   for (std::size_t position = 4; position < fields.size(); ++position) {
      auto record = decode(fields[position]);
      if (record.size() != 11) {
         throw std::invalid_argument("invalid IPC status row");
      }
      reply.records.push_back(std::move(record));
   }
   return reply;
}

}
