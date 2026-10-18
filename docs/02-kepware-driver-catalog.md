# 02. Kepware Server 드라이버 전체 카탈로그 (장기 로드맵 기준선)

> 작성일: 2026-10-06
> 목적: Linux(x86-64 산업용 PC) 기반 Kepware 유사 커넥티비티 서버의 장기 드라이버 로드맵 기준선 확보
> 1차 근거: PTC 공식 드라이버 도움말 색인 `support.ptc.com/help/kepware/drivers/en/kepware.html` (Kepware Server 7.x 기준, 2026-10 수집)
> 보조 근거: PTC 공식 데이터시트 "Driver Options for KEPServerEX"(J7845, 2018), Software Toolbox TOP Server 드라이버 목록(Kepware OEM), 각 리셀러의 Suite 페이지

---

## 1. 요약

### 1.1 전체 개수

| 구분 | 개수 | 비고 |
|---|---|---|
| **현행 Windows용 드라이버 (Kepware Server 7.x)** | **142** | PTC 도움말 색인 141개 + U-CON(별도 PDF 매뉴얼로 배포, 현행 제품) |
| Kepware Edge(Linux) 전용 빌드 | 6 | 도움말 색인의 "... for Edge" 항목. 모두 Windows 드라이버의 Linux 포팅판 |
| 구 데이터시트에만 있고 현행 색인에서 사라진 드라이버 | 3 | AB Data Highway Plus, Hilscher Universal, SattBus Serial (단종 추정) |
| 이름만 바뀐 드라이버 | 6 | 아래 1.3 참고 |

마케팅 문구 "150+ drivers"는 Edge 빌드, 구 드라이버, 플러그인을 섞어서 센 숫자로 보입니다. 실제로 서로 다른 프로토콜 드라이버는 **약 142개**입니다. Software Toolbox 목록의 "174개"에는 Suite 상품과 플러그인 SKU가 포함되어 있습니다.

### 1.2 카테고리별 개수 (현행 142개)

| 카테고리 | 개수 |
|---|---|
| PLC/FA (벤더별) | 65 |
| 공정계측/레코더/I/O/기타 장비 | 32 |
| Modbus 계열 (범용) | 5 |
| 빌딩 자동화 (BACnet) | 1 |
| 전력/유틸리티 (DNP3, IEC 60870-5, IEC 61850) | 5 |
| 석유·가스/EFM | 12 (Enron/Lufkin Modbus 포함) |
| CNC/기계 | 6 |
| 범용/커스텀/IT/OPC/IoT | 16 |
| **합계** | **142** |

### 1.3 이름 변경 (구 데이터시트 이름 → 현행 이름)

| 구 이름 (2018 데이터시트) | 현행 이름 (7.x 도움말) |
|---|---|
| Allen-Bradley ControlLogix Unsolicited | Allen-Bradley ControlLogix Server |
| Allen-Bradley Unsolicited Ethernet | Allen-Bradley Server Ethernet |
| Siemens TCP/IP Unsolicited Ethernet | Siemens TCP/IP Server Ethernet |
| Modbus Unsolicited Serial | Modbus Server Serial |
| Modbus Ethernet / Modbus Serial | Modbus TCP/IP Ethernet / Modbus RTU Serial |
| DNP3 Master Ethernet/Serial, IEC 60870-5-101/104 Master | DNP Client Ethernet/Serial, IEC 60870-5-101/104 Client |

2018년 이후 새로 생긴 드라이버: **Siemens S7 Plus Ethernet**, **Universal Device (JavaScript 스크립트형)**, **Acroloop CIP Ethernet**, **Yokogawa GX Ethernet**, **Simulator(기본형)**, 그리고 6개 **for Edge** 빌드.

### 1.4 핵심 결론: LS ELECTRIC(LSIS) / CIMON

- **LS ELECTRIC(구 LSIS) XGT/XGK/XGI/XGB(FEnet, Cnet) 전용 드라이버는 Kepware에 없습니다.** PTC 도움말 색인 142개, 2018 데이터시트, TOP Server 174개 목록 어디에도 LS, LSIS, XGT, Master-K, GLOFA가 나오지 않습니다. Kepware를 쓰는 현장은 보통 XGT의 Modbus TCP/RTU 기능(내장 기능 또는 통신 모듈)으로 Modbus 드라이버에 붙이거나, U-CON으로 XGT 전용 프로토콜을 직접 정의합니다.
- **CIMON(PLC-S / XP 시리즈) 전용 드라이버도 없습니다.** PTC 커뮤니티에는 CIMON SCADA의 OPC UA 서버에 Kepware OPC UA Client로 접속하는 사례만 있습니다(BadTcpMessageTooLarge 이슈).
- **이 둘이 국내 시장에서 Kepware와 차별화할 수 있는 지점입니다.** LS XGT FEnet은 P0, LS XGT Cnet과 CIMON은 P1으로 별도 항목을 둡니다(표 2.9).

### 1.5 Kepware Edge(Linux) 드라이버 (도움말 색인에서 확인)

| Edge 빌드 | 대응 Windows 드라이버 |
|---|---|
| Allen-Bradley ControlLogix Ethernet for Edge | Allen-Bradley ControlLogix Ethernet |
| Mitsubishi Ethernet Driver for Edge | Mitsubishi Ethernet |
| Modbus TCP/IP Ethernet for Edge | Modbus TCP/IP Ethernet |
| OPC UA Client Driver for Edge | OPC UA Client |
| Siemens S7 Plus Ethernet for Edge | Siemens S7 Plus Ethernet |
| Siemens TCP/IP Ethernet for Edge | Siemens TCP/IP Ethernet |

시리얼 드라이버는 Edge에 하나도 없습니다. **Linux 산업용 PC에서 Modbus RTU를 포함한 시리얼을 지원하는 것만으로도 Kepware Edge와 차별화**됩니다.

---

## 2. 전체 드라이버 표 (카테고리별)

### 범례

