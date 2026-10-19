# 04. logict 아키텍처 설계 (Phase 1)

작성일: 2026-10-06 · 상태: v0.6 (M0~M4 + 드라이버 9종; 11절 참고)
근거 문서: [01 레퍼런스](01-reference-research.md) · [02 드라이버 카탈로그](02-kepware-driver-catalog.md) · [03 Kepware 구조·Northbound](03-kepware-architecture-and-northbound.md)

---

## 1. 목표와 범위

**제품 목표**: Kepware Server와 동등한 기능을 갖춘 Linux 네이티브(x86-64 산업용 PC) 산업용 연결 서버.

| 구분 | Phase 1 (이 문서 범위) | 이후 |
|---|---|---|
| Southbound | 드라이버 SDK, Simulator, **Modbus TCP**(→ RTU, LS XGT FEnet, S7, MC로 확장) | 02 카탈로그 순서대로 확장 |
| Northbound | **OPC UA Server**, **MQTT Agent**(Kepware IoT Gateway 호환) | REST agent, Sparkplug B, Advanced Tags, DataLogger 등 (03 B절) |
| 관리 | **Configuration REST API**(Kepware 경로·속성 체계), JSON 프로젝트 파일 | Web UI, 사용자/권한, 이중화 |
| 운영 | systemd 서비스, 로그, `_System`/`_Statistics` 태그 | .deb 패키지, 라이선스 |

**설계 원칙**
1. **Kepware 호환이 기본값이다.** 개념(Channel/Device/Tag), 속성 이름, 기본값, NodeId 규칙, MQTT 페이로드를 그대로 따른다. 기존 SI 인력과 클라이언트가 학습 없이 옮겨올 수 있게 한다.
2. **Kepware의 약점은 개선한다.** 03 D-4의 MQTT 개선점과 Linux 시리얼, 국산 PLC 지원이 여기에 해당한다.
3. **채널 = 스레드.** 장비 하나가 느려도 다른 채널에 영향을 주지 않는다.
4. **드라이버는 플러그인이다.** 코어를 다시 빌드하지 않고 `.so`만 추가해 드라이버를 늘린다.

---

## 2. 기술 스택

| 영역 | 선택 | 라이선스 | 이유 |
|---|---|---|---|
| 언어 | **C++23** (GCC 13, Ubuntu 24.04 기본) | — | Kepware 코어와 같은 언어. `std::expected` 때문에 23 사용 |
| 빌드 | CMake ≥3.28 + Ninja, CMakePresets | — | 표준 |
| 의존성 관리 | CMake **FetchContent**(버전 고정) | — | 오프라인 빌드를 위해 이후 vendor 미러로 전환 가능 |
| OPC UA | **open62541** v1.5.9 (OpenSSL 암호화, 멀티스레딩 모드) | MPL-2.0 | 검증된 C 스택. Kepware도 자체 C 스택을 쓴다 |
| MQTT | **Eclipse Paho C** (MQTTAsync, 3.1.1/5.0, TLS) | EPL-2.0/EDL-1.0 | Kepware도 Paho(Java)로 추정. JVM 없이 내장 |
| 네트워크/시리얼 I/O | POSIX 소켓 + `poll()` (SDK `TcpClient`) | — | 채널 스레드에서 동기 처리하므로 Asio가 필요 없다. 시리얼은 termios로 구현 예정 |
| REST | **cpp-httplib** | MIT | 헤더 하나로 충분 |
| JSON | **nlohmann/json** | MIT | 프로젝트 파일과 API |
| 로그 | **spdlog** | MIT | |
| CLI | **CLI11** | BSD-3 | |
| 테스트 | **GoogleTest** + Python(pymodbus, asyncua) 통합 테스트 | BSD-3 | |

GPL 의존성은 쓰지 않는다. LGPL(libmodbus, snap7)도 쓰지 않는다. Modbus, S7, MC 프로토콜은 직접 구현한다.

---

## 3. 프로세스와 모듈 구성

Phase 1은 **단일 프로세스 `logictd`**로 간다. 스크립트 엔진처럼 크래시 위험이 큰 구성요소는 이후에 별도 프로세스로 분리한다(Kepware의 V8 분리와 같은 방식).

```
                         logictd (single process)
 ┌──────────────────────────────────────────────────────────────────────┐
 │  Management                                                            │
 │   ├─ REST Config API (cpp-httplib, :57412)   /config/v1/...            │
 │   └─ ProjectStore  (project.json  ⇄  in-memory Project model)          │
 │                                                                        │
 │  Core                                                                  │
 │   ├─ TagDB          : 모든 태그의 VQT 캐시 + 구독 refcount + 변경 통지  │
 │   ├─ ChannelManager : 채널마다 ChannelWorker(스레드 1개, Asio io_context)│
 │   │     └─ ChannelWorker ─ DeviceScheduler(scan group) ─ DeviceRuntime │
 │   ├─ PluginLoader   : /usr/lib/logict/drivers/*.so  (dlopen)           │
 │   └─ SystemTags     : _System / _Statistics 가상 태그                  │
 │                                                                        │
 │  Northbound                                                            │
 │   ├─ OpcUaServer    : open62541, 별도 스레드, DataSource → TagDB       │
 │   └─ MqttAgents     : agent마다 Paho 클라이언트, TagDB 구독             │
 └──────────────────────────────────────────────────────────────────────┘
          │ .so plugin (C++ SDK)            │
   [simulator] [modbus] [ls_xgt] [s7] [melsec] ...
```

