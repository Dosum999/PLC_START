# Kepware급 Linux 산업용 연결 서버 — 레퍼런스 조사

조사일: 2026-10-06 · 범위: 상용 제품 / 오픈소스 게이트웨이 / PLC 프로토콜·라이브러리
`[미확인]` 표시는 1차 출처로 직접 확인하지 못한 항목이다.

---

## 0. 한 페이지 요약

**시장 공백은 실제로 존재한다.**
- PTC는 2025-12에 KEPServerEX를 **Kepware Server 7.x**로 통합했다. 하지만 Linux판 **Kepware Edge**에는 큰 제약이 있다.
  - 컨테이너로만 배포되고 x86-64만 지원하며 구독으로만 판매한다.
  - 드라이버가 **6종**뿐이다: AB CLX, Modbus TCP, Siemens TCP/IP, S7 Plus, Mitsubishi Ethernet, OPC UA Client.
  - 시리얼과 ARM을 지원하지 않는다.
- Linux 네이티브 경쟁 제품도 저마다 약점이 있다.
  - Litmus Edge: 고가 구독 모델.
  - EMQX Neuron: 오픈소스 버전에는 Modbus만 있고, 나머지 드라이버는 상용이다.
  - Softing: 컨테이너 하나에 PLC 20대까지만 붙는다.
  - Takebishi: 일본 PLC 위주이고 LS 지원은 확인되지 않았다.
- **국산 범용 연결 서버는 없다.** 국내 현장은 Kepware로 LS PLC를 수집하는 경우가 흔하다.

**우리 차별점 후보**
- Linux 네이티브 + ARM64
- 시리얼 지원 (Modbus RTU, LS Cnet)
- **LS XGT / Mitsubishi / Omron 등 아시아 PLC 드라이버**
- Kepware 호환 모델과 Config API (기존 SI 인력의 학습 비용 0)
- 영구 라이선스 옵션

**권장 기술 방향 (초안)**
- 코어 언어: **Rust**. MVP 속도를 우선하면 Go.
- 드라이버: 대부분 자체 구현한다. 프로토콜이 공개되어 있거나 문서화가 잘 되어 있고, 오픈소스 라이브러리는 어차피 얇다.
- Rockwell 드라이버는 **libplctag**(MPL-2.0)를 FFI로 붙인다.
- Northbound OPC UA 서버는 **open62541**(MPL-2.0)을 FFI로 붙인다.

---

## 1. 상용 제품

### 1.1 PTC Kepware — 모방 대상

| 항목 | Kepware Server 7.x (Windows) | Kepware Edge 1.0 (Linux, 2025-11) |
|---|---|---|
| 배포 | Windows 서비스 + Config UI | OCI 컨테이너(Docker/Podman/K8s), x86-64, RHEL 검증 |
| 드라이버 | 150개 이상 | 6종 |
| 플러그인 | IoT Gateway, Advanced Tags, DataLogger, Local Historian, A&E, Security Policies, MLR 등 | 대부분 없음 [부분 미확인] |
| Northbound | OPC UA/DA, IoT Gateway(MQTT/REST), ThingWorx | OPC UA, MQTT agent |
| 라이선스 | 영구(드라이버별 약 $400~1,500, S&M 20%/년) 또는 구독(Suite당 €600~900/년) | 구독만 가능, 태그·디바이스 수 무제한 |

**Kepware 핵심 개념** (설계에 그대로 가져올 것)