- **매체**: E = Ethernet, S = Serial(RS-232/485), E/S = 둘 다, SW = 소프트웨어/PC 내부, HW = 전용 PC 카드 필요
- **Suite 약어**: MFG = Manufacturing Suite, AMS = Advanced Manufacturing Suite(전 드라이버 포함, 모든 행에 해당하므로 생략), AB = Allen-Bradley, SIE = Siemens, SIE+ = Siemens Plus, MIT = Mitsubishi, OMR = Omron, GE = GE, MOD = Modbus, BA = Building Automation, PWR = Power, OG = Oil and Gas, IT = IT and Infrastructure, OPC = OPC Connectivity, YOK = Yokogawa, HON = Honeywell, ADC = AutomationDirect, ARO = Aromat, CH = Cutler-Hammer, CON = Contrex, TOS = Toshiba, TOY = Toyopuc, YAS = Yaskawa, SIX = SIXNET, TW = Thermo Westronics, SAT = SattBus, SMT = Simatic(TI505), FOC = Fanuc Focas, ROC = Fisher ROC, WITS, DNP3, IEC = IEC 60870-5, SNMP, SIMU = Simulation. `?`는 미검증.
- **Edge**: O = Kepware Edge(Linux) 빌드 있음, - = 없음
- **스펙 공개**: 공개 = 표준 문서 또는 제조사 공개 매뉴얼로 구현 가능 / 부분 = 일부만 공개되었거나 회원 전용 / 역공학 = 비공식 오픈소스 구현만 있음 / 비공개 = 암호화되었거나 SDK·라이선스가 필요 / 라이브러리 = 제조사 API(DLL/SO)를 호출하는 방식
- **우리 우선순위**: P0 = 1차 출시 필수, P1 = 국내 제조 시장의 다음 단계, P2 = 수요가 있을 때, P3 = 레거시나 해외 니치라서 보류

### 2.1 PLC/FA — Rockwell Automation (Allen-Bradley) — 9

| # | Driver | 매체 | 지원 모델 (요약) | Suite | Edge | 스펙 공개 | 우리 우선순위 |
|---|---|---|---|---|---|---|---|
| 1 | Allen-Bradley ControlLogix Ethernet | E | ControlLogix, CompactLogix, GuardLogix, FlexLogix, SoftLogix (EtherNet/IP CIP, 심볼릭 태그) | AB, MFG | O | 부분 (ODVA CIP 회원 전용, Logix Data Access 매뉴얼 공개, libplctag 참고 가능) | **P1** |
| 2 | Allen-Bradley ControlLogix Server | E | Logix 컨트롤러가 MSG로 밀어주는 데이터 수신(구 Unsolicited) | AB, MFG | - | 부분 | P3 |
| 3 | Allen-Bradley Ethernet | E | PLC-5/SLC 500/MicroLogix (CSPv4, EtherNet/IP PCCC) | AB, MFG | - | 부분 (PCCC는 DF1 매뉴얼 공개) | P2 |
| 4 | Allen-Bradley Server Ethernet | E | PLC-2 Unprotected 메시지 수신(구 Unsolicited Ethernet) | AB, MFG | - | 부분 | P3 |
| 5 | Allen-Bradley DF1 | S | PLC-5, SLC 500, MicroLogix (DF1 full/half duplex, DH+/DH-485 게이트웨이 경유) | AB, MFG | - | 공개 (1770-6.5.16) | P3 |
| 6 | Allen-Bradley Micro800 Ethernet | E | Micro810/820/830/850/870 | AB, MFG | - | 부분 | P2 |
| 7 | Allen-Bradley Micro800 Serial | S | Micro800 (CIP over DF1) | AB, MFG | - | 부분 | P3 |
| 8 | Allen-Bradley Bulletin 900 | S | Bulletin 900 온도 컨트롤러 | MFG | - | 부분 | P3 |
| 9 | Allen-Bradley 1609 UPS | E | 1609 UPS (SNMP 기반) | MFG | - | 공개 (SNMP MIB) | P3 |

### 2.2 PLC/FA — Siemens — 10

| # | Driver | 매체 | 지원 모델 (요약) | Suite | Edge | 스펙 공개 | 우리 우선순위 |
|---|---|---|---|---|---|---|---|
| 10 | Siemens TCP/IP Ethernet | E | S7-200(CP243), S7-300, S7-400, S7-1200, S7-1500 (S7comm, 절대 주소, PUT/GET), NetLink | SIE, SIE+, MFG | O | 역공학 (S7comm, Snap7 등 오픈소스 성숙) | **P0** |
| 11 | Siemens S7 Plus Ethernet | E | S7-1200/S7-1500 심볼릭 접근 (S7CommPlus, TIA 태그명) | SIE+ | O | 비공개 (암호화/무결성 보호, 오픈소스는 일부 펌웨어만 지원) | P2 (난이도 매우 높음, 대안은 OPC UA Client) |
| 12 | Siemens TCP/IP Server Ethernet | E | S7-300/400/1200/1500이 BSEND/USEND로 보내는 데이터 수신(구 Unsolicited) | SIE, SIE+, MFG | - | 역공학 | P3 |
| 13 | Siemens S7 MPI | S | S7-300/400 (MPI 어댑터 경유) | SIE, SIE+, MFG | - | 역공학 | P3 |
| 14 | Siemens S7-200 | S | S7-200 PPI | SIE, SIE+, MFG | - | 역공학 | P3 |
| 15 | Siemens S5 | S | S5 AS511 PG 포트 | SIE, SIE+, MFG | - | 역공학 | P3 |
| 16 | Siemens S5 3964R | S | S5/S7 CP의 3964R/RK512 | SIE, SIE+, MFG | - | 공개 (3964R/RK512 문서) | P3 |
| 17 | Simatic / TI 505 Ethernet | E | Siemens/TI 505 시리즈 (CTI 이더넷 모듈) | SMT, MFG | - | 부분 | P3 |
| 18 | Simatic / TI 505 Serial | S | TI 505 시리즈 (NITP/CCM) | SMT, MFG | - | 부분 | P3 |
| 19 | TIWAY Host Adapter | HW | TIWAY 네트워크 (전용 Host Adapter 카드) | SMT?, MFG | - | 부분 | P3 |

### 2.3 PLC/FA — Mitsubishi Electric — 4 (CNC는 2.12에)

