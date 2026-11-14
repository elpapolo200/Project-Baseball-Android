# Multijugador en la misma wifi (investigación, 2026-11-13)

Objetivo: dos teléfonos (o PC), cada uno con su juego y la misma APK; uno crea una sala, el otro entra con un
código y juegan un partido de **Exhibición** uno contra otro.

## Qué trae el juego (MLB 15 de la Vita)

- **Un solo mando** (`sceCtrlPeekBufferPositive`); el propio juego dice «two-player games (not supported in the
  PlayStation Vita system)». No hay juego local por wifi (sin `sceNetAdhoc`).
- **Sí trae modo en línea completo**, que dependía de servidores de SCEA (apagados):
  - `FRONTEND/servers_burn.xml` (en el WAD): servidor `mlb15-production.online.scea.com`, puerto 10050
    (10051 con SSL), `url_root="/"`, UUID de título por plataforma (psv `6e0b8e51-...`).
  - `FRONTEND/services.xml`: cada orden con su `uri`, `method`, `use_ssl`, `requires_auth` (327 órdenes;
    casi todas de otros modos: Diamond Dynasty, ligas, mercado...).
  - API web estilo Rails: `POST mlb_logins/login.xml` (SSL), respuestas **XML** (rapidxml), peticiones
    `application/x-www-form-urlencoded` o `multipart/form-data`, cabeceras `X-Title-UUID`, `X-SC-Tmx`,
    `X-Analytics-Session-UUID`.
  - Respuesta de login: `login_data` (`player_id`, `player_name`) y `fls_data` (`server_address`,
    `server_port`, `server_private_key`, `ticket_session_uuid`, `ticket_signature`, `ticket_player_id`,
    `ticket_username`, `ticket_expiration_date`). Envía `console_id` y `ticket` (de PSN).
  - Crear partida: `POST game_players/create_game_and_join_server.xml` con `game[host_player_id]`,
    `game[remote_player_id]`, `game[host_team_id]`, `game[remote_team_id]`, `game[game_type]` (EXHIBITION),
    `game[is_ranked]`, `game[lobby_channel_id]`, `game[stadium]`; responde `game` con
    `local_service_ticket`, `remote_service_ticket`, `service_address`, `service_port`.
- **FLS** (`OnlineUtils/fls.cpp`): servicio intermedio por TCP con mensajes **XML**
  (`<message layer target targetID delivery>`, longitud de 2 bytes al principio, `nRecv == 2`), saludo `Hello`
  con el ticket (`session_uuid`, `signature`, `player_id`, `username`, `expiration_date`), capas de «plugins»,
  tipos `ZonePlayerJoin`, `ZonePlayerQuit`, `BuddyInvite`, `PlayerKick`, `System`, `Keepalive`. Las dos consolas
  no se hablaban directamente: todo pasaba por el FLS.
- **Sincronización del partido**: por eventos (`PACKET_PITCH_SELECTED`, `PACKET_BATTER_SWING`,
  `PACKET_GAMESTATE`, `PACKET_ONLINE_PAD`, `PACKET_RESULT`... ~90 tipos) y detección de desincronización
  (`game_player.divergence`). Ese código es del juego y no hay que tocarlo.
- Sockets en el juego: una clase propia con UDP (`2,2,0x11`), TCP (`2,1,6`) y P2P de la Vita (`2,6,0`).

## Plan propuesto

El teléfono que crea la sala hace de «servidor de SCEA» (web + FLS) para los dos; el código de sala es su
dirección en la wifi. El juego de cada teléfono cree que está en línea con PSN.

1. **Red del port de verdad** (`hle_net.c`): sockets reales, `sceHttp` propio (sin TLS real: lo hacemos nosotros),
   `sceNetCtl` conectado, PSN «con sesión» (NpManager/NpBasic/NetCheckDialog/restricciones). Traza de cada
   petición (URL, cabeceras, cuerpo) y de cada mensaje FLS. Se prueba en el PC con dos copias del port.
