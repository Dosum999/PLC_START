// OPC UA Client driver: reads and writes variables of another OPC UA server
// (e.g. an S7-1500, Omron NX or another gateway). Device ID: endpoint URL
// (opc.tcp://host:4840). Tag address: NodeId ("ns=3;s=\"DB1\".\"Speed\"",
// "ns=2;i=1001"). With data type Default the server's type is used.
//
// Polled mode: Read service in batches; writes use the Write service and wait
// for the server's result.

#include <open62541/client.h>
#include <open62541/client_config_default.h>
#include <open62541/client_highlevel.h>
#include <open62541/plugin/certificategroup_default.h>
#include <open62541/plugin/create_certificate.h>
#include <unistd.h>

#include <cstdarg>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>

#include "logict/sdk/driver.h"
#include "opcua/variant.h"

namespace {

using namespace logict;
using namespace logict::sdk;

constexpr const char* kPolicy = "opcua_client.DEVICE_SECURITY_POLICY";  // None, Basic256Sha256, Aes128_Sha256_RsaOaep, Aes256_Sha256_RsaPss
constexpr const char* kMode = "opcua_client.DEVICE_SECURITY_MODE";      // None, Sign, SignAndEncrypt
constexpr const char* kUser = "opcua_client.DEVICE_USERNAME";
constexpr const char* kPassword = "opcua_client.DEVICE_PASSWORD";
constexpr const char* kPki = "opcua_client.DEVICE_PKI_DIR";
constexpr const char* kTrustAll = "opcua_client.DEVICE_TRUST_ALL_SERVERS";
constexpr const char* kMaxNodes = "opcua_client.DEVICE_MAX_NODES_PER_READ";
constexpr const char* kPlaintext = "opcua_client.DEVICE_ALLOW_PLAINTEXT_PASSWORD";

UA_String ua_str(std::string_view s) { return UA_String{s.size(), reinterpret_cast<UA_Byte*>(const_cast<char*>(s.data()))}; }

std::optional<DataType> sdk_type(const UA_DataType* t) {
  if (!t) return std::nullopt;
  switch (t->typeKind) {
    case UA_DATATYPEKIND_BOOLEAN: return DataType::Boolean;
    case UA_DATATYPEKIND_SBYTE: return DataType::Char;
    case UA_DATATYPEKIND_BYTE: return DataType::Byte;
    case UA_DATATYPEKIND_INT16: return DataType::Short;
    case UA_DATATYPEKIND_UINT16: return DataType::Word;
    case UA_DATATYPEKIND_INT32: return DataType::Long;
    case UA_DATATYPEKIND_UINT32: return DataType::DWord;
    case UA_DATATYPEKIND_INT64: return DataType::LLong;
    case UA_DATATYPEKIND_UINT64: return DataType::QWord;
    case UA_DATATYPEKIND_FLOAT: return DataType::Float;
    case UA_DATATYPEKIND_DOUBLE: return DataType::Double;
    case UA_DATATYPEKIND_STRING: return DataType::String;
    case UA_DATATYPEKIND_DATETIME: return DataType::Date;
    default: return std::nullopt;
  }
}

bool is_connection_error(UA_StatusCode s) {
  return s == UA_STATUSCODE_BADCONNECTIONCLOSED || s == UA_STATUSCODE_BADSECURECHANNELCLOSED ||
         s == UA_STATUSCODE_BADSESSIONIDINVALID || s == UA_STATUSCODE_BADSESSIONCLOSED ||
         s == UA_STATUSCODE_BADSECURECHANNELIDINVALID || s == UA_STATUSCODE_BADNOTCONNECTED ||
         s == UA_STATUSCODE_BADCOMMUNICATIONERROR || s == UA_STATUSCODE_BADCONNECTIONREJECTED ||
         s == UA_STATUSCODE_BADSERVERNOTCONNECTED || s == UA_STATUSCODE_BADDISCONNECT;
}

struct UaAddress final : Address {
  UA_NodeId node = UA_NODEID_NULL;
  ~UaAddress() override { UA_NodeId_clear(&node); }
};

std::optional<std::vector<UA_Byte>> read_file(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return std::nullopt;
  return std::vector<UA_Byte>(std::istreambuf_iterator<char>(in), {});
}

bool write_file(const std::filesystem::path& p, const UA_ByteString& b, std::filesystem::perms perms) {
  std::error_code ec;
  std::filesystem::create_directories(p.parent_path(), ec);
  std::ofstream out(p, std::ios::binary);
  out.write(reinterpret_cast<const char*>(b.data), static_cast<std::streamsize>(b.length));
  out.close();
  if (!out) return false;
  std::filesystem::permissions(p, perms, ec);
  return true;
}

class UaClientDevice final : public DeviceRuntime {
 public:
  UaClientDevice(const DeviceConfig& cfg, HostContext& host) : host_(host) {
    const auto& p = cfg.properties;
    url_ = cfg.id;
    policy_ = property<std::string>(p, kPolicy, "None");
    mode_ = property<std::string>(p, kMode, policy_ == "None" ? "None" : "SignAndEncrypt");
    user_ = property<std::string>(p, kUser, "");
    password_ = property<std::string>(p, kPassword, "");
    pki_ = property<std::string>(p, kPki, "/var/lib/logict/pki-client");
    trust_all_ = property(p, kTrustAll, false);
    allow_plaintext_ = property(p, kPlaintext, false);
    max_nodes_ = std::clamp<std::size_t>(property<std::size_t>(p, kMaxNodes, 500), 1, 10000);
    connect_timeout_ = cfg.connect_timeout;
    request_timeout_ = cfg.request_timeout;
    logger_.log = [](void* ctx, UA_LogLevel level, UA_LogCategory, const char* msg, va_list args) {
      auto* self = static_cast<UaClientDevice*>(ctx);
      if (level < UA_LOGLEVEL_WARNING || !self->host_.log.enabled(LogLevel::Debug)) return;
      UA_String out = UA_STRING_NULL;
      va_list copy;
      va_copy(copy, args);
      UA_String_vformat(&out, msg, copy);
      va_end(copy);
      self->host_.log.debug("ua: {}", std::string_view(reinterpret_cast<const char*>(out.data), out.length));
      UA_String_clear(&out);
    };
    logger_.context = this;
    logger_.clear = nullptr;
  }