**스레드 모델**

| 스레드 | 개수 | 역할 |
|---|---|---|
| main | 1 | 시작과 종료, 시그널, ProjectStore 적용 |
| ChannelWorker | 채널당 1 | 소속 디바이스 연결·폴링·쓰기. 채널 안에서는 순차 처리(Kepware와 동일) |
| OPC UA | 1 | `UA_Server_run_iterate` 루프 |
| MQTT | agent당 Paho 내부 스레드 + publish 타이머 1 | |
| REST | httplib 스레드풀 | 설정 변경은 main 큐를 거쳐 직렬화 |

---

## 4. 데이터 모델

### 4.1 계층 (Kepware 동일)

```
Project
 ├─ Channel  (driver, 매체 설정, write 최적화, inter-request delay)
 │   └─ Device (model, ID, timing, auto-demotion, scan mode, 드라이버 고유 속성)
 │       ├─ TagGroup (무제한 중첩)
 │       └─ Tag (address, data_type, client_access, scan_rate, scaling, description)
 └─ IoT Gateway
     └─ MqttAgent (connection, message template, LWT, write subscription)
         └─ IotItem (server_tag, scan_rate, publish_on_change, deadband, enabled)
```

**태그 전체 경로**: `Channel.Device[.Group...].Tag`. 이 경로가 OPC UA NodeId `ns=2;s=<경로>`와 MQTT `id`로 그대로 쓰인다.

### 4.2 값(VQT)

```cpp
using Value = std::variant<std::monostate, bool, int8_t, uint8_t, int16_t, uint16_t,
                           int32_t, uint32_t, int64_t, uint64_t, float, double,
                           std::string, DateTime,
                           std::vector<...> /* 각 스칼라의 배열 */>;
struct Vqt { Value value; StatusCode quality; Timestamp source_ts; Timestamp server_ts; };
```

- **Quality는 내부적으로 OPC UA StatusCode(uint32)를 그대로 쓴다.** Kepware는 DA quality를 쓰지만, UA로 변환할 필요가 없어지고 정보 손실도 없다. MQTT 표준 템플릿의 `q`는 `IsGood(status)`로 bool 변환한다(Kepware 호환).
- **데이터 타입**은 Kepware 목록을 따른다: Default, Boolean, Char, Byte, Short, Word, Long, DWord, LLong, QWord, Float, Double, BCD, LBCD, Date, String, 그리고 각 타입의 배열. UA 매핑은 03 C-4 표를 쓴다.
- **Scaling**: None/Linear/SquareRoot, Clamp, Negate, Units(Kepware 공식). 쓰기 시에는 역변환한다.

### 4.3 프로젝트 파일

- Phase 1의 기준 데이터는 **`project.json` 하나**다(Kepware도 `.json` 프로젝트를 지원한다).
- **속성 키는 Kepware 명명을 따른다**: `common.ALLTYPES_NAME`, `servermain.TAG_ADDRESS`, `servermain.DEVICE_SCAN_MODE` 등. 드라이버 고유 키는 `<driver>.<KEY>` 형태다.
  - 장점: Kepware Config API 응답이나 프로젝트 JSON을 거의 그대로 가져올 수 있다(import 도구는 P1).
- 저장은 원자적으로 한다: 임시 파일 → `fsync` → `rename`. 이전 버전은 `project.json.bak.N`으로 보관한다.
- 경로
  - 설정: `/etc/logict/logictd.conf` (포트, 경로, 로그 레벨)
  - 데이터: `/var/lib/logict/{project.json, pki/}`
  - 로그: `/var/log/logict/`

---

## 5. 드라이버 SDK

### 5.1 플러그인 경계

- 각 `.so`는 `extern "C" const logict_plugin_v1* logict_plugin_entry(void)` 하나만 export한다.
  - 반환 구조체: ABI 버전, 드라이버 이름, 그리고 `Driver` 팩토리 함수 포인터.
- Phase 1의 드라이버는 **모두 같은 툴체인으로 빌드하는 1st-party 코드**다. 그래서 경계 안쪽은 C++ 인터페이스를 쓴다.
  - ABI 버전이 다른 `.so`는 로드를 거부한다.
  - 3rd-party 공개 SDK가 필요해지면 순수 C ABI와 out-of-process 드라이버 호스트를 추가한다. 인터페이스가 메시지 지향이라 이 전환이 가능하다.

