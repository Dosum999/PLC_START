# 03. Kepware 아키텍처·구현 기술 및 Northbound 인터페이스 조사

- 작성일: 2026-10-06
- 목적: Linux(x86-64 산업용 PC)용 Kepware 유사 connectivity server 개발 시 (1) Kepware와 같은 구현 언어/기술 선정 근거 확보, (2) Northbound(client-side) 인터페이스 범위·우선순위 결정, (3) P0 항목(OPC UA Server, IoT Gateway MQTT) 상세 스펙 확보
- 표기 규칙: **[확인]** = 공식 문서/채용공고 등 1차 출처로 확인, **[추정]** = 간접 증거에 기반한 추론, **[미확인]** = 근거 부족

---

## 요약 (TL;DR)

- **핵심 runtime 언어는 C++ (일부 C)**. PTC 채용공고에 "codebase primarily written in C++, with components in C# and Java", "Developing software for both Windows and Linux"라고 명시됨 **[확인]**. Claroty Team82는 Kepware의 OPC UA stack을 "Unknown / Proprietary, C"로 분류 **[확인]**.
- **IoT Gateway(MQTT/REST)는 Java 별도 프로세스**(`server_iotgateway.exe` / `tkedge_iotgateway.service`). Kepware Server 7.1부터 Java 21 사용 **[확인]**. Windows에서는 **32-bit** Java 21이 필요함 → Windows runtime은 32-bit 프로세스로 추정 **[추정]**.
- **UDD(Universal Device Driver)는 V8 JavaScript engine**을 별도 격리 서비스(`server_script_engine.exe`, "Script Engine")에서 실행 **[확인]**.
- **멀티 프로세스 + 플러그인 DLL 구조**: `server_runtime.exe`(코어), `server_config.exe`(GUI), `server_admin.exe`, `server_eventlog.exe`, `config_api_service.exe`, `server_iotgateway.exe`, `server_historian.exe`, `server_script_engine.exe`, `server_store_and_forward.exe`, 그리고 `Drivers\*.dll`(드라이버), `IoT_Gateway.dll`, `ddeserver.dll`, `aeserverinterface.dll`, `hdaserver.dll` 등 **[확인]**.
- **Kepware Edge(Linux)**: GUI 없이 REST Configuration API + `edge_admin` CLI로 관리하고, systemd 서비스 4개(`tkedge_runtime`, `tkedge_configapi`, `tkedge_eventlog`, `tkedge_iotgateway`)로 구성됨. x86-64 전용이며 컨테이너로 배포 가능 **[확인]**. Windows C++ 코드베이스를 Linux로 포팅한 방식(Qt/Boost/자체 추상화 등)은 공개 자료 없음 **[미확인]**.
- **권장 결론**: Kepware와 같은 기술 선택은 **코어/드라이버/OPC UA server = C++ (C++17/20)**입니다. MQTT/REST 게이트웨이는 Kepware처럼 Java로 분리할 수도 있지만, 산업용 PC 리소스와 배포 단순성을 생각하면 **C++ 단일 런타임 안에 넣는 것을 권장**합니다(Kepware가 Java를 쓴 것은 Paho Java·Handlebars 등 라이브러리 재사용 목적으로 추정). 스크립트 확장(UDD 대응)은 별도 프로세스의 JS engine(V8 또는 QuickJS)을 쓰는 방식이 Kepware와 같은 구조입니다.

---

## A) 기술 스택 (확인 / 추정 구분)

### A-1. 코어 런타임 언어