  ~UaClientDevice() override { destroy(); }

  Result<std::shared_ptr<const Address>> parse_address(std::string_view text, TagType requested) override {
    auto a = std::make_shared<UaAddress>();
    a->text = std::string(text);
    std::string s(text);
    if (!s.starts_with("ns=") && !s.starts_with("i=") && !s.starts_with("s=") && !s.starts_with("g=") &&
        !s.starts_with("b="))
      return make_error(status::BadInvalidArgument, "expected a NodeId such as ns=2;s=Name or ns=0;i=2258");
    if (UA_NodeId_parse(&a->node, ua_str(s)) != UA_STATUSCODE_GOOD)
      return make_error(status::BadInvalidArgument, "invalid NodeId");
    if (requested.scalar == DataType::BCD || requested.scalar == DataType::LBCD)
      return make_error(status::BadTypeMismatch, "BCD types are not available over OPC UA");
    a->type = requested;
    return std::shared_ptr<const Address>(std::move(a));
  }

  StatusCode connect(Deadline) override {
    destroy();
    // Build the whole configuration first and create the client once:
    // UA_Client_new() would install defaults that a second setDefault() leaks.
    UA_ClientConfig cc;
    std::memset(&cc, 0, sizeof(cc));
    cc.logging = &logger_;
    if (UA_StatusCode rc = UA_ClientConfig_setDefault(&cc); rc != UA_STATUSCODE_GOOD) return fail_config(cc, rc, "client config");
    cc.timeout = static_cast<UA_UInt32>(connect_timeout_.count());
    cc.allowNonePolicyPassword = allow_plaintext_;
    if (StatusCode st = configure_security(cc); !is_good(st)) {
      clear_config(cc);
      return st;
    }
    client_ = UA_Client_newWithConfig(&cc);
    if (!client_) return fail_config(cc, UA_STATUSCODE_BADOUTOFMEMORY, "client creation");

    UA_StatusCode rc = user_.empty()
                           ? UA_Client_connect(client_, url_.c_str())
                           : UA_Client_connectUsername(client_, url_.c_str(), user_.c_str(), password_.c_str());
    if (rc != UA_STATUSCODE_GOOD) {
      host_.log.warn("cannot connect to {}: {}", url_, UA_StatusCode_name(rc));
      if (rc == UA_STATUSCODE_BADIDENTITYTOKENREJECTED && policy_ == "None" && !user_.empty() && !allow_plaintext_)
        host_.log.warn("the server may only offer an unencrypted password login; use a secure policy or set {}", kPlaintext);
      destroy();
      return rc == UA_STATUSCODE_BADTIMEOUT ? status::BadTimeout : status::BadNotConnected;
    }
    UA_Client_getConfig(client_)->timeout = static_cast<UA_UInt32>(request_timeout_.count());
    host_.log.info("connected to {} ({} / {})", url_, policy_, mode_);
    return status::Good;
  }

