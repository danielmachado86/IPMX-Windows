# IPMX Windows

Implementación Windows/C++ de transporte H.264 sobre RTP multicast para IPMX, incluidas las Fases
1–3. Produce H.264 Main o High 4:2:0/8-bit con VBR-HRD, VUI/SEI normativos, RTP limitado, SDP
`TP=2110TPW` e instrumentación del tráfico de salida específica de Windows.

```text
patrón o WGC -> BGRA -> NV12 -> x264 High/8-bit/4:2:0/sin B-frames
              -> Annex B -> RFC 6184 (Single NAL/FU-A) -> RTP multicast
              -> Annex B -> Media Foundation -> D3D11
```

La organización se inspira en [IPMX-machine](https://github.com/danielmachado86/IPMX-machine):
un núcleo reutilizable, aplicaciones separadas, pruebas del núcleo y scripts de operación. Aquí se
aplican convenciones propias de Windows, MSVC, CMake y vcpkg.

## Estructura

```text
apps/
  ipmx-sender/             WGC, patrón de prueba y x264
  ipmx-receiver/           Media Foundation y render D3D11
  ipmx-load/               carga artificial de CPU y GPU D3D11
src/ipmx-core/             Annex B, RTP/RTCP, SDP, shaping, PCAP, Winsock, color y métricas
tests/IPMXCoreTests/       pruebas unitarias del núcleo
tests/IPMXConformanceTests/ x264/MF, golden bitstream y PCAP de conformidad
cmake/                     configuración compartida de compilación
scripts/                   build, loopback y estabilidad
specs/                     documentos VSF de referencia
```

### Límites entre componentes

`ipmx_core` no conoce Windows Graphics Capture, D3D11, Media Foundation ni x264. Contiene lógica
transportable y comprobable mediante pruebas unitarias. `ipmx_sender` y `ipmx_receiver` actúan como
adaptadores Windows y sus headers no forman parte de la API instalada. Los codecs viven en las
bibliotecas estáticas `ipmx_sender_codec` e `ipmx_receiver_codec`, compartidas por aplicaciones y
pruebas sin recompilar sus fuentes.

Los headers públicos se incluyen desde `ipmx/<componente>.hpp`. El núcleo vive en `namespace ipmx`
con `inline namespace v0`: los consumidores escriben `ipmx::RtpPacketizer`, mientras que
`ipmx::v0::RtpPacketizer` conserva una versión ABI explícita durante la evolución del prototipo.
Los adaptadores específicos de las aplicaciones viven en `ipmx::sender` e `ipmx::receiver` y no
forman parte de la API instalada.

La dirección permitida de las dependencias es:

```text
IPMXCoreTests --> ipmx_core
IPMXConformanceTests --> ipmx_sender_codec + ipmx_receiver_codec
ipmx_sender_codec   --> ipmx_core + x264
ipmx_receiver_codec --> ipmx_core + Media Foundation
ipmx_sender         --> ipmx_sender_codec + WGC/D3D11
ipmx_receiver       --> ipmx_receiver_codec + D3D11
```

No se permiten dependencias en sentido contrario. Las futuras implementaciones de PTP, NMOS u otros
servicios de transporte deben incorporarse como módulos del núcleo o servicios separados, no dentro de los
archivos `main.cpp`.

### Convenciones Windows

- C++20, Unicode y Windows 10 como versión mínima de API.
- MSVC con `/W4`, modo conforme, UTF-8, SDL y protecciones del enlazador.
- Dependencias declaradas en `vcpkg.json`; no se versionan rutas absolutas.
- CMake/Ninja para compilaciones locales y CI; los artefactos viven bajo `out/`.
- `scripts/build.ps1` descubre Visual Studio y vcpkg. Cuando el checkout contiene espacios, monta
  temporalmente una letra de unidad porque el sistema de build Unix de x264 no instala
  correctamente desde esas rutas.
- Pruebas mediante CTest y automatización en GitHub Actions sobre Windows.

### Nombres permanentes y fases del proyecto

El código nombra **qué contrato implementa**, no **en qué fase del roadmap se añadió**. No se deben
usar identificadores como `phase0`, `phase1` o `fase2` en headers instalados, APIs, mensajes de
ejecución, targets o suites de pruebas permanentes, fixtures ni identificadores que lleguen al
cable. Esos nombres caducan y fragmentan la conformidad según la historia del proyecto.

Las fases sólo pertenecen al roadmap, encabezados de estado, historial de Git y artefactos
temporales que se eliminan al cerrar un hito. La inestabilidad de la API se expresa únicamente con
`inline namespace v0`; los validadores permanentes deben nombrar el estándar o contrato comprobado,
por ejemplo `validate_ipmx_sps` y `validate_ipmx_sdp`.

## Requisitos

- Windows 10 1903 o posterior; Windows 11 recomendado.
- Visual Studio Build Tools 2022 o posterior con desarrollo de escritorio C++ y Windows SDK.
- CMake 3.25 o posterior, Ninja y vcpkg.
- PowerShell 7 recomendado.

x264 está bajo GPL-2.0-or-later. El manifiesto activa explícitamente su feature `gpl`; revise las
obligaciones de licencia antes de distribuir binarios.

## Compilar y probar

El script descubre Visual Studio y el vcpkg incluido. También evita el problema conocido del build
de x264 cuando el checkout vive en una ruta con espacios:

```powershell
./scripts/build.ps1
```

Opciones habituales:

```powershell
./scripts/build.ps1 -Configuration Debug
./scripts/build.ps1 -WarningsAsErrors
./scripts/build.ps1 -SkipTests
```

Si usa un vcpkg independiente, defina `VCPKG_ROOT`. En una Developer PowerShell también puede usar
los presets directamente:

```powershell
$env:VCPKG_ROOT = "C:\src\vcpkg"
cmake --preset windows-msvc
cmake --build --preset windows-msvc
ctest --preset windows-msvc
```

Los binarios quedan bajo `out/build/windows-msvc/bin`.

## Ejecutar el loopback

El runner abre el emisor y la ventana receptora, compartiendo el SDP generado:

```powershell
./scripts/run-loopback.ps1 -Source test
./scripts/run-loopback.ps1 -Source screen
```

También se pueden iniciar manualmente. Arranque primero el emisor para producir el SDP y luego el
receptor:

```powershell
./out/build/windows-msvc/bin/ipmx-sender.exe --source test --sdp ipmx.sdp
./out/build/windows-msvc/bin/ipmx-receiver.exe --sdp ipmx.sdp --require-zero-loss
```

Use `--interface A.B.C.D` en ambos procesos si Windows elige una interfaz multicast distinta. El
emisor configura TTL 1 y `IP_MULTICAST_LOOP=TRUE`; el receptor usa `IP_ADD_MEMBERSHIP`.

## Criterio de salida de Fase 1

La prueba prolongada ejecuta ambos procesos, guarda logs, mide el working set cada segundo y falla
ante pérdida/reordenamiento, falta de cuadros o latencia, o crecimiento medio superior al umbral:

```powershell
./scripts/run-stability.ps1 -DurationSeconds 90 -Source test
```

Los retardos de planificación se pueden cambiar sin recompilar mediante
`--encoder-delay-us`, `--sender-reports-delay-us` y `--access-unit-offset-us` en el emisor, o
mediante `-EncoderDelayUs`, `-SenderReportsDelayUs` y `-AccessUnitOffsetUs` en este runner. Los
tres aceptan cero. Si no se especifican, `encoder_delay` es tres períodos de cuadro,
`sender_reports_delay` coincide con éste y el AU offset es 1 ms.

Debe repetirse con `-Source screen`. La existencia del código y del runner no demuestra por sí sola
el criterio: hace falta conservar una ejecución real de 30 minutos con pérdida y reordenamiento en
cero, memoria estable y latencia de captura a presentación reportada.

## Alcance actual

Las Fases 1–3 incluyen BGRA a NV12; x264 Main/High 4:2:0/8-bit sin B-frames; VBR limitado con VBV y Type
II NAL HRD; VUI BT.709 de rango estrecho; Buffering Period y Picture Timing SEI; parsing
independiente de SPS/PPS/SEI; Single NAL/FU-A; RTP de 90 kHz con shaping TR-10-7; RTCP Sender
Reports IPMX con bloques `0x0005` y H.264 `0x000A`; `MAXUDP` configurable sin fragmentación IPv4; SDP IPMX; y golden H.264/PCAP
comprobados en CTest.

## Traffic shaper e instrumentación Windows

El emisor desacopla encoder y red mediante una cola CPB acotada. El hilo de distribución usa
`QueryPerformanceCounter` como reloj monotónico, un high-resolution waitable timer para la parte
gruesa de cada espera y espera activa únicamente en los 500 microsegundos finales. Se registra en
MMCSS con la tarea `Distribution` y prioridad AVRT alta; el proceso no usa
`REALTIME_PRIORITY_CLASS`.

El shaper aplica simultáneamente el máximo bitrate IP y el Network Compatibility Model con
`beta=1.10`. La ráfaga inicial queda limitada por:

```text
CMAX = MAX(16, INT(MaxRate / 21600))
```

Se miden lateness de SR, inserción CPB y paquetes; jitter de envío; paquetes tardíos; ocupación y
profundidad máxima de la cola CPB; `CMAX`, máximo `CINST` y violaciones NCM. `--dscp` vale 36
(AF42) por defecto. Los resultados se exportan con `--metrics-csv PATH` y
`--metrics-json PATH`; `--late-packet-threshold-us` configura el umbral de paquete tardío.
`--dump-pcap PATH` genera PCAP Ethernet con MAC multicast RFC 1112, IPv4 DF y timestamps de envío
correlacionados con QPC.

La aceptación automatizada descarga y ejecuta el paquete oficial de pruebas IPMX, arranca carga de
CPU/GPU y conserva SDP, PCAP, métricas y logs en un directorio único:

```powershell
./scripts/build.ps1
./scripts/fetch-ipmx-testing.ps1
./scripts/run-phase3-acceptance.ps1 -DurationSeconds 300 -PythonPath C:\ruta\python.exe
```

También puede validar un artefacto ya capturado:

```powershell
./scripts/verify-ipmx-h264.ps1 -PcapPath captura.pcap -SdpPath stream.sdp `
  -ConfigPath tests/IPMXConformanceTests/data/ipmx_h264_720p60.cfg `
  -PythonPath C:\ruta\python.exe
```

La captura interna sirve para diagnóstico. Para el criterio final, capture desde otra máquina o
con timestamps hardware y ejecute:

```powershell
./scripts/run-phase3-acceptance.ps1 -ExternalPcapPath captura-externa.pcap `
  -ExternalSdpPath sesion-original.sdp `
  -CaptureProvenance 'Host, interfaz, método de timestamps, fecha y sesión' `
  -RequireExternalCapture -PythonPath C:\ruta\python.exe
```

El runner falla si el emisor incumple temporización/NCM, si la recepción presenta pérdida o si el
validador oficial imprime cualquier resultado `FAIL`. Los requisitos que el validador marque
`CANNOT_TEST` siguen necesitando el entorno o evidencia externa correspondiente; en particular, el
PCAP generado dentro del propio emisor no sustituye una medición independiente.

No se incluyen un grandmaster PTP externo, NMOS, redundancia ni FEC; por ello la conformidad
declarada se limita al bitstream y transporte de esta fase y no al nodo IPMX completo.

## Diseño técnico de Fase 1

### Contrato de video

- Cuadros progresivos y dimensiones pares.
- Conversión BGRA a NV12 con coeficientes BT.709 y rango de estudio.
- x264 `veryfast` y `zerolatency`, Main o High Profile, entrada NV12 de 8 bits e `i_bframe=0`.
- GOP con IDR como máximo cada segundo y SPS/PPS repetidos en banda.
- ABR/VBR con VBV aproximado de un cuadro, `X264_NAL_HRD_VBR`, un solo CPB y `cbr_flag=0`.
- VUI con BT.709, rango estrecho, resolución y timing exacto (`num_units_in_tick=denominador`,
  `time_scale=2*numerador`). El encoder se rechaza al inicio si el SPS real no cumple.
- Buffering Period SEI en cada IDR y Picture Timing SEI en cada access unit. En ejecución se cuentan
  y advierten las infracciones sin derribar el emisor; las pruebas siguen tratándolas como fallo.
- WGC captura el monitor primario, excluye el cursor y conserva únicamente el cuadro más reciente,
  evitando que una sobrecarga genere una cola de latencia creciente.

### RTP y RFC 6184

Cada datagrama contiene RTP v2, payload type dinámico 96, secuencia de 16 bits, timestamp de 32 bits
y SSRC aleatorio. El reloj es de 90 kHz y todos los paquetes de un access unit comparten el mismo
timestamp. El marker sólo aparece en el último paquete del access unit.

El packetizer emite únicamente:

- Single NAL Unit Packet para las NAL units que caben en el MTU configurado.
- FU-A para las NAL units mayores, conservando F/NRI y los bits Start/End.

No se emiten STAP-A, STAP-B, MTAP ni FU-B. Por tanto, un paquete UDP nunca contiene más de un VCL
NAL. El receptor descarta un access unit dañado o un modo no soportado. `MAXUDP` vale 1200 bytes por
defecto, puede configurarse hasta 1460 e incluye RTP y la extensión de medición. Un hilo de red con
cola acotada emite ráfagas de hasta `CMAX`, aplica un límite de deriva y mantiene el primer paquete
de cada cuadro sobre el periodo nominal. La escritura PCAP ocurre en otro hilo.

Antes del primer RTP de cada cuadro se envía a `media+1` un compound RTCP con Sender Report, IPMX
Info Block, Media Info Block de vídeo comprimido `0x0005`, Media Info Block H.264 `0x000A` y SDES
CNAME. El `0x000A` replica `profile-level-id`, `packetization-mode` y `sprop-parameter-sets` del SDP.
Los tiempos nominales `encoder_delay` y `sender_reports_delay` se fijan al iniciar la sesión, se
programan respecto al tiempo de captura y se conserva el SR incluso si el encoder omite el access
unit. El emisor muestra los tres retardos efectivos al arrancar. Sin una referencia PTP
común, el SDP y el Info Block identifican el reloj interno con `ts-refclk:localmac`; las fuentes
síncronas declaran `mediaclk:direct=0`.

### Medición de latencia

La implementación conserva una extensión RTP local con perfil RFC 8285 `0xBEDE`, ID 1 y un timestamp monotónico
de captura de 64 bits en nanosegundos. Como ambos procesos se ejecutan en la misma máquina, el
receptor puede calcular la diferencia inmediatamente después de `IDXGISwapChain::Present`.

El SDP declara la extensión como:

```text
a=extmap:1 urn:ipmx-windows:rtp-hdrext:capture-time-ns
```

Esta extensión es diagnóstica y privada; no forma parte de IPMX. Una fase posterior deberá
sustituir esta correlación local por el modelo temporal requerido por VSF TR-10.

### SDP

El emisor genera el SDP después de inicializar y validar x264. De este modo,
`profile-level-id` y `sprop-parameter-sets` proceden de los SPS/PPS reales. El `fmtp` declara además
resolución, profundidad, frame rate exacto, sampling, colorimetría, `TP=2110TPW`, `MAXUDP`, TCS,
rango y `IPMX`. `b=AS` representa el máximo a nivel IP y `a=rtcp` reserva `media+1`. El parser
recupera campos y SPS/PPS de forma tolerante; `validate_ipmx_sdp` produce por separado el
informe de conformidad que usa el receptor. `a=framesize` se contrasta con `fmtp` y no lo sobrescribe.

Opciones relevantes del emisor:

```powershell
ipmx-sender --profile high --bitrate-kbps 4000 --max-ip-bitrate-kbps 4400 `
  --port 5004 --maxudp 1200 --sdp ipmx.sdp
```

`--dump-h264` y `--dump-pcap` permiten conservar artefactos de regresión. El PCAP usa
`LINKTYPE_ETHERNET`, incluye Ethernet multicast, IPv4 con DF y conserva en orden los datagramas RTP
y RTCP enviados.

### Contadores y aceptación

El receptor informa cada segundo:

- paquetes recibidos;
- huecos de secuencia contabilizados como pérdida;
- paquetes atrasados contabilizados como reordenamiento;
- datagramas RTP inválidos;
- access units reconstruidos y cuadros presentados;
- latencia media, mínima, máxima y desviación estándar.

`--require-zero-loss` retorna código 2 si hay pérdida, reordenamiento, paquetes inválidos o cuadros
descartados por tardíos; también falla si no se presentó ningún cuadro, no existe una muestra válida
de latencia o la latencia máxima supera `--max-latency-ms` (250 ms de forma predeterminada).

`scripts/run-stability.ps1` excluye un calentamiento inicial y compara la media de las primeras y
últimas ventanas posteriores de hasta 60 muestras. El límite predeterminado de crecimiento es 5 MiB
por proceso y puede cambiarse con `-MaximumGrowthMiB`; el calentamiento se controla mediante
`-WarmupSeconds`. Cada ejecución conserva logs, SDP y CSV en un subdirectorio único bajo
`out/build/windows-msvc/stability`.
También activa el gate de temporización: en cualquier ventana de dos segundos, la diferencia entre
el intervalo máximo y mínimo de los primeros paquetes de cuadro no puede superar 2 ms.

### Golden media y pruebas

`IPMXCoreTests` cubre SPS/PPS/SEI, VUI/HRD, puertos, SDP, pacing y RTP. `IPMXConformanceTests` abre
los golden bajo `tests/IPMXConformanceTests/data`, valida cada paquete y codifica en vivo con x264. También
comprueba que el decoder Media Foundation consume Main y High 4:2:0/8-bit.

Para regenerar ambos goldens de forma explícita después de un cambio intencional de formato:

```powershell
./scripts/regenerate-goldens.ps1
```

### Exclusiones conscientes

No hay sincronización contra un grandmaster PTP externo, NMOS, redundancia, FEC, sincronización
entre streams ni manejo de cambios dinámicos de formato. Son ampliaciones posteriores y siguen
impidiendo afirmar conformidad completa de dispositivo con toda la familia VSF TR-10.