| 항목 | 내용 | 등급 | 근거 |
|---|---|---|---|
| 주 언어 | C++ ("codebase primarily written in C++, with components in C# and Java") | [확인] | PTC Associate Software Engineer 채용공고(Portland, ME, Kepware) |
| 플랫폼 | "Developing software for both Windows and Linux" 경험 우대 | [확인] | 같은 채용공고 |
| 개발 도구 | Visual Studio, GitLab, JIRA | [확인] | 같은 채용공고 |
| OPC UA stack | Team82 분류: "Unknown / Proprietary" 구현, 언어 C. `memcpy`, `sprintf` 오용 취약점(CVE-2020-27263) 존재 | [확인] | Claroty Team82 OPC UA Deep Dive Part 5 |
| OPC UA stack 출처 | Kepware 자체 구현 stack(OEM 제품 Rockwell KEPServer Enterprise, GE IGS, TOP Server에도 내장). Unified Automation 웹사이트에 KEPServerEX가 "end user product" 사례로 언급되나, UA SDK 사용 여부는 확인되지 않음 | [추정] | Claroty "Kepware's OPC protocol stack is embedded as a third-party component in many products", CISA ICSA-20-352-02 |
| 프로세스 비트수(Windows) | 설치 경로 `C:\Program Files (x86)\Kepware\KEPServerEX 6\`, IoT Gateway는 "32-bit Java 21 runtime"만 지원 → 런타임 32-bit | [추정] (경로·Java 요구사항은 [확인]) | IoT Gateway 매뉴얼 External Dependencies, uninstaller 파일 목록 |
| C# 사용처 | 채용공고에 C# 컴포넌트 존재 명시. 위치는 미공개(License Utility, Kepware+ agent, Configuration API 일부 등으로 추정) | [추정] | 채용공고 |
| Java 사용처 | IoT Gateway service(`server_iotgateway.exe`가 JVM 호스팅). 7.1에서 "Updated components to Java 21" | [확인] | IoT Gateway 매뉴얼, 7.1.271.0 What's New |

### A-2. 프로세스 / 모듈 구조 (Windows Kepware Server 6.x/7.x)

설치 파일 목록 **[확인]**:

| 실행 파일 / DLL | 역할 |
|---|---|
| `server_runtime.exe` | 코어 런타임(서비스). 프로젝트(.opf/.json) 로드, 드라이버 DLL 로드, OPC DA/UA/DDE 등 client interface 제공 |
| `server_config.exe` | Windows GUI 설정 도구(Configuration). UI 프레임워크는 미공개이며, 전통적인 Win32/MFC 스타일 UI로 추정 **[추정]** |
| `server_admin.exe` | 시스템 트레이 Administration(설정, User Manager, 인증서 등) |
| `server_eventlog.exe` | Event Log 서비스 |
| `config_api_service.exe` | REST Configuration API 서비스(HTTP/HTTPS 내장 서버). 정적 파일 `CfgApiStcFls.zip` 포함 |
| `server_iotgateway.exe` | IoT Gateway Java 서비스 호스트 |
| `server_historian.exe` | Local Historian 서비스 |
| `server_script_engine.exe` | UDD용 V8 JavaScript Script Engine(격리 프로세스) |
| `server_store_and_forward.exe` | Store and Forward 서비스 |
| `opcuacm.exe` | OPC UA Configuration Manager(엔드포인트, 신뢰 인증서 관리) |
| `opcquickclient.exe` | OPC Quick Client(테스트 클라이언트) |
| `keysvc.exe`, `activationclient.exe` | 라이선스(USB key/소프트웨어 activation) |
| `Drivers\*.dll` | 드라이버 플러그인. 예: `modbus_ethernet.dll`, `controllogix_ethernet.dll`, `mqtt_client.dll`, `mtconnect_client.dll`, `BACnet.dll`, `iec_61850_mms_client.dll` 등 |
| `ddeserver.dll`, `aeserverinterface.dll` | DDE interface, OPC AE interface |
| `IoT_Gateway.dll` | IoT Gateway 서버 플러그인(설정, 데이터 수집, 라이선스) |
| `hdaserver.dll` | OPC HDA 1.2 server(Local Historian) |

→ **구조 결론 [확인]**: "코어 런타임 + 드라이버 DLL 플러그인 + 기능 플러그인 DLL + 보조 서비스(별도 프로세스)" 모델입니다. 위험하거나 이질적인 런타임(Java, V8)은 **별도 프로세스로 격리**하고, localhost TCP로 통신합니다(IoT Gateway 기본 포트 57212/Edge 57213, "gateway service does not accept remote connections").

### A-3. 하위 기술 요소별

| 요소 | 내용 | 등급 |
|---|---|---|
| Configuration API | 런타임과 분리된 REST 서비스. Windows 기본 포트는 HTTP 57412 / HTTPS 57512로 알려짐 **[추정]**. Edge 기본값은 HTTPS **57513**(활성), HTTP **57413**(비활성), Runtime↔ConfigAPI는 localhost **32403** **[확인]**. 경로 체계: `/config/v1/project/...`, `/config/v1/admin/...`, `/config/v1/doc/...`, `/config/v1/event_log`, `/config/v1/transaction_log`. 속성 이름은 `<namespace>.<PROPERTY>` 형식(`servermain.TAG_ADDRESS`, `iot_gateway.AGENTTYPES_ENABLED`, `uaserverinterface.PROJECT_OPC_UA_ENABLE`, `libadminsettings.UACONFIGMANAGER_ENDPOINT_PORT`) | [확인] |
| 프로젝트 파일 | `.opf`(바이너리), `.json`, `.lpf`(Edge 예제) 사용. 암호화 저장 옵션 있음 | [확인] |
| IoT Gateway | Java 21(Windows는 32-bit, Edge는 OpenJDK 또는 Amazon Corretto). MQTT 3.1/3.1.1 | [확인] |
| MQTT client 라이브러리 | 이벤트 로그 메시지 "MqttException", "MqttSecurityException"은 **Eclipse Paho Java**(`org.eclipse.paho.client.mqttv3`) 예외 클래스 이름과 같음 | [추정] (근거 강함) |
| Advanced Template 엔진 | "uses a subset of Handlebars" → Java 구현(Handlebars.java 계열)으로 추정 | 문구는 [확인], 라이브러리는 [추정] |
| UDD | V8 JavaScript engine을 별도 Script Engine 서비스에서 실행. 사용 후 5분 뒤 정지, 실패 시 cool-off. 공개 profile 예제: GitHub `PTCInc/Universal-Device-Driver-Examples` | [확인] |
| Local Historian | 자체 time-series 파일 포맷: `<datastore id>.name`(태그 메타, 64-bit ID), `<time>.active` → 롤오버 후 `.tsd`(read-only). OPC HDA 1.2(`hdaserver.dll`)로 조회 | [확인] (상용 DB 미사용) |
| DataLogger | ODBC 기반(외부 RDBMS: SQL Server, MySQL, Oracle, Access 등) | [확인] (제품 일반 지식, 매뉴얼 기준) |
| OPC UA 인증서 | self-signed, rsa-sha256, 유효기간 3년. 7.0부터 기본 2048-bit key. 파일 이름 `<product>_ua_server.der`. import는 PKCS#12(.pfx) | [확인] |
| Windows PKI 경로 | `C:\ProgramData\Kepware\KEPServerEX\V6\UA\Server\...`(KSE 문서는 `C:\ProgramData\Kepware\KEPServerEnterprise\V6\UA\Ser...`) | [확인] |
| Edge PKI | `edge_admin manage-truststore --list uaserver` / `--trust=<thumbprint> uaserver` / `-i <cert> licensing`로 관리 | [확인] |
| Kepware Edge 포팅 방식 | 같은 C++ 코드베이스를 Linux로 빌드한 것으로 추정(드라이버·이벤트 메시지·Config API 속성 이름이 Windows와 동일). Qt/Boost 등 cross-platform framework 사용 여부는 공개 자료 없음 | [추정]/[미확인] |
| Edge 플랫폼 | Ubuntu 18.04+ x86-64 전용(TKE), Kepware Edge(2025)는 OCI 컨테이너(Docker/Podman/K8s). ARM 미지원 | [확인] |

### A-4. 서드파티 확장 SDK 여부

| 수단 | 설명 | 등급 |
|---|---|---|
| Universal Device Driver (UDD) | JavaScript profile로 TCP/UDP 기반 커스텀 프로토콜 작성(V8). 사실상 공식 "드라이버 SDK" 역할 | [확인] |
| U-CON (User-Configurable Driver) | GUI 기반 transaction 편집기(`UserConfigDrv_GUI.exe`)로 시리얼/이더넷 ASCII·바이너리 프로토콜 정의 | [확인] |
| Custom Interface Driver (CID) | 사용자 애플리케이션이 shared memory와 XML 정의 파일로 KEPServerEX와 연동. C++/C# 레퍼런스 샘플 제공, Windows API 의존 | [확인] (구형 자료) |
| Plug-In Development Kit (PDK) | 리셀러 블로그(TTPSC)에 ".NET 또는 C++로 네이티브 드라이버 개발" 언급이 있으나 PTC 공식 공개 자료는 찾지 못함 → 일반 공개 SDK는 아닌 것으로 판단 | [미확인] |
| Configuration API SDK | GitHub `PTCInc/Kepware-ConfigAPI-SDK-Python`(pypi `kepconfig`) 등 설정 자동화 SDK(드라이버 개발용 아님) | [확인] |

### A-5. 우리 프로젝트에 주는 시사점 (권장 기술 선택)

1. **코어 런타임, 드라이버, OPC UA server: C++17/20.** Kepware와 같은 선택입니다. Kepware는 UA stack을 자체 구현했지만(C, 취약점 다수), 우리는 검증된 오픈소스 stack을 쓰는 것이 현실적입니다. 후보는 **open62541**(C, MPL-2.0, Linux 친화, PubSub 지원)이며, 상용으로는 Unified Automation C++ SDK, Softing이 있습니다.
2. **드라이버 = 공유 라이브러리 플러그인(`.so`)**과 안정된 C ABI 인터페이스. Kepware의 `Drivers\*.dll` 모델과 같습니다.
3. **격리 서비스 패턴**: 스크립트 엔진(UDD 대응)은 별도 프로세스로 둡니다. MQTT/REST 게이트웨이는 같은 프로세스 안의 C++ 모듈로 두되(Eclipse Paho C/C++ 또는 mosquitto lib), 설계상 분리 가능하게 만듭니다.
4. **설정은 REST Configuration API가 1급 시민**. Kepware Edge처럼 GUI 없이 API와 CLI만으로 모든 설정이 가능해야 합니다. Kepware 속성 체계(`namespace.PROPERTY`, `/config/v1/project/channels/{ch}/devices/{dev}/tags`)를 참고하면 마이그레이션 도구도 만들 수 있습니다.
5. **systemd 서비스 분리**: runtime / configapi / eventlog / (gateway). Kepware Edge 구성을 그대로 참고합니다.

---

## B) Northbound 인터페이스 전체 목록

범례 - Edge: Kepware Edge(Linux) 지원 여부. 우선순위: P0 = 반드시 1차 출시, P1 = 2차, P2 = 선택, X = 불필요(Windows COM 전용 등).

| # | 인터페이스 / 플러그인 | 기능 | 설정 방식 / 핵심 파라미터 | Kepware Edge | 우리 우선순위 |
|---|---|---|---|---|---|
| 1 | **OPC UA Server** | UA client가 태그 browse/read/write/subscribe. Standard UA Server Profile, Core + Data Access Server Facet, UA-TCP UA-SC UA Binary | Project Properties → OPC UA(enable, anonymous, max connections 등), OPC UA Configuration Manager(endpoints, trusted clients, discovery servers, 인증서). Windows 기본 포트 49320, Edge 49330 | O [확인] | **P0** |
| 2 | **IoT Gateway - MQTT Client agent** | 태그를 JSON(템플릿)으로 broker에 publish하고, write topic을 subscribe해 태그 write | Agent(URL, topic, QoS, rate, format, template, security, LWT, subscriptions) + IoT Item(scan rate, deadband 등). 상세는 D절 | O [확인] | **P0** |
| 3 | IoT Gateway - REST Client agent | HTTP POST/PUT으로 외부 REST endpoint에 publish | URL, Method(POST/PUT), Publish Type, Rate, Format, Max events, Timeout, **Buffer on Failed Publish**, Send Initial Update, HTTP Header, Message template, Basic auth | X (Edge 문서에는 MQTT만) [추정] | P1 |
| 4 | IoT Gateway - REST Server agent | 내장 HTTP(S) 서버. `/iotgateway/browse`, `/read?ids=`, POST `/read`, POST `/write` | Network Adapter, Port(기본 39320), CORS Allowed Origins, Use HTTPS(기본 on), Enable write endpoint(기본 off), Allow anonymous login(기본 off). 태그별 Scan Tag/Explicit Read | X [추정] | P1 |
| 5 | ThingWorx Native Interface | ThingWorx AlwaysOn 프로토콜. Store and Forward 지원 | hostname, port, resource `/ThingWorx/WS`, AppKey, Thing name, 인증서 옵션 | O [확인] | X (PTC 전용) |
| 6 | OPC DA 1.0a / 2.05a / 3.0 | Classic OPC(COM/DCOM) | DCOM, Process Mode, OPC DA compliance 옵션 | X (Windows 전용) | X |
| 7 | OPC AE | Alarms & Events(COM). 시스템 이벤트 및 A&E 플러그인 이벤트 | `aeserverinterface.dll` | X | X (UA A&C로 대체 시 P2) |
| 8 | OPC HDA 1.2 | Local Historian 데이터 조회(COM) | `hdaserver.dll` | X | X (UA HA로 대체 시 P2) |
| 9 | OPC .NET (OPC XI) | WCF 기반 | **7.0에서 제거** [확인] | X | X |
| 10 | DDE (CF_Text, XL_Table, AdvanceDDE) | 레거시 Windows DDE | 서비스 이름, interactive 모드 필요 | X | X |
| 11 | FastDDE / SuiteLink | AVEVA(Wonderware) 전용 프로토콜 | Wonderware 라이브러리 필요 | X | X |
| 12 | iFIX Native Interface (NIO) | GE iFIX PDB 직접 연동 | iFIX 설치 필요 | X | X |
| 13 | OPC UA Gateway (UA Gateway plug-in) | 여러 UA server를 aggregation해 하나의 UA server로 재노출(UA client↔server 터널). CSR 지원, 서버/클라이언트 PKI 분리(7.0), namespace 영속화(7.1). 최대 128 인스턴스 | 별도 플러그인 | X [추정] | P2 |
| 14 | Advanced Tags | Derived(수식), Average, Complex, Cumulative, Link(태그→태그 복사), Min/Max 등 서버 측 계산 태그 | `_AdvancedTags` 그룹 | **X (TKE에 없음, 커뮤니티 확인)** | P1 (수식/링크 태그는 가치 큼) |
| 15 | Alarms & Events plug-in | 조건(limit/deviation/digital) 기반 알람 생성 → OPC AE / UA A&C | 알람 영역·소스 정의 | X | P2 |
| 16 | DataLogger | ODBC로 RDBMS에 태그 로깅(wide/narrow 테이블, trigger) | DSN, 테이블 매핑, trigger(시간/조건) | X | P1 (Linux에서는 ODBC 대신 PostgreSQL/InfluxDB/TimescaleDB 직접 연결 권장) |
| 17 | Local Historian | 로컬 time-series 저장(.tsd) + OPC HDA 조회 | datastore 경로, 롤오버 크기/기간, retention | X | P2 (UA HistoryRead와 묶어서) |
| 18 | Industrial Data Forwarder for Splunk | Splunk HEC로 forwarding | Splunk URL/token | X | X (MQTT/REST로 대체 가능) |
| 19 | SNMP Agent | Kepware 태그를 SNMP MIB로 노출(northbound SNMP agent) | community, trap | X | P2 |
| 20 | EFM Exporter | 유량계(EFM: ROC, TotalFlow 등)의 hourly/daily history와 alarm을 CSV/Flowcal 등으로 export | EFM 드라이버와 연동 | X | X (도메인 특화) |
| 21 | Security Policies plug-in | 태그/그룹 단위 접근 권한(User Manager 그룹별 read/write/browse) | Admin → Security Policies | (Edge는 User Manager의 그룹 권한만 확인) | P1 (RBAC) |
| 22 | Media Level Redundancy | 채널/디바이스 단위 primary/secondary 통신 경로 이중화 | 디바이스별 secondary 경로, 전환 조건 | X | P2 |
| 23 | Scheduler | 디바이스 스캔을 시간 스케줄로 제어(배터리 장비 등) | 스케줄 정의 | X | P2 |
| 24 | Store and Forward | 업스트림(ThingWorx 등) 단절 시 디스크 버퍼 | `server_store_and_forward.exe` | (ThingWorx용) | P1 (MQTT 디스크 버퍼로 구현) |
| 25 | Configuration API (REST) | 프로젝트/관리 설정 CRUD, 서비스(ProjectLoad/Save, ForceLicenseCheck), 로그 조회 | HTTPS 기본 활성. Basic auth(User Manager) | O [확인] | **P0** (관리 인터페이스) |
| 26 | Kepware+ / Kepware+ Manager | SaaS 기반 다수 Kepware 인스턴스의 원격 설정·감사 관리 | 클라우드 에이전트 | O (Edge 1.0 호환) | P2 (자체 fleet manager) |
| 27 | MQTT Client **driver** (southbound) | broker를 subscribe해 JSON payload를 태그로 매핑(auto tag 생성, `#Payload` selector). 최대 60 채널, 채널당 1024 디바이스 | 드라이버 채널/디바이스 설정 | Edge 지원 목록에 없음 [추정] | P1 (southbound이지만 같은 MQTT 라이브러리 재사용) |