  void disconnect() override { destroy(); }

  bool connected() const override {
    if (!client_) return false;
    UA_SessionState session;
    UA_Client_getState(client_, nullptr, &session, nullptr);
    return session == UA_SESSIONSTATE_ACTIVATED;
  }

  StatusCode read(std::span<ReadItem*> items) override {
    if (!client_) return status::BadNotConnected;
    for (std::size_t start = 0; start < items.size(); start += max_nodes_) {
      const std::size_t n = std::min(max_nodes_, items.size() - start);
      std::vector<UA_ReadValueId> ids(n);
      for (std::size_t i = 0; i < n; ++i) {
        UA_ReadValueId_init(&ids[i]);
        ids[i].nodeId = static_cast<const UaAddress&>(*items[start + i]->address).node;  // shallow
        ids[i].attributeId = UA_ATTRIBUTEID_VALUE;
      }
      UA_ReadRequest req;
      UA_ReadRequest_init(&req);
      req.nodesToRead = ids.data();
      req.nodesToReadSize = n;
      req.timestampsToReturn = UA_TIMESTAMPSTORETURN_SOURCE;
      UA_ReadResponse resp = UA_Client_Service_read(client_, req);
      StatusCode st = service_status(resp.responseHeader.serviceResult);
      if (is_good(st) && resp.resultsSize != n) st = status::BadCommunicationError;
      if (!is_good(st)) {
        UA_ReadResponse_clear(&resp);
        return st;
      }
      const auto now = Clock::now();
      for (std::size_t i = 0; i < n; ++i) store(*items[start + i], resp.results[i], now);
      UA_ReadResponse_clear(&resp);
    }
    return status::Good;
  }

  StatusCode write(const Address& address, const Value& value) override {
    if (!client_) return status::BadNotConnected;
    const auto& a = static_cast<const UaAddress&>(address);
    // The variant must carry the server's data type (learned from reads, else
    // read now); the configured tag type only applies on the logict side.
    auto it = server_types_.find(&a);
    if (it == server_types_.end()) {
      ReadItem probe;
      probe.address = &a;
      ReadItem* p = &probe;
      if (StatusCode st = read(std::span(&p, 1)); !is_good(st)) return st;
      it = server_types_.find(&a);
    }
    const DataType target = it != server_types_.end() ? it->second : a.type.scalar;
    if (target == DataType::Default) return status::BadTypeMismatch;
    auto typed = convert(value, target);
    if (!typed) return typed.error().code;
    UA_WriteValue wv;
    UA_WriteValue_init(&wv);
    wv.nodeId = a.node;  // shallow
    wv.attributeId = UA_ATTRIBUTEID_VALUE;
    wv.value.hasValue = true;
    if (UA_StatusCode rc = opcua::to_variant(*typed, TagType{target, a.type.rows, a.type.cols}, &wv.value.value); rc != UA_STATUSCODE_GOOD)
      return rc;
    UA_WriteRequest req;
    UA_WriteRequest_init(&req);
    req.nodesToWrite = &wv;
    req.nodesToWriteSize = 1;
    UA_WriteResponse resp = UA_Client_Service_write(client_, req);
    UA_Variant_clear(&wv.value.value);
    StatusCode st = service_status(resp.responseHeader.serviceResult);
    if (is_good(st)) st = resp.resultsSize == 1 ? resp.results[0] : status::BadCommunicationError;
    UA_WriteResponse_clear(&resp);
    return st;
  }

 private:
  StatusCode service_status(UA_StatusCode rc) {
    if (rc == UA_STATUSCODE_GOOD) return status::Good;
    if (rc == UA_STATUSCODE_BADTIMEOUT) return status::BadTimeout;
    if (is_connection_error(rc)) return status::BadNotConnected;
    host_.log.warn("{}: service error {}", url_, UA_StatusCode_name(rc));
    return status::BadCommunicationError;
  }

  void store(ReadItem& item, const UA_DataValue& dv, Timestamp now) {
    const auto& a = static_cast<const UaAddress&>(*item.address);
    item.source_time = dv.hasSourceTimestamp
                           ? Timestamp(std::chrono::duration_cast<Clock::duration>(
                                 std::chrono::nanoseconds((dv.sourceTimestamp - UA_DATETIME_UNIX_EPOCH) * 100)))
                           : now;
    const StatusCode quality = dv.hasStatus ? dv.status : status::Good;
    if (is_bad(quality) || !dv.hasValue) {
      item.quality = is_bad(quality) ? quality : status::BadNoCommunication;
      return;
    }
    if (auto t = sdk_type(dv.value.type)) server_types_[&a] = *t;
    auto v = opcua::from_variant(dv.value);
    if (!v) {
      item.quality = v.error().code;
      return;
    }
    if (a.type.scalar != DataType::Default) {
      auto c = convert(*v, a.type.scalar);  // keep the configured tag type
      if (!c) {
        item.quality = c.error().code;
        return;
      }
      v = std::move(c);
    }
    item.value = std::move(*v);
    item.quality = quality;
  }

