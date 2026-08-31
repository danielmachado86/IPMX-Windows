# IPMX Windows

Prototipo Windows/C++ de transporte H.264 sobre RTP multicast. La Fase 0 conecta un emisor y un
receptor en la misma máquina y permite usar un patrón generado o Windows Graphics Capture.

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
src/ipmx-core/             Annex B, RTP, SDP, Winsock, color y métricas
tests/IPMXCoreTests/       pruebas unitarias del núcleo
cmake/                     configuración compartida de compilación
scripts/                   build, loopback y estabilidad
specs/                     documentos VSF de referencia
```

### Límites entre componentes

`ipmx_core` no conoce Windows Graphics Capture, D3D11, Media Foundation ni x264. Contiene lógica
transportable y comprobable mediante pruebas unitarias. `ipmx_sender` y `ipmx_receiver` actúan como
adaptadores Windows y sus headers no forman parte de la API instalada. x264 se enlaza únicamente al
emisor.

Los headers públicos se incluyen desde `ipmx/<componente>.hpp`. El núcleo vive en `namespace ipmx`
con `inline namespace v0`: los consumidores escriben `ipmx::RtpPacketizer`, mientras que
`ipmx::v0::RtpPacketizer` conserva una versión ABI explícita durante la evolución del prototipo.
Los adaptadores específicos de las aplicaciones viven en `ipmx::sender` e `ipmx::receiver` y no
forman parte de la API instalada.

La dirección permitida de las dependencias es:

```text
IPMXCoreTests --> ipmx_core
ipmx_sender   --> ipmx_core + x264 + WGC/D3D11
ipmx_receiver --> ipmx_core + Media Foundation/D3D11
```

No se permiten dependencias en sentido contrario. Las futuras implementaciones de RTCP, PTP, NMOS
o shaping deben incorporarse como módulos del núcleo o servicios separados, no dentro de los
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

## Criterio de salida de Fase 0

La prueba prolongada ejecuta ambos procesos, guarda logs, mide el working set cada segundo y falla
ante pérdida/reordenamiento, falta de cuadros o latencia, o crecimiento medio superior al umbral:

```powershell
./scripts/run-stability.ps1 -DurationSeconds 1800 -Source test
```

Debe repetirse con `-Source screen`. La existencia del código y del runner no demuestra por sí sola
el criterio: hace falta conservar una ejecución real de 30 minutos con pérdida y reordenamiento en
cero, memoria estable y latencia de captura a presentación reportada.

## Alcance actual

La Fase 0 incluye BGRA a NV12, x264 High Profile sin B-frames, parsing Annex B, Single NAL/FU-A,
RTP de 90 kHz, SSRC/secuencia, marker de access unit, multicast loopback, reconstrucción Annex B,
decodificación Media Foundation, render D3D11 y SDP manual.

No incluye RTCP, PTP, NMOS, redundancia ni traffic shaping; por ello no debe presentarse todavía
como un nodo IPMX conforme.

## Diseño técnico de Fase 0

### Contrato de video

- Cuadros progresivos y dimensiones pares.
- Conversión BGRA a NV12 con coeficientes BT.709 y rango de estudio.
- x264 `veryfast` y `zerolatency`, High Profile, entrada NV12 de 8 bits e `i_bframe=0`.
- GOP con IDR como máximo cada segundo y SPS/PPS repetidos en banda.
- ABR con VBV aproximado de un cuadro para evitar ráfagas de un segundo. Esta fase no implementa
  todavía el modelo HRD/IPMX ni shaping.
- WGC captura el monitor primario, excluye el cursor y conserva únicamente el cuadro más reciente,
  evitando que una sobrecarga genere una cola de latencia creciente.

### RTP y RFC 6184

Cada datagrama contiene RTP v2, payload type dinámico 96, secuencia de 16 bits, timestamp de 32 bits
y SSRC aleatorio. El reloj es de 90 kHz y todos los paquetes de un access unit comparten el mismo
timestamp. El marker sólo aparece en el último paquete del access unit.

El packetizer emite únicamente:

- Single NAL Unit Packet para las NAL units que caben en el MTU configurado.
- FU-A para las NAL units mayores, conservando F/NRI y los bits Start/End.

No se emiten STAP-A, STAP-B, MTAP ni FU-B. El receptor descarta un access unit dañado o un modo de
paquetización no soportado. El MTU predeterminado es 1200 bytes e incluye RTP y la extensión de
medición.

### Medición de latencia

La Fase 0 añade una extensión RTP local con perfil RFC 8285 `0xBEDE`, ID 1 y un timestamp monotónico
de captura de 64 bits en nanosegundos. Como ambos procesos se ejecutan en la misma máquina, el
receptor puede calcular la diferencia inmediatamente después de `IDXGISwapChain::Present`.

El SDP declara la extensión como:

```text
a=extmap:1 urn:ipmx-windows:rtp-hdrext:capture-time-ns
```

Esta extensión es diagnóstica y privada; no forma parte de IPMX. Una fase posterior deberá
sustituir esta correlación local por el modelo temporal requerido por VSF TR-10.

### SDP

El emisor escribe el SDP manualmente después de inicializar x264. De este modo,
`profile-level-id` y `sprop-parameter-sets` proceden de los SPS/PPS reales. El archivo también
declara grupo, puerto, reloj de 90 kHz, packetization-mode 1, bitrate, resolución y frame rate. El
receptor obtiene grupo, puerto, resolución y frame rate mediante `--sdp`.

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

### Exclusiones conscientes

No hay RTCP Sender Reports, IPMX Info Blocks, PTP, NMOS, redundancia, FEC, traffic shaping,
sincronización entre streams ni manejo de cambios dinámicos de formato. Estas exclusiones coinciden
con la definición de Fase 0, pero impiden afirmar conformidad completa con VSF TR-10-1/TR-10-7.