**Sparkplug B 확인 결과**:
- IoT Gateway MQTT agent는 **JSON(또는 템플릿 텍스트) publish만 지원**하고 Sparkplug B(protobuf) publish는 지원하지 않습니다. PTC 커뮤니티 스태프 답변: "IoT Gateway publishes data to MQTT brokers as JSON payloads, but it does not natively support publishing each tag as an individual topic or in UNS structure", UNS 대응은 "not part of the released Kepware roadmap" **[확인]**. 즉 **agent당 단일 publish topic**이며, 태그별 topic도 없습니다.
- 일부 리셀러 자료는 "MQTT Client driver가 subscribe 시 Sparkplug B를 decode한다"고 설명하지만, PTC 공식 KB(CS399502 "Sparkplug B communication with PTC Kepware Products")는 403으로 본문을 확인하지 못했습니다 **[미확인]**. 업계에서는 Kepware에서 Sparkplug B를 쓰려면 HiveMQ Edge 등 변환 브로커가 필요하다는 것이 일반적 인식입니다.
- **우리 제품 기회**: Sparkplug B publish(NBIRTH/DBIRTH/DDATA, bdSeq, rebirth 명령)와 **태그별/UNS topic 템플릿**은 Kepware에 없는 차별화 요소이므로 P1으로 권장합니다.