| # | Driver | 매체 | 지원 모델 (요약) | Suite | Edge | 스펙 공개 | 우리 우선순위 |
|---|---|---|---|---|---|---|---|
| 20 | Mitsubishi Ethernet | E | MELSEC iQ-R, iQ-F, Q, L, QnA, A, FX3U-ENET 등 (MC 프로토콜 3E/1E, TCP/UDP) | MIT, MFG | O | 공개 (MC 프로토콜 레퍼런스 매뉴얼) | **P0** |
| 21 | Mitsubishi Serial | S | A/QnA/Q/L 시리즈 시리얼 MC 프로토콜(계산기 링크) | MIT, MFG | - | 공개 | P1 |
| 22 | Mitsubishi FX | S | FX 시리즈 CPU 포트 (프로그래밍 프로토콜) | MIT, MFG | - | 부분 (FX 프로그래밍 포트 프로토콜은 비공식) | P2 |
| 23 | Mitsubishi FX Net | S | FX 시리즈 485BD (전용 프로토콜 형식 1/4) | MIT, MFG | - | 공개 | P2 |

### 2.4 PLC/FA — Omron — 6

| # | Driver | 매체 | 지원 모델 (요약) | Suite | Edge | 스펙 공개 | 우리 우선순위 |
|---|---|---|---|---|---|---|---|
| 24 | Omron FINS Ethernet | E | CS1, CJ1/CJ2, CP1, CV, NJ/NX(FINS 모드) (FINS/UDP, TCP) | OMR, MFG | - | 공개 (FINS Commands Reference W227 등) | **P1** |
| 25 | Omron NJ Ethernet | E | NJ/NX 시리즈 (EtherNet/IP CIP 심볼릭 변수) | OMR, MFG | - | 부분 (CIP + Omron 공개 매뉴얼 W506) | P1 |
| 26 | Omron FINS Serial | S | CS/CJ/CP (Host Link 위의 FINS) | OMR, MFG | - | 공개 | P2 |
| 27 | Omron Host Link | S | C/CV/CS/CJ 시리즈 C-mode Host Link | OMR, MFG | - | 공개 | P2 |
| 28 | Omron Toolbus | S | CS/CJ 주변기기 포트 Toolbus | OMR, MFG | - | 역공학 | P3 |
| 29 | Omron Process Suite | S | E5 계열 온도 컨트롤러 등 (CompoWay/F) | OMR, MFG | - | 공개 (CompoWay/F) | P2 |

### 2.5 PLC/FA — GE (Emerson) — 5

| # | Driver | 매체 | 지원 모델 (요약) | Suite | Edge | 스펙 공개 | 우리 우선순위 |
|---|---|---|---|---|---|---|---|
| 30 | GE Ethernet | E | Series 90-30/90-70, VersaMax, PACSystems RX3i/RX7i (SRTP) | GE, MFG | - | 역공학 (SRTP) | P2 |
| 31 | GE EGD | E | Ethernet Global Data (UDP 생산/소비) | GE, MFG | - | 부분 | P3 |
| 32 | GE SNP | S | Series 90 SNP | GE, MFG | - | 부분 | P3 |
| 33 | GE SNPX | S | Series 90/VersaMax SNP-X | GE, MFG | - | 부분 | P3 |
| 34 | GE CCM | S | Series Six/90 CCM | GE, MFG | - | 부분 | P3 |

### 2.6 PLC/FA — 기타 PLC 벤더 — 31

| # | Driver | Vendor | 매체 | 지원 모델 (요약) | Suite | Edge | 스펙 공개 | 우리 우선순위 |
|---|---|---|---|---|---|---|---|---|
| 35 | Telemecanique Uni-Telway | Schneider | S | TSX 7/Micro/Premium (Uni-Telway) | MFG | - | 부분 | P3 |
| 36 | Square D | Schneider | S | SY/MAX, PowerLogic (SY/MAX 프로토콜) | MFG | - | 부분 | P3 |
| 37 | Yaskawa MP Series Ethernet | Yaskawa | E | MP2000/MP3000 (Extended MEMOBUS) | YAS, MFG | - | 공개 (MEMOBUS 매뉴얼) | P2 |
| 38 | Yaskawa MP Series Serial | Yaskawa | S | MP 시리즈 (MEMOBUS 시리얼) | YAS, MFG | - | 공개 | P3 |
| 39 | Yaskawa Memobus Plus | Yaskawa | HW | Memobus Plus 네트워크 (전용 카드) | YAS, MFG | - | 부분 | P3 |
| 40 | Toshiba Ethernet | Toshiba | E | Unified Controller nv, V 시리즈 | TOS, MFG | - | 부분 | P3 |
| 41 | Toshiba Serial | Toshiba | S | T/EX/Model 시리즈 | TOS, MFG | - | 부분 | P3 |
| 42 | Toyopuc PC3/PC2 Ethernet | JTEKT | E | TOYOPUC PC3J/PC2J/PC10 | TOY, MFG | - | 공개 (JTEKT 통신 매뉴얼) | P2 (국내 자동차 라인 일부) |
| 43 | Toyopuc Serial | JTEKT | S | TOYOPUC 시리얼 | TOY, MFG | - | 공개 | P3 |
| 44 | Aromat Ethernet | Panasonic | E | FP 시리즈 (MEWTOCOL over ET-LAN) | ARO, MFG | - | 공개 (MEWTOCOL) | P2 |
| 45 | Aromat Serial | Panasonic | S | FP0/FP2/FPΣ/FP-X (MEWTOCOL-COM) | ARO, MFG | - | 공개 | P2 |
| 46 | Cutler-Hammer ELC Ethernet | Eaton | E | ELC 시리즈 (Modbus 기반) | CH, MFG | - | 공개 | P3 |
| 47 | Cutler-Hammer ELC Serial | Eaton | S | ELC 시리즈 | CH, MFG | - | 공개 | P3 |
| 48 | Cutler-Hammer D50/300 | Eaton | S | D50/D300 | CH, MFG | - | 부분 | P3 |
| 49 | AutomationDirect DirectNET | AutomationDirect(Koyo) | S | DL05/06/205/405 DirectNET | ADC, MFG | - | 공개 | P3 |
| 50 | AutomationDirect K Sequence | AutomationDirect(Koyo) | S | DL 시리즈 K-Sequence | ADC, MFG | - | 부분 | P3 |
| 51 | AutomationDirect EBC | AutomationDirect | E | Terminator I/O EBC | ADC, MFG | - | 부분 | P3 |
| 52 | AutomationDirect ECOM | AutomationDirect | E | DL 시리즈 H*-ECOM | ADC, MFG | - | 부분 | P3 |
| 53 | AutomationDirect Productivity Series Ethernet | AutomationDirect | E | Productivity 1000/2000/3000 (태그 import) | ADC, MFG | - | 부분 (Modbus 매핑) | P3 |
| 54 | IDEC Serial | IDEC | S | MicroSmart, FA-2J/3S, OpenNet | MFG | - | 공개 | P3 |
| 55 | Fuji Flex | Fuji Electric | S | MICREX-F / FLEX-PC | MFG | - | 부분 | P3 |
| 56 | Keyence Ethernet (KV Ethernet) | Keyence | E | KV-5000/7000/8000/Nano, KV-NC (상위 링크) | MFG | - | 공개 (KV 상위 링크 매뉴얼) | **P1** (국내 사용 많음) |
| 57 | Beckhoff TwinCAT | Beckhoff | E | TwinCAT 2/3 런타임 (ADS/AMS, 심볼) | MFG | - | 공개 (ADS 문서, 오픈소스 ADS 라이브러리) | P2 |
| 58 | CODESYS | CODESYS 생태계 | E | CODESYS V2.3/V3 기반 컨트롤러 (Gateway/심볼) | MFG | - | 비공개 (V3 프로토콜) | P2 |
| 59 | Opto 22 Ethernet | Opto 22 | E | SNAP PAC, SNAP Ultimate I/O | MFG | - | 공개 (OptoMMP) | P3 |
| 60 | WAGO Ethernet | WAGO | E | 750 시리즈 이더넷 커플러/컨트롤러 (Modbus TCP 기반) | MFG | - | 공개 | P2 (Modbus로 대체 가능) |
| 61 | Triconex Ethernet | Schneider(Triconex) | E | Tricon/Trident/Tri-GP (TSAA) | AMS 전용 (MFG 제외) | - | 부분 | P3 |
| 62 | Alstom Redundant Ethernet | Alstom/GE | E | Alspa C80-75 등 이중화 이더넷 | MFG? | - | 비공개 | P3 |
| 63 | SattBus Ethernet | ABB(Satt) | E | SattCon/SattLine (SattBus over UDP) | SAT, MFG | - | 비공개 | P3 |
| 64 | Optimation OptiLogic | Optimation | S | OptiLogic 컨트롤러 | MFG | - | 부분 | P3 |
| 65 | Acroloop CIP Ethernet | Acroloop | E | ACR 모션 컨트롤러 (CIP) | MFG? | - | 부분 | P3 |