### 5.2 인터페이스

```cpp
namespace logict::sdk {

class Driver {                      // 드라이버 종류당 1개
 public:
  virtual DriverInfo info() const = 0;               // name, display, version, medium(Ethernet/Serial)
  virtual nlohmann::json schema() const = 0;         // channel/device/tag 속성 정의 → /config/v1/doc/drivers/{name}
  virtual std::unique_ptr<DeviceRuntime>
      create_device(const ChannelConfig&, const DeviceConfig&, HostContext&) = 0;
};

class DeviceRuntime {               // ChannelWorker 스레드에서만 호출된다
 public:
  virtual Result<std::shared_ptr<const Address>>
      parse_address(std::string_view addr, DataType type) = 0;   // 태그 추가 시 검증·파싱
  virtual Status connect(Deadline) = 0;
  virtual void   disconnect() = 0;
  virtual void   read(std::span<ReadItem> items, Deadline) = 0;  // 각 item에 VQT를 채운다
  virtual Status write(const Address&, const Value&, Deadline) = 0;
};
}
```

- **요청 병합(coalescing)은 SDK 유틸리티 `BlockPlanner`로 제공한다.**
  - 드라이버는 `Address`를 `(area, offset, length, bit)`로 바꾸고, 영역별 최대 블록 크기와 허용 gap만 넘긴다.
  - 플래너는 블록 목록을 돌려주고, 항목 집합이 바뀌지 않으면 결과를 캐시한다.
  - 사용 여부는 드라이버가 정한다. 그래서 S7 multi-var, CIP Multiple Service Packet처럼 프로토콜 고유 최적화도 가능하다.
- **바이트/워드 순서 코덱**(ABCD/CDAB/BADC/DCBA, BCD, 문자열 패킹)도 SDK 공통 유틸리티로 둔다.
- `HostContext`는 로거, 시계, 시리얼·소켓 팩토리(Asio io_context)를 제공한다.

### 5.3 채널/디바이스 동작 (Kepware 기본값)

| 항목 | 기본값 |
|---|---|
| Connect timeout | 3 s |
| Request timeout | 1000 ms |
| Attempts before timeout | 3 |
| Inter-request delay | 0 ms |
| Auto-Demotion | 타임아웃 3회 → 10 s off-scan, 그동안 요청은 버리지 않음(설정 가능) |
| Write 최적화 | Write Only Latest Value for All Tags, Duty Cycle 10 |
| Scan mode | Respect Client-Specified Scan Rate(기본), Request Data No Faster Than, Request All Data at Scan Rate, Do Not Scan/Demand Poll Only, Respect Tag-Specified |
| Non-normalized float | 0으로 치환(기본) / 그대로 전달 |

**폴링 원칙**: **구독된 태그만 폴링한다.** 구독 주체는 OPC UA MonitoredItem, MQTT IotItem, 그리고 내부 사용이다. 각 구독은 요청 주기를 가진다. 태그의 실효 주기는 scan mode에 따라 정해진다. 디바이스 스케줄러는 같은 주기의 태그를 scan group으로 묶어 드라이버 `read()`를 호출한다. 구독이 없는 태그를 단발로 읽으면(UA Read) 1회 demand read를 하고, tag cache timeout(5 s) 동안 캐시한다.

---

## 6. Northbound

### 6.1 OPC UA Server (03 C절 기준)

- **Endpoint**: `opc.tcp://<host>:49320`(Kepware 호환, 설정 가능)
  - 보안 정책: None(기본 off), Basic256Sha256(Sign / SignAndEncrypt), Aes128_Sha256_RsaOaep, Aes256_Sha256_RsaPss
- **User token**: Anonymous(기본 **off**), UserName. X509는 P1.
- **PKI**: `pki/{own,trusted,rejected,issuers}`. 처음 접속한 클라이언트 인증서는 rejected에 저장하고, REST/CLI로 trust 처리한다.
- **주소 공간**: `Objects/<Channel>/<Device>/<Group>/<Tag>`, NodeId `ns=2;s=Channel.Device.Tag`, 그리고 `_System`, `_Statistics` 노드.
- **값 전달**: 태그 노드는 DataSource 콜백으로 만든다.
  - Read → TagDB 캐시
  - Write → ChannelWorker 큐 → 디바이스 응답까지 대기(타임아웃 시 BadTimeout)
- **구독 연동**: open62541 `monitoredItemRegisterCallback`으로 TagDB 구독을 등록·해제한다. 샘플링 주기를 요청 주기로 넘긴다.
- **기본 설정**(Kepware): max sessions 128, session timeout 15–60 s, data queue 2, retransmit queue 10, max notifications/publish 65536. 추가 설정: min sampling 10 ms, min publishing 50 ms.
- **주소 공간 증분 갱신**: 태그 추가·삭제 시 `UA_Server_addNode`/`deleteNode`를 쓴다. 재시작이 필요 없다.