- **계층**: `Project → Channel → Device → TagGroup(중첩) → Tag`. 부가 객체로 Alias가 있다.
  - **Channel**: 드라이버 인스턴스 1개와 매체(NIC 또는 COM) 1개를 묶은 단위다. **채널 하나가 스레드 하나**이고, 채널 안의 디바이스는 순차로 폴링한다.
    - Write Optimization: 기본값은 Latest Value Only이고, Duty Cycle 10은 쓰기 10건마다 읽기 1회를 보장한다.
    - 그 밖의 설정: Inter-Request Delay, Non-normalized Float 처리, 통신 프레임 진단 캡처.
  - **Device**: 모델, ID, Simulated 모드.
    - Timing: Connect 3 s, Request 1000 ms, Attempts.
    - **Auto-Demotion**: 타임아웃 3회가 나면 10 s 동안 off-scan으로 내려서 같은 채널의 다른 디바이스를 보호한다.
    - 그 밖의 설정: 자동 Tag Generation, 블록 크기, 워드 순서, 0/1 base 주소.
  - **Tag**: Address(드라이버별 문법), DataType, Client Access, Scan Rate, Scaling.
    - Static 태그와 Dynamic 태그가 있다. Dynamic 태그는 클라이언트가 `Channel.Device.<주소>`를 직접 지정한다.
    - `._Address`, `._ScalingRawLow` 같은 Property 태그가 붙는다.
  - **Data types**: Boolean, Char, Byte, Short, Word, Long, DWord, LLong, QWord, Float, Double, BCD, LBCD, Date, String과 각각의 1D/2D 배열.
- **Scan Mode** (디바이스 단위): Respect Client Rate(기본), No Faster Than, All Data at Rate, Demand Poll Only, Respect Tag-Specified. 여기에 Initial Updates from Cache 옵션이 있다.
  → **구독된 태그만 폴링한다.** 주기별로 묶은 뒤 연속 주소를 블록 읽기로 합치는 것이 성능의 핵심이다.
- **Scaling**: Linear / Square Root. Clamp, Negate, Units를 지원하고, 쓰기 시에는 역변환한다.
- **진단**
  - `_System` 태그: `_Error`, `_Enabled`, `_ScanRateMs` 등. 일부는 쓰기가 가능해 런타임 제어에 쓴다.
  - `_Statistics` 태그: 성공/실패 읽기·쓰기 수, Rx/Tx 바이트.
  - 로그: Event Log, Comm Diagnostics(프레임 뷰어), Audit Log.
  - 모든 값은 VQT(Value, Quality, Timestamp)로 다룬다.
- **Configuration API (REST)**: 포트 57412(HTTP)와 57512(HTTPS).
  - 경로: `/config/v1/project/channels/{ch}/devices/{d}/tag_groups/.../tags/{t}`
  - **`/config/v1/doc/drivers/{driver}`**: 드라이버별 속성 스키마를 제공하는 self-describing API다. UI가 이 스키마로 폼을 동적으로 만든다. **반드시 모방할 것.**
  - 속성 키는 평면 JSON이고 네임스페이스 접두사가 붙는다. 예: `servermain.TAG_ADDRESS`, `modbus_tcpip_ethernet.*`
  - 낙관적 동시성 제어: 요청에 `PROJECT_ID`를 실어 보낸다.
  - 비동기 서비스(TagGeneration, ProjectLoad 등)는 202 Accepted를 돌려주고 job URL을 폴링한다.
  - 공식 SDK: Python `kepconfig`, .NET.
- **IoT Gateway**
  - Agent 종류: MQTT Client, REST Client, REST Server.
  - IoT Item마다 주기와 데드밴드를 설정한다.
  - 페이로드 기본형은 `{id,v,q,t}` JSON이고, 템플릿으로 사용자 정의 형식을 만든다.
  - Java 기반 별도 프로세스로 돈다.
- **이중화**
  - MLR(Media-Level Redundancy): 통신 경로 또는 디바이스 쌍 이중화.
  - RedundancyMaster: OPC Classic 서버 이중화.
- 기본 OPC UA 포트는 49320 [미확인].

### 1.2 경쟁 제품 비교

