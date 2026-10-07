# ESP32 + RFM95W — LoRaWAN Node Base (ABP · AU915 · ChirpStack)

> **Firmware base para nó LoRaWAN com ESP32 + RFM95W usando ativação ABP, stack MCCI LMiC v3.3.0, plano de canais AU915 sub-banda 1 e servidor de rede ChirpStack.**  
> Inclui persistência do frame counter em NVS, impressão do MIC no Serial, diagnósticos completos de sessão e **reset remoto via downlink** na FPort 101.

---

## Índice

- [Sobre o projeto](#sobre-o-projeto)
- [Hardware necessário](#hardware-necessário)
- [Mapeamento de pinos](#mapeamento-de-pinos)
- [Dependências de software](#dependências-de-software)
- [Instalação da biblioteca LMiC](#instalação-da-biblioteca-lmic)
- [Configuração das credenciais ABP](#configuração-das-credenciais-abp)
- [Plano de canais AU915 — sub-banda 1](#plano-de-canais-au915--sub-banda-1)
- [Persistência do frame counter em NVS](#persistência-do-frame-counter-em-nvs)
- [Reset remoto via downlink — FPort 101](#reset-remoto-via-downlink--fport-101)
- [Diagnósticos de sessão e MIC](#diagnósticos-de-sessão-e-mic)
- [Saída esperada no Serial Monitor](#saída-esperada-no-serial-monitor)
- [Cadastro no ChirpStack](#cadastro-no-chirpstack)
- [Problemas conhecidos e soluções](#problemas-conhecidos-e-soluções)
- [Estrutura do repositório](#estrutura-do-repositório)
- [Licença](#licença)

---

## Sobre o projeto

Este repositório contém o firmware base para um nó LoRaWAN construído com **ESP32** e módulo de rádio **RFM95W** (SX1276), operando com ativação **ABP** (*Activation By Personalization*) no plano de canais **AU915, sub-banda 1** (canais 8–15 + 65).

O código é parte de um projeto maior de medição de energia elétrica em tempo real usando uma rede LoRaWAN ponta a ponta, com backend em **Java/Spring**, frontend em **TypeScript/Angular** e aplicativo móvel em **Dart/Flutter**. Este repositório isola a camada de firmware do nó sensor, servindo como ponto de partida para qualquer aplicação que precise enviar dados via LoRaWAN com ESP32.

### O que este firmware faz

- Transmite um payload LoRaWAN a cada 10 segundos usando SF7/BW125 (DR5) com 14 dBm de potência.
- Usa **ABP** — sem procedimento de JOIN, sessão pré-provisionada com DevAddr + NwkSKey + AppSKey.
- Persiste o frame counter (`seqnoUp`) na **NVS** (Non-Volatile Storage) do ESP32 entre reboots, evitando o erro `Frame-counter reset or rollover detected` no ChirpStack.
- Imprime o **MIC** (*Message Integrity Code*) de cada frame no Serial Monitor para diagnóstico.
- Imprime as chaves de sessão, DevAddr, frame raw e contadores FCnt/seqnoUp a cada transmissão.
- Recebe e exibe downlinks do servidor nas janelas RX1/RX2 (Class A).
- **Processa comandos de reset remoto** recebidos na FPort 101 com payload de 8 bytes estruturado (header + command + tail).
- Configura corretamente o plano de canais AU915 sub-banda 1 e a janela RX2 (923.3 MHz / SF12/BW500).

### Por que ABP e não OTAA?

| | ABP | OTAA |
|---|---|---|
| JOIN procedure | Não — sessão pré-provisionada | Sim — troca de chaves over-the-air |
| Primeira transmissão | Imediata | Após JOIN Accept |
| Segurança | Menor (chaves fixas) | Maior (chaves derivadas por sessão) |
| Ideal para | Desenvolvimento e testes | Produção |
| Frame counter | Requer persistência em NVS | Gerenciado automaticamente |

ABP é recomendado durante o desenvolvimento por eliminar a dependência do JOIN e simplificar o diagnóstico de problemas de comunicação. Para produção, migre para OTAA.

---

## Hardware necessário

| Componente | Especificação | Observação |
|---|---|---|
| Microcontrolador | ESP32 (qualquer variante com 4 MB de flash) | Testado com ESP32-DevKitC |
| Módulo de rádio | RFM95W (HopeRF) — SX1276 | Frequência 915 MHz |
| Antena | 1/4 de onda para 915 MHz (~8,2 cm) | Obrigatória — nunca transmita sem antena |
| Cabo / conector | SMA ou u.FL dependendo do módulo | — |

### Diagrama de conexão ESP32 ↔ RFM95W

```
ESP32           RFM95W
─────────────────────────────────
GPIO 18 (SCLK) → SCK
GPIO 19 (MISO) → MISO
GPIO 23 (MOSI) → MOSI
GPIO  5 (NSS)  → NSS / CS
GPIO 14        → RESET
GPIO 34        → DIO0
GPIO 35        → DIO1
3.3 V          → VCC
GND            → GND
```

> ⚠️ **GPIOs 34, 35 e 39 no ESP32 são input-only** — não possuem resistor de pull-up interno. São adequados para DIO0 e DIO1 do RFM95W pois essas linhas são saídas do rádio (o ESP32 apenas lê). Nunca use esses pinos como saída.

---

## Mapeamento de pinos

Definido no sketch:

```cpp
#define PIN_SCLK    18
#define PIN_MISO    19
#define PIN_MOSI    23
#define PIN_NSS      5
#define PIN_RESET   14
#define PIN_DIO0    34
#define PIN_DIO1    35   // conectado mas não usado pelo LMiC nesta placa
#define PIN_DIO2    39   // não conectado — mantido como referência
```

Ajuste conforme sua montagem antes de compilar.

---

## Dependências de software

| Software | Versão | Link |
|---|---|---|
| Arduino IDE | 2.x | [arduino.cc/en/software](https://www.arduino.cc/en/software) |
| ESP32 Arduino core | 3.3.8 | [github.com/espressif/arduino-esp32](https://github.com/espressif/arduino-esp32) |
| MCCI LoRaWAN LMIC | **3.3.0** ← obrigatório | Ver seção abaixo |

> ⚠️ **A versão 6.x da biblioteca LMiC é incompatível com ABP LoRaWAN 1.0.x.** Ela introduz derivação interna de chaves para LoRaWAN 1.1 que faz o MIC calculado pelo dispositivo nunca bater com o esperado pelo ChirpStack — os frames chegam ao gateway mas são descartados silenciosamente com a mensagem `None of the device-sessions for dev_addr resulted in valid MIC`. Use obrigatoriamente a **v3.3.0**.

---

## Instalação da biblioteca LMiC

### Opção A — Repositório com a versão correta (recomendado)

```bash
cd ~/Arduino/libraries

git clone \
  https://github.com/AdailSilva/IBM-LMIC-LoRaWAN-MAC-in-C-library-v3.3.0.git \
  MCCI_LoRaWAN_LMIC_library
```

### Opção B — Repositório upstream com tag v3.3.0

```bash
cd ~/Arduino/libraries

git clone --branch v3.3.0 \
  https://github.com/mcci-catena/arduino-lmic.git \
  MCCI_LoRaWAN_LMIC_library
```

### Correção obrigatória — conflito `hal_init` com ESP32 core 3.x

O ESP32 Arduino core 3.x define `hal_init` internamente, conflitando com a mesma função na LMiC v3.3.0. Corrija com dois comandos `sed`:

```bash
# Renomear hal_init no arquivo de implementação
sed -i 's/void hal_init(/void lmic_hal_init(/g' \
  ~/Arduino/libraries/MCCI_LoRaWAN_LMIC_library/src/hal/hal.cpp

# Atualizar as chamadas internas
sed -i 's/hal_init()/lmic_hal_init()/g' \
  ~/Arduino/libraries/MCCI_LoRaWAN_LMIC_library/src/lmic/lmic.c \
  ~/Arduino/libraries/MCCI_LoRaWAN_LMIC_library/src/hal/hal.cpp
```

Após isso, compile normalmente pelo Arduino IDE — o erro `multiple definition of 'hal_init'` não deve mais aparecer.

### Configuração da região no `lmic_project_config.h`

Edite o arquivo de configuração da biblioteca para habilitar apenas a região AU915, reduzindo o tamanho do binário:

```cpp
// ~/Arduino/libraries/MCCI_LoRaWAN_LMIC_library/project_config/lmic_project_config.h

#define CFG_au915 1
// #define CFG_eu868 1   // comentar
// #define CFG_us915 1   // comentar
// #define CFG_as923 1   // comentar

#define LMIC_USE_INTERRUPTS
```

---

## Configuração das credenciais ABP

Edite as três constantes no topo do sketch com os valores obtidos do ChirpStack:

```cpp
// DevAddr — 4 bytes, big-endian (ex: 0x00aa88ca)
static const u4_t DEVADDR = 0x00000000; // <-- substitua

// NwkSKey — Network Session Key, 16 bytes, big-endian
static const u1_t PROGMEM NWKSKEY[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00  // <-- substitua
};

// AppSKey — Application Session Key, 16 bytes, big-endian
static const u1_t PROGMEM APPSKEY[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00  // <-- substitua
};
```

**Onde encontrar no ChirpStack:**

> Applications → sua application → Devices → seu device → **Activation**

Os valores são exibidos em hexadecimal sem separadores. Para converter para o formato do array C, insira `0x` e vírgula a cada 2 caracteres:

```
ChirpStack: 151F7779D7AAEE422A74568380DB7045
Código C:   { 0x15, 0x1F, 0x77, 0x79, 0xD7, 0xAA, 0xEE, 0x42,
              0x2A, 0x74, 0x56, 0x83, 0x80, 0xDB, 0x70, 0x45 }
```

### netID — ChirpStack vs TTN

```cpp
// ChirpStack (padrão deste projeto)
LMIC_setSession(0x000000, DEVADDR, nwkKey, artKey);

// The Things Network (TTN)
LMIC_setSession(0x000013, DEVADDR, nwkKey, artKey);
```

---

## Plano de canais AU915 — sub-banda 1

O AU915 define 72 canais de uplink organizados em 8 sub-bandas de 9 canais cada (8 × BW125 + 1 × BW500). Este firmware opera na **sub-banda 1** (canais 8–15 + 65), adotada como padrão pela comunidade brasileira e configurada no ChirpStack com `region_config = au915_1`.

| Canal | Frequência (MHz) | BW (kHz) | DR suportados | Uso |
|---|---|---|---|---|
| 8 | 916.8 | 125 | DR0–DR5 | Uplink dados |
| 9 | 917.0 | 125 | DR0–DR5 | Uplink dados |
| 10 | 917.2 | 125 | DR0–DR5 | Uplink dados |
| 11 | 917.4 | 125 | DR0–DR5 | Uplink dados |
| 12 | 917.6 | 125 | DR0–DR5 | Uplink dados |
| 13 | 917.8 | 125 | DR0–DR5 | Uplink dados |
| 14 | 918.0 | 125 | DR0–DR5 | Uplink dados |
| 15 | 918.2 | 125 | DR0–DR5 | Uplink dados |
| 65 | 917.5 | 500 | DR6 | JOIN/ADR |
| **RX2** | **923.3** | **500** | **DR8 (SF12)** | **Downlink fixo** |

O firmware configura o plano desabilitando todos os canais e habilitando apenas os da sub-banda 1:

```cpp
for (u1_t b  =  0; b  <  8; ++b)   LMIC_disableSubBand(b);
for (u1_t ch =  0; ch < 72; ++ch)  LMIC_disableChannel(ch);
for (u1_t ch =  8; ch <= 15; ++ch) LMIC_enableChannel(ch);  // BW125
LMIC_enableChannel(65);                                       // BW500

LMIC_setDrTxpow(DR_SF7, 14);      // DR5 = SF7/BW125, 14 dBm
LMIC.dn2Dr    = DR_SF12CR;        // RX2 = SF12/BW500 (DR8) — AU915
LMIC_setClockError(MAX_CLOCK_ERROR * 1 / 100);  // 1% — ESP32 XTAL
```

> ⚠️ **Gateway e dispositivo devem operar na mesma sub-banda.** Se o gateway escuta a sub-banda 0 (canais 0–7 + 64) e o dispositivo transmite na sub-banda 1, os frames chegam ao gateway mas o ChirpStack não consegue associá-los a nenhum device.

---

## Persistência do frame counter em NVS

Em ABP, o LMiC mantém o frame counter (`seqnoUp`) apenas na SRAM — que é **volátil**. Após qualquer reset, o contador volta a zero. O ChirpStack rejeita frames com contador regressivo por proteção anti-replay, registrando:

```
Frame-counter reset or rollover detected
```

A solução implementada neste firmware é persistir o `seqnoUp` na **NVS** (*Non-Volatile Storage*) do ESP32 — uma partição da flash interna que sobrevive a reboots e cortes de energia — via `Preferences.h`, a API nativa do ESP32 para acesso ao NVS.

```
NVS (flash interna ESP32)
└── namespace: "lmic"
    └── chave: "seqnoUp"  →  valor: 47  (uint32)
```

O firmware salva o contador após cada transmissão (`EV_TXCOMPLETE`) e o restaura no boot, sempre **após** `LMIC_setSession()` — que reseta `seqnoUp` para 0 internamente:

```cpp
// setup() — ordem obrigatória
prefs.begin("lmic", false);           // abre namespace NVS
LMIC_setSession(0x000000, ...);       // reseta seqnoUp para 0
LMIC.seqnoUp = loadSeqnoUp();         // restaura do NVS (sobrescreve o 0)

// EV_TXCOMPLETE — salvar após cada TX
saveSeqnoUp();                        // persiste o próximo contador
```

**Sequência com NVS ativo:**

```
Boot 1:  seqnoUp = 0 → 1 → 2 → ... → 47   NVS salva 48
Reset
Boot 2:  NVS restaura seqnoUp = 48
         FCnt = 48 → ChirpStack aceita ✅
```

---

## Reset remoto via downlink — FPort 101

O firmware implementa um protocolo de **reset remoto** que permite reiniciar o ESP32 enviando um downlink específico pelo ChirpStack. Útil em campo, quando o dispositivo está instalado em local de difícil acesso e precisa ser reiniciado para limpar algum estado interno ou forçar uma nova provisão.

### Protocolo do comando

O comando de reset usa uma porta e estrutura de payload dedicados para evitar resets acidentais por qualquer downlink genérico.

| Campo | Valor |
|---|---|
| **FPort** | `101` (0x65) |
| **Tamanho do payload** | `8 bytes` exatos |

**Estrutura do payload (8 bytes):**

```
Byte:   0     1     2     3     4     5     6     7
       ┌─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────┐
       │ HDR │ HDR │ CMD │ CMD │ CMD │ CMD │TAIL │TAIL │
       │0xAD │0xA1 │0xDE │0xAD │0xBE │0xEF │0x0D │0x0A │
       └─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┘
         \_______/   \___________________________/  \___/
          Header             Command               Tail
         "AdAil"           Magic word             CRLF
```

| Segmento | Bytes | Valor | Significado |
|---|---|---|---|
| **Header** | 0–1 | `0xAD 0xA1` | Assinatura "**Ad**ail Silv**a1**" |
| **Command** | 2–5 | `0xDE 0xAD 0xBE 0xEF` | Magic word de reset (DEADBEEF) |
| **Tail** | 6–7 | `0x0D 0x0A` | Marcador de fim de frame (CRLF) |

**Representação em Base64** (necessário para enviar via MQTT/ChirpStack):

```bash
# Verificar o payload
printf '\xAD\xA1\xDE\xAD\xBE\xEF\x0D\x0A' | xxd
# 00000000: ada1 dead beef 0d0a

# Valor Base64 para usar no ChirpStack / MQTT:
# raHerb7vDQo=
```

### Regras de validação no firmware

O firmware verifica **todos** os bytes antes de executar o reset:

```cpp
static bool isResetCommand(const uint8_t *data, uint8_t len) {
    if (len != RESET_CMD_LEN) return false;  // tamanho exato: 8 bytes

    return (data[0] == 0xAD &&   // Header byte 0
            data[1] == 0xA1 &&   // Header byte 1
            data[2] == 0xDE &&   // Command byte 0
            data[3] == 0xAD &&   // Command byte 1
            data[4] == 0xBE &&   // Command byte 2
            data[5] == 0xEF &&   // Command byte 3
            data[6] == 0x0D &&   // Tail byte 0 (CR)
            data[7] == 0x0A);    // Tail byte 1 (LF)
}
```

- FPort diferente de 101 → downlink exibido normalmente, comando ignorado.
- Tamanho diferente de 8 bytes → comando ignorado.
- Qualquer byte divergente → comando ignorado, payload esperado impresso no Serial.
- Payload correto → `saveSeqnoUp()` salva o contador na NVS, depois `esp_restart()` é chamado após `2000 ms`.

### Detalhe ABP — o frame counter é salvo antes do reset

No projeto ABP, o `saveSeqnoUp()` é chamado **antes** do `esp_restart()` dentro de `handleDownlink()`, garantindo que o contador persistido seja válido mesmo após o reboot induzido pelo comando de reset:

```cpp
// Em EV_TXCOMPLETE — ordem garantida:
handleDownlink();   // (1) processa reset — salva NVS antes de reiniciar
saveSeqnoUp();      // (2) salva NVS para o próximo ciclo normal
```

### Como disparar o reset pelo MQTT

**ChirpStack v4 — com TLS:**

```bash
APP_ID="56560a65-2fb8-444c-a1ee-bd0ee4be0946"
DEV_EUI="08fdbe4bc604e8f8"

mosquitto_pub \
  -h chirpstack-v4.adailsilva.com.br \
  -p 8883 \
  --cafile ~/chirpstack/mqtt-certs/ca.crt \
  -u "adailsilva" \
  -P "H@cker101" \
  -t "application/${APP_ID}/device/${DEV_EUI}/command/down" \
  -m '{
    "devEui": "08fdbe4bc604e8f8",
    "confirmed": false,
    "fPort": 101,
    "data": "raHerb7vDQo="
  }'
```

**Via interface web do ChirpStack:**

> Devices → seu device → **Queue** → Add item
> - FPort: `101`
> - Confirmed: `false`
> - Base64 data: `raHerb7vDQo=`

O downlink ficará na fila até o próximo uplink do dispositivo (Class A). Após receber, o ESP32 salva o frame counter na NVS e reinicia em ~2 segundos.

### Saída no Serial Monitor — reset bem-sucedido

```
159750: EV_TXCOMPLETE (includes waiting for RX windows)
  Downlink received — 8 byte(s) on FPort 101: AD A1 DE AD BE EF 0D 0A
  [RESET PORT] Command received on FPort 101 — validating...
  [RESET] Valid reset command — rebooting in 2 s...

ets Jun  8 2016 00:22:57
rst:0xc (SW_CPU_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)
...
Starting — ChirpStack AU915 ABP
ABP session provisioned:
  DevAddr : 0xAA88CA
  NwkSKey : 15-1F-77-79-...
  AppSKey : FD-91-D5-5D-...
  seqnoUp : 48          ← contador restaurado do NVS após o reset ✅
```

### Saída no Serial Monitor — payload inválido

```
159750: EV_TXCOMPLETE (includes waiting for RX windows)
  Downlink received — 8 byte(s) on FPort 101: AD A1 DE AD BE EF 0D FF
  [RESET PORT] Command received on FPort 101 — validating...
  [RESET] Invalid payload — command ignored.
  Expected: AD A1 DE AD BE EF 0D 0A
  Received: AD A1 DE AD BE EF 0D FF
```

### Personalização do protocolo

Todos os bytes e parâmetros são definidos como constantes no topo do sketch:

```cpp
#define RESET_FPORT      101    // FPort dedicada ao comando
#define RESET_CMD_LEN      8    // tamanho exato do payload
#define RESET_HDR_0     0xAD    // Header byte 0
#define RESET_HDR_1     0xA1    // Header byte 1
#define RESET_CMD_0     0xDE    // Command byte 0 (DEADBEEF)
#define RESET_CMD_1     0xAD    // Command byte 1
#define RESET_CMD_2     0xBE    // Command byte 2
#define RESET_CMD_3     0xEF    // Command byte 3
#define RESET_TAIL_0    0x0D    // Tail byte 0 — CR
#define RESET_TAIL_1    0x0A    // Tail byte 1 — LF
#define RESET_DELAY_MS  2000    // delay antes do esp_restart()
```

---

## Diagnósticos de sessão e MIC

A cada transmissão (`EV_TXSTART`), o firmware imprime no Serial Monitor:

- **NwkSKey** e **AppSKey** que o LMiC está de fato usando internamente
- **DevAddr** em hex
- **Frame raw** completo em hex (MHDR + FHDR + FPort + FRMPayload + MIC)
- **FCnt** (seqnoUp e seqnoDn)
- **MIC** isolado — últimos 4 bytes do frame

O MIC impresso pode ser comparado diretamente com o campo `mic` do JSON capturado no gateway ChirpStack. O JSON exibe o MIC em decimal — converta para hex:

```bash
# Converter array decimal do JSON para hex
printf '%02X-%02X-%02X-%02X\n' 20 46 220 190
# → 14-2E-DC-BE
```

Se o MIC do Serial bater com o do gateway, o ChirpStack aceita o frame. Se não bater, há divergência de chaves entre o dispositivo e o servidor.

### Cálculo manual do MIC

O MIC é calculado com **AES-128-CMAC** sobre o frame completo precedido de um bloco B0 de 16 bytes:

```
MIC = AES-128-CMAC(NwkSKey, B0 | msg)[0:4]

B0  = 0x49 | 0x00×4 | Dir=0 | DevAddr(LE,4) | FCnt(LE,4) | 0x00 | len(msg)
msg = MHDR | DevAddr(LE,4) | FCtrl | FCnt(LE,2) | FPort | FRMPayload
```

**Python** (`pip install pycryptodome`):

```python
from Crypto.Hash import CMAC
from Crypto.Cipher import AES
import struct

def calcular_mic(nwkskey_hex, devaddr_hex, fcnt, frame_sem_mic_hex):
    nwkskey    = bytes.fromhex(nwkskey_hex)
    devaddr_le = bytes.fromhex(devaddr_hex)[::-1]   # big-endian → little-endian
    msg        = bytes.fromhex(frame_sem_mic_hex)

    b0 = (bytes([0x49, 0x00, 0x00, 0x00, 0x00, 0x00])
          + devaddr_le
          + struct.pack('<I', fcnt)
          + bytes([0x00, len(msg)]))

    c = CMAC.new(nwkskey, ciphermod=AES)
    c.update(b0 + msg)
    return c.digest()[:4].hex()

mic = calcular_mic(
    nwkskey_hex       = "151f7779d7aaee422a74568380db7045",
    devaddr_hex       = "00aa88ca",
    fcnt              = 0,
    frame_sem_mic_hex = "40CA88AA0000000001F56CB7FFFA9C7C0D6C6C72368ECD"
)
print(f"MIC: {mic}")  # → 142edcbe
```

**Java** (sem dependências externas — usa `javax.crypto` da JDK):

```java
import javax.crypto.Cipher;
import javax.crypto.spec.IvParameterSpec;
import javax.crypto.spec.SecretKeySpec;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.HexFormat;

public class LoRaWANMic {

    public static String calcularMic(String nwksKeyHex,
                                     String devAddrHex,
                                     int fcnt,
                                     String frameWithoutMicHex) throws Exception {
        HexFormat hex = HexFormat.of();
        byte[] nwksKey   = hex.parseHex(nwksKeyHex);
        byte[] devAddrBe = hex.parseHex(devAddrHex);
        byte[] msg       = hex.parseHex(frameWithoutMicHex);

        // DevAddr em little-endian (invertido)
        byte[] devAddrLe = new byte[]{ devAddrBe[3], devAddrBe[2], devAddrBe[1], devAddrBe[0] };

        // FCnt em little-endian 32-bit
        byte[] fcntLe = ByteBuffer.allocate(4)
                                  .order(ByteOrder.LITTLE_ENDIAN)
                                  .putInt(fcnt)
                                  .array();

        // Monta o bloco B0 (16 bytes)
        byte[] b0 = new byte[16];
        b0[0]  = 0x49;
        b0[1]  = 0x00; b0[2] = 0x00; b0[3] = 0x00; b0[4] = 0x00;
        b0[5]  = 0x00; // Dir = 0 (uplink)
        b0[6]  = devAddrLe[0]; b0[7]  = devAddrLe[1];
        b0[8]  = devAddrLe[2]; b0[9]  = devAddrLe[3];
        b0[10] = fcntLe[0];    b0[11] = fcntLe[1];
        b0[12] = fcntLe[2];    b0[13] = fcntLe[3];
        b0[14] = 0x00;
        b0[15] = (byte) msg.length;

        // Concatena B0 || msg
        byte[] data = new byte[b0.length + msg.length];
        System.arraycopy(b0,  0, data, 0,         b0.length);
        System.arraycopy(msg, 0, data, b0.length, msg.length);

        // AES-128-CMAC (RFC 4493) — sem dependências externas
        byte[] mic = aesCmac(nwksKey, data);
        return hex.formatHex(mic, 0, 4);
    }

    // AES-128-CMAC conforme RFC 4493
    private static byte[] aesCmac(byte[] key, byte[] data) throws Exception {
        byte[] L  = aesCbc(key, new byte[16], new byte[16]);
        byte[] K1 = generateSubkey(L);
        byte[] K2 = generateSubkey(K1);

        int blockCount = (data.length + 15) / 16;
        boolean lastBlockComplete = (data.length > 0) && (data.length % 16 == 0);
        if (blockCount == 0) { blockCount = 1; lastBlockComplete = false; }

        byte[] lastBlock = new byte[16];
        if (lastBlockComplete) {
            System.arraycopy(data, (blockCount - 1) * 16, lastBlock, 0, 16);
            xorBlock(lastBlock, K1);
        } else {
            int sz = data.length % 16;
            System.arraycopy(data, (blockCount - 1) * 16, lastBlock, 0, sz);
            lastBlock[sz] = (byte) 0x80;
            xorBlock(lastBlock, K2);
        }

        byte[] x = new byte[16];
        for (int i = 0; i < blockCount - 1; i++) {
            byte[] block = new byte[16];
            System.arraycopy(data, i * 16, block, 0, 16);
            xorBlock(block, x);
            x = aesCbc(key, new byte[16], block);
        }
        xorBlock(lastBlock, x);
        return aesCbc(key, new byte[16], lastBlock);
    }

    private static byte[] aesCbc(byte[] key, byte[] iv, byte[] data) throws Exception {
        Cipher cipher = Cipher.getInstance("AES/CBC/NoPadding");
        cipher.init(Cipher.ENCRYPT_MODE,
                    new SecretKeySpec(key, "AES"),
                    new IvParameterSpec(iv));
        return cipher.doFinal(data);
    }

    private static byte[] generateSubkey(byte[] input) {
        byte[] output = new byte[16];
        boolean msb = (input[0] & 0x80) != 0;
        for (int i = 0; i < 15; i++) {
            output[i] = (byte) ((input[i] << 1) | ((input[i + 1] & 0xFF) >> 7));
        }
        output[15] = (byte) (input[15] << 1);
        if (msb) output[15] ^= 0x87;
        return output;
    }

    private static void xorBlock(byte[] a, byte[] b) {
        for (int i = 0; i < 16; i++) a[i] ^= b[i];
    }

    public static void main(String[] args) throws Exception {
        String mic = calcularMic(
            "151f7779d7aaee422a74568380db7045",  // NwkSKey
            "00aa88ca",                           // DevAddr (big-endian)
            0,                                    // FCnt
            "40CA88AA0000000001F56CB7FFFA9C7C0D6C6C72368ECD"
        );
        System.out.println("MIC: " + mic);  // → 142edcbe
    }
}
```

> Para compilar e executar: `javac LoRaWANMic.java && java LoRaWANMic`
> Requer Java 17+ para `HexFormat`. Em versões anteriores, substitua `HexFormat.of().parseHex(...)` por `javax.xml.bind.DatatypeConverter.parseHexBinary(...)`.

---

## Saída esperada no Serial Monitor

Configure o Serial Monitor para **9600 baud**. Na inicialização e a cada uplink:

```
Starting — ChirpStack AU915 ABP
ABP session provisioned:
  DevAddr: 0xAA88CA
  AppSKey: FD-91-D5-5D-CE-3A-F1-02-30-93-E2-89-A1-D9-F3-DF
  NwkSKey: 15-1F-77-79-D7-AA-EE-42-2A-74-56-83-80-DB-70-45
25396: EV_TXSTART
LMiC NwkSKey: 151F7779D7AAEE422A74568380DB7045
LMiC AppSKey: FD91D55DCE3AF1023093E289A1D9F3DF
LMiC DevAddr: 0xAA88CA

Frame raw: 40 CA 88 AA 00 00 00 00 01 F5 6C B7 FF FA 9C 7C 0D 6C 6C 72 36 8E CD 14 2E DC BE
FCnt/seqnoUp: 1
FCnt/seqnoDn: 0
  MIC: 14-2E-DC-BE
Packet queued.
159750: EV_TXCOMPLETE (includes waiting for RX windows)
  seqnoUp saved to NVS: 1
```

---

## Cadastro no ChirpStack

### Device Profile (ABP)

| Campo | Valor |
|---|---|
| Region | `AU915` |
| Region configuration | `au915_1` |
| MAC version | `LoRaWAN 1.0.3` |
| Regional parameters revision | `B` |
| Supports OTAA | ☐ Não |
| RX1 delay | `1` |
| RX1 DR offset | `0` |
| RX2 DR | `8` |
| RX2 frequency (Hz) | `923300000` |
| Supports Class-C | ☐ Não |

### Activation (ABP)

| Campo | Valor |
|---|---|
| Device address | valor de `DEVADDR` no firmware |
| Network session key | bytes de `NWKSKEY` no firmware |
| Application session key | bytes de `APPSKEY` no firmware |
| Uplink frame-counter | `0` (primeiro boot) |

Após preencher, clique em **(Re)activate device**.

### Frame counter validation

Durante o desenvolvimento, habilite em:

> Devices → seu device → Configuration → ✅ **Disable frame-counter validation**

Em produção, **nunca desabilite** — use a persistência em NVS implementada neste firmware.

---

## Problemas conhecidos e soluções

### ❌ `multiple definition of 'hal_init'`

**Causa:** conflito entre ESP32 Arduino core 3.x e LMiC v3.3.0.

**Solução:** aplicar o patch `sed` descrito na seção [Instalação da biblioteca LMiC](#instalação-da-biblioteca-lmic).

---

### ❌ MIC inválido — frames chegam ao gateway mas não aparecem nos Events

**Causa:** uso da biblioteca LMiC **v6.x**, que deriva chaves internas para LoRaWAN 1.1 e calcula o MIC com uma chave diferente da NwkSKey cadastrada no ChirpStack. O Serial Monitor mostra as chaves corretas — mas a biblioteca usa uma chave derivada internamente.

**Log do ChirpStack:**
```
None of the device-sessions for dev_addr resulted in valid MIC
```

**Solução:** fazer downgrade para a **v3.3.0**.

Repositórios para referência:
- v3.3.0 (correta): [github.com/AdailSilva/IBM-LMIC-LoRaWAN-MAC-in-C-library-v3.3.0](https://github.com/AdailSilva/IBM-LMIC-LoRaWAN-MAC-in-C-library-v3.3.0)
- v6.0.1 (problemática): [github.com/AdailSilva/IBM-LMIC-LoRaWAN-MAC-in-C-library-v6.0.1](https://github.com/AdailSilva/IBM-LMIC-LoRaWAN-MAC-in-C-library-v6.0.1)

---

### ❌ `Frame-counter reset or rollover detected`

**Causa:** `seqnoUp` voltou a zero após um reset do ESP32 — persistência em NVS não está funcionando.

**Verificação:** no Serial Monitor, após o boot o valor de `seqnoUp` deve ser maior que zero se já houve transmissões anteriores. Se aparecer `0`, verifique se `prefs.begin("lmic", false)` está sendo chamado antes de qualquer operação LMiC, e se `saveSeqnoUp()` está dentro de `EV_TXCOMPLETE`.

---

### ❌ `UPLINK_F_CNT_RETRANSMISSION`

**Causa:** `LMIC_clrTxData()` sendo chamado no `do_send()`, ou `LMIC.seqnoUp = 0` sendo setado manualmente após a restauração do NVS — ambos causam retransmissão com o mesmo FCnt.

**Solução:** remova qualquer chamada a `LMIC_clrTxData()` e qualquer atribuição manual de `LMIC.seqnoUp = 0` após `loadSeqnoUp()`.

---

### ❌ Downlinks não chegam ao dispositivo

**Causa mais comum:** Device Profile com `Supports Class-C → yes` no ChirpStack. Quando ativo, o servidor envia downlinks com timing `"immediately"` — sem aguardar as janelas RX1/RX2. Dispositivos Class A nunca recebem nesses downlinks imediatos.

**Solução:** desabilite `Supports Class-C` no Device Profile e limpe a fila de downlinks do device antes de testar novamente.

---

## Estrutura do repositório

```
.
├── LoRaWAN_Node_Base_ABP/
│   └── LoRaWAN_Node_Base_ABP.ino   ← sketch principal
├── .gitattributes
├── LICENSE
└── README.md
```

---

## Licença

MIT — veja [LICENSE](LICENSE) para detalhes.

---

**Autor:** Adail dos Santos Silva  
**E-mail:** adail101@hotmail.com  
**WhatsApp:** +55 89 9 9412-9256