2. **Mini servidor** dentro del port: las órdenes que use Exhibición en línea (login, `session.ping`,
   `server.get_ping_server`, salas `lobby_channel*`, `game_player.*`, `policy.*`, lo mínimo de perfil) con las
   etiquetas XML que espera el código del juego, y el FLS como intermediario entre los dos jugadores.
3. **Android**: «Crear sala» / «Unirse» con código en la pantalla de opciones; el servidor se redirige con el
   XML de servidores (o en el HLE).

## Fase 1 hecha (2026-11-13) y hallazgo importante

- Respaldo previo: `Desktop\ProjectBaseball_Android_respaldo_2026-11-13` (APK, app, fuente, docs y `pc_port_host`).
- `VitaRE\mlb15\recomp\host\hle_net.c` reescrito: con `MLB15_EN_LINEA=1` hay sockets reales (Winsock/POSIX; el
  socket P2P de la Vita va por UDP), todos los nombres se resuelven a `MLB15_SERVIDOR` (127.0.0.1 por defecto;
  nada sale a internet), `sceHttp` propio en HTTP plano (443/10051 -> 10050), NetCtl conectado (códigos de
  `InetGetInfo` 1 = CNF_NAME ... 15 = IP), PSN con sesión (`MLB15_JUGADOR`, ticket falso de NpAuth con callback en
  `sceNpCheckCallback`), `sceNetCheckDialog` OK (`hle_dialog.c`). Sin la variable, todo igual que antes.
- `VitaRE\tools\sala_prueba.py`: servidor HTTP que anota cada petición (`sala_prueba.log`) y responde XML vacío o
  el archivo de `tools\sala_respuestas\<ruta>`.
- Probado en el PC: el juego se cree en línea, pide el ticket de PSN, consulta
  `high_priority_news_updates/view.xml` y manda telemetría (JSON a `mlb15.api.wwsga.me/games/<uuid>/telemetry`).
- **Hallazgo**: la versión de Vita NO tenía partidos en línea contra otra persona (las reseñas lo confirman:
  PSU «multiplayer is all but absent»; el H2H en línea era de PS4/PS3). El menú no ofrece nada en línea (PLAY NOW:
  solo Exhibition / The Show Live) y en el WAD de Vita no están las pantallas de sala (`Online\OnlineEntry`,
  `ol_*`): solo dos tutoriales sueltos. El código de red del partido (FLS, `PACKET_*`, `create_game_and_join_server`)
  sí está compilado, pero no se llega a él desde ningún menú.

## ¡FUNCIONA! Conexión directa de desarrolladores (2026-11-13)

El juego trae un modo de los desarrolladores para jugar un partido en línea conectando dos consolas por IP,
**sin servidores ni pantallas de sala**:

- `main`/init (`sub_81000B10` -> `sub_8182B386`) guarda los argumentos de arranque como textos de 128 B en
  `0x81FC0474` (número en `0x81FC0470`; se saltan los que empiezan por `#`).
- La plataforma en línea (`olHost`, `0x8169713E`, creada desde `0x8164B16E`) los lee: `olHost` (anfitrión, modo 1),
  `olConnect=<ip>` (modo 2), `olPeer` (3), `olUser=`, `olRHost=`, `olRPort=`, `olCoop`, `olSession=`,
  `olSessionHost=`, `olRandomNumber=`. Objeto de conexión en `+0x25C` (0x324 B, modo en `+0x104`).
- Al empezar CUALQUIER partido, `Game_Enter` (`0x81251EF0`) llama a `sub_81695942` -> `0x816AD354`: si el modo
  es 1 o 2 conecta con la otra consola (bucle hasta `+0x30C`), pone el gestor en `ONLINE_INGAME`, decide
  home/away y hace ping. Gestor en línea global en `0x81E75DD0` (`+0` amIserver, `+0x24C` estado).