### 2.7 공정 계측/레코더/I/O/기타 장비 — 32

| # | Driver | Vendor | 매체 | 지원 모델 (요약) | Suite | Edge | 스펙 공개 | 우리 우선순위 |
|---|---|---|---|---|---|---|---|---|
| 66 | Yokogawa Controller | Yokogawa | S | UT/UP/US 계열 Green Series 컨트롤러 (PC link) | YOK, MFG | - | 공개 | P2 |
| 67 | Yokogawa CX | Yokogawa | E | CX1000/CX2000 | YOK, MFG | - | 공개 | P3 |
| 68 | Yokogawa Darwin Ethernet | Yokogawa | E | DARWIN DA/DC/DR | YOK, MFG | - | 공개 | P3 |
| 69 | Yokogawa Darwin Serial | Yokogawa | S | DARWIN | YOK, MFG | - | 공개 | P3 |
| 70 | Yokogawa DX Ethernet | Yokogawa | E | DAQSTATION DX100/200/1000/2000 | YOK, MFG | - | 공개 | P2 |
| 71 | Yokogawa DX Serial | Yokogawa | S | DX 시리즈 | YOK, MFG | - | 공개 | P3 |
| 72 | Yokogawa DXP | Yokogawa | E | DXAdvanced P | YOK, MFG | - | 공개 | P3 |
| 73 | Yokogawa GX Ethernet | Yokogawa | E | SMARTDAC+ GX/GP/GM | YOK, MFG | - | 공개 | P2 |
| 74 | Yokogawa HR | Yokogawa | S | HR2400 레코더 | YOK, MFG | - | 공개 | P3 |
| 75 | Yokogawa MW | Yokogawa | E | MW100 | YOK, MFG | - | 공개 | P3 |
| 76 | Yokogawa MX | Yokogawa | E | MX100 | YOK, MFG | - | 공개 | P3 |
| 77 | Yokogawa YS100 | Yokogawa | S | YS100 계열 단일루프 컨트롤러 | YOK, MFG | - | 공개 | P3 |
| 78 | Honeywell HC900 Ethernet | Honeywell | E | HC900 하이브리드 컨트롤러 (Modbus/TCP 기반) | HON, MFG | - | 공개 | P3 |
| 79 | Honeywell UDC Ethernet | Honeywell | E | UDC2500/3500 등 | HON, MFG | - | 공개 | P3 |
| 80 | Honeywell UDC Serial | Honeywell | S | UDC 시리즈 | HON, MFG | - | 공개 | P3 |
| 81 | Micro-DCI | ABB(Bailey/Fischer & Porter) | S | Micro-DCI 53MC 등 | MFG | - | 부분 | P3 |
| 82 | Partlow ASCII | Partlow/West | S | Partlow 컨트롤러/레코더 | MFG | - | 부분 | P3 |
| 83 | Thermo Westronics Ethernet | Thermo Westronics | E | 레코더 | TW, MFG | - | 부분 | P3 |
| 84 | Thermo Westronics Serial | Thermo Westronics | S | 레코더 | TW, MFG | - | 부분 | P3 |
| 85 | Contrex M-Series | Contrex (Fenner) | S | M-Drive/M-Trim 등 | CON, MFG | - | 부분 | P3 |
| 86 | Contrex Serial | Contrex | S | Contrex 모터 컨트롤러 | CON, MFG | - | 부분 | P3 |
| 87 | Dataforth isoLynx | Dataforth | S/E | isoLynx SLX200 데이터 수집 | MFG | - | 부분 (Modbus 기반) | P3 |
| 88 | Analog Devices | Analog Devices | S | 6B 시리즈 신호 조절 모듈 | MFG | - | 공개 | P3 |
| 89 | IOtech PointScan 100 | IOtech | E | PointScan 100 I/O | MFG | - | 부분 | P3 |
| 90 | Scanivalve Ethernet | Scanivalve | E | 압력 스캐너 DSA/ZOC | MFG | - | 공개 | P3 |
| 91 | Mettler Toledo | Mettler Toledo | S | 계량 인디케이터/저울 (SICS 등) | MFG | - | 공개 (SICS) | P2 (계량 수요) |
| 92 | SIXNET EtherTRAK | Red Lion(SIXNET) | E | EtherTRAK I/O | SIX, MFG | - | 부분 | P3 |
| 93 | SIXNET UDR | Red Lion(SIXNET) | E/S | SIXNET UDR 프로토콜 | SIX, MFG | - | 부분 | P3 |
| 94 | BUSWARE Ethernet | BUSWARE | E | BUSWARE 이더넷 I/O | MFG | - | 부분 | P3 |
| 95 | Philips P8/PC20 | Philips | S | P8/PC20 PLC | MFG | - | 비공개 | P3 |
| 96 | Intelligent Actuator Super SEL | IAI | S | Super SEL 로봇 컨트롤러 | MFG | - | 부분 | P3 |
| 97 | Torque Tool Ethernet | Atlas Copco 등 (Open Protocol) | E | 너트러너/조임 공구 (Atlas Copco Open Protocol 계열) | MFG | - | 공개 (Open Protocol) | P2 (국내 자동차 조립 라인 수요) |

