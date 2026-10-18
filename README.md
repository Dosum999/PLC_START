# logict

Linux 네이티브 산업용 연결 서버입니다(Kepware 호환 모델). PLC 드라이버에서 읽은 태그를 OPC UA와 MQTT로 내보냅니다.

- 설계: [docs/04-architecture.md](docs/04-architecture.md)
- 조사 자료: [docs/01](docs/01-reference-research.md), [02 Kepware 드라이버 목록](docs/02-kepware-driver-catalog.md), [03 Kepware 구조·Northbound](docs/03-kepware-architecture-and-northbound.md)

## 빌드

Ubuntu 24.04 (x86-64) 기준입니다.

```sh
sudo apt install -y build-essential cmake ninja-build pkg-config libssl-dev git
cmake --preset debug          # ASan/UBSan 포함. 릴리스 빌드는 --preset release
cmake --build build/debug
ctest --test-dir build/debug
```

의존성(open62541 1.5.9, Eclipse Paho C 1.3.16, spdlog, nlohmann/json, CLI11, GoogleTest)은 처음 configure할 때 버전을 고정해서 받아옵니다.

## 실행 (개발용)

```sh
build/debug/app/logictd/logictd -c config/logictd.dev.json --watch 1000
```

- `config/logictd.dev.json`은 개발용입니다. 보안 None과 익명 접속을 허용하고, OPC UA는 `opc.tcp://localhost:49320`에서 열립니다.
- 노드 ID는 Kepware와 같은 `ns=2;s=<채널>.<디바이스>.<태그>` 형식입니다. 예: `ns=2;s=Simulator.Device1.Ramp`
- 운영 환경 기본값은 보안 Basic256Sha256/Aes128/Aes256 (Sign & Encrypt)이고 익명 접속은 꺼져 있습니다.
- 사용자 비밀번호 해시는 다음 명령으로 만듭니다.

  ```sh
  echo 'my-password' | logictd --hash-password
  ```

- 처음 접속한 클라이언트 인증서는 `<pki_dir>/ApplCerts/rejected/certs`에 저장됩니다. 신뢰하려면 `ApplCerts/trusted/certs`로 옮기세요.

## LS XGT FEnet 드라이버

드라이버 이름은 `LS XGT FEnet`이고, FEnet 모듈 또는 XGB 내장 Ethernet의 XGT 전용 서비스(TCP 2004)로 접속합니다. 예시는 `config/example-ls-xgt.json`에 있습니다.

| 모델 (`servermain.DEVICE_MODEL`) | 주소 예시 |
|---|---|
| XGK, XGB(XBM/XBC) | `D100`, `D100.A`(워드 비트, 16진), `M0010A`(비트), `T5`(Word=현재값, Boolean=접점), `ZR100`, `D300.16`(String 16자), `D400 [10]` |
| XGI, XGR, XEC | `%MW100`, `%MD10`(Float), `%MX1600`, `%MW100.3`, `%IX0.0.3`, `%QW0.1.0` |

PLC 쪽에서는 XG5000에서 FEnet "전용 서비스"를 켜고 접속 수를 설정해야 합니다. 이 드라이버는 아직 실제 PLC로 검증하지 않았습니다.

## 드라이버

| 드라이버 | 대상 | 주소 예시 |
|---|---|---|
| `Modbus TCP/IP Ethernet` / `Modbus RTU Serial` | Modbus 장치 전반 | `40001`, `400001`, `30001`, `000001`, `40010.3`, `40020.10H`, `40100 [10]` |
| `LS XGT FEnet` / `LS XGT Cnet` | XGK, XGB, XGI, XGR, XEC | `D100`, `D100.A`, `M0010A`, `T5`, `%MW100`, `%MD10`, `%IX0.0.3` |
| `OPC UA Client` | OPC UA 서버(S7-1500, Omron NX 등) | `ns=3;s="DB1"."Speed"`, `ns=2;i=1001` |
| `Allen-Bradley ControlLogix Ethernet` | ControlLogix, CompactLogix, Micro800, PLC-5/SLC/MicroLogix | `MyTag`, `Program:Main.Tag`, `Arr[0]{10}`, `MyDint.5`, `N7:0`, `B3:0/5` |
| `Siemens TCP/IP Ethernet` | S7-300/400/1200/1500 (S7comm, TCP 102) | `DB1.DBW10`, `DB1.DBD20`(+Float = REAL), `DB1.DBX30.3`, `DB1.STRING40.10`, `MW0`, `I0.1`, `QB1`, `DB1.DBW100 [5]` |
| `Mitsubishi Ethernet` | Q, L, iQ-R, iQ-F (MC 3E 바이너리) | `D100`, `D100.A`, `M10`, `X1F`, `Y20`, `TN5`, `ZR100`, `D110.6`(String), `M0 [16]` |

