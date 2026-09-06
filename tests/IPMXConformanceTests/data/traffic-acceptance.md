# Estado de aceptación del tráfico

El runner usa `--source stress`: textura determinista que cambia cada cuadro para ejercitar
VBV y pacing. La duración predeterminada es 300 segundos; el límite receptor sigue siendo
250 ms. `-MaximumLatencyMs` permite experimentar con otro límite, que queda registrado.
La carga CPU/GPU y el contenido exigente no garantizan por sí solos saturación de bitrate.

El validador solicita todos los cuadros hasta EOF y exige PASS explícito para CMAX,
SEI, HRD, simulación de CPB y timing. FFmpeg debe estar disponible en PATH o en
`ipmx/ffmpeg/bin` dentro del paquete oficial. Un CANNOT_TEST esencial falla la ejecución.

La validación externa usa exclusivamente el PCAP y SDP suministrados, no inicia otra sesión.
Registra en la salida sus hashes SHA256 y procedencia declarada por el operador. El software
no puede verificar que esa declaración corresponda a otra máquina o timestamps hardware.

La prueba del 2026-09-05 de 30 segundos, con dos trabajadores CPU al 80% y GPU, falló:
el informe oficial detectó cadencia SR y timing HRD incorrectos bajo contenido exigente.
Evidencia local: `out/build/windows-msvc/phase3/20260905-131835-3341a4ab`.
No se considera aceptada la fase hasta corregir y repetir esa prueba y obtener captura externa.
Los fallos conservan `result.json`, logs y artefactos para diagnóstico.