---

## C) OPC UA 서버 상세 스펙 (Kepware 기준과 우리 스펙 제안)

### C-1. 프로파일 / 보안

| 항목 | Kepware 값 | 등급 | 우리 스펙 제안 |
|---|---|---|---|
| 지원 버전 표기 | "1.02 optimized binary TCP" | [확인] | 1.04/1.05 기준으로 구현 |
| Fully supported profiles | Standard UA Server Profile, Core Server Facet, Data Access Server Facet, SecurityPolicy Basic128Rsa15(Deprecated), Basic256(Deprecated), Basic256Sha256, None(Insecure), UA-TCP UA-SC UA Binary | [확인] | 같은 범위 + **Aes128_Sha256_RsaOaep, Aes256_Sha256_RsaPss** 추가 권장. Basic128Rsa15/Basic256은 기본 비활성 |
| Partially supported | Base Server Behavior Facet(Security Administrator XML Schema 미지원) | [확인] | - |
| Message security mode | 정책별로 Sign / Sign and Encrypt / Sign;Sign and Encrypt 선택, 기본 Sign and Encrypt | [확인] | 같음 |
| Config API enum | `ENDPOINT_SECURITY_BASIC256_SHA256` 기본 2(=Sign and Encrypt). 0=Disabled, 1=Sign, 2=SignAndEncrypt, 3=Sign;SignAndEncrypt. `ENDPOINT_SECURITY_NONE` 기본 False | [확인] | 같은 의미의 enum 사용 |
| User token | Anonymous(기본 **비허용**), UserName/Password(User Manager 사용자. 비밀번호는 endpoint security policy로 암호화). 사용자 검증은 endpoint 단위가 아닌 **서버 인스턴스 단위** | [확인] | Anonymous, UserName 필수. **X509 user token은 Kepware 문서에 없음**(우리는 P1로 추가 권장) |
| 사용자 권한 | User group 권한: `USERMANAGER_IO_TAG_READ`, `USERMANAGER_IO_TAG_WRITE`, `USERMANAGER_BROWSE_BROWSENAMESPACE` 등(+Security Policies plug-in으로 태그 단위) | [확인] | 그룹 RBAC + 태그 경로 패턴 ACL |
| 인증서 | 모든 endpoint가 하나의 instance certificate 공유. self-signed rsa-sha256, 3년, 2048-bit(7.0+), import는 PKCS#12(.pfx). 재발급 시 기존 신뢰 관계 무효 | [확인] | 같음 + CSR 생성(Kepware UA Gateway는 7.0에서 CSR 지원) |
| Trust store | Trusted Clients 목록(Import/Export/Remove/Reject/Trust). 최초 접속한 client 인증서는 **Rejected 상태로 저장** → 관리자가 thumbprint로 trust | [확인] | OPC UA Part 12 GDS 디렉터리 구조(`pki/own`, `pki/trusted/certs`, `pki/rejected`, `pki/issuers`, CRL) 채택 권장 |
| Discovery | 자체 LDS 미제공. 외부 LDS 등록 가능(등록 주기 설정) | [확인] | 자체 GetEndpoints/FindServers는 필수, LDS 등록은 P2 |
| Endpoint | URL 형식 `opc.tcp://<hostname>:<port>`, network adapter(Default/특정 NIC/Localhost only) 바인딩, 최대 100개 endpoint, endpoint 변경 시 runtime 재초기화 필요. Edge는 **IPv4만** 사용(IPv4 주소 없는 NIC은 무시) | [확인] | 같음 + IPv6 지원 |
| 기본 포트 | Windows **49320**, Edge **49330** | [확인] | 49320(호환) 또는 4840(표준). 설정 가능 |

Config API 속성(Edge, `/config/v1/admin/ua_endpoints/{name}`) **[확인]**:
`common.ALLTYPES_NAME`, `common.ALLTYPES_DESCRIPTION`, `libadminsettings.UACONFIGMANAGER_ENDPOINT_ENABLE`(기본 true), `..._ENDPOINT_ADAPTER`(기본 "Default"), `..._ENDPOINT_PORT`(기본 49330), `..._ENDPOINT_URL`(read-only), `..._ENDPOINT_SECURITY_NONE`(false), `..._ENDPOINT_SECURITY_BASIC256_SHA256`(2), `..._ENDPOINT_SECURITY_BASIC128_RSA15`(0), `..._ENDPOINT_SECURITY_BASIC256`(0). 서버 interface 활성화: PUT `/config/v1/project` `{"uaserverinterface.PROJECT_OPC_UA_ENABLE": true}`.

### C-2. 런타임 설정 (Project Properties → OPC UA) [확인]

| 그룹 | 속성 | 범위 / 기본값 | 설명 |
|---|---|---|---|
| Server Interface | Enable | - | UA server interface 초기화 |
| | Log diagnostics | 기본 off | UA stack 진단 로그(문제 해결용으로만 사용) |
| Client Sessions | Allow anonymous login | 기본 **No** | No이면 Default User로 로그인 불가 |
| | Max. connections | 1–256, 기본 **128** | |
| | Minimum session timeout | 기본 **15 s** | client가 요청한 session timeout의 하한 |
| | Maximum session timeout | 기본 **60 s** | 상한 |
| | Tag cache timeout | 0–60 s, 기본 **5 s** | client가 사용을 마친 태그(미등록 read/write)를 캐시해 두는 시간 |
| Browsing | Return tag properties | 기본 off | 태그의 property(OPC DA item properties)를 browse 노출 |
| | Return address hints | 기본 off | 드라이버 주소 힌트 노드 노출(일부 client 오동작 원인) |
| Monitored Items | Max. Data Queue Size | 1–100, 기본 **2** | monitored item당 큐 크기(sampling이 publish보다 빠를 때) |
| Subscriptions | Max. retransmit queue size | 0–100(0=재전송 비활성), 기본 **10** | Republish용 큐 |
| | Max. notifications per publish | 1–65536, 기본 **65536** | |

**문서에 없는 항목 [미확인]**: 서버 측 최소/최대 sampling interval, 최소/최대 publishing interval, subscription 수 상한, session당 monitored item 상한, max message size / max chunk count, operation limits(MaxNodesPerRead 등). Kepware는 monitored item의 sampling interval을 내부 태그 scan rate로 쓰는 것으로 알려져 있습니다 **[추정]**. 디바이스 scan mode("Respect client-specified scan rate", "Request data no faster than scan rate" 등)에 따라 실제 폴링 주기가 결정됩니다.
→ **우리 스펙 제안**: minSamplingInterval 10 ms(Kepware IoT Gateway 최소 rate와 맞춤), minPublishingInterval 50 ms, maxSubscriptionsPerSession 100, maxMonitoredItemsPerSubscription 100k, maxSessions 128(설정 가능), OperationLimits 노출(Server/ServerCapabilities/OperationLimits). sampling interval → 디바이스 폴링 그룹 매핑 정책은 Kepware의 scan mode 개념을 그대로 차용합니다.