  // Crypto policies are loaded for secure channels and also whenever a user
  // name is used: servers commonly require the password inside the UserName
  // token to be encrypted with their certificate even on a None channel.
  StatusCode configure_security(UA_ClientConfig& cc) {
    if (policy_ == "None" && user_.empty()) return status::Good;
    static const std::map<std::string, std::string> uris{
        {"None", "http://opcfoundation.org/UA/SecurityPolicy#None"},
        {"Basic256Sha256", "http://opcfoundation.org/UA/SecurityPolicy#Basic256Sha256"},
        {"Aes128_Sha256_RsaOaep", "http://opcfoundation.org/UA/SecurityPolicy#Aes128_Sha256_RsaOaep"},
        {"Aes256_Sha256_RsaPss", "http://opcfoundation.org/UA/SecurityPolicy#Aes256_Sha256_RsaPss"},
    };
    auto uri = uris.find(policy_);
    if (uri == uris.end()) return fail(UA_STATUSCODE_BADCONFIGURATIONERROR, "unknown security policy " + policy_);
    char host[256] = {};
    gethostname(host, sizeof(host) - 1);
    const std::string app_uri = std::format("urn:{}:logict:client", host);

    // Client application certificate, created on first use.
    const std::filesystem::path cert_path = pki_ / "own/certs/client.der", key_path = pki_ / "own/private/client.key.der";
    auto cert_bytes = read_file(cert_path);
    auto key_bytes = read_file(key_path);
    UA_ByteString cert = UA_BYTESTRING_NULL, key = UA_BYTESTRING_NULL;
    if (cert_bytes && key_bytes) {
      UA_ByteString_allocBuffer(&cert, cert_bytes->size());
      std::memcpy(cert.data, cert_bytes->data(), cert_bytes->size());
      UA_ByteString_allocBuffer(&key, key_bytes->size());
      std::memcpy(key.data, key_bytes->data(), key_bytes->size());
    } else {
      std::string cn = std::format("CN=logict-client@{}", host), dns = std::format("DNS:{}", host), su = "URI:" + app_uri;
      UA_String subject[2] = {ua_str("O=logict"), ua_str(cn)};
      UA_String san[2] = {ua_str(dns), ua_str(su)};
      UA_KeyValueMap* params = UA_KeyValueMap_new();
      UA_UInt16 days = 3 * 365, bits = 2048;
      UA_KeyValueMap_setScalar(params, UA_QUALIFIEDNAME(0, const_cast<char*>("expires-in-days")), &days, &UA_TYPES[UA_TYPES_UINT16]);
      UA_KeyValueMap_setScalar(params, UA_QUALIFIEDNAME(0, const_cast<char*>("key-size-bits")), &bits, &UA_TYPES[UA_TYPES_UINT16]);
      UA_StatusCode rc = UA_CreateCertificate(&logger_, subject, 2, san, 2, UA_CERTIFICATEFORMAT_DER, params, &key, &cert);
      UA_KeyValueMap_delete(params);
      if (rc != UA_STATUSCODE_GOOD) return fail(rc, "client certificate creation");
      using P = std::filesystem::perms;
      if (!write_file(cert_path, cert, P::owner_read | P::owner_write | P::group_read | P::others_read) ||
          !write_file(key_path, key, P::owner_read | P::owner_write)) {
        UA_ByteString_clear(&cert);
        UA_ByteString_clear(&key);
        return fail(UA_STATUSCODE_BADCONFIGURATIONERROR, "cannot write client certificate to " + pki_.string());
      }
      host_.log.info("created client certificate {}", cert_path.string());
    }
    UA_StatusCode rc = UA_ClientConfig_setDefaultEncryption(&cc, cert, key, nullptr, 0, nullptr, 0);
    UA_ByteString_clear(&cert);
    UA_ByteString_clear(&key);
    if (rc != UA_STATUSCODE_GOOD) return fail(rc, "encryption setup");

    // Server certificate trust: pki/trusted (rejected certs land in pki/rejected).
    if (cc.certificateVerification.clear) cc.certificateVerification.clear(&cc.certificateVerification);
    if (trust_all_) {
      UA_CertificateGroup_AcceptAll(&cc.certificateVerification);
    } else {
      UA_NodeId group = UA_NS0ID(SERVERCONFIGURATION_CERTIFICATEGROUPS_DEFAULTAPPLICATIONGROUP);
      std::string dir = pki_.string();
      rc = UA_CertificateGroup_Filestore(&cc.certificateVerification, &group, ua_str(dir), &logger_, nullptr);
      if (rc != UA_STATUSCODE_GOOD) return fail(rc, "trust store " + dir);
    }
    UA_String_clear(&cc.clientDescription.applicationUri);
    cc.clientDescription.applicationUri = UA_STRING_ALLOC(app_uri.c_str());
    UA_String_clear(&cc.securityPolicyUri);
    cc.securityPolicyUri = UA_STRING_ALLOC(uri->second.c_str());
    cc.securityMode = policy_ == "None"   ? UA_MESSAGESECURITYMODE_NONE
                      : mode_ == "Sign"   ? UA_MESSAGESECURITYMODE_SIGN
                                          : UA_MESSAGESECURITYMODE_SIGNANDENCRYPT;
    return status::Good;
  }

