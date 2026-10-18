#include "opcua/opcua_server.h"

#include <open62541/plugin/accesscontrol_default.h>
#include <open62541/plugin/certificategroup_default.h>
#include <open62541/plugin/create_certificate.h>
#include <open62541/server_config_default.h>
#include <unistd.h>

#include <cstdarg>
#include <format>
#include <fstream>
#include <sstream>

#include "opcua/variant.h"
#include "util/log.h"
#include "util/password.h"

namespace logict::opcua {

namespace status = sdk::status;
using core::TagId;

namespace {

constexpr std::chrono::milliseconds kAsyncWriteTimeout{15000};

UA_String ua_string(std::string_view s) {
  return UA_String{s.size(), reinterpret_cast<UA_Byte*>(const_cast<char*>(s.data()))};
}

std::string to_std(const UA_String& s) { return std::string(reinterpret_cast<const char*>(s.data), s.length); }

void* tag_context(TagId id) { return reinterpret_cast<void*>(static_cast<std::uintptr_t>(id) + 1); }

std::optional<TagId> tag_from_context(void* ctx) {
  if (!ctx) return std::nullopt;
  return static_cast<TagId>(reinterpret_cast<std::uintptr_t>(ctx) - 1);
}

OpcUaServer* self_of(UA_Server* server) { return static_cast<OpcUaServer*>(UA_Server_getConfig(server)->context); }

void ua_log(void* ctx, UA_LogLevel level, UA_LogCategory, const char* msg, va_list args) {
  auto* log = static_cast<spdlog::logger*>(ctx);
  spdlog::level::level_enum lvl = level <= UA_LOGLEVEL_TRACE   ? spdlog::level::trace
                                  : level <= UA_LOGLEVEL_DEBUG ? spdlog::level::debug
                                  : level <= UA_LOGLEVEL_INFO  ? spdlog::level::info
                                  : level <= UA_LOGLEVEL_WARNING ? spdlog::level::warn
                                  : level <= UA_LOGLEVEL_ERROR   ? spdlog::level::err
                                                                 : spdlog::level::critical;
  if (!log->should_log(lvl)) return;
  UA_String out = UA_STRING_NULL;
  va_list copy;
  va_copy(copy, args);
  UA_String_vformat(&out, msg, copy);
  va_end(copy);
  log->log(lvl, "{}", std::string_view(reinterpret_cast<const char*>(out.data), out.length));
  UA_String_clear(&out);
}

std::optional<std::vector<UA_Byte>> read_file(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return std::nullopt;
  return std::vector<UA_Byte>(std::istreambuf_iterator<char>(in), {});
}

bool write_file(const std::filesystem::path& p, const UA_ByteString& data, std::filesystem::perms perms) {
  std::error_code ec;
  std::filesystem::create_directories(p.parent_path(), ec);
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(reinterpret_cast<const char*>(data.data), static_cast<std::streamsize>(data.length));
  out.close();
  std::filesystem::permissions(p, perms, ec);
  return static_cast<bool>(out);
}

UA_ByteString to_bytestring(const std::vector<UA_Byte>& v) {
  UA_ByteString bs;
  UA_ByteString_allocBuffer(&bs, v.size());
  std::memcpy(bs.data, v.data(), v.size());
  return bs;
}

}  // namespace

// ---- settings -----------------------------------------------------------------

Settings settings_from_json(const nlohmann::json& j) {
  Settings s;
  if (!j.is_object()) return s;
  s.enabled = sdk::property(j, "enabled", s.enabled);
  s.port = sdk::property<std::uint16_t>(j, "port", s.port);
  s.bind_addresses = sdk::property(j, "bind_addresses", s.bind_addresses);
  s.security_none = sdk::property(j, "security_none", s.security_none);
  s.basic256sha256 = sdk::property(j, "basic256sha256", s.basic256sha256);
  s.aes128_sha256_rsaoaep = sdk::property(j, "aes128_sha256_rsaoaep", s.aes128_sha256_rsaoaep);
  s.aes256_sha256_rsapss = sdk::property(j, "aes256_sha256_rsapss", s.aes256_sha256_rsapss);
  s.allow_anonymous = sdk::property(j, "allow_anonymous", s.allow_anonymous);
  s.max_sessions = std::clamp<std::uint32_t>(sdk::property(j, "max_sessions", s.max_sessions), 1, 256);
  s.max_session_timeout_s = sdk::property(j, "max_session_timeout_s", s.max_session_timeout_s);
  s.tag_cache_timeout_s = std::min<std::uint32_t>(sdk::property(j, "tag_cache_timeout_s", s.tag_cache_timeout_s), 60);
  s.max_data_queue_size = std::clamp<std::uint32_t>(sdk::property(j, "max_data_queue_size", s.max_data_queue_size), 1, 100);
  s.max_retransmit_queue_size =
      std::min<std::uint32_t>(sdk::property(j, "max_retransmit_queue_size", s.max_retransmit_queue_size), 100);
  s.max_notifications_per_publish = std::clamp<std::uint32_t>(
      sdk::property(j, "max_notifications_per_publish", s.max_notifications_per_publish), 1, 65536);
  s.min_sampling_interval_ms = std::max(5.0, sdk::property(j, "min_sampling_interval_ms", s.min_sampling_interval_ms));
  s.min_publishing_interval_ms =
      std::max(5.0, sdk::property(j, "min_publishing_interval_ms", s.min_publishing_interval_ms));
  s.pki_dir = sdk::property<std::string>(j, "pki_dir", s.pki_dir.string());
  s.hostname = sdk::property<std::string>(j, "hostname", s.hostname);
  return s;
}

// ---- lifecycle ----------------------------------------------------------------

OpcUaServer::OpcUaServer(core::Runtime& runtime, Settings settings, std::vector<UserAccount> users)
    : rt_(runtime), settings_(std::move(settings)), users_(std::move(users)), log_(core::make_logger("opcua")) {
  ua_logger_.log = ua_log;
  ua_logger_.context = log_.get();
  ua_logger_.clear = nullptr;
  if (settings_.hostname.empty()) {
    char buf[256] = {};
    gethostname(buf, sizeof(buf) - 1);
    settings_.hostname = buf[0] ? buf : "localhost";
  }
}

OpcUaServer::~OpcUaServer() { stop(); }

sdk::Result<void> OpcUaServer::start() {
  if (server_) return {};
  UA_ServerConfig config;
  std::memset(&config, 0, sizeof(config));
  config.logging = &ua_logger_;
  if (UA_ServerConfig_setBasics_withPort(&config, settings_.port) != UA_STATUSCODE_GOOD)
    return sdk::make_error(status::BadInternalError, "OPC UA base configuration failed");
  if (auto r = configure(config); !r) {
    config.logging = nullptr;  // ours, not heap-allocated
    UA_ServerConfig_clear(&config);
    return r;
  }
  server_ = UA_Server_newWithConfig(&config);
  if (!server_) return sdk::make_error(status::BadInternalError, "UA_Server_newWithConfig failed");

  auto& db = rt_.tags();
  cache_sub_ = db.add_subscriber([](TagId, const sdk::Vqt&, bool) {});
  monitor_sub_ = db.add_subscriber([](TagId, const sdk::Vqt&, bool) {});
  build_address_space();

  if (UA_Server_run_startup(server_) != UA_STATUSCODE_GOOD) {
    UA_Server_delete(server_);
    server_ = nullptr;
    return sdk::make_error(status::BadCommunicationError,
                           std::format("OPC UA server could not listen on port {}", settings_.port));
  }
  running_ = true;
  thread_ = std::thread([this] { run(); });
  log_->info("OPC UA server listening on opc.tcp://{}:{} (namespace {})", settings_.hostname, settings_.port, ns_);
  return {};
}

void OpcUaServer::stop() {
  if (!server_) return;
  running_ = false;
  if (thread_.joinable()) thread_.join();
  UA_Server_run_shutdown(server_);  // cancels pending async ops -> on_async_cancel
  {
    std::unique_lock alive(server_alive_mu_);
    UA_ServerConfig* cfg = UA_Server_getConfig(server_);
    cfg->logging = nullptr;  // owned by us; prevent the stack from clearing it
    UA_Server_delete(server_);
    server_ = nullptr;
  }
  auto& db = rt_.tags();
  db.remove_subscriber(cache_sub_);
  db.remove_subscriber(monitor_sub_);
  rt_.set_client_count("opcua", 0);
  log_->info("OPC UA server stopped");
}

void OpcUaServer::run() {
  auto next_housekeeping = std::chrono::steady_clock::now();
  while (running_) {
    UA_Server_run_iterate(server_, true);
    auto now = std::chrono::steady_clock::now();
    if (now >= next_housekeeping) {
      next_housekeeping = now + std::chrono::milliseconds(500);
      expire_cache();
      auto stats = UA_Server_getStatistics(server_);
      rt_.set_client_count("opcua", static_cast<int>(stats.ss.currentSessionCount));
    }
  }
}

// ---- configuration ------------------------------------------------------------

sdk::Result<std::pair<UA_ByteString, UA_ByteString>> OpcUaServer::load_or_create_certificate(const std::string& app_uri) {
  const auto cert_path = settings_.pki_dir / "ApplCerts/own/certs/logict.der";
  const auto key_path = settings_.pki_dir / "ApplCerts/own/private/logict.key.der";
  auto cert = read_file(cert_path);
  auto key = read_file(key_path);
  if (cert && key && !cert->empty() && !key->empty()) return std::pair{to_bytestring(*cert), to_bytestring(*key)};

  log_->info("creating self-signed application certificate in {}", settings_.pki_dir.string());
  std::string cn = std::format("CN=logict@{}", settings_.hostname);
  std::string dns = std::format("DNS:{}", settings_.hostname);
  std::string uri = std::format("URI:{}", app_uri);
  UA_String subject[2] = {ua_string("O=logict"), ua_string(cn)};
  UA_String san[2] = {ua_string(dns), ua_string(uri)};
  UA_KeyValueMap* params = UA_KeyValueMap_new();
  UA_UInt16 days = 3 * 365;  // Kepware: 3 years
  UA_UInt16 bits = 2048;
  UA_KeyValueMap_setScalar(params, UA_QUALIFIEDNAME(0, const_cast<char*>("expires-in-days")), &days,
                           &UA_TYPES[UA_TYPES_UINT16]);
  UA_KeyValueMap_setScalar(params, UA_QUALIFIEDNAME(0, const_cast<char*>("key-size-bits")), &bits,
                           &UA_TYPES[UA_TYPES_UINT16]);
  UA_ByteString key_bs = UA_BYTESTRING_NULL, cert_bs = UA_BYTESTRING_NULL;
  UA_StatusCode rc = UA_CreateCertificate(&ua_logger_, subject, 2, san, 2, UA_CERTIFICATEFORMAT_DER, params,
                                          &key_bs, &cert_bs);
  UA_KeyValueMap_delete(params);
  if (rc != UA_STATUSCODE_GOOD)
    return sdk::make_error(status::BadInternalError, std::format("certificate creation failed: {}", UA_StatusCode_name(rc)));
  using P = std::filesystem::perms;
  if (!write_file(cert_path, cert_bs, P::owner_read | P::owner_write | P::group_read | P::others_read) ||
      !write_file(key_path, key_bs, P::owner_read | P::owner_write)) {
    UA_ByteString_clear(&key_bs);
    UA_ByteString_clear(&cert_bs);
    return sdk::make_error(status::BadConfigurationError, std::format("cannot write to {}", settings_.pki_dir.string()));
  }
  return std::pair{cert_bs, key_bs};
}

sdk::Result<void> OpcUaServer::configure(UA_ServerConfig& config) {
  const std::string app_uri = std::format("urn:{}:logict", settings_.hostname);
  config.context = this;

  // Application description (URI must match the certificate's SAN URI).
  UA_ApplicationDescription& app = config.applicationDescription;
  UA_String_clear(&app.applicationUri);
  UA_String_clear(&app.productUri);
  UA_LocalizedText_clear(&app.applicationName);
  app.applicationUri = UA_STRING_ALLOC(app_uri.c_str());
  app.productUri = UA_STRING_ALLOC("urn:logict");
  app.applicationName = UA_LOCALIZEDTEXT_ALLOC("", std::format("logict@{}", settings_.hostname).c_str());

  auto certs = load_or_create_certificate(app_uri);
  if (!certs) return std::unexpected(certs.error());
  auto [cert, key] = *certs;
  struct Cleanup {
    UA_ByteString* c;
    UA_ByteString* k;
    ~Cleanup() {
      UA_ByteString_clear(c);
      UA_ByteString_clear(k);
    }
  } cleanup{&cert, &key};

  // Trust store: pki/ApplCerts/{own,trusted,issuer,rejected}/. Unknown client
  // certificates land in rejected/certs until an administrator moves them to
  // trusted/certs.
  std::error_code ec;
  std::filesystem::create_directories(settings_.pki_dir, ec);
  const std::string pki = settings_.pki_dir.string();
  // The base config installs accept-all verifiers for both groups. Replace the
  // channel one with the file store and remove the session one entirely, so
  // X509 user identity tokens are neither advertised nor accepted (Kepware
  // offers none either).
  for (UA_CertificateGroup* g : {&config.secureChannelPKI, &config.sessionPKI}) {
    if (g->clear) g->clear(g);
    std::memset(g, 0, sizeof(*g));
  }
  UA_NodeId app_group = UA_NS0ID(SERVERCONFIGURATION_CERTIFICATEGROUPS_DEFAULTAPPLICATIONGROUP);
  if (UA_CertificateGroup_Filestore(&config.secureChannelPKI, &app_group, ua_string(pki), &ua_logger_, nullptr) !=
      UA_STATUSCODE_GOOD)
    return sdk::make_error(status::BadConfigurationError, std::format("cannot open PKI directory {}", pki));

  // Security policies. None is always present for discovery (GetEndpoints) but
  // only exposed as an endpoint when explicitly enabled (Kepware default: off).
  if (UA_ServerConfig_addSecurityPolicyNone(&config, &cert) != UA_STATUSCODE_GOOD)
    return sdk::make_error(status::BadInternalError, "cannot add SecurityPolicy None");
  config.securityPolicyNoneDiscoveryOnly = !settings_.security_none;

  struct Policy {
    int mode;
    const char* uri;
    UA_StatusCode (*add)(UA_ServerConfig*, const UA_ByteString*, const UA_ByteString*);
  };
  const Policy policies[] = {
      {settings_.basic256sha256, "http://opcfoundation.org/UA/SecurityPolicy#Basic256Sha256",
       UA_ServerConfig_addSecurityPolicyBasic256Sha256},
      {settings_.aes128_sha256_rsaoaep, "http://opcfoundation.org/UA/SecurityPolicy#Aes128_Sha256_RsaOaep",
       UA_ServerConfig_addSecurityPolicyAes128Sha256RsaOaep},
      {settings_.aes256_sha256_rsapss, "http://opcfoundation.org/UA/SecurityPolicy#Aes256_Sha256_RsaPss",
       UA_ServerConfig_addSecurityPolicyAes256Sha256RsaPss},
  };
  bool any_secure = false;
  for (const auto& p : policies) {
    if (p.mode <= 0) continue;
    if (p.add(&config, &cert, &key) != UA_STATUSCODE_GOOD)
      return sdk::make_error(status::BadInternalError, std::format("cannot add security policy {}", p.uri));
    any_secure = true;
  }
  if (!any_secure && !settings_.security_none)
    return sdk::make_error(status::BadConfigurationError, "no OPC UA security policy enabled");

  // User authentication (anonymous off by default, like Kepware). The stack
  // only advertises the UserName token when its static login list is
  // non-empty, so pass one placeholder; the callback does the real check.
  UA_UsernamePasswordLogin placeholder{UA_STRING(const_cast<char*>("\x01")), UA_STRING(const_cast<char*>("\x01"))};
  if (UA_AccessControl_defaultWithLoginCallback(
          &config, settings_.allow_anonymous, nullptr, users_.empty() ? 0 : 1, &placeholder,
          [](const UA_String* user, const UA_ByteString* password, size_t, const UA_UsernamePasswordLogin*,
             void**, void* ctx) -> UA_StatusCode {
            if (user->length == 0 && password->length == 0) return UA_STATUSCODE_GOOD;  // anonymous token
            auto* self = static_cast<OpcUaServer*>(ctx);
            return self->authenticate(to_std(*user), to_std(*password)) ? UA_STATUSCODE_GOOD
                                                                         : UA_STATUSCODE_BADUSERACCESSDENIED;
          },
          this) != UA_STATUSCODE_GOOD)
    return sdk::make_error(status::BadInternalError, "access control setup failed");

  // Endpoints.
  if (settings_.security_none)
    UA_ServerConfig_addEndpoint(&config, UA_STRING(const_cast<char*>("http://opcfoundation.org/UA/SecurityPolicy#None")),
                                UA_MESSAGESECURITYMODE_NONE);
  for (const auto& p : policies) {
    if (p.mode == 1 || p.mode == 3)
      UA_ServerConfig_addEndpoint(&config, UA_STRING(const_cast<char*>(p.uri)), UA_MESSAGESECURITYMODE_SIGN);
    if (p.mode == 2 || p.mode == 3)
      UA_ServerConfig_addEndpoint(&config, UA_STRING(const_cast<char*>(p.uri)), UA_MESSAGESECURITYMODE_SIGNANDENCRYPT);
  }

  // Listen addresses.
  if (!settings_.bind_addresses.empty()) {
    UA_Array_delete(config.serverUrls, config.serverUrlsSize, &UA_TYPES[UA_TYPES_STRING]);
    config.serverUrlsSize = settings_.bind_addresses.size();
    config.serverUrls = static_cast<UA_String*>(UA_Array_new(config.serverUrlsSize, &UA_TYPES[UA_TYPES_STRING]));
    for (std::size_t i = 0; i < settings_.bind_addresses.size(); ++i)
      config.serverUrls[i] =
          UA_STRING_ALLOC(std::format("opc.tcp://{}:{}", settings_.bind_addresses[i], settings_.port).c_str());
  }

  // Limits (Kepware Project Properties -> OPC UA defaults).
  config.maxSessions = static_cast<UA_UInt16>(settings_.max_sessions);
  config.maxSecureChannels = static_cast<UA_UInt16>(settings_.max_sessions + 8);
  config.maxSessionTimeout = settings_.max_session_timeout_s * 1000.0;
  config.maxRetransmissionQueueSize = settings_.max_retransmit_queue_size;
  config.maxNotificationsPerPublish = settings_.max_notifications_per_publish;
  config.queueSizeLimits.max = settings_.max_data_queue_size;
  config.samplingIntervalLimits.min = settings_.min_sampling_interval_ms;
  config.publishingIntervalLimits.min = settings_.min_publishing_interval_ms;
  config.asyncOperationTimeout = static_cast<UA_Double>(kAsyncWriteTimeout.count());
  config.asyncOperationCancelCallback = on_async_cancel;
  config.monitoredItemRegisterCallback = on_monitored_item;
  return {};
}

// ---- address space ------------------------------------------------------------

void OpcUaServer::build_address_space() {
  building_ = true;
  ns_ = UA_Server_addNamespace(server_, "urn:logict:tags");
  if (ns_ != 2) log_->warn("tag namespace index is {} (Kepware clients expect 2)", ns_);

  auto& db = rt_.tags();
  std::set<std::string> folders;
  const UA_NodeId objects = UA_NS0ID(OBJECTSFOLDER);

  auto add_folder = [&](const std::string& path, const std::string& name, const std::string& parent) {
    if (!folders.insert(path).second) return;
    UA_ObjectAttributes oa = UA_ObjectAttributes_default;
    oa.displayName = UA_LOCALIZEDTEXT(const_cast<char*>(""), const_cast<char*>(name.c_str()));
    UA_NodeId parent_id = parent.empty() ? objects : UA_NODEID_STRING(ns_, const_cast<char*>(parent.c_str()));
    UA_StatusCode rc = UA_Server_addObjectNode(
        server_, UA_NODEID_STRING(ns_, const_cast<char*>(path.c_str())), parent_id, UA_NS0ID(ORGANIZES),
        UA_QUALIFIEDNAME(ns_, const_cast<char*>(name.c_str())), UA_NS0ID(FOLDERTYPE), oa, nullptr, nullptr);
    if (rc != UA_STATUSCODE_GOOD) log_->warn("cannot add folder {}: {}", path, UA_StatusCode_name(rc));
  };

  UA_CallbackValueSource source;
  source.read = on_read;
  source.write = on_write;

  std::size_t added = 0;
  for (TagId id : db.all()) {
    const auto& info = db.info(id);
    // Create the folder chain for every path segment but the last.
    std::string parent;
    std::size_t start = 0;
    for (std::size_t dot = info.path.find('.'); dot != std::string::npos; dot = info.path.find('.', start)) {
      std::string path = info.path.substr(0, dot);
      add_folder(path, info.path.substr(start, dot - start), parent);
      parent = std::move(path);
      start = dot + 1;
    }
    const std::string name = info.path.substr(start);

    UA_VariableAttributes va = UA_VariableAttributes_default;
    va.displayName = UA_LOCALIZEDTEXT(const_cast<char*>(""), const_cast<char*>(name.c_str()));
    if (!info.config.description.empty())
      va.description = UA_LOCALIZEDTEXT(const_cast<char*>(""), const_cast<char*>(info.config.description.c_str()));
    va.dataType = ua_type(info.type.scalar)->typeId;
    UA_UInt32 dims[2] = {info.type.rows, info.type.cols};
    if (info.type.is_array()) {
      va.valueRank = info.type.cols > 0 ? 2 : 1;
      va.arrayDimensions = dims;
      va.arrayDimensionsSize = info.type.cols > 0 ? 2 : 1;
    } else {
      va.valueRank = UA_VALUERANK_SCALAR;
    }
    const bool writable = info.kind == core::TagKind::Device && info.owner &&
                          info.config.access == sdk::Access::ReadWrite;
    va.accessLevel = UA_ACCESSLEVELMASK_READ | (writable ? UA_ACCESSLEVELMASK_WRITE : 0);
    va.userAccessLevel = va.accessLevel;
    // A typed initial value keeps the stack from creating (and writing) a default.
    UA_Variant initial;
    UA_Variant_init(&initial);
    to_variant(sdk::default_value(info.type), info.type, &initial);
    va.value = initial;

    UA_StatusCode rc = UA_Server_addCallbackValueSourceVariableNode(
        server_, UA_NODEID_STRING(ns_, const_cast<char*>(info.path.c_str())),
        parent.empty() ? objects : UA_NODEID_STRING(ns_, const_cast<char*>(parent.c_str())),
        parent.empty() ? UA_NS0ID(ORGANIZES) : UA_NS0ID(HASCOMPONENT),
        UA_QUALIFIEDNAME(ns_, const_cast<char*>(name.c_str())), UA_NS0ID(BASEDATAVARIABLETYPE), va, source,
        tag_context(id), nullptr);
    UA_Variant_clear(&initial);
    if (rc == UA_STATUSCODE_GOOD) ++added;
    else log_->warn("cannot add node {}: {}", info.path, UA_StatusCode_name(rc));
  }
  building_ = false;
  log_->info("address space: {} variable(s), {} folder(s)", added, folders.size());
}

// ---- callbacks ------------------------------------------------------------------

UA_StatusCode OpcUaServer::on_read(UA_Server* server, const UA_NodeId*, void*, const UA_NodeId*, void* node_ctx,
                                   UA_Boolean include_source_ts, const UA_NumericRange* range, UA_DataValue* dv) {
  auto id = tag_from_context(node_ctx);
  if (!id) return UA_STATUSCODE_BADNODEIDUNKNOWN;
  OpcUaServer* self = self_of(server);
  auto& db = self->rt_.tags();
  const auto& info = db.info(*id);
  if (self->building_) {
    // The stack type-checks new nodes by reading them; answer with a typed
    // default instead of touching the device.
    if (to_variant(sdk::default_value(info.type), info.type, &dv->value) == UA_STATUSCODE_GOOD) dv->hasValue = true;
    return UA_STATUSCODE_GOOD;
  }
  if (info.kind == core::TagKind::Device && info.owner) self->touch_cache(*id);

  const sdk::Vqt v = db.read(*id);
  // Bad quality is returned as the callback result: the stack then reports it
  // as the value's status (a Good return with no value would be rewritten to
  // BadWaitingForInitialData). Values accompanying Bad are omitted (Part 4).
  if (sdk::is_bad(v.quality)) return v.quality;
  if (v.quality != status::Good) {
    dv->hasStatus = true;
    dv->status = v.quality;
  }
  if (!v.value.empty()) {
    UA_Variant full;
    UA_Variant_init(&full);
    UA_StatusCode rc = to_variant(v.value, info.type, &full);
    if (rc == UA_STATUSCODE_GOOD && range) {
      rc = UA_Variant_copyRange(&full, &dv->value, *range);
      UA_Variant_clear(&full);
    } else if (rc == UA_STATUSCODE_GOOD) {
      dv->value = full;
    }
    if (rc != UA_STATUSCODE_GOOD) {
      dv->status = rc;
    } else {
      dv->hasValue = true;
    }
  }
  if (include_source_ts && v.source_time != sdk::Timestamp{}) {
    dv->hasSourceTimestamp = true;
    dv->sourceTimestamp = to_ua_time(v.source_time);
  }
  return UA_STATUSCODE_GOOD;
}

void OpcUaServer::touch_cache(TagId id) {
  bool subscribe = false;
  {
    std::lock_guard lock(cache_mu_);
    auto [it, inserted] = cache_.try_emplace(id);
    it->second.last_access = std::chrono::steady_clock::now();
    subscribe = inserted;
  }
  if (subscribe) rt_.tags().subscribe(id, cache_sub_, rt_.tags().info(id).config.scan_rate_ms);
}

void OpcUaServer::expire_cache() {
  const auto cutoff = std::chrono::steady_clock::now() - std::chrono::seconds(settings_.tag_cache_timeout_s);
  std::vector<TagId> expired;
  {
    std::lock_guard lock(cache_mu_);
    for (auto it = cache_.begin(); it != cache_.end();) {
      if (it->second.last_access < cutoff) {
        expired.push_back(it->first);
        it = cache_.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (TagId id : expired) rt_.tags().unsubscribe(id, cache_sub_);
}

void OpcUaServer::on_monitored_item(UA_Server* server, const UA_NodeId*, void*, const UA_NodeId*, void* node_ctx,
                                    UA_UInt32 attribute_id, UA_Boolean removed) {
  auto id = tag_from_context(node_ctx);
  if (!id || attribute_id != UA_ATTRIBUTEID_VALUE) return;
  OpcUaServer* self = self_of(server);
  auto& db = self->rt_.tags();
  if (!(db.info(*id).kind == core::TagKind::Device && db.info(*id).owner)) return;
  bool first = false, last = false;
  {
    std::lock_guard lock(self->monitor_mu_);
    int& n = self->monitored_[*id];
    if (removed) {
      last = --n <= 0;
      if (last) self->monitored_.erase(*id);
    } else {
      first = ++n == 1;
    }
  }
  // The MonitoredItem's sampling interval is not exposed by the stack, so the
  // tag's own scan rate is requested (subject to the device scan mode).
  if (first) db.subscribe(*id, self->monitor_sub_, db.info(*id).config.scan_rate_ms);
  if (last) db.unsubscribe(*id, self->monitor_sub_);
}

struct OpcUaServer::InlineWrite {
  std::thread::id thread = std::this_thread::get_id();
  std::atomic<bool> in_call{true};
  bool done = false;
  sdk::StatusCode result = status::Good;
};

UA_StatusCode OpcUaServer::on_write(UA_Server* server, const UA_NodeId*, void*, const UA_NodeId*, void* node_ctx,
                                    const UA_NumericRange* range, const UA_DataValue* data) {
  auto id = tag_from_context(node_ctx);
  if (!id) return UA_STATUSCODE_BADNODEIDUNKNOWN;
  OpcUaServer* self = self_of(server);
  if (self->building_) return UA_STATUSCODE_GOOD;  // stack-internal writes during node creation
  if (range) return UA_STATUSCODE_BADWRITENOTSUPPORTED;  // Kepware: whole arrays only
  if (!data->hasValue) return UA_STATUSCODE_BADTYPEMISMATCH;
  auto value = from_variant(data->value);
  if (!value) return value.error().code;
  return self->begin_write(*id, *value, data);
}

UA_StatusCode OpcUaServer::begin_write(TagId id, const sdk::Value& value, const UA_DataValue* key) {
  auto state = std::make_shared<InlineWrite>();
  {
    std::lock_guard lock(pending_mu_);
    pending_writes_.insert(key);
  }
  rt_.write_async(id, value, [this, key, state](sdk::StatusCode st) {
    if (state->in_call.load() && std::this_thread::get_id() == state->thread) {
      state->done = true;  // rejected synchronously; answered by the return value
      state->result = st;
      return;
    }
    complete_write(key, st);
  });
  state->in_call = false;
  if (state->done) {
    std::lock_guard lock(pending_mu_);
    pending_writes_.erase(key);
    return state->result;
  }
  return UA_STATUSCODE_GOODCOMPLETESASYNCHRONOUSLY;
}

void OpcUaServer::complete_write(const UA_DataValue* key, sdk::StatusCode st) {
  {
    std::lock_guard lock(pending_mu_);
    if (pending_writes_.erase(key) == 0) return;  // cancelled (timeout or shutdown)
  }
  std::shared_lock alive(server_alive_mu_);
  if (server_) UA_Server_setAsyncWriteResult(server_, key, st);
}

void OpcUaServer::on_async_cancel(UA_Server* server, const void* out) {
  OpcUaServer* self = self_of(server);
  std::lock_guard lock(self->pending_mu_);
  self->pending_writes_.erase(static_cast<const UA_DataValue*>(out));
}

bool OpcUaServer::authenticate(std::string_view user, std::string_view password) const {
  for (const auto& u : users_)
    if (u.name == user) {
      bool ok = core::verify_password(password, u.password_hash);
      if (!ok) log_->warn("OPC UA login failed for user '{}'", user);
      return ok;
    }
  log_->warn("OPC UA login failed: unknown user '{}'", user);
  return false;
}

}  // namespace logict::opcua