### C-3. Address space 구조

- **NodeId**: `ns=2;s=<Channel>.<Device>[.<TagGroup>...].<Tag>` (string NodeId, 점 구분 full path) **[확인]**(다수 client 예제·포럼). ns=2가 Kepware tag namespace입니다.
- **계층**: Objects → Channel(Folder) → Device(Folder) → TagGroup(Folder) → Tag(Variable). BrowseName/DisplayName은 각 요소 이름입니다.
- **시스템 태그** (`_System` 폴더) **[확인]**:
  - 서버 레벨(`_System._...`): `_ActiveTagCount`, `_ClientCount`, `_Date`, `_DateTime`(UTC), `_DateTimeLocal`, `_Date_Day/_DayOfWeek/_Month/_Year2/_Year4`, `_Time`, `_Time_Hour...`, `_ExpiredFeatures`, `_LicensedFeatures`, `_FullProjectName`, `_ProjectName`, `_ProjectTitle`, `_ProductName`, `_ProductVersion`, `_OpcClientNames`. 7.1에는 연결된 UA client의 ApplicationName/URI를 보여주는 시스템 태그가 추가됨.
  - 채널 레벨(`<Channel>._System._...`): 시리얼은 `_BaudRate`, `_ComId` 등, 이더넷은 `_NetworkAdapter` 등.
  - 디바이스 레벨(`<Channel>.<Device>._System._...`): `_Error`(Bool, RO), `_NoError`, `_Enabled`(RW), `_Simulated`(RW), `_ScanMode`(String RO), `_ScanRateMs`(DWord RO, 기본 1000), `_SecondsInError`, `_FailedConnection`, `_RequestTimeout`(RW 100–30000 ms), `_RequestAttempts`(RW 1–10), `_InterRequestDelay`(RW 0–30000 ms), `_DeviceId`, `_EncapsulationIp/Port/Protocol` 등. **parameter control 태그는 write로 런타임 설정 변경 가능**(통신 이중화 등에 활용).
- **통계 태그** (`<Channel>._Statistics._...`) **[확인]**: `_SuccessfulReads`, `_SuccessfulWrites`, `_FailedReads`, `_FailedWrites`, `_RxBytes`, `_TxBytes`, `_StatisticsReset` 등. 통신 직렬화(`_CommunicationSerialization._VirtualNetwork`, `_NetworkOwner` 등)도 있습니다.
- **Property 태그** (Return tag properties on): 태그별 `_Description`, `_RawDataType`, `_AccessRights`, `_ScanRate`, `_Address` 등 **[추정]**(OPC DA item property 기반, TKE 매뉴얼에 "Property Tags" 절 존재).
- **Dynamic tags**: client가 `Channel.Device.<address>` 형식으로 정의되지 않은 주소를 직접 요청하면 런타임에 태그를 생성합니다(Tag cache timeout 대상) **[확인]**(TKE 매뉴얼 "Dynamic Tags" 절).
- **Server 객체(ns=0)**: 표준 Server/ServerStatus/ServerCapabilities 노드. 상세 지원 범위는 [미확인].

### C-4. 데이터 타입 매핑 (Kepware → UA)

Kepware 데이터 타입 목록 **[확인]**: Boolean, Char, Byte, Short, Word, Long, DWord, LLong, QWord, Float, Double, String, BCD, LBCD, Date와 각 Array 형태.
아래 매핑은 Kepware OPC UA Client driver의 UA→Kepware 표를 역방향으로 적용한 것이고, BCD/LBCD 처리는 PTC KB CS333221 기준입니다 **[확인/역추정]**.

| Kepware | OPC UA DataType | 비고 |
|---|---|---|
| Boolean | Boolean (i=1) | |
| Char | SByte (i=2) | signed 8 |
| Byte | Byte (i=3) | |
| Short | Int16 (i=4) | |
| Word | UInt16 (i=5) | |
| Long | Int32 (i=6) | |
| DWord | UInt32 (i=7) | |
| LLong | Int64 (i=8) | |
| QWord | UInt64 (i=9) | |
| Float | Float (i=10) | non-normalized(NaN/Inf) 처리 옵션 있음(드라이버별 "Non-Normalized Float Handling") |
| Double | Double (i=11) | |
| String | String (i=12) | |
| Date | DateTime (i=13) | `YYYY-MM-DDThh:mm:ss.000` |
| BCD | UInt16 (Word로 노출) | OPC variant에 BCD 타입이 없음 [확인] |
| LBCD | UInt32 (DWord로 노출) | [확인] |
| *Array | 같은 타입 ValueRank=1(2차원 배열 존재 → ValueRank=2, ArrayDimensions) | IoT Gateway에서 2x2 배열 표기 `"[1,2][3,4]"` [확인] |

### C-5. Quality → StatusCode 매핑

Kepware 내부 quality는 OPC DA quality(8-bit QQSSSSLL) 체계입니다 **[확인]**. UA StatusCode 변환표는 Kepware 문서에서 찾지 못했으므로, 아래는 **OPC Foundation COM Interop(DA→UA) 표준 매핑에 따른 권장값**입니다 **[추정]**.

| DA quality (hex) | 의미 | UA StatusCode |
|---|---|---|
| 0xC0 | Good | Good (0x00000000) |
| 0xD8 | Good, Local Override | GoodLocalOverride (0x00960000) |
| 0x00 | Bad (non-specific) | Bad (0x80000000) |
| 0x04 | Bad, Config Error | BadConfigurationError (0x80890000) |
| 0x08 | Bad, Not Connected | BadNotConnected (0x808A0000) |
| 0x0C | Bad, Device Failure | BadDeviceFailure (0x808B0000) |
| 0x10 | Bad, Sensor Failure | BadSensorFailure (0x808C0000) |
| 0x14 | Bad, Last Known Value | UncertainNoCommunicationLastUsableValue (0x408F0000) 또는 BadNoCommunication. 검증 필요 |
| 0x18 | Bad, Comm Failure | BadNoCommunication (0x80310000) / BadCommunicationError (0x80050000) |
| 0x1C | Bad, Out of Service | BadOutOfService (0x808D0000) |
| 0x20 | Bad, Waiting for initial data | BadWaitingForInitialData (0x80320000) |
| 0x40 | Uncertain | Uncertain (0x40000000) |
| 0x44 | Uncertain, Last Usable | UncertainLastUsableValue (0x40900000) |
| 0x50 | Uncertain, Sensor Not Accurate | UncertainSensorNotAccurate (0x40930000) |
| 0x54 | Uncertain, EU Exceeded | UncertainEngineeringUnitsExceeded (0x40940000) |
| 0x58 | Uncertain, Sub-Normal | UncertainSubNormal (0x40950000) |

동작 특성 **[확인/추정]**: 디바이스가 timeout×재시도 끝에 error 상태가 되면 해당 디바이스 태그 전체가 Bad로 바뀌고 `_Error=true`가 됩니다. 이후 auto-demotion 정책에 따라 폴링을 일시 중단합니다(Device Properties: Timing / Auto-Demotion).

### C-6. Write 동작