### 6.2 MQTT Agent (03 D절 기준)

- **호환 범위**
  - D-2와 D-3의 모든 속성(이름과 기본값 동일)
  - Standard template `{"timestamp":..,"values":[{"id","v","q","t"}]}`
  - Advanced template(`|#each VALUES|`, `|#unless @last|`, 변수 치환)
  - Wide/Narrow 포맷, Interval/On Data Change
  - LWT와 online/shutdown 메시지
  - Write topic `iotgateway/write` `[{"id","v"}]`
  - `_DroppedEvents`, `_PublishesSent` 시스템 태그
- **개선 사항(Phase 1 포함)**
  - **Topic 템플릿**: `{channel}/{device}/{tag}` 같은 태그별 토픽(UNS)
  - MQTT 5.0 선택
  - write 결과 응답 topic
  - 절대값 deadband
  - 선택 필드 `qc`(StatusCode)
- **이후(P1)**: 디스크 store-and-forward, Sparkplug B, WebSocket.

### 6.3 Configuration REST API

- 경로와 동작은 Kepware Config API를 따른다.
  - `GET/POST/PUT/DELETE /config/v1/project/channels/{ch}/devices/{dev}/tag_groups/{g}/tags/{t}`
  - `/config/v1/project/_iot_gateway/mqtt_clients/{agent}/iot_items/{item}`
  - `/config/v1/doc/drivers`, `/config/v1/doc/drivers/{driver}`: 드라이버 schema. UI가 이걸 보고 폼을 자동 생성한다.
  - `PROJECT_ID` 낙관적 동시성, `/config/v1/event_log`, `/config/v1/status`
- **보안**: 기본은 localhost 바인딩과 Basic 인증(HTTPS는 P1).
- **변경 적용**: REST → main 큐 → 모델 diff → 영향받은 채널만 재시작 또는 증분 적용. UA 노드와 MQTT 아이템도 함께 갱신한다.

---

## 7. 진단

- **`_System`**
  - 디바이스: `_Error`, `_NoError`, `_Enabled`(RW), `_ScanMode`, `_ScanRateMs`, `_SecondsInError`, `_RequestTimeout`(RW), `_RequestAttempts`(RW), `_DeviceId`
  - 서버: `_ActiveTagCount`, `_ClientCount`, `_ProductVersion`, `_DateTime`
- **`_Statistics`**(채널): `_SuccessfulReads`, `_FailedReads`, `_SuccessfulWrites`, `_FailedWrites`, `_RxBytes`, `_TxBytes`, `_StatisticsReset`
- **Event Log**: 메모리 링버퍼(기본 10,000건)와 파일. REST로 조회한다.
- **Comm Diagnostics**(채널별 TX/RX 프레임 캡처)는 P1이다. 다만 SDK의 송수신 경로에 hook 자리는 처음부터 만들어 둔다.

---

## 8. 저장소 구조

```
logict/
├─ CMakeLists.txt, CMakePresets.json
├─ cmake/Dependencies.cmake        # FetchContent 버전 고정
├─ sdk/include/logict/sdk/         # 드라이버 SDK (공개 헤더)
│   ├─ types.h  value.h  status.h  driver.h  block_planner.h  codec.h
├─ sdk/src/                        # BlockPlanner, codec 구현 (liblogict_sdk)
├─ core/                           # liblogict_core
│   ├─ model/   (Project, Channel, Device, Tag, 속성 키, JSON 직렬화)
│   ├─ tagdb/   (TagDB, 구독, 통지)
│   ├─ runtime/ (ChannelWorker, DeviceScheduler, PluginLoader, SystemTags)
│   └─ util/
├─ northbound/opcua/   northbound/mqtt/
├─ api/rest/
├─ app/logictd/main.cpp
├─ drivers/simulator/  drivers/modbus/  (이후 ls_xgt/ s7/ melsec/ ...)
├─ tests/unit/  tests/integration/ (pytest + pymodbus + asyncua + paho)
├─ packaging/ (systemd unit, 기본 conf)
└─ docs/
```

---

## 9. 마일스톤