  StatusCode fail(UA_StatusCode rc, const std::string& what) {
    host_.log.error("{}: {} failed: {}", url_, what, UA_StatusCode_name(rc));
    return status::BadConfigurationError;
  }

  void clear_config(UA_ClientConfig& cc) {
    cc.logging = nullptr;  // ours, not heap-allocated
    UA_ClientConfig_clear(&cc);
  }

  StatusCode fail_config(UA_ClientConfig& cc, UA_StatusCode rc, const std::string& what) {
    clear_config(cc);
    return fail(rc, what);
  }

  void destroy() {
    if (!client_) return;
    UA_Client_disconnect(client_);
    UA_Client_getConfig(client_)->logging = nullptr;  // ours, not heap-allocated
    UA_Client_delete(client_);
    client_ = nullptr;
  }

  HostContext& host_;
  std::string url_, policy_, mode_, user_, password_;
  std::filesystem::path pki_;
  bool trust_all_ = false, allow_plaintext_ = false;
  std::size_t max_nodes_ = 500;
  std::chrono::milliseconds connect_timeout_{3000}, request_timeout_{1000};
  UA_Logger logger_{};
  UA_Client* client_ = nullptr;
  std::map<const UaAddress*, DataType> server_types_;
};

nlohmann::json prop(const char* key, const char* type, nlohmann::json def, const char* desc) {
  return {{"key", key}, {"type", type}, {"default", std::move(def)}, {"description", desc}};
}

class UaClientDriver final : public Driver {
 public:
  DriverInfo info() const override { return {"opcua_client", "OPC UA Client", "0.1.0", Medium::Ethernet, {"OPC UA Server"}}; }

  nlohmann::json schema() const override {
    using nlohmann::json;
    return {
        {"driver", "opcua_client"},
        {"display_name", "OPC UA Client"},
        {"channel", json::array()},
        {"device",
         json::array({
             prop("servermain.DEVICE_ID_STRING", "string", "opc.tcp://192.168.0.1:4840", "Server endpoint URL"),
             prop(kPolicy, "enum", "None", "None, Basic256Sha256, Aes128_Sha256_RsaOaep, Aes256_Sha256_RsaPss"),
             prop(kMode, "enum", "SignAndEncrypt", "Sign or SignAndEncrypt (ignored for None)"),
             prop(kUser, "string", "", "User name (empty = anonymous)"),
             prop(kPassword, "string", "", "Password"),
             prop(kPki, "string", "/var/lib/logict/pki-client", "Client certificate and server trust list directory"),
             prop(kTrustAll, "boolean", false, "Accept any server certificate (testing only)"),
             prop(kMaxNodes, "integer", 500, "Nodes per Read request"),
             prop(kPlaintext, "boolean", false, "Send the password unencrypted if the server only offers that (not recommended)"),
         })},
        {"address_help", "NodeId: ns=2;s=Channel1.Device1.Tag1, ns=3;s=\"DB1\".\"Speed\", ns=0;i=2258. Data type Default uses the server's type."},
    };
  }

  Result<std::unique_ptr<DeviceRuntime>> create_device(const ChannelConfig&, const DeviceConfig& device,
                                                       HostContext& host) override {
    if (!device.id.starts_with("opc.tcp://"))
      return make_error(status::BadConfigurationError, "device ID must be an endpoint URL (opc.tcp://host:port)");
    return std::make_unique<UaClientDevice>(device, host);
  }
};

}  // namespace

LOGICT_DECLARE_DRIVER(UaClientDriver)