- Red: UDP, puerto **3450**, paquetes con número de secuencia (`01 03 00 03 00 29 ...`).
- En el port: `MLB15_ARGS="olHost olUser=Kiki"` / `MLB15_ARGS="olConnect=<ip> olUser=Amigo"` (escribe la tabla
  antes de `module_start`, `host\main.c`) + `MLB15_EN_LINEA=1`. `MLB15_IP_LOCAL` para dos copias en el mismo PC
  (127.0.0.1 y 127.0.0.2).
- **Probado en el PC con dos copias** y el guion del partido: las dos entran en `ONLINE_INGAME`, se conectan,
  intercambian ~1 300-1 700 paquetes, el partido avanza (etiqueta «UNRNK» en el marcador, lanzamientos, jugadas,
  «Scorer's Decision: E2») y **las dos terminan las mismas 7 jugadas y 8 lanzamientos, sin avisos de
  desincronización**.

- **Sin la sala HTTP de prueba** también: dos copias, 9 jugadas y 10 lanzamientos idénticos, sin desincronización ni
  trampas (las peticiones de noticias/telemetría fallan sin romper nada).

### Android (2026-11-13)

- Sincronizado desde el PC (`sincronizar_desde_pc.ps1`: `hle_net.c`, `hle_dialog.c`, `main.c`; 53 cambios de
  parches, sin avisos).
- `OpcionesActivity`: recuadro «Multijugador (misma wifi)» con el código de sala (dos últimas cifras de la IP de la
  wifi, p. ej. `1.23`), **Crear sala** y campo + **Unirse** (acepta `23`, `1.23` o la IP entera; recuerda el último
  en `[multijugador] ultimo_codigo`). Lanza el juego con el extra `multi` (`host` o la IP).
- `ProjectBaseballActivity.getArguments()` -> `--multi=...` llega a `SDL_main`; `arranque_android.c` pone
  `MLB15_EN_LINEA=1`, `MLB15_ARGS` (`olHost olUser=Local` / `olConnect=<ip> olUser=Visitante`), `MLB15_JUGADOR` y
  `MLB15_SERVIDOR`. Solo para esa partida: JUGAR normal sigue fuera de línea.
- Manifest: permisos `INTERNET` y `ACCESS_NETWORK_STATE`.
- Arreglo: el anfitrión abría la sala en 127.0.0.1 (su IP se calculaba hacia la «sala», que era él mismo). Ahora
  `ip_local` usa la ruta por defecto (connect UDP a 8.8.8.8, no envía nada) si la sala es 127.x.
- **Probado teléfono (S22, Crear sala) <-> PC (unido a la IP del teléfono) por la wifi**: los dos en `ONLINE_INGAME`,
  ~3 670 paquetes, **10 jugadas y 10 lanzamientos iguales en los dos, sin desincronización**, aunque cada uno lleva su
  propia compilación del juego (arm64 y x64). En el teléfono, el anfitrión (Giants, en casa) lanza y el PC batea.

### Lo que queda

1. ~~Launcher, Android, sin servidor~~ hecho.
2. Partido jugado por dos personas de verdad (no con el guion) y en dos teléfonos: retraso, cortes, qué pasa si
   uno sale o se va a otra app.
3. Elección de equipos y estadio: en las pruebas los dos eligieron lo mismo (mismo guion). Ver qué pasa si cada
   uno elige equipos distintos (¿manda el anfitrión?).
4. Launcher de PC: el mismo Crear sala / Unirse (ahora solo con `MLB15_ARGS`). Cortafuegos de Windows si el PC es
   anfitrión.

## Riesgos

- Las respuestas XML hay que reconstruirlas desde el código del juego (no hay grabaciones de los servidores
  reales). Cuantas más cosas pida el menú en línea antes de dejar jugar, más trabajo.
- El protocolo exacto del FLS (marco, capas de plugins, cómo van los paquetes del partido) está por leer.
- La sincronización la hace el juego; entre dos copias del mismo port debería cuadrar (mismo código), pero solo
  se sabe probando.