### 2.8 Modbus 계열 (범용) — 5

| # | Driver | 매체 | 지원 모델 (요약) | Suite | Edge | 스펙 공개 | 우리 우선순위 |
|---|---|---|---|---|---|---|---|
| 98 | Modbus TCP/IP Ethernet | E | 모든 Modbus TCP 장비, Modbus RTU over TCP, Modicon(Quantum/M340/M580), 마스터와 슬레이브 모드 | MOD, MFG, BA, PWR, OG, IT | O | 공개 (modbus.org) | **P0** |
| 99 | Modbus RTU Serial | S | 모든 Modbus RTU 장비 (RS-232/485) | MOD, MFG, BA, PWR, OG, IT | - | 공개 | **P0** |
| 100 | Modbus ASCII Serial | S | Modbus ASCII 장비 | MOD, MFG, BA, PWR, OG, IT | - | 공개 | P1 |
| 101 | Modbus Server Serial | S | 시리얼 Modbus 슬레이브로 동작(구 Unsolicited) | MOD, MFG | - | 공개 | P1 |
| 102 | Modbus Plus | HW | Modicon Modbus Plus (SA85 등 전용 카드) | MOD, MFG | - | 부분 (하드웨어 의존) | P3 |

### 2.9 [Kepware 미지원] 국내 시장 추가 드라이버 (우리가 따로 둘 항목)

| # | Driver (가칭) | Vendor | 매체 | 지원 모델 | Kepware 해당 드라이버 | 스펙 공개 | 우리 우선순위 |
|---|---|---|---|---|---|---|---|
| K1 | LS XGT FEnet | LS ELECTRIC | E | XGK/XGI/XGR, XGB(XBC/XEC) 내장 이더넷, XGL-EFMT (XGT 전용 프로토콜 TCP 2004/UDP 2005) | **없음** (Modbus 우회 또는 U-CON) | 공개 (XGT FEnet 사용설명서 "XGT 전용 프로토콜") | **P0** |
| K2 | LS XGT Cnet | LS ELECTRIC | S | XGT/XGB Cnet (XGT 전용 프로토콜) | **없음** | 공개 | **P1** |
| K3 | LS Master-K / GLOFA (레거시) | LS ELECTRIC | E/S | MASTER-K, GLOFA-GM | **없음** | 공개 (레거시 매뉴얼) | P2 |
| K4 | CIMON PLC (PLC-S/CP/XP) Ethernet/Serial | KDT(CIMON) | E/S | CIMON-PLC (PLC-S, CP 시리즈) | **없음** (CIMON SCADA의 OPC UA 경유만 가능) | 부분 (CIMON 통신 매뉴얼 공개 여부 추가 확인 필요) | **P1** |

### 2.10 빌딩 자동화 — 1

| # | Driver | 매체 | 지원 모델 (요약) | Suite | Edge | 스펙 공개 | 우리 우선순위 |
|---|---|---|---|---|---|---|---|
| 103 | BACnet/IP | E | BACnet/IP 장비(HVAC, 조명, 출입 등), COV 구독, 장비 디스커버리 | BA (MFG 제외) | - | 공개 (ASHRAE 135, BACnet Stack 오픈소스) | P2 |

### 2.11 전력/유틸리티 — 5

| # | Driver | 매체 | 지원 모델 (요약) | Suite | Edge | 스펙 공개 | 우리 우선순위 |
|---|---|---|---|---|---|---|---|
| 104 | DNP Client Ethernet | E | DNP3 Outstation (RTU, IED, 계량기), Secure Authentication SAv2/v5 | PWR, DNP3 (MFG 제외) | - | 공개 (IEEE 1815, opendnp3) | P2 (한전 배전 자동화 수요) |
| 105 | DNP Client Serial | S | DNP3 시리얼 Outstation | PWR, DNP3 | - | 공개 | P2 |
| 106 | IEC 60870-5-101 Client | S | 101 Controlled station | PWR, IEC | - | 공개 (IEC 표준은 유료, lib60870 오픈소스) | P3 |
| 107 | IEC 60870-5-104 Client | E | 104 Controlled station | PWR, IEC | - | 공개 (IEC 유료, lib60870) | P2 |
| 108 | IEC 61850 MMS Client | E | IEC 61850 IED (MMS, 리포트, SCL import) | PWR | - | 공개 (IEC 유료, libIEC61850) | P2 (변전소/ESS 수요) |

### 2.12 석유·가스/EFM — 12