| 제품 | 배포 | 드라이버 | 아키텍처 포인트 | 라이선스 |
|---|---|---|---|---|
| **Ignition 8.3** | Java, Win/Linux/Docker | AB, S7, Modbus, Omron, MELSEC, DNP3, BACnet, 61850 등 | Device 연결과 Tag Provider(UDT)를 분리. **8.3부터 설정이 파일 기반 JSON(Git 관리 가능)이고 OpenAPI REST를 제공.** 드라이버는 Java Module SDK로 작성 | 영구 라이선스. Platform $1,200, 드라이버 $275~1,425 |
| **Litmus Edge** | Linux 어플라이언스, VM, Docker | 250개 이상 | DeviceHub(Driver→Device→Tag), 내부 브로커, Flows, 컨테이너 앱 | 구독, 고가 |
| **EMQX Neuron** | Linux 네이티브, ARM, Docker | 70개 이상(상용) | C, 동적 `.so` 플러그인, NNG 버스, SQLite | 30태그까지 무료, 이후 상용 |
| **Cybus Connectware** | Docker, K8s | ~20개 프로토콜 | **YAML 커미셔닝 파일(GitOps)**, 내부 MQTT | 구독 |
| **Softing edgeConnector** | Docker(amd64/arm) | 제품을 드라이버별로 분리(S7, CLX, Modbus, FOCAS 등) | 컨테이너당 PLC 20대, REST | 영구 라이선스 + 업그레이드 |
| **Siemens Industrial Edge** | Edge 디바이스, Docker 앱 | S7, S7+, OPC UA, Modbus, EIP 커넥터 | **커넥터마다 별도 앱 + 공통 MQTT Databus + 공통 설정기** | 앱별 라이선스 |
| **Takebishi DeviceGateway** | Win/Linux/Docker | 80개 이상 벤더, 320종 이상 | 일본 PLC와 CNC에 강함. 연결 255개 | 비공개 |
| **Matrikon MDB** | Win + Linux 컨테이너 | 어댑터 소수 | Data Broker 코어 + 어댑터 | 비공개 |
| **Cogent DataHub** | 주로 Windows | OPC/Modbus/MQTT | 미러링, 터널링, 데이터 다이오드 | 인스턴스 단위 |
| **HMS Red Lion Crimson** | 하드웨어 | 300개 이상 | 동시 20개 프로토콜 변환 | 하드웨어 판매 |
| **Unified Automation / Prosys** | SDK | — | 상용 OPC UA SDK, 런타임 로열티 없음 | 개발자 라이선스 |

### 1.3 한국 생태계
- **LS Electric XGL-EOPCT**: PLC 측 OPC UA 서버 모듈이다. XGI-CPUUN 또는 EFMTB v7 이상이 필요하다. 설치 기반의 대부분(XGK, XGB)에는 OPC UA가 없다.
- 국내 OPC 소프트웨어: 다빈소프트 OPC Server(LS, Cimon, MELSEC, CNC), LSE OPC Server(opchub), 팡데이터, 코펜스 PlantPulse, 유비씨 OCTOPUS Hub. 모두 소규모이고 Windows 중심으로 추정한다 [미확인].
- **Cimon** 등 국산 PLC 드라이버는 해외 제품에 거의 없으므로 차별화 포인트가 된다.

---

## 2. 오픈소스 레퍼런스

| 프로젝트 | 언어 / 라이선스 | 오픈 드라이버 | 격리 방식 | 설정 저장 | 배울 점 |
|---|---|---|---|---|---|
| **EMQ Neuron** | C / LGPL-3 | Modbus뿐(나머지 상용) | 단일 프로세스, 노드별 스레드, `.so` 핫플러그 | SQLite + 마이그레이션, REST/MQTT API | **구조가 가장 가까움.** Northbound 노드가 driver group을 구독하는 모델 |
| **EdgeX 4.0** | Go, C / Apache-2 | Modbus, OPC UA, S7, BACnet… | **프로토콜당 프로세스** | PostgreSQL | **Device Profile(템플릿)과 Device 분리.** SDK 4개 메서드 |
| **UMH Core + benthos-umh** | Go / Apache-2 | OPC UA, S7, Modbus, ADS, EIP | **파이프라인당 프로세스(S6 supervisor)** | 단일 YAML을 에이전트가 reconcile | 선언형 설정, 상태 머신, ISA-95 UNS |
| **Apache PLC4X 1.0** (2026-09) | Java, Go / Apache-2 | Go 14종, Java 20종 이상 | 라이브러리 | — | 주소 문자열 문법, MSpec 코드 생성. **LS, FINS, Keyence는 없음** |
| **IoTGateway** | C# .NET 10 / MIT | S7, MC, Modbus, Omron, AB, OPC | 인프로세스 DLL | SQLite(EF Core) | 변수별 표현식, RPC 쓰기 |
| **ThingsBoard GW** | Python / Apache-2 | 약 18개 | 스레드, 선택적 gRPC 외부 프로세스 | JSON, 원격 push | 커넥터와 컨버터 분리 |
| **OpenMUC** | Java OSGi / **GPL-3** | 에너지 계열 | OSGi 번들 | XML | 깔끔한 Driver SPI(scan/browse 포함) |
| **Shifu** | Go / Apache-2 | 소수 | **디바이스당 Pod** | K8s CRD | 너무 무거움 |
| **Telegraf** | Go / MIT | modbus, opcua, s7comm | 컴파일 내장 + **execd(stdin/stdout)** | TOML | 값싼 외부 프로세스 플러그인 패턴 |
| **FUXA / Rapid SCADA / Node-RED** | JS, C# | 다수 | 인프로세스 | — | HMI 쪽 참고용. Node-RED 노드 중 일부는 GPL |