- 태그 Client Access가 Read/Write일 때만 쓸 수 있고, 사용자 그룹 `IO_TAG_WRITE` 권한(및 Security Policies)이 필요합니다 **[확인]**.
- Write 값은 태그 data type으로 변환해 드라이버 write 큐에 넣습니다. 드라이버가 write 결과를 동기적으로 Write 응답에 반영하는지(디바이스 ACK 후 Good 반환인지), 큐 적재 즉시 Good을 반환하는지는 [미확인]입니다. **우리 스펙**: 디바이스 ACK 후 결과 반환(timeout 시 BadTimeout), 옵션으로 "fire-and-forget"을 제공합니다.
- Scaling(Linear/Square root, raw↔scaled)이 설정된 태그는 역스케일링 후 write합니다 **[확인]**(Tag Properties — Scaling 존재).
- 배열 write는 전체 배열 단위로만 허용합니다(IoT Gateway 문서 기준) **[확인]**.
- VQT write(quality/timestamp 포함)는 OPC DA 3.0 WriteVQ 문맥에서만 언급됩니다. UA에서는 Value만 쓰는 것을 권장합니다.

### C-7. 우리 P0 OPC UA 서버 체크리스트

1. Endpoint: None(옵션, 기본 off), Basic256Sha256 Sign/SignAndEncrypt(기본), Aes128_Sha256_RsaOaep, Aes256_Sha256_RsaPss. 다중 endpoint, NIC 바인딩.
2. User token: Anonymous(기본 off), UserName, X509(P1).
3. PKI: own/trusted/rejected/issuers/CRL. 첫 접속 인증서 rejected 저장 → CLI/REST로 trust(`edge_admin manage-truststore`와 같은 UX).
4. Address space: `ns=2;s=Channel.Device.Group.Tag`(Kepware 호환 NodeId → 마이그레이션 용이), `_System`, `_Statistics` 노드, 선택적 property 노드.
5. Services: Discovery, Session, View(Browse/BrowseNext/TranslateBrowsePaths), Attribute(Read/Write), Subscription/MonitoredItem(DataChange filter: Status/StatusValue/StatusValueTimestamp + Absolute/Percent deadband), RegisterNodes.
6. 설정 파라미터: Kepware C-2 표의 항목 이름과 기본값을 그대로 채택하고, C-2 "미확인" 항목(sampling/publishing 한계, OperationLimits)을 추가합니다.
7. Data type, quality 매핑: C-4, C-5 표 사용.

---

## D) IoT Gateway MQTT Agent 상세 스펙

### D-1. 아키텍처 [확인]

- 서버 플러그인 `IoT_Gateway.dll`(C++)은 agent 설정, 런타임 데이터 수집(일반 client처럼 태그별 scan rate로 server item reference 폴링), 라이선스를 담당합니다.
- Gateway 서비스 `server_iotgateway.exe`(Java)는 endpoint 연결 관리, 버퍼링, 인증·암호화 계층을 담당합니다. 플러그인과 localhost TCP로 통신합니다(Windows 57212, Edge 57213, 원격 접속 불가).
- 각 이벤트는 4요소(ID, value, quality, timestamp)로 구성됩니다. **agent당 메모리 버퍼 100,000 이벤트**를 두며, 가득 차면 **새 데이터를 버리고** 오래된 데이터를 유지합니다. 디스크에는 쓰지 않습니다. Wide format에서는 버퍼가 없습니다.
- 처리량 한계 언급: 지속 유입률이 100,000 tag updates/s를 넘으면 데이터 손실 가능.
- 설정 변경 시 버퍼 이벤트는 새 설정으로 이어지지만, agent를 disable하면 버퍼가 삭제됩니다(Troubleshooting 절에는 "설정 변경 시 손실"로 상반되게 기술).
- 시스템 태그(5초 주기, 아무 값이나 write하면 0으로 reset): `_DroppedEvents`, `_PublishesSent`.

### D-2. Agent 속성 전체

**General**
| 속성 | 값 |
|---|---|
| Name / Description | |
| Type | MQTT Client / REST Client / REST Server |
| Enabled | Yes/No |
| Ignore Quality Changes | Yes이면 값 변화만 전송(quality만 바뀐 경우 무시) |

**MQTT Client Connection**
| 속성 | 값 / 범위 | 비고 |
|---|---|---|
| URL | `tcp://<host>:<port>` 또는 `ssl://<host>:<port>`. 포트 뒤에 `/` 금지 | WebSocket 미지원(문서상) |
| Topic | publish topic(**agent당 1개**, 템플릿 변수 미지원) | 기본 `iotgateway` [추정] |
| QoS | 0 / 1 / 2 | |
| Publish Type | **Interval**(기본) / **On Data Change** | On Data Change 선택 시 Rate와 Format 비활성 |
| Rate (ms) | 10 – 99,999,990 | |
| Format | **Wide Format**(매 publish마다 모든 enabled 태그의 최신값, 버퍼 없음, 실패 시 다음 publish는 최신값) / **Narrow Format**(변경된 태그만, 버퍼링, 태그당 여러 변화 포함 가능) | |
| Max. events per publish | narrow format에서 한 메시지에 담는 이벤트 수 | 기본 1000 [추정] |
| Transaction timeout (s) | 1 – 60 | |
| Send Initial Update | agent 시작 시 각 태그의 초기값 전송 여부 | |
| (Buffer on Failed Publish) | REST Client에만 명시. MQTT는 narrow에서 항상 버퍼 | [확인] |

**Message**
| 속성 | 값 |
|---|---|
| Message Format | Standard Template / Advanced Template |
| Template(Standard) | VALUES 앞뒤 텍스트. 변수: `\|SERVERTIMESTAMP\|`(epoch ms), `\|SERVERISO8601UTC\|`, `\|SERVERISO8601LOCAL\|`, `\|SERVERDATE\|`(사람용), `\|SERVERNAME\|`(호스트명, 7.1 추가), `\|VALUES\|` |
| Expansion of \|VALUES\| | 태그 이벤트당 반복되는 JSON 조각. 변수: `\|CHANNEL\|`, `\|DEVICE\|`, `\|TAGNAME\|`(Channel.Device.Tag), `\|TAGVALUE\|`, `\|TAGQUALITY\|`, `\|TAGTIMESTAMP\|`, `\|TAG\|`(짧은 이름), `\|TAGADDRESS\|`(1), `\|TAGDESCRIPTION\|`(1,2), `\|TAGCLIENTACCESS\|`(1), `\|TAGISO8601UTC\|`, `\|TAGISO8601LOCAL\|` |

(1) agent 시작(enable/disable 주기) 때만 갱신됨. (2) Advanced Tags 태그에는 동작하지 않음.

**Standard template 기본 출력** [확인]:
```json
{"timestamp":1438011255230,
 "values":[
   {"id":"Channel1.Device1.Tag1","v":250,"q":true,"t":1438011254668}
 ]}
```
- `timestamp`: publish 시각(epoch ms), `id`: 태그 full name, `v`: 값, `q`: true=good / false=bad(**boolean으로 축약되어 세부 quality 정보 손실**), `t`: 샘플 시각(epoch ms)
- 배열 값은 문자열로 표현: `"v":"[1,2][3,4]"`
- 문자열 배열은 read만 가능