| # | Driver | Vendor | 매체 | 지원 모델 (요약) | Suite | Edge | 스펙 공개 | 우리 우선순위 |
|---|---|---|---|---|---|---|---|---|
| 109 | ABB Totalflow | ABB | E/S | Totalflow 유량 컴퓨터/RTU (EFM 포함) | OG | - | 비공개 | P3 |
| 110 | Bristol/IP | Emerson(Bristol) | E | ControlWave, Network 3000 (BSAP/IP) | OG | - | 비공개 | P3 |
| 111 | Enron Modbus | 범용 | E/S | Enron Modbus 유량 컴퓨터 (EFM 이력/알람) | OG | - | 공개 | P3 |
| 112 | Lufkin Modbus | Lufkin | E/S | Lufkin SAM/WM (ELAM) | OG | - | 부분 | P3 |
| 113 | Fisher ROC Ethernet | Emerson | E | ROC300/800 계열 (ROC 프로토콜) | OG, ROC | - | 공개 (ROC Protocol 사양서) | P3 |
| 114 | Fisher ROC Serial | Emerson | S | ROC 계열 | OG, ROC | - | 공개 | P3 |
| 115 | Fisher ROC Plus Ethernet | Emerson | E | ROC800/FloBoss 107 (ROC Plus) | OG, ROC | - | 공개 | P3 |
| 116 | Fisher ROC Plus Serial | Emerson | S | ROC Plus 계열 | OG, ROC | - | 공개 | P3 |
| 117 | OMNI Flow Computer | OMNI | E/S | OMNI 3000/6000 (Modbus 변형, 아카이브) | OG | - | 부분 | P3 |
| 118 | Weatherford 8500 | Weatherford | S | 8500 Rod Pump Controller | OG | - | 비공개 | P3 |
| 119 | WITS Level 0 Active | 범용(시추) | S | WITS Level 0 장비 (요청형) | OG, WITS | - | 공개 | P3 |
| 120 | WITS Level 0 Passive | 범용(시추) | S | WITS Level 0 장비 (수신형) | OG, WITS | - | 공개 | P3 |

### 2.13 CNC/기계 — 6

| # | Driver | Vendor | 매체 | 지원 모델 (요약) | Suite | Edge | 스펙 공개 | 우리 우선순위 |
|---|---|---|---|---|---|---|---|---|
| 121 | Fanuc Focas Ethernet | FANUC | E | Series 0i/16i/18i/21i/30i/31i/32i/35i, Power Mate (FOCAS1/2) | FOC, MFG | - | 라이브러리 (FOCAS 라이브러리, Linux용 libfwlib32 있음) | **P1** (국내 가공 라인 수요 큼) |
| 122 | Fanuc Focas HSSB | FANUC | HW | HSSB 광 카드 연결 CNC | FOC, MFG | - | 라이브러리 (Windows 전용 카드) | P3 |
| 123 | Mitsubishi CNC Ethernet | Mitsubishi | E | M700/M70/M800/M80 계열 | MIT?, MFG | - | 비공개/라이브러리 | P2 |
| 124 | MTConnect | 범용 (MTConnect Institute) | E | MTConnect Agent (HTTP/XML; Mazak, Okuma, Haas, DMG 등) | MFG | - | 공개 | P2 |
| 125 | EUROMAP 63 | 범용 (사출기) | E | EUROMAP 63 파일 기반 사출기 (Engel, Arburg 등) | MFG | - | 공개 | P2 (사출 라인; 후속 표준 EUROMAP 77/OPC UA도 고려) |
| 126 | Krauss Maffei MC4 Ethernet | KraussMaffei | E | MC4 사출기 컨트롤러 | MFG | - | 비공개 | P3 |

### 2.14 범용/커스텀/IT/OPC/IoT — 16

| # | Driver | 매체 | 기능 (요약) | Suite | Edge | 스펙 공개 | 우리 우선순위 |
|---|---|---|---|---|---|---|---|
| 127 | Simulator | SW | 기본 시뮬레이션 태그 (램프, 사인, 랜덤) | SIMU, MFG | - | 해당 없음 | **P0** (개발/데모 필수) |
| 128 | Advanced Simulator | SW | 수식/스크립트 기반 고급 시뮬레이션 | SIMU, MFG | - | 해당 없음 | P1 |
| 129 | Memory Based | SW | 서버 메모리 태그 (외부 쓰기 버퍼) | SIMU, MFG | - | 해당 없음 | **P0** |
| 130 | User Configurable (U-CON) | E/S | 사용자가 프레임/트랜잭션을 정의하는 ASCII/바이너리 커스텀 프로토콜 (바코드, 저울 등) | MFG, BA, OG, IT | - | 해당 없음 | P1 (LS/CIMON 같은 미지원 장비 대응) |
| 131 | Universal Device | E/S | JavaScript 프로필로 만드는 커스텀 드라이버 (TCP/UDP/HTTP 페이로드, 2020년대 신규) | AMS? | - | 해당 없음 | P1 (Linux에서는 Lua/JS 스크립트 드라이버로 구현) |
| 132 | Custom Interface | SW | 사용자 DLL 연동 (Windows API) | MFG, BA, OG, IT | - | 해당 없음 | P3 (Linux에서는 플러그인 SDK로 대체) |
| 133 | OPC UA Client | E | 외부 OPC UA 서버 접속 (구독, 브라우즈, 보안정책) | OPC, SIE+, MFG | O | 공개 (OPC UA, open62541) | **P0** |
| 134 | OPC DA Client | SW | 외부 OPC DA(COM) 서버 접속 | OPC, MFG | - | 공개 (Windows COM 전용) | P3 (Linux 불가, 원격 게이트웨이 필요) |
| 135 | OPC XML-DA Client | E | OPC XML-DA (SOAP) | OPC, MFG | - | 공개 | P3 |
| 136 | DDE Client | SW | Windows DDE | MFG | - | 공개 (Windows 전용) | 해당 없음 (Linux 불가) |
| 137 | InTouch Client | SW | AVEVA InTouch 태그 접근 | MFG | - | 비공개 | 해당 없음 |
| 138 | ODBC Client | SW | DB 테이블/쿼리/저장 프로시저를 태그로 매핑 | MFG | - | 공개 | P1 (Linux에서는 unixODBC 또는 네이티브 PostgreSQL/MySQL/MSSQL) |
| 139 | MQTT Client | E | MQTT 브로커 구독 → 태그 (JSON 페이로드) | MFG | - | 공개 (MQTT 3.1.1/5) | **P1** (Sparkplug B 포함 권장) |
| 140 | SNMP | E | SNMP v1/v2c/v3 장비 (스위치, UPS, PDU) | IT, SNMP | - | 공개 | P2 |
| 141 | Ping | E | ICMP 장비 생존 확인 | IT, SNMP | - | 공개 | P2 (구현 쉬움) |
| 142 | System Monitor | SW | 호스트 CPU/메모리/디스크/프로세스 | BA, IT | - | 해당 없음 | P1 (Linux /proc 기반, 구현 쉬움) |

### 2.15 단종 추정 (2018 데이터시트에는 있으나 7.x 도움말 색인에는 없음)

