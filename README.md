# smart-pay · teste ESP32 ↔ api-teste

Sketch Arduino para validar a comunicação HTTPS entre um ESP32 e a [api-teste](https://api-teste.redesmartshop.com/docs) do smart-pay, que simula cobranças sem dinheiro real.

## Hardware e software

- ESP32 DOIT DevKit V1 (ESP32-WROOM-32). Na IDE: placa **DOIT ESP32 DEVKIT V1**.
- Arduino IDE 2 com o pacote **esp32 by Espressif Systems** (3.x) e a biblioteca **ArduinoJson** 7.
- Rede Wi-Fi 2.4 GHz.

## Como rodar

1. Copie `secrets.h.example` para `secrets.h` e preencha Wi-Fi e token. O `secrets.h` não vai para o git.
2. Grave na placa. Se travar em `Connecting...`, segure o botão BOOT.
3. Abra o Serial Monitor em 115200 baud e aperte EN.

No boot a placa conecta no Wi-Fi, sincroniza o relógio por NTP e chama `/health`. O certificado é validado com as raízes da Let's Encrypt em `root_ca.h`.

## Comandos (Serial Monitor)

| Tecla | Teste | Esperado |
|---|---|---|
| `h` | health | 200 |
| `a` | cobrança aprovada (também pelo botão BOOT) | 201 `approved` |
| `d` | cobrança recusada | 201 `declined` |
| `e` | erro simulado | 500, placa segue respondendo |
| `t` | timeout (API demora 8 s) | read timeout antes dos 8 s |
| `u` | token inválido | 401 |
| `i` | idempotência (mesma chave 2x) | 201 e depois 200 com o mesmo `charge_id` |
| `l` | listar cobranças | 200 |
| `s` | estresse, 20 cobranças | 20/20, com latência média |

### Crédito da máquina

A máquina é identificada por `MACHINE_ID` no sketch (padrão `99999`). O produto de teste é o `23`, por R$ 3,50 (`PRECO_PRODUTO`).

| Tecla | Teste | Esperado |
|---|---|---|
| `c` | consulta o crédito | `available` e valor em R$ |
| `r` | carrega R$ 5,00 (simula pagamento finalizado) | 200 |
| `v` | vende o produto (também pelo botão BOOT) | com crédito: LIBERADO, LED aceso 2 s, confirma `success`; sem crédito: 402, não libera |
| `f` | vende, mas o motor "trava" | LIBERADO, confirma `failed`, o crédito volta |
| `m` | liga/desliga o monitoramento | consulta a cada 3 s e avisa quando o crédito muda |

Sem resposta válida da API, a placa **não libera** o produto (fail-closed).

LED azul (GPIO 2): aceso 2 s = liberado, 1 piscada longa = aprovada, 3 rápidas = recusada ou sem crédito, 6 rápidas = erro.

## Resultado do teste (05/10/2026)

Todos os cenários passaram. Estresse 20/20 com latência média de 1,68 s e sinal de −82 dBm.