| M | 내용 | 완료 기준 |
|---|---|---|
| **M0** | 빌드 뼈대, 의존성, CI 스크립트 | `cmake --preset debug && ctest` 통과 |
| **M1** | 모델, 프로젝트 로드, TagDB, ChannelWorker, PluginLoader, **Simulator 드라이버** | 예제 project.json 로드 후 시뮬레이터 태그 값이 주기적으로 갱신됨(로그 확인) |
| **M2** | **OPC UA Server** | UaExpert/asyncua로 browse, read, write, subscribe 가능. `_System` 노드 노출 |
| **M3** | **Modbus TCP 드라이버** + BlockPlanner + 코덱 | pymodbus 서버 대상 통합 테스트: 블록 병합, 워드 순서 4종, 타임아웃·데모션 동작 |
| **M4** | **MQTT Agent** | mosquitto 대상: standard/advanced template, narrow/wide, LWT, write topic 테스트 통과 |
| **M5** | **REST Config API** + 증분 반영 | API로 채널, 디바이스, 태그 CRUD → UA 주소 공간에 즉시 반영 |
| M6 | systemd 패키징, 부하 테스트(태그 1만 개, 100 ms) | CPU·메모리 기준치 기록 |
| 이후 | Modbus RTU → **LS XGT FEnet** → S7 → MC → OPC UA Client → ... | 02 카탈로그 P0/P1 |

**Phase 1 = M0~M6.** M1~M3를 끝내면 "Modbus 장비 → OPC UA" 종단 간 PoC가 완성된다.

---

## 10. 결정 사항과 열린 질문

| # | 내용 | 상태 |
|---|---|---|
| D1 | 산업용 PC x86-64 Linux 대상 | 확정 |
| D2 | C++23 (Kepware와 같은 언어) | 확정 |
| D3 | Northbound 1단계는 OPC UA + MQTT | 확정 |
| D4 | 드라이버 로드맵은 Kepware 142종 기준, 국산(LS, Cimon) 추가 | 확정 |
| D5 | 내부 quality = UA StatusCode | 제안 |
| D6 | 프로젝트 저장 = JSON 파일, Kepware 속성 키 체계 | 제안 |
| D7 | 단일 프로세스 + in-process `.so` 드라이버(out-of-process는 이후) | 제안 |
| Q1 | 제품명과 네임스페이스(가칭 `logict`) | 열림 |
| Q2 | 라이선스와 과금 모델(영구/구독, 태그 수 제한) | 열림 |
| Q3 | Web UI 기술과 시기 | 열림 |

---

## 11. 구현 현황 (2026-10-06)