| Driver | 매체 | 비고 | 우리 우선순위 |
|---|---|---|---|
| Allen-Bradley Data Highway Plus | HW | 1784-PKTX 등 전용 카드. 리셀러 AB Suite 목록에는 아직 남아 있음 | P3 |
| Hilscher Universal | HW | Hilscher cifX 카드 경유 필드버스 | P3 (PROFINET/EtherCAT 마스터는 별도 검토) |
| SattBus Serial | S | SattBus Ethernet만 남음 | P3 |

---

## 3. 우선순위 요약 (국내 제조 시장 기준)

| 등급 | 드라이버 |
|---|---|
| **P0** (1차 출시) | Modbus TCP/IP Ethernet (마스터, 가능하면 슬레이브도), Modbus RTU Serial, Siemens TCP/IP Ethernet (S7comm), Mitsubishi Ethernet (MC 3E/1E), **LS XGT FEnet (Kepware 미지원, 차별화)**, OPC UA Client, Simulator, Memory Based |
| **P1** | Modbus ASCII, Modbus Server Serial, AB ControlLogix Ethernet, Omron FINS Ethernet, Omron NJ Ethernet, Mitsubishi Serial, Keyence KV Ethernet, **LS XGT Cnet**, **CIMON PLC**, Fanuc Focas Ethernet, MQTT Client (+Sparkplug B), ODBC/DB Client, U-CON 유사 사용자 정의 드라이버, 스크립트 드라이버(Universal Device 대응), Advanced Simulator, System Monitor |
| **P2** | Siemens S7 Plus (비공개 프로토콜, 우선은 S7-1200/1500의 OPC UA 서버로 대체), AB Ethernet (PLC-5/SLC/MicroLogix), AB Micro800 Ethernet, GE Ethernet, Beckhoff TwinCAT ADS, CODESYS, Yaskawa MP Ethernet, Toyopuc Ethernet, Panasonic(Aromat) Ethernet/Serial, Mitsubishi FX/FX Net, Omron Host Link/FINS Serial/CompoWay-F, Yokogawa Controller/DX/GX, Mettler Toledo, Torque Tool(Open Protocol), Mitsubishi CNC, MTConnect, EUROMAP 63, BACnet/IP, DNP3 (Ethernet/Serial), IEC 60870-5-104, IEC 61850 MMS, SNMP, Ping, WAGO, LS Master-K/GLOFA |
| **P3** | 나머지 레거시와 해외 니치 드라이버 (Siemens S5/MPI/S7-200, TI505, GE 시리얼, AutomationDirect, Cutler-Hammer, Honeywell, Yokogawa 레코더류, 석유·가스/EFM 전부, 전용 HW 카드 드라이버 등) |
| 해당 없음 | DDE Client, InTouch Client, OPC DA Client (Windows COM 전용이라 Linux에서는 의미 없음) |

참고: Kepware Edge가 Linux에서 제공하는 6개(AB ControlLogix, Modbus TCP, Siemens TCP/IP, S7 Plus, Mitsubishi Ethernet, OPC UA Client)는 PTC가 고른 "Linux 최소 셋"입니다. 우리는 S7 Plus 대신 **Modbus RTU(시리얼)와 LS XGT FEnet**을 P0에 넣어 국내 시장에 맞춥니다.

---

## 4. Suite 구성

| Suite | 포함 드라이버 | 확인 정도 |
|---|---|---|
| **Advanced Manufacturing Suite** | 모든 Kepware 드라이버 (+ DataLogger 등 일부 플러그인) | 리셀러 페이지 |
| **Manufacturing Suite** | 100~140개 이상. 제조용 PLC/CNC/OPC/MQTT/ODBC/시뮬레이터/U-CON 포함. **제외**: DNP3, BACnet, IEC 61850(그리고 IEC 60870 추정), Triconex, 석유·가스 드라이버 | Software Toolbox 페이지 |
| **Allen-Bradley Suite** | ControlLogix Ethernet, ControlLogix Unsolicited(=Server), Data Highway Plus, DF1, Ethernet, Micro800 Ethernet, Micro800 Serial, Unsolicited Ethernet(=Server Ethernet) | kepware-opc.cz |
| **Siemens Suite** | Siemens S5, S5 3964R, S7 MPI, S7-200, TCP/IP Ethernet, TCP/IP Unsolicited(=Server) Ethernet | Software Toolbox |
| **Siemens Plus Suite** | Siemens Suite + Siemens S7 Plus Ethernet + OPC UA Client | Software Toolbox, kepware-opc.cz |
| **Modbus Suite** | Modbus TCP/IP Ethernet, Modbus RTU Serial, Modbus ASCII, Modbus Server(Unsolicited) Serial, Modbus Plus | allied (부분), 미검증 |
| **Mitsubishi Suite** | Mitsubishi Ethernet, Serial, FX, FX Net (+ CNC Ethernet 포함 여부 미확인) | allied, 미검증 |
| **Omron Suite** | FINS Ethernet, FINS Serial, Host Link, NJ Ethernet, Process Suite, Toolbus | kepware-opc.cz |
| **GE Suite** | GE CCM, EGD, Ethernet, SNP, SNPX | allied, Software Toolbox |
| **Building Automation Suite** | BACnet/IP, Custom Interface, Modbus ASCII, Modbus Ethernet, Modbus Serial, System Monitor, U-CON (7개) | kepware-opc.cz (TOP Server도 7개) |
| **Power Suite** | DNP3 Master(Client) Ethernet/Serial, IEC 61850 MMS, IEC 60870-5-101, IEC 60870-5-104, Modbus ASCII, Modbus Ethernet, Modbus Serial (8개; TOP Server는 10개라고 표기) | kepware-opc.cz |
| **Oil and Gas Suite** | ABB Totalflow, Bristol/IP, Custom Interface, Enron Modbus, Fisher ROC Ethernet/Serial, ROC Plus Ethernet/Serial, Lufkin Modbus, Modbus ASCII/Ethernet/Serial, OMNI Flow Computer, U-CON, Weatherford 8500, WITS Level 0 Active/Passive (17개) + Media Level Redundancy 플러그인 | kepware-opc.cz, PTC 스토어 요약 |
| **IT and Infrastructure Suite** | SNMP, Ping, System Monitor, Modbus Ethernet/Serial/ASCII, Custom Interface, U-CON (8개) | Software Toolbox |
| **OPC Connectivity Suite** | OPC DA Client, OPC UA Client, OPC XML-DA Client | kepware-opc.cz |
| **EFM Suite** | EFM Exporter 플러그인 + EFM 지원 유량 컴퓨터 드라이버 (Totalflow, ROC, Enron, OMNI 등) | 미검증 |
| 벤더별 소형 Suite | Aromat, AutomationDirect, Contrex, Cutler-Hammer, DNP3, Fanuc Focas, Fisher ROC, Honeywell, IEC 60870-5, SattBus, Simatic(TI505), Simulation, SIXNET, SNMP, Thermo Westronics, Toshiba, Toyopuc, WITS, Yaskawa, Yokogawa: 이름 그대로 해당 벤더의 드라이버 묶음 | kepware-opc.cz (Suite 이름만 확인) |
| Kepware+ (구독) | 전 드라이버. Kepware Server(Windows)와 Kepware Edge(Linux) 모두 사용 가능 | ttpsc (2차 자료) |