**Advanced Template** [확인]: Handlebars 부분집합으로 JSON, XML, CSV 등 임의 형식을 만들 수 있습니다.
- 최상위 변수: `|SERVERNAME|`, `|SERVERTIMESTAMP|`, `|SERVERDATE|`, `|SERVERISO8601UTC|`, `|SERVERISO8601LOCAL|`
- `|#each VALUES| ... |/each|` 블록 안 변수: `|CHANNEL|`, `|DEVICE|`, `|TAGNAME|`, `|VALUE|`, `|QUALITY|`("true"/"false"), `|TIMESTAMP|`(epoch ms UTC), `|TAG|`, `|ADDRESS|`, `|DESCRIPTION|`, `|CLIENTACCESS|`, `|ISO8601UTC|`, `|ISO8601LOCAL|`, `|LINEFEED|`(\n 강제)
- 보조 구문: `|#unless @last|,|/unless|`(마지막 요소 뒤 콤마 제거)
- 예: wide JSON
  ```
  {
  |#each VALUES|
    "|TAGNAME|_value": |VALUE|,
    "|TAGNAME|_quality": |QUALITY|,
    "|TAGNAME|_timestamp": |TIMESTAMP||#unless @last|,|/unless|
  |/each|
  }
  ```
- 템플릿 오류가 나면 Event Log에 줄 번호와 원인을 기록하고, 올바른 템플릿이 들어올 때까지 **publish를 중지**합니다.

**Security (MQTT)** [확인]
| 속성 | 값 |
|---|---|
| Client ID | 고유 ID(같은 Client ID로 여러 agent가 접속하면 끊김 발생) |
| Username / Password | 평문 TCP일 때 평문 전송 |
| TLS Version | Default(JVM 의존) / v1.2 / v1.3. **7.1에서 TLS 1.0/1.1 제거, 1.3 추가** |
| Client Certificate | Enable 시 상호 인증(mTLS). 인증서는 Administration → IoT Gateway에서 **모든 MQTT agent 공통 1개**를 import(PFX/DER/CER/CRT/PEM + private key) |
| 서버 인증서 검증 | Windows 컴퓨터 레벨 Trusted Root 저장소 사용(self-signed broker는 `certutil -addstore "Root"`). Edge는 "Configuring Self-Signed Certificates for MQTT Agent" 절 참조(JVM truststore로 추정) |

**Last Will** [확인]
| 속성 | 값 |
|---|---|
| Enable Last Will and Testament | 기본 No |
| Topic / Message | 예: "offline" |
| 추가 동작 | LWT 활성화 시 첫 publish 성공 때 LWT topic에 `"Server is online."`, 정상 종료(프로젝트 수정, 재초기화, 셧다운) 때 `"Server is shutting down."`을 publish |

**Subscriptions (write)** [확인]
| 속성 | 값 |
|---|---|
| Listen for Write Requests | 기본 No |
| Topic | 기본 `iotgateway/write` |
| QoS | 고정 0 |
| Payload | `[{"id":"Channel1.Device1.Tag1","v":42},{"id":"Channel1.Device1.Tag2","v":523}]` 또는 단일 `{"id":"...","v":42}` |
| 제약 | **해당 agent에 추가된 태그만** write 가능. **User Manager/Security Policies 권한을 검사하지 않음**(broker ACL에 의존). 실패는 Event Log에만 기록(응답 topic 없음) |
| 오류 메시지 | "failed to process write request on topic <t>. Reason - <JSON error>", "Write rejected ... invalid write format", "unsupported data type", "cannot be converted to the tag data type" |

### D-3. IoT Item(태그) 속성 [확인]

| 속성 | 값 |
|---|---|
| Server Tag | `Channel.Device.Tag` full path(browse 가능) |
| Scan Rate (ms) | 10 – 99,999,990, 기본 1000 |
| Only on Data Changes(Publish on data change) | 값 변화 시에만 publish |
| Deadband (%) | **데이터 타입 전체 범위 대비 %**: `Deadband(%) = 100 × (Threshold / DataTypeFullRange)`. 예: Float에서 0.1 → 1.47e-38%. **마지막 publish 값과 비교**(마지막 샘플 값과 비교하지 않음). 0이면 모든 변화 전송 |
| Every Scan | 변화 없어도 good scan마다 전송(deadband 무시). Bad quality는 1회만 전송하고 good 복귀까지 전송 안 함 |
| Enabled | 비활성 태그도 라이선스 카운트에 포함 |
| 기타 | 같은 태그를 여러 agent에 넣으면 agent마다 따로 카운트됨. CSV import/export 지원(같은 이름은 교체) |

**Config API** (Edge) [확인]:
- Agent: POST `/config/v1/project/_iot_gateway/mqtt_clients` `{"common.ALLTYPES_NAME":"NewMqttClient","iot_gateway.AGENTTYPES_TYPE":"MQTT Client","iot_gateway.AGENTTYPES_ENABLED":true}`
- Item: POST `/config/v1/project/_iot_gateway/mqtt_clients/{agent}/iot_items` `{"common.ALLTYPES_NAME":"Simulator_Word1","iot_gateway.IOT_ITEM_SERVER_TAG":"Simulator.SimulatorDevice.Registers.Word1","iot_gateway.IOT_ITEM_ENABLED":true}`
- 그 밖의 속성 키(`iot_gateway.MQTT_CLIENT_URL`, `iot_gateway.MQTT_CLIENT_TOPIC`, ...)는 패턴상 존재하는 것으로 추정되며, 정확한 전체 목록은 `/config/v1/doc`에서 확인해야 합니다 **[추정]**.
- 사용 이유: Edge에서 IoT Gateway를 쓰려면 Java JRE 설치가 필요하고(OpenJDK/Corretto 검증), MQTT agent는 **기본 비활성**입니다.

### D-4. Kepware MQTT의 한계와 우리 개선 포인트

| Kepware 한계 | 우리 스펙 제안(P0/P1) |
|---|---|
| agent당 publish topic 1개, 태그별 topic 없음, UNS 미지원 | **P0**: topic 템플릿(`{channel}/{device}/{tag}` 등)과 agent 단일 topic 모드 둘 다 지원 |
| MQTT 3.1.1만 지원 | **P0**: 3.1.1 + **5.0**(user properties, response topic으로 write 응답) |
| 메모리 버퍼만(100k), 디스크 store-and-forward 없음 | **P0**: 메모리 버퍼 + **디스크 버퍼(옵션)**, 넘칠 때 정책(drop-new/drop-old) 선택 |
| quality가 boolean(q) | 표준 템플릿 호환(`q`: bool)을 유지하고, 선택 필드로 UA StatusCode(`qc`)를 추가 |
| write 요청 권한 검사 없음, QoS 0 고정, 응답 없음 | **P0**: write topic QoS 선택, agent 태그 화이트리스트 + RBAC 매핑, 결과를 응답 topic에 publish |
| Deadband가 %(타입 전체 범위 기준)로 비직관적 | Kepware 호환 % 모드 + **absolute deadband** 모드 |
| Sparkplug B 미지원 | **P1**: Sparkplug B 3.0 Edge Node(NBIRTH/NDEATH/DBIRTH/DDATA/DCMD, bdSeq, rebirth) |
| Java 의존 | C++ 내장(Paho C/C++ 또는 mosquitto client), JVM 불필요 |
| WebSocket 미지원 | P1: `ws://`, `wss://` |