**OPC UA 스택** (northbound 서버 후보)

| 스택 | 언어 / 라이선스 | 서버 성숙도 |
|---|---|---|
| **open62541** v1.5 | C / MPL-2.0 | ◎ 최선. 런타임 노드 추가·삭제, PubSub 지원 |
| Eclipse Milo 1.x | Java / EPL-2.0 | ◎ |
| node-opcua | TS / MIT | ○ |
| gopcua v0.8 | Go / MIT | 클라이언트 ◎, **서버 실험적** |
| async-opcua 0.19 | Rust / MPL-2.0 | △ ("production-ready Rust 서버 없음", basysKom 2025) |
| asyncua | Python / LGPL-3 | 테스트·시뮬레이터용 |

**Sparkplug B**: Eclipse Tahu(EPL-2.0)의 protobuf와 레퍼런스 구현을 쓴다.

---

## 3. PLC 프로토콜과 드라이버

| 벤더 | 프로토콜 | 스펙 | 오픈소스 | PLC 측 설정·주의점 |
|---|---|---|---|---|
| **LS Electric** | **XGT FEnet**(TCP 2004 / UDP 2005, "LSIS-XGT" 헤더, 개별·연속 읽기, 약 1,400 B), **Cnet**(시리얼 ASCII), Modbus, (신형) OPC UA | **공개**(매뉴얼) | 빈약함: VagabondK.Protocols(C#, LGPL), PyXGT(라이선스 없음), golanglsplc. **PLC4X에 없음** | XGK(P/M/D)와 XGI(%MW 등)는 **주소 문법이 다름.** FEnet 전용 서비스 연결 수, 호스트 테이블 |
| **Siemens** | **S7comm**(TCP 102, PDU 240/480/960), S7comm-plus(TLS, 비공개), OPC UA(1500, 1200 FW4.4 이상, **CPU별 유료 라이선스**) | 비공개지만 S7comm은 사실상 표준 | snap7, python-snap7(순수 Python), gos7, S7.Net, PLC4X. S7CommPlusDriver는 "운영 사용 불가" | **PUT/GET 허용 + Optimized block access 해제** 필요. 고객이 거부하는 경우가 많음 |
| **Rockwell** | EtherNet/IP CIP(44818, 심볼릭, Multiple Service Packet, Large Forward Open 4002 B), PCCC(SLC, ML) | ODVA 회원 전용이지만 클라이언트 구현은 관행적으로 허용 | **libplctag**(C, MPL/LGPL, 2012년부터 운영), pycomm3, pylogix, gologix | 연결 수 제한, UDT·배열은 통째로 읽기, path `1,0` |
| **Mitsubishi** | **MC/SLMP 3E/4E**(바이너리·ASCII), FX3는 1E만 | **공개** | pymcprotocol, gomcprotocol, plc-comm-slmp-rust, PLC4X SLMP | GX Works에서 포트 오픈, 바이너리·ASCII 일치, RUN 중 쓰기 허용 |
| **Omron** | FINS UDP 9600/TCP, NJ/NX는 CIP 심볼릭과 OPC UA | 공개 | libfins(MIT), omron-fins(Rust), gofins | 노드 번호 중복 금지. NJ/NX는 Network Publish 필요 |
| **Beckhoff** | ADS/AMS(TCP 48898) | 공개 | **Beckhoff/ADS(공식 C++, MIT)**, pyads | AMS route 등록, Sum 명령, Notification |
| **Keyence** | KV Host Link(TCP 8501, ASCII), MC 호환 | 공개 | plc-comm-kv-hostlink | 느리므로 RDS 연속 읽기 |
| **Panasonic** | MEWTOCOL-COM / 7 | 공개 | mewtocol-go 등(소규모) | — |
| **Schneider** | Modbus, UMAS(비공개) | — | PLC4X UMAS | Modbus 우선 |
| **Delta / Inovance** | Modbus 기반 | 공개 | Modbus 라이브러리 + **주소 별칭 템플릿** | D0=0x1000 같은 매핑 |
| **Yokogawa FA-M3 / Fuji** | PC Link / SX(비공개) | 일부 공개 | 거의 없음 | 후순위 |
| **공통 Modbus** | TCP/RTU/ASCII | 공개 | libmodbus, tokio-modbus, grid-x/modbus, pymodbus | 125 레지스터 제한, **워드·바이트 순서 4종**, 0/1 base, 연결 1~4개 제한 |

**범용 접근 3가지의 평가**
1. **Modbus 우선**: 필수지만 충분하지 않다. 심볼이 없어서 PLC 측에 레지스터 맵 작업이 필요하다.
2. **PLC 내장 OPC UA**: 깔끔하고 법적으로도 안전하다. 다만 PLC마다 비용이 들고 설치 기반의 대부분은 미지원이다. → **OPC UA 클라이언트 드라이버를 초기에 만든다.** S7-1500 최적화 DB 접근은 이것으로 해결한다.
3. **PLC4X를 공통 계층으로 쓰기**: 참고하거나 선택적 백엔드로 쓰기에 좋다. 아시아 벤더는 결국 자체 드라이버가 필요하다.

**법적 메모**: S7comm-plus와 UMAS는 인증·무결성 메커니즘을 리버스 엔지니어링한 것이다. 저작권법 상호운용성 조항이나 기술적 보호조치 이슈가 걸릴 수 있으므로 **출시 전 법률 검토가 필요하다.** Hsl은 v7.0.1 이후 상용 라이선스이므로 사용하지 않는다.

---

## 4. 설계 시사점 (종합)

### 4.1 Tag 모델 — 3계층
1. **Raw 계층 (Kepware 호환)**: Channel → Device → TagGroup → Tag.
   - Tag 속성: address, type, array, access, scan rate, scaling, deadband, VQT.
2. **템플릿 계층**: EdgeX Device Profile, Neuron Template, HighByte Model 방식. 기종별로 한 번 정의해 여러 번 인스턴스화한다.
   - 예: "LS XGK 사출기 표준"을 정의해 두고 장비 30대에 적용.
3. **UNS 계층 (선택)**: ISA-95 경로(`enterprise/site/area/line/cell`)로 MQTT·Sparkplug에 발행하고, OPC UA 객체 트리로도 미러링한다.

### 4.2 런타임 아키텍처 후보
```
          ┌──────────── Core (supervisor + tag DB + scheduler) ────────────┐
REST/Web ─┤ Config Store (SQLite+migration ⇄ YAML/JSON import/export)      │
          │ Tag Cache (VQT) ── Subscription Manager ── Scan Planner         │
          └──────┬────────────────────────────┬─────────────────────────────┘
       IPC (Unix socket / shm / NNG)           │ in-proc bus
   ┌─────────┼─────────┐               ┌───────┼────────┐
 [Driver]  [Driver]  [Driver]        [OPC UA]  [MQTT/    [REST
 LS FEnet  S7comm    libplctag       Server    Sparkplug] Server]
 (채널=프로세스 또는 스레드)          (open62541)
```
- **드라이버 격리**: 신뢰하는 내장 드라이버는 인프로세스 고속 경로로 돌린다. 외부 또는 서드파티 드라이버는 **채널 단위 프로세스**로 띄우고 supervisor가 재시작한다. 이 방식은 EdgeX, UMH, Telegraf execd, TB gRPC 사례에서 가져왔다.
- **Driver SPI**: `init / connect / validate_address / plan(coalesce) / read(batch) / write / subscribe / browse / health`
  - **요청 병합 플래너는 코어가 공통으로 제공**한다. 드라이버는 최대 블록 크기, 다중 항목 지원 여부 같은 능력만 선언한다.
- **폴링은 구독 기반**(Kepware 방식)이다. Northbound가 group을 구독하는 구조(Neuron 방식)로 폴링과 발행을 분리한다.
- **핫 리로드**: 객체 단위로 diff해서 적용한다. 영향받은 채널만 재시작하고, OPC UA 주소 공간은 증분으로 수정한다.
- **Config API**: Kepware 경로와 속성 체계를 따르고 `/doc/drivers` 스키마를 제공한다. 여기에 OpenAPI를 더하고, Git 친화적인 파일 export를 지원한다(Ignition 8.3 방식).
- **진단**: `_System` / `_Statistics` 태그, 프레임 캡처, Auto-Demotion을 처음부터 넣는다.

### 4.3 MVP 드라이버 우선순위 (국내 시장)
| 순위 | 드라이버 | 근거 |
|---|---|---|
| P0 | Modbus TCP/RTU | 공통 대안(Delta, Inovance, 계측기, 인버터) |
| P0 | **LS XGT FEnet** | 국내 1위이고 오픈소스가 없다. **핵심 차별점** |
| P0 | Siemens S7comm | 자동차·반도체 협력사 |
| P0 | Mitsubishi MC/SLMP 3E(+1E) | 국내 점유율 높음 |
| P1 | OPC UA Client | S7-1500 최적화 DB, 신형 PLC |
| P1 | Rockwell EIP (libplctag) | 외투 기업, 수입 라인 |
| P1 | LS Cnet (시리얼) | 레거시 |
| P2 | Omron FINS/CIP, Beckhoff ADS, Keyence, Panasonic, Cimon | 장비사, 검사 라인 |
| 보류 | S7comm-plus | 법률 검토 전까지 |
| 제외 | Profinet IO, EtherCAT 마스터 | 필드버스 영역 |

### 4.4 언어 선택
| | 장점 | 단점 |
|---|---|---|
| **Rust (추천)** | 장기 실행 데몬에 적합하고 메모리 안전. tokio-modbus 사용 가능, libplctag와 open62541을 FFI로 붙이기 쉬움 | 자체 드라이버 비중이 큼. Rust OPC UA 서버는 미성숙해서 FFI가 필요 |
| Go | MVP가 가장 빠름(gos7, gologix, PLC4Go, grid-x) | OPC UA 서버가 약함 → open62541을 cgo로 붙여야 함 |
| C#/.NET | PLC 라이브러리 생태계가 가장 풍부(VagabondK LS 포함) | OPC Foundation 스택이 GPL 또는 회원 전용, Hsl은 상용 |
| Java | PLC4J와 Milo가 우수 | 엣지 장비에서 JVM 메모리 부담, 한국 벤더용 라이브러리 없음 |
| C/C++ | 모든 C 라이브러리를 네이티브로 사용 | 네트워크 서비스로서 메모리 안전성 위험, 개발 속도 |

### 4.5 라이선스 안전 재사용 목록
- 사용 가능
  - open62541 (MPL-2.0)
  - libplctag (MPL-2.0)
  - Beckhoff/ADS (MIT)
  - tokio-modbus (Apache-2.0)
  - PLC4X (Apache-2.0)
  - gos7, grid-x/modbus, gopcua (BSD/MIT)
  - Tahu (EPL-2.0)
  - libfins (MIT)
- **회피**: OpenMUC, libiec61850, node-red-contrib-s7/eip (GPL), Hsl 최신판(상용), Redpanda Connect 엔터프라이즈 커넥터
- **동적 링크만 허용**: snap7, libmodbus, Neuron core (LGPL)

---

## 5. 다음 단계 제안
1. 타깃 고객과 하드웨어 확정: IPC x86인지 ARM 게이트웨이인지, 어떤 PLC가 실제로 있는지.
2. 언어와 OPC UA 스택 PoC: Rust + open62541 FFI로 태그 1만 개, 주소 공간 증분 갱신을 검증한다.
3. 드라이버 SPI와 요청 병합 플래너 설계. Modbus와 LS FEnet 두 개로 먼저 검증한다.
4. Kepware Config API 호환 범위 결정: 경로와 속성 키를 어디까지 맞출지.
5. 법률 검토 항목 정리: S7comm-plus, UMAS, 의존성 라이선스.

## 6. 미확인 목록
- LS FEnet 개별 읽기 변수 수(16?)와 기종별 연결 수
- S7-1200 FW4.x PDU 최대값
- MC 960워드와 FINS 999워드 한도
- Kepware Edge의 REST agent, 플러그인, 가격
- Kepware OPC UA 기본 포트 49320, 일부 system tag 이름
- 다빈소프트와 LSE OPC Server의 OS와 라이선스
- Takebishi의 LS 지원 여부
- snap7, S7.Net, bacnet-stack의 라이선스 재확인
- OPC Foundation .NET 스택 조건

## 7. 주요 출처
- Kepware
  - https://support.ptc.com/help/kepware/kepware_server/en/kepware/server/whats_new/whats_new_7_1_271_0.html
  - https://support.ptc.com/help/kepware/kepware_edge/en/kepware/kepware-edge/Welcome.html
  - https://support.ptc.com/help/kepware/kepware_server/en/kepware/server/config-api-service.html
  - https://support.ptc.com/help/kepware/drivers/en/kepware/drivers/Device_Properties_Scan_Mode.html
  - https://support.ptc.com/help/kepware/kepware_server/en/kepware/server/scaling.html
  - https://github.com/PTCInc/Kepware-ConfigAPI-SDK-Python
- 경쟁 제품
  - https://inductiveautomation.com/downloads/releasenotes/8.3.0
  - https://www.emqx.com/en/products/emqx-neuron
  - https://www.cybus.io/en/product/licenses/
  - https://industrial.softing.com/us/products/docker/edgeconnector.html
  - https://www.siemens.com/en-us/products/simatic/s7-plus-connector/
  - https://catalog.redhat.com/en/software/containers/takebishi/devicegateway/656c7dfab49ac3ebda15d672
- 오픈소스
  - https://github.com/emqx/neuron
  - https://docs.emqx.com/en/neuron/latest/introduction/architecture/architecture.html
  - https://github.com/edgexfoundry/edgex-go
  - https://docs.umh.app/benthos-umh
  - https://github.com/apache/plc4x
  - https://github.com/open62541/open62541
  - https://www.basyskom.de/en/opc-ua-and-rust-in-2025/
  - https://github.com/eclipse-tahu/tahu
- 프로토콜
  - LS FEnet: https://www.ls-electric.com/upload/customer/download/f8a96bfb-6212-4a0f-8d07-a1ba8868240f/User's%20Manual_XGT%20FEnet_V2.30.pdf
  - XGL-EOPCT: https://sol.ls-electric.com/uploads/document/17115908516980/XGL-EOPCT_Manual_V1.0_202402_EN.pdf
  - https://github.com/Vagabond-K/VagabondK.Protocols
  - https://github.com/libplctag/libplctag
  - https://github.com/gijzelaerr/python-snap7
  - https://github.com/thomas-v2/S7CommPlusDriver
  - https://github.com/Beckhoff/ADS
  - https://plc4x.apache.org/plc4x/latest/users/protocols/index.html
- 국내
  - https://www.davinsoft.com/ko/%EC%A0%9C%ED%92%88/opc-server/
  - http://opchub.com/product/lsis.asp