---

## 5. 미검증 항목과 출처

### 5.1 미검증/주의 항목

1. **Manufacturing Suite의 정확한 포함 목록**: Software Toolbox 요약은 "DNP3, BACnet, IEC 61850, Triconex, O&G 제외"까지만 알려줍니다. SNMP, Universal Device, Alstom, Acroloop, Mitsubishi CNC, IEC 60870의 포함 여부는 확인하지 못했습니다(표의 `?`).
2. **Modbus Suite / Mitsubishi Suite 구성**은 공식 문서로 확인하지 못했습니다. allied 페이지의 요약을 근거로 했습니다.
3. **Power Suite 개수**: kepware-opc.cz는 8개, TOP Server는 "10 drivers"로 표기합니다. TOP Server 쪽에 Ping/SNMP 등이 더 들어 있을 가능성이 있습니다.
4. **Kepware Edge 드라이버**는 도움말 색인의 "for Edge" 항목 6개를 근거로 했습니다. 최신 Edge 릴리스에서 더 늘었을 수 있으니 Kepware Edge 릴리스 노트를 추가로 확인해야 합니다.
5. **단종 추정 3종**(DH+, Hilscher Universal, SattBus Serial)은 현행 도움말 색인에 없다는 사실만 근거입니다. 공식 단종 공지는 확인하지 못했습니다.
6. **U-CON**은 7.x 도움말 HTML 색인에 없지만, 2023년판 PDF 매뉴얼이 PTC 커뮤니티에 있고 Suite 구성에도 들어 있으므로 현행 드라이버로 셌습니다.
7. **"REST Client" 드라이버**: 일부 2차 자료에 나오지만 공식 색인에는 없습니다. Kepware의 REST/HTTP 기능은 IoT Gateway 플러그인(REST Client/Server Agent, MQTT Agent)과 Universal Device 드라이버가 맡습니다. 2차 자료(ttpsc)에 나오는 "PROFINET IO Controller", "EtherNet/IP 범용", "Sparkplug B" 드라이버도 **공식 색인에는 없으므로 믿지 마십시오**.
8. **지원 모델 열과 스펙 공개 열**은 각 드라이버 도움말 개요와 일반적인 업계 지식을 바탕으로 요약했습니다. 구현에 들어가기 전에 드라이버별 도움말 Overview 페이지와 제조사 매뉴얼로 다시 확인해야 합니다.
9. **LS XGT / CIMON 미지원**은 공식 색인 142개, 2018 데이터시트, TOP Server 174개 목록에 없다는 점(부재 증명)과 PTC 커뮤니티 사례를 근거로 했습니다. CIMON PLC 통신 프로토콜 문서가 공개되어 있는지는 따로 확인해야 합니다.

### 5.2 출처 URL

- PTC Kepware 드라이버 도움말 색인 (1차 근거, 전체 드라이버 목록): https://support.ptc.com/help/kepware/drivers/en/kepware.html
- 드라이버별 도움말 예: https://support.ptc.com/help/kepware/drivers/en/kepware/drivers/CONTROLLOGIXSERVER/Advanced_Addressing.html , https://support.ptc.com/help/kepware/drivers/en/kepware/drivers/BECKHOFFTWINCAT/Setup.html
- Kepware Server 7.0 What's New: https://support.ptc.com/help/kepware/kepware_server/en/kepware/server/whats_new/whats_new_7_0_236_0.html
- Kepware Server 7.1 What's New: https://support.ptc.com/help/kepware/kepware_server/en/kepware/server/whats_new/whats_new_7_1_271_0.html
- PTC 데이터시트 "Driver Options for KEPServerEX" (J7845, 2018): https://www.ats-global.com/wp-content/uploads/2023/10/Driver-Options-for-KEPServerEX.pdf
- Software Toolbox TOP Server 드라이버 목록 (174 SKU): https://softwaretoolbox.com/top-server/drivers-plugins-list
- TOP Server Manufacturing Suite (포함/제외): https://shop.softwaretoolbox.com/TOP-Server-Manufacturing-Suite-1-year-Subscription
- Allied Solutions Kepware 드라이버 목록 2026: https://www.alliedsolutionsglobal.com/en/news/kepware-driver-list-supported-plc-protocols-kepserverex
- kepware-opc.cz Suite 페이지: https://kepware-opc.cz/en/ovladace/single-opc-drivers/ (power-suite, oil-and-gas-suite, allen-bradley-suite, siemens-plus-suite, opc-connectivity-suite, omron-suite 하위 페이지), https://kepware-opc.cz/en/ovladace/jednotlive-opc-ovladace/building-automation-suite/
- PTC 스토어 Oil and Gas Suite: https://www.ptc.com/en/store/products/kepware/suites/oil-gas-suite (403 가능)
- Kepware Edge 제품 페이지: https://www.ptc.com/en/products/kepware/kepware-edge , https://softwaretoolbox.com/kepware-edge
- ttpsc Kepware 드라이버 블로그 (2차 자료, 오류 포함 주의): https://ttpsc.com/en/blog/kepware-drivers-list/
- PTC 커뮤니티 CIMON OPC UA 연동 이슈: https://community.ptc.com/t5/Kepware/KEPServerEX-OPC-UA-Client-Driver-Unable-to-Connect-to-CIMON-OPC/td-p/986087
- U-CON 드라이버 매뉴얼 (2023): https://community.ptc.com/sejnu66972/attachments/sejnu66972/kepware/2074/1/User-Configurable%20(U-CON)%20Driver.pdf
- LS ELECTRIC XGT FEnet (Pro-face 드라이버 페이지, 모델 범위 참고): https://www.proface.com/en/download/exdriver/plc/ls_xgte