**P0 MQTT agent 구현 범위(호환 기준)**: D-2와 D-3의 모든 속성(이름과 기본값 동일), Standard template JSON 형식 그대로, Advanced template(Handlebars 부분집합: each/unless/@last + 위 변수), LWT와 online/shutdown 메시지, write subscription JSON 형식, `_DroppedEvents`/`_PublishesSent` 시스템 태그.

---

## E) 미확인 사항

1. Kepware OPC UA stack의 정확한 출처: 완전 자체 구현인지, OPC Foundation ANSI C stack을 변형한 것인지. Team82는 "Unknown/Proprietary, C"로만 분류했습니다.
2. Windows Config GUI(`server_config.exe`)의 UI 프레임워크(MFC 여부)와 C# 컴포넌트의 정확한 위치.
3. Kepware Edge의 cross-platform 기반(Qt, Boost, 자체 OS abstraction 등)과 사용 컴파일러/toolchain. 서드파티 라이선스 고지 문서를 확보하지 못했습니다.
4. OPC UA 서버의 sampling/publishing interval 한계, subscription/monitored item 상한, OperationLimits, max message size.
5. Kepware 내부 quality → UA StatusCode 공식 매핑표(C-5는 표준 COM interop 기준 추정).
6. UA Write의 동기/비동기 의미(디바이스 ACK 대기 여부).
7. UA PubSub 지원 여부: Kepware 문서 어디에도 언급이 없으므로 미지원으로 추정합니다.
8. MQTT agent의 Config API 전체 속성 키 목록과 기본 Topic/Max events 기본값(`iotgateway`, 1000은 추정).
9. Sparkplug B: PTC KB CS399502 본문(403으로 접근 불가). MQTT Client driver의 Sparkplug B decode 지원 여부가 불명확합니다.
10. "Plug-In Development Kit(PDK)"의 실재 여부, 제공 조건(파트너/OEM 한정인지).
11. Windows Configuration API 기본 포트(57412/57512)의 1차 출처 확인.
12. Kepware Edge(2025, 1.x)에서 REST Client/Server agent와 Advanced Tags 지원 여부(1.0 릴리스 노트에는 MQTT Client agent만 있음).

---

## F) 출처

채용 / 회사
- PTC Associate Software Engineer (Portland, ME, Kepware) - https://talents.vaia.com/companies/ptc-inc/associate-software-engineer-portland-me-usa-software-engineering-posted-a-day-ago-52201643/
- Maine JobLink 같은 공고 - https://joblink.maine.gov/jobs/1272382

공식 문서(PTC / Kepware)
- IoT Gateway Manual (©2026, Help v1.094) - https://ptc-p-001.sitecorecontenthub.cloud/api/public/content/59f8a382116e44c3a5d66555bc3f27c9?v=0dfaffcd
- IoT Gateway Agent Properties — Subscriptions - https://support.ptc.com/help/kepware/features/en/kepware/features/IOTGATEWAY/mqtt_subscriptions.html
- IoT Gateway Agent Properties — MQTT Client Connection - https://support.ptc.com/help/kepware/features/en/kepware/features/IOTGATEWAY/configure_mqtt.html
- Kepware Edge IoT Gateway — MQTT - https://support.ptc.com/help/kepware/kepware_edge/en/kepware/kepware-edge/edge-iot-mqtt.html
- OPC UA Interface - https://support.ptc.com/help/kepware/kepware_server/en/kepware/server/interface-opc-ua.html
- Project Properties — OPC UA - https://support.ptc.com/help/kepware/kepware_server/en/kepware/server/project-properties-opc-ua.html
- OPC UA Configuration Manager Manual (©2025) - https://downloads.softwaretoolbox.com/demodnld/prod_docs/topserver_help_pdf/Common/opc-ua-configuration-manager-manual.pdf
- ThingWorx Kepware Edge Quick Start Guide (©2023) - https://ptc-p-001.sitecorecontenthub.cloud/api/public/content/8988e1b930354af2ac375aa71337b6d8?v=4ae11512
- ThingWorx Kepware Edge User Manual (©2019) - https://www.opcturkey.com/uploads/thingworx-kepware-edge-user-manual.pdf
- Kepware Edge 1.0.236.0 What's New - https://support.ptc.com/help/kepware/kepware_edge/en/kepware/kepware-edge/whats_new/whats_new_1_0_236_0.html
- Kepware Server 7.0.236.0 What's New - https://support.ptc.com/help/kepware/kepware_server/en/kepware/server/whats_new/whats_new_7_0_236_0.html
- Kepware Server 7.1.271.0 What's New - https://support.ptc.com/help/kepware/kepware_server/en/kepware/server/whats_new/whats_new_7_1_271_0.html
- OPC UA Client Driver — OPC UA Data Types - https://support.ptc.com/help/kepware/drivers/en/kepware/drivers/OPCUACLIENT/Structured_Types.html
- PTC KB CS333221 (BCD를 Word로 표시) - https://www.ptc.com/en/support/article/CS333221
- PTC KB CS399502 (Sparkplug B, 본문 미확인) - https://www.ptc.com/en/support/article/CS399502
- Local Historian General Operation - https://support.ptc.com/help/kepware/features/en/kepware/features/localhistorian/GeneralOperation.html
- Universal Device Driver Manual (Script Engine / V8) - https://community.ptc.com/sejnu66972/attachments/sejnu66972/kepware/1173/1/Universal%20Device%20Driver.pdf
- UDD Profile Examples (GitHub PTCInc) - https://github.com/PTCInc/Universal-Device-Driver-Examples
- Kepware ConfigAPI SDK Python (GitHub PTCInc) - https://github.com/PTCInc/Kepware-ConfigAPI-SDK-Python
- MQTT Client Driver Setup - https://support.ptc.com/help/kepware/drivers/en/kepware/drivers/MQTT/Setup.html
- Rockwell KEPServer Enterprise User Manual (KEPServerEX OEM, 2023) - https://literature.rockwellautomation.com/idc/groups/literature/documents/um/kse-um001_-en-e.pdf
- Custom Interface Driver Help (2011) - https://cdn.logic-control.com/docs/kepware/Manuals/Drivers/Custom%20Interface/Custom%20Interface%20Driver.pdf

보안 연구 / 기타
- Claroty Team82 OPC UA Deep Dive Part 5 (Kepware = Proprietary, C) - https://claroty.com/team82/research/opc-ua-deep-dive-series-part-5-inside-team82-s-research-methodology
- Claroty Team82 Critical Flaws in OPC Protocol - https://claroty.com/team82/research/critical-flaws-in-opc-protocol
- CISA ICSA-20-352-02 - https://www.cisa.gov/news-events/ics-advisories/icsa-20-352-02
- KEPServerEX 6 설치 파일 목록 - https://www.advanceduninstaller.com/KEPServerEX-6-24c7f83054d7db7600873915da7bc251-application.htm
- PTC Community: Kepware IoT Gateway and UNS - https://community.ptc.com/kepware-396/kepware-iot-gateway-and-uns-164143
- PTC Community: Advanced Tags missing in TKE - https://community.ptc.com/kepware-396/advance-tags-are-missing-in-thingworx-kepware-edge-124409
- Software Toolbox Kepware Edge - https://softwaretoolbox.com/kepware-edge/containerized-industrial-connectivity-for-linux
- TTPSC Kepware drivers list (PDK 언급, 리셀러) - https://ttpsc.com/en/blog/kepware-drivers-list/
- Unified Automation ANSI C SDK - https://www.unified-automation.com/products/server-sdk/ansi-c-ua-server-sdk.html