- S7-1200/1500: TIA Portal에서 "PUT/GET 통신 허용"을 켜고, 읽을 DB의 "최적화된 블록 액세스"를 꺼야 합니다. 랙·슬롯 기본값은 S7-300 0/2, S7-400 0/3, S7-1200/1500 0/1입니다.
- Mitsubishi: GX Works에서 SLMP(MC 프로토콜) 포트를 바이너리·TCP로 열어야 합니다(기본 포트 5000). 쓰기를 하려면 "RUN 중 쓰기 허용"도 켜야 합니다.
- 시리얼 드라이버(Modbus RTU, LS Cnet): 채널이 시리얼 포트이고 디바이스 ID가 국번입니다. 채널 속성은 `servermain.CHANNEL_SERIAL_COMMUNICATIONS_PORT`(예: `/dev/ttyUSB0`), `_BAUD_RATE`, `_PARITY`, `_DATA_BITS`, `_STOP_BITS`, `_RS485`입니다.

## MQTT (Kepware IoT Gateway 호환)

에이전트는 프로젝트 JSON의 `_iot_gateway.mqtt_clients`에 정의합니다. `config/example-project.json`의 `LocalBroker` 예시는 기본으로 꺼져 있으니 `iot_gateway.AGENTTYPES_ENABLED`를 `true`로 바꿔서 쓰세요.

```sh
mosquitto_sub -t 'logict/example/#' -v
mosquitto_pub -t logict/example/write -m '{"id":"Simulator.Device1.Setpoint","v":123}'
```

- 기본 페이로드: `{"timestamp":…, "values":[{"id":"Channel.Device.Tag","v":…,"q":true,"t":…}]}`
- Kepware 속성(`iot_gateway.*`) 외 확장 속성(`logict.*`): MQTT 5(`MQTT_PROTOCOL_VERSION`), 태그별 토픽(`MQTT_TOPIC_MODE`, `MQTT_TAG_TOPIC_TEMPLATE`), 쓰기 결과 응답 토픽(`MQTT_WRITE_RESPONSE_TOPIC`), 절대값 데드밴드, TLS 인증서 파일. 전체 목록은 `northbound/mqtt/agent_config.h`에 있습니다.

## 통합 테스트

```sh
python3 -m venv .venv && .venv/bin/pip install asyncua pymodbus paho-mqtt python-snap7 pymcprotocol pyserial pytest pytest-asyncio
sudo apt install -y mosquitto socat   # MQTT 브로커, 가상 시리얼 포트 (없으면 해당 테스트는 건너뜀)
cd tests/integration && ../../.venv/bin/pytest -q      # LOGICT_BUILD_DIR=../../build/debug
```

ASan 빌드로 돌릴 때는 `LOGICT_DAEMON_PREFIX="setarch -R"`를 지정합니다. GCC 13 ASan과 Linux 6.5 이상의 ASLR 조합 문제 때문입니다.

## 구조

```
sdk/         드라이버 SDK (Value/VQT, Driver 인터페이스, BlockPlanner, 코덱, TcpClient)
core/        프로젝트 모델(Kepware JSON), TagDB, ChannelWorker(채널당 스레드), 플러그인 로더
northbound/  OPC UA 서버 (open62541), MQTT 에이전트 (Paho C)
drivers/     simulator, modbus (TCP/RTU), ls_xgt (FEnet/Cnet), siemens (S7), mitsubishi (MC), opcua_client, rockwell  - 각각 .so 플러그인
app/logictd  데몬
tests/       단위 테스트(gtest) + 통합 테스트(pytest: asyncua, pymodbus)
```