| M | 상태 | 비고 |
|---|---|---|
| M0 빌드 뼈대 | ✅ | CMake presets(debug=ASan/UBSan, release), FetchContent로 버전 고정 |
| M1 코어 + 시뮬레이터 | ✅ | Kepware JSON 프로젝트, TagDB, 채널당 스레드, 구독 기반 폴링, scan mode 5종, 스케일링, Auto-Demotion, write 최적화(duty cycle), `_System`/`_Statistics` |
| M2 OPC UA 서버 | ✅ | `ns=2;s=Channel.Device.Tag`, Basic256Sha256/Aes128/Aes256, UserName(PBKDF2), 익명 기본 off, PKI 파일 저장소(rejected→trusted), 비동기 write(디바이스 ACK 후 응답), 태그 캐시 |
| M3 Modbus TCP | ✅ | Kepware 주소 문법(5·6자리, `.bit`, `.nH/.nL`, 배열), 바이트·워드 순서, FC 1~6/15/16/22, 블록 병합, 잘못된 주소 격리, 재시도·stale 응답 폐기 |
| M4 MQTT 에이전트 | ✅ | Kepware IoT Gateway 호환: 표준/고급 템플릿, Narrow/Wide, Interval/On Data Change, 데드밴드(%·절대값), Every Scan, LWT + online/shutdown 메시지, 쓰기 토픽, 끊긴 동안 메모리 버퍼(초과 시 새 이벤트 폐기), `_IoT_Gateway.<agent>.*` 상태 태그. 확장: MQTT 5, 태그별 토픽, 쓰기 결과 응답(MQTT 5 ResponseTopic/CorrelationData 지원), TLS(ssl://, CA·클라이언트 인증서) |
| 드라이버: LS XGT FEnet | ✅ (실장비 미검증) | XGT 전용 프로토콜 TCP 2004 (FEnet 매뉴얼 V2.30 8장 기준). XGK/XGB는 XG5000 표기(D100, D100.A, M0010A, T5, ZR100), XGI/XGR/XEC는 IEC 직접변수(%MW100, %MD10, %MX5, %IX0.0.3). 연속 읽기(최대 1400바이트)로 병합, 연속 미지원 영역은 개별 읽기(16블록), 비트 쓰기는 %X 또는 읽기-수정-쓰기, NAK 주소 격리 |
| 드라이버: Siemens TCP/IP Ethernet | ✅ | S7comm(ISO-on-TCP 102, PUT/GET). Kepware 주소 문법(I/Q/M, DBn.DBX/DBB/DBW/DBD, DBn.STRINGo.len). PDU 협상, 다중 항목 Read Var로 묶음, PDU보다 큰 태그 분할. **독립 구현인 python-snap7 S7 서버로 교차 검증** |
| 드라이버: Mitsubishi Ethernet | ✅ | MC/SLMP 3E 바이너리(Q/L/iQ-R/iQ-F). GX Works 표기(D100, D100.A, M10, X1F, TN5, ZR100), iQ-F X/Y 8진수. 워드·비트 단위 일괄 읽기/쓰기. 시뮬레이터를 **독립 클라이언트 pymcprotocol로 먼저 검증**한 뒤 드라이버 테스트 |
| 드라이버: Modbus RTU Serial | ✅ | SDK SerialPort(termios, RS-485 옵션) 신설, TCP/RTU가 Modbus 장치 로직 공유. CRC-16, 3.5문자 무통신 간격, 브로드캐스트. 한 채널(포트)을 여러 국이 공유하고, 응답 없는 국이 포트를 닫지 않음. socat 가상 시리얼 + **pymodbus RTU 슬레이브**로 검증 |
| 드라이버: OPC UA Client | ✅ | 배치 Read 폴링, 서버 타입으로 Write. None/Basic256Sha256/Aes128/Aes256, 클라이언트 인증서 자동 생성, 서버 인증서 신뢰 저장소, 사용자 로그인(토큰 암호화, 평문은 명시적 옵트인). **asyncua 서버(독립 구현)**와 logictd 2대 연결(보안·인증서 신뢰 흐름)로 검증 |
| 드라이버: Allen-Bradley ControlLogix Ethernet | ✅ | libplctag 2.7.2 정적 링크. Logix 심볼릭(프로그램 범위, 배열 `{n}`, UDT 멤버, 비트), Micro800, PCCC(PLC-5/SLC/MicroLogix). Default 타입은 PLC가 알려주는 타입 사용. RMW 서비스 미지원 시 비트 쓰기 대체 경로. libplctag **ab_server** + `tag_rw2`로 검증 |
| 드라이버: LS XGT Cnet | ✅ (실장비 미검증) | XBL-C21A 매뉴얼 7장 기준 ASCII 전용 프로토콜(SS/SB, BCC 선택). 연속 읽기 60워드, 개별 16블록, 국 공유 시리얼. 매뉴얼 예제 프레임과 바이트 단위 일치 확인 + 매뉴얼 기반 시뮬레이터로 검증 |
| 드라이버: Omron FINS Ethernet | ✅ | FINS/UDP(SID로 늦은 응답 폐기)와 FINS/TCP(노드 주소 교환). Memory Area Read/Write, CIO/W/H/A/D/EM 뱅크/T·C(PV·완료 플래그), 비트 단위 쓰기(읽기-수정-쓰기 불필요). 하위 워드 먼저(옵션). SDK에 UdpClient 추가. 시뮬레이터를 **독립 클라이언트 `fins` 패키지로 먼저 검증**한 뒤 UDP·TCP 각각 드라이버 테스트 |
| 드라이버: Keyence KV Ethernet | ✅ (실장비 미검증) | 상위 링크 ASCII(TCP 8501): RDS/WRS/RD, KEYENCE 표기(R·MR·LR·CR 채널+비트, B·VB·W 16진수)와 XYM 별칭, 워드는 `.H`로 읽어 하위 워드 먼저 조합, 타이머·카운터 `.D`. SDK TcpClient에 `recv_some` 추가. 시뮬레이터를 **독립 클라이언트 `kv-hostlink`로 먼저 검증**한 뒤 드라이버 테스트 |
| 드라이버: Modbus ASCII Serial | ✅ | `:` + 16진수 + LRC + CR LF 프레임. RTU와 시리얼 회선 공유·국 처리 코드를 `modbus_serial.h`로 합쳐 프레이밍만 다름. LRC는 규격 예제와 일치, socat + **pymodbus ASCII 슬레이브**로 RTU와 같은 시나리오 검증 |
| 드라이버: Omron NJ Ethernet | ✅ (실장비 미검증) | EtherNet/IP(CIP) 변수 이름 접근, libplctag `omron-njnx`. Rockwell 드라이버와 장치 코드를 `cip_device.h`로 공유(CIP 타입 코드 BYTE/WORD/DWORD/LWORD/STRING 추가). libplctag **ab_server(Omron 모드)** + `tag_rw2`로 숫자·배열·비트 검증 |
| Mitsubishi Ethernet: 3E ASCII 코드 | ✅ | 통신 데이터 코드 ASCII 옵션. 디바이스 번호는 디바이스 표기대로(16진수 X/Y/B/W, iQ-F X/Y 8진수), 데이터는 바이너리 배치로 변환해 드라이버 공통 처리. 매뉴얼 예제 프레임과 일치. pymcprotocol ASCII로 10진수 디바이스 검증(pymcprotocol은 16진수 디바이스 번호를 10진수로 보내 매뉴얼과 다름), 드라이버 통합 테스트는 바이너리·ASCII 모두 |
| Mitsubishi Ethernet: 1E 프레임 | ✅ (실장비 미검증) | FX3U/FX3G + FX3U-ENET(-ADP)용 A 호환 1E 바이너리. 요청 길이가 응답 길이를 정하고, 0x5B 뒤의 이상 코드 처리. 1회 최대 64워드/256비트. FX 모델은 1E와 8진수 X/Y가 기본. 시뮬레이터를 **HslCommunication MelsecA1ENet**(독립 구현)으로 검증 |
| 드라이버: Mitsubishi Serial | ✅ (실장비 미검증) | C24 시리얼 모듈용 MC 프로토콜 3C/4C 프레임, ASCII 형식 1·4, 합계 체크 선택, 국 공유 시리얼. 디바이스 처리는 Ethernet과 `mc_device.h`로 공유. **매뉴얼(SH-080008) 부록 7 예제 프레임과 바이트 단위 일치**, socat + 시뮬레이터(장치 메모리를 3E TCP로도 노출해 pymcprotocol로 값 확인)로 3가지 조합 검증 |
| 드라이버: CIMON Serial / CIMON Ethernet | ✅ (실장비 미검증) | Kepware에 없는 국산 드라이버. CIMON 공식 매뉴얼(cimon.atlassian.net) 기준: 시리얼 컴퓨터 링크(ASCII, BCC)와 이더넷 전용 서비스(`KDT_PLC_M`, TCP 10260). R/W/r/w 명령, 블록 병합·오류 격리·비트 RMW 공통 처리(`cimon_device.h`). **시리얼 프레임은 매뉴얼 예제 7개(읽기·쓰기·비트·응답·오류)와 바이트 단위 일치**. 매뉴얼 기반 시뮬레이터로 시리얼(socat)·이더넷 통합 테스트 |
| 드라이버: Modbus Server Serial | ✅ | logictd가 시리얼 Modbus 슬레이브(RTU/ASCII)로 동작. 국번마다 데이터 테이블, 요청 처리기(FC 1~6/15/16/22/23)를 마스터 응답과 태그 읽기·쓰기가 함께 써서 Modbus 장치 코드(주소·인코딩·배열·문자열)를 그대로 재사용. 태그는 입력 영역(1x/3x)도 쓸 수 있음. RTU는 기능 코드로 요청 길이를 계산하고 CRC 오류·다른 슬레이브 응답은 무통신 간격까지 건너뜀. **처리기는 Modbus 규격서 예제 프레임과 일치**, socat + **pymodbus 시리얼 마스터**로 RTU·ASCII 검증 |
| 드라이버: Memory Based | ✅ | Kepware와 같은 `RAM` 주소(태그마다 별도 값)에 이름으로 공유하는 `RAM.<name>` 확장. 기본 타입 Double, 배열 지원 |
| 드라이버: System Monitor | ✅ | Kepware는 Windows 성능 카운터, logict는 Linux `/proc`·`/sys`·`statvfs`: CPU 사용률·코어 수, 메모리·스왑, 부하 평균, 가동 시간, 호스트명, 프로세스 수, 디스크, 네트워크 카운터·링크 상태, 프로세스 이름별 개수, 온도. 파서는 단위 테스트, 값은 Python(os, shutil)이 같은 호스트에서 읽은 값과 비교 |
| M5 REST Config API | ⏳ | |
| M6 패키징·부하 테스트 | ⏳ | |

**테스트**: 단위 123개(gtest) + 통합 52개(pytest: asyncua, pymodbus, paho-mqtt + Mosquitto, python-snap7, pymcprotocol, HslCommunication, fins, kv-hostlink, libplctag ab_server, socat 가상 시리얼). 일반 빌드와 ASan/UBSan 빌드 모두 통과.

**구현 중 발견해서 고친 문제** (회귀 테스트 있음)
- open62541이 노드를 만들 때 기본값을 write 콜백으로 써 넣으려 함 → 시작할 때 PLC에 0이 쓰일 위험. 빌드 중에는 write를 차단하고 타입에 맞는 초기값을 준다.
- open62541 기본 설정의 세션 PKI가 AcceptAll → X509 사용자 토큰으로 비밀번호 없이 로그인할 수 있었음. 세션 PKI를 제거해서 Certificate 토큰을 노출하지 않는다.
- Bad 품질이 BadWaitingForInitialData로 덮어써짐 → Bad는 콜백 반환값으로 전달한다.
- Modbus 잘못된 주소를 격리한 뒤에도 그 공백을 포함해 다시 병합 → 매 스캔 예외가 발생해 요청이 3배. 금지 구간으로 기록해서 병합하지 않는다.
- OPC UA Client: `UA_Client_new()`의 기본 설정 위에 `setDefault()`를 다시 불러 연결할 때마다 이벤트 루프 등이 누수(LeakSanitizer로 발견). 설정을 먼저 만들고 `UA_Client_newWithConfig`로 한 번만 생성한다.
- Rockwell: `{n}` 개수와 배열 데이터 타입을 같이 쓰면 1개만 읽던 문제. `{n}`이 개수를 정한다.

**알려진 제약 (다음 작업 후보)**
- OPC UA MonitoredItem의 sampling interval을 open62541이 노출하지 않음 → 구독 요청 주기로 태그의 Scan Rate를 쓴다("Respect client rate" 모드가 근사치).
- 구독되지 않은 태그를 처음 읽으면 BadWaitingForInitialData를 반환하고, 그 시점에 폴링을 시작한다(Kepware는 demand read로 첫 값을 기다림). async read로 개선할 예정.
- 프로젝트 핫 리로드, 사용자별 권한(RBAC), Comm Diagnostics 프레임 뷰어 미구현.
- **LS XGT 드라이버는 실제 PLC로 검증하지 않았다.** 매뉴얼 기반 테스트 서버로만 검증했다. 실장비에서 확인할 항목: 요청 헤더 CPU Info 값, R·ZR 영역의 연속 읽기 지원 여부(지금은 개별 읽기), XGB 내장 FEnet의 최대 블록 크기, NAK 에러 코드 의미, U 영역(미지원).
- Siemens: S7comm-plus(최적화 블록 접근)는 지원하지 않음(01 문서의 법적 검토 사항). S7-1200/1500은 PUT/GET 허용과 "최적화된 블록 액세스" 해제가 필요하다. 타이머·카운터(T/C) 영역 미지원.
- Mitsubishi: 3E 바이너리·ASCII, 1E 바이너리 지원. 1E는 S(스텝 릴레이) 디바이스 미지원, 1회 최대 점수(64워드)는 FX3U-ENET 기준이며 실장비 미검증. iQ-R 확장 디바이스(4바이트 번호), 시리얼 형식 2·3·5(바이너리)와 1C/2C 프레임은 다음 단계.
- OPC UA Client: 폴링 방식만 지원(구독/MonitoredItem 방식은 다음 단계). 구조체(ExtensionObject) 값 미지원.
- Omron FINS: CS/CJ 모드 FINS만 지원. Host Link(시리얼)는 다음 단계. 실장비 미검증.
- Omron NJ: STRING 변수는 검증하지 못함(`tag_rw2`도 ab_server Omron 모드와 문자열을 왕복하지 못해 기준이 없음). 구조체 통째 읽기·변수 브라우즈 미지원. 실장비 미검증.
- Keyence: TCP만 지원(UDP 상위 링크는 다음 단계). 문자열 바이트 순서(첫 글자가 상위 바이트)와 T/C 접점 응답 형식(`접점,현재값,설정값`)은 kv-hostlink 기준이며 실장비 미검증.
- CIMON: 이더넷 프레임의 2바이트 필드(길이·워드 데이터·체크섬) 바이트 순서는 매뉴얼 예제(크기·오류 코드가 상위 바이트 먼저)에서 추론한 빅엔디안이다. 매뉴얼의 워드 쓰기 페이지가 읽기 페이지 복사본이라 쓰기 형식도 추론이다. 비트 디바이스 워드 접근 주소(비트 자리 0), T/C 접점 번호(10진수), 문자열 바이트 순서(첫 글자가 상위 바이트)도 실장비 확인이 필요하다. UDP(10262)는 미지원.
- Rockwell: UDT 전체를 한 태그로 읽기(구조체 값) 미지원, 멤버 단위로 읽어야 함. 태그 브라우즈(자동 태그 생성) 미지원.
- Modbus Server Serial: 데이터 테이블은 메모리 전용(재시작하면 0). 다중 슬레이브 버스에서 다른 슬레이브의 응답은 CRC로 걸러 다음 무통신 간격까지 건너뛰므로, 짧은 간격으로 연속되는 프레임은 놓칠 수 있다(실장비 버스 미검증). 8·16번 진단 기능 미지원. Modbus TCP 서버(슬레이브)는 다음 단계.
- Memory Based: 값은 재시작하면 사라진다(Kepware와 같음). `RAM` 주소의 기본 타입(Double)은 추정이다.
- System Monitor: 주소 체계는 Kepware(Windows 성능 카운터 경로)와 다르다. 컨테이너 안에서는 컨테이너가 보는 `/proc` 값이다.
- 시리얼(RTU, ASCII, Cnet, Mitsubishi, Modbus Server): 가상 포트(pty)는 패리티를 흉내 내지 못해 8N1로만 검증했다.
- MQTT 버퍼는 메모리 전용이라 재시작하면 사라진다(Kepware와 동일). 디스크 Store & Forward, Sparkplug B, WebSocket은 다음 단계.
- MQTT `_PublishesSent`는 Paho 송신 큐에 넣은 건수다(QoS 1/2 브로커 ACK 기준이 아님).
- Kepware 속성 키·데이터 타입 코드는 문서 기반 재구성이다. 실제 Kepware `/config/v1/doc`와 대조해야 한다.
