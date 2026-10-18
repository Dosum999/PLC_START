#pragma once

// Payload templates compatible with Kepware IoT Gateway.
//
// Standard: a top-level template containing |VALUES|, plus an "expansion"
// repeated per event and joined with ','. Advanced: a Handlebars subset with
// |#each VALUES| ... |/each| and |#unless @last| ... |/unless|.
//
// Variables (both forms accept both spellings):
//   top level : SERVERNAME SERVERTIMESTAMP SERVERDATE SERVERISO8601UTC SERVERISO8601LOCAL LINEFEED
//   per event : TAGNAME/ CHANNEL DEVICE TAG, VALUE/TAGVALUE, QUALITY/TAGQUALITY,
//               TIMESTAMP/TAGTIMESTAMP, ADDRESS/TAGADDRESS, DESCRIPTION/TAGDESCRIPTION,
//               CLIENTACCESS/TAGCLIENTACCESS, ISO8601UTC/TAGISO8601UTC,
//               ISO8601LOCAL/TAGISO8601LOCAL, STATUSCODE/TAGSTATUSCODE (logict extension)
// VALUE renders as a JSON literal (strings quoted, arrays as Kepware "[1,2]"
// strings); names and other text are JSON-escaped without quotes.

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "logict/sdk/status.h"
#include "logict/sdk/value.h"

namespace logict::mqtt {

inline constexpr const char* kDefaultStandardTemplate = R"({ "timestamp": |SERVERTIMESTAMP|, "values": [ |VALUES| ] })";
inline constexpr const char* kDefaultExpansion =
    R"({ "id": "|TAGNAME|", "v": |TAGVALUE|, "q": |TAGQUALITY|, "t": |TAGTIMESTAMP| })";

// Everything a template can reference about one event.
struct EventView {
  std::string_view path, channel, device, tag, address, description;
  bool writable = false;
  const sdk::Vqt* vqt = nullptr;
  const sdk::TagType* type = nullptr;
};

class Template {
 public:
  static sdk::Result<Template> standard(std::string_view top, std::string_view expansion);
  static sdk::Result<Template> advanced(std::string_view text);

  std::string render(std::string_view server_name, sdk::Timestamp now, const std::vector<EventView>& events) const;

  struct Node;

 private:
  std::vector<std::shared_ptr<const Node>> nodes_;
};

// Value as a JSON literal, as used for |VALUE|.
std::string json_value(const sdk::Value& v, const sdk::TagType* shape);

}  // namespace logict::mqtt
