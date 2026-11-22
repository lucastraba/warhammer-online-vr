# ProjectWAR private server on Linux (Mono)

> Notes from the test machine (hal9000). Paths assume `~/ross-vr/server` with these scripts in
> `~/ross-vr/server/scripts`; in this repo they live in `tools/linux-rig/server`.

Local-only Warhammer Online server emulator for testing Ross Scott's WAR VR mod
(https://github.com/accursed-farms/warhammer-online-vr). Linux port of Ross's Windows procedure
(`extras/server emulator instructions.txt` in the mod repo). Set up 2026-11-21.
**Never point this at, or the client at, the official Return of Reckoning server.**

## Status (verified 2026-11-21)

All four servers run as systemd `--user` units under Mono, every listener bound to 127.0.0.1:

| Unit | Process | Port(s) (127.0.0.1 only) | Purpose |
|---|---|---|---|
| `war-accountcacher.service` | `mono --debug AccountCacher.exe` | 6800 (RPC hub) | accounts, auth |
| `war-launcher.service` | `mono --debug LauncherServer.exe` | **8000** (client Launcher.exe), 6002 (RPC) | launcher login / create account |
| `war-lobby.service` | `mono --debug LobbyServer.exe` | **8048** (WAR.exe login/realm list), 6001 (RPC) | character select |
| `war-world.service` | `mono --debug WorldServer.exe` | **10300** (game world), 6003 (RPC), 51933 (bot-editor HTTP API) | the world |
| `war-server.target` | - | - | starts/stops all four |

MySQL 8.4: 127.0.0.1:3306 and X-protocol 127.0.0.1:33060.

Checked: launcher-protocol account creation (`CL_CREATE`) and login (`CL_START` → `LOGIN_SUCCESS` +
session token, wrong password → `LOGIN_INVALID_USERNAME_PASSWORD`), WorldServer loads the world
(`LoaderMgr Loading complete in : ~32 s`, 0 `ERROR` lines), the native LOS library loads zone 100
(6491 fixtures) and answers a line-of-sight query, SIGTERM stop runs the WorldServer's save-and-stop
path (`Closing the server (unix signal)`). A real WAR client under Wine logs in, creates a character
and enters the world (2026-11-22; see `../README.md`).

## Versions

- ProjectWAR branch `RESTART`, commit `0f87f389d9fc17e7fe59a2489c8c4b518e6a1093`
  (2026-11-01, "Reassemble split world frames, bind RPC to loopback, add validation runner"), the HEAD
  on 2026-11-21. Ross's build is from 2026-09-09; the nearest commit on or before that date is `de01aed1`
  (2026-09-09 14:13, "db: fold the 01-98 migration series into the base dumps"). Use it if HEAD
  misbehaves with the mod: `git -C src checkout de01aed1`, then re-run `scripts/build-server.sh` and
  `scripts/db-setup.sh` (it drops nothing, so recreate the DBs first; see "Reset the databases").
- zones.zip: release `zones-data-v1` (2026-02-21, the same one Ross used), sha256
  `8f659cef1e850640b4d03cc2dc2f4e4b328d647fbd2d189ca79b942264ae5f5c`, kept in `dl/zones.zip`,
  unpacked into `src/deps/zones` (the build copies it to `bin/Release/zones`).
- Mono 6.14.1 (Ubuntu `mono-runtime`/`mono-libraries`/`mono-devel` 6.14.1+ds2-2, `libgdiplus` 6.1)
- .NET SDK 10.0.115 (`dotnet-sdk-10.0`): compiler only; the servers run on Mono
- MySQL 8.4.13 (`mysql-server` 8.4.13-0ubuntu0.26.04.2). Ross used 8.0.46; 8.0 is not packaged for 26.04.
- g++ 15.2 (already installed) for the native LOS library

## Installed on hal (apt)

`mono-runtime mono-libraries mono-devel dotnet-sdk-10.0 mysql-server`, which pulled in libgdiplus,
dotnet-runtime/aspnetcore-runtime/targeting packs 10.0, mysql-client/-server-core/-common 8.4, libaio1t64,
libmecab2, libprotobuf-lite32t64, libtcmalloc-minimal4t64, liblttng-ust*, libfcgi*, libcgi-*-perl,
libhtml-template-perl. `unzip`, `7zip`, `git`, `build-essential` were already there.
MySQL is a system service (`mysql.service`, enabled at boot, as packaged).

## Database

- Databases `war_accounts`, `war_characters`, `war_world` (from `src/Database/*.sql`).
- User **`war`** / password **`warlocal`** (`war@localhost` and `war@127.0.0.1`, ALL on the three DBs).
  Throwaway local password. MySQL root = `sudo mysql` (auth_socket, no password).
- `/etc/mysql/mysql.conf.d/zz-projectwar.cnf`: `lower_case_table_names = 1`. **Required**: the emulator
  was written on Windows MySQL (case-insensitive table names). With the Linux default the WorldServer
  created empty duplicate tables (`war_world.Ability` next to `ability`, ...) instead of using the data.
  The setting only works at data-dir init, so the data dir was re-initialised once
  (`mysqld --initialize-insecure --lower-case-table-names=1`). The first, unusable data dir is still at
  `/var/lib/mysql.case-sensitive-initial` (can be deleted).
- Bind: Ubuntu default `bind-address = 127.0.0.1`, `mysqlx-bind-address = 127.0.0.1`.
- Shell: `MYSQL_PWD=warlocal mysql -u war -h 127.0.0.1 war_world`

## Test account

- **`ross` / `ross`, GmLevel 5.** Created through the LauncherServer the way the client Launcher's
  "Create Account" button does (`scripts/create-account.py ross ross`), then
  `UPDATE war_accounts.accounts SET GmLevel = 5 WHERE Username = 'ross';` (Ross's step).
- Stored like the emulator does it: `CryptPassword = BCrypt(sha256_hex(lower(user) + ":" + lower(pass)))`.
  The launcher lower-cases user name and password.
- More accounts: `python3 -I ~/ross-vr/server/scripts/create-account.py <user> <pass>` (servers running),
  then the GmLevel UPDATE if wanted, then `warctl restart` (AccountCacher caches accounts in memory).
- Login check: `python3 -I ~/ross-vr/server/scripts/test-login.py ross ross` → `LOGIN_SUCCESS` + token.
- Also present: `botaccount` (system account the dump creates for the bot system; not loginable).

## Start / stop / check

```
~/ross-vr/server/scripts/warctl start      # systemctl --user start war-server.target
~/ross-vr/server/scripts/warctl stop       # world, lobby, launcher, then accountcacher (SIGTERM)
~/ross-vr/server/scripts/warctl restart
~/ross-vr/server/scripts/warctl status     # units + mono processes
~/ross-vr/server/scripts/warctl ports      # ss -ltnp for mono + mysqld
~/ross-vr/server/scripts/warctl logs world 200   # journalctl --user -u war-world (account|launcher|lobby|world)
```

- Startup takes ~60 s: the WorldServer loads for ~30 s and opens 10300 last. Wait for
  `TCPManager Server listening to : 127.0.0.1:10300` in `warctl logs world`.
- Units live in `~/.config/systemd/user/` (`war-*.service`, `war-server.target`). They are started by
  hand and are **not** enabled at boot. With `loginctl enable-linger` they keep running after SSH logout.
  Autostart if wanted: `systemctl --user enable war-server.target`.
- `Restart=on-failure` restarts a crashed server after 15 s.
- RAM: WorldServer ~4 GB RSS after load, the other three 80-180 MB each. hal had ~5 GB free with
  everything up, so stop the servers when not testing.
- Stopping: SIGTERM. The WorldServer catches it (Linux patch below) and runs its own save, kick-players and
  stop sequence (`Closing the server (unix signal)` in the log; TimeoutStopSec 120). Ross's advice still
  applies: as GM type `.boot` in game before stopping, so the save is done while the world is running.
- Logs: the systemd journal (`warctl logs ...`), plus the servers' own files in `src/bin/Release/Logs/` (`<Server>Log.txt`) and
  `src/bin/Release/logs/` (NLog: `<Server>_<date>.log`, Campaign, REWARD, DEATH).
- Server consoles: under Mono `Environment.UserInteractive` is false, so the servers run in their
  "non-interactive" mode (no console commands such as `.create`). Use the scripts above instead.

## Layout

```
~/ross-vr/server/
  README.md            this file
  src/                 git clone of Shmerrick/ProjectWAR, branch RESTART (+ Linux patch, see below)
  src/bin/Release/     build output = server root (Configs/, zones/, los/, *.exe, libWarZone64.so)
  src/bin/Release/Launcher/   client-side Launcher (see "Client launcher")
  build/               libWarZone64.so build copy
  dl/zones.zip         Ross's zones release
  scripts/             everything below; logs/ build and DB-import logs
  tools/nuget.exe      unused (nuget.exe hangs under Mono 6.14; scripts/nuget-restore.py replaces it)
```

## How it was built (and how to rebuild)

1. `git clone -b RESTART https://github.com/Shmerrick/ProjectWAR.git src`
2. `unzip dl/zones.zip -x '.vs*' -d src/deps/zones` (the zip uses backslashes; unzip converts them),
   `cd src/Database && 7z x war_world.7z`
3. `scripts/db-setup.sh`: creates user and DBs, imports the three dumps, Ross's TokUnlock3 step, then the
   incremental scripts (see below). Run once on an empty server.
4. `scripts/build-server.sh` (idempotent; run again after a `git pull`):
   - `make-linux-patch.py`: applies the Linux source patch (below); result saved in `scripts/patches/linux.patch`
   - `fix-case.py`: 3 source files are named with different case in the csproj than on disk
     (`Characters_tok_kills.cs`, `LiveEventSubTask_Info.cs`, `GlompDaSquigMasta.cs`); adds untracked symlinks
   - `nuget-restore.py`: packages.config restore straight from nuget.org into `src/packages/`
     (79 packages; replaces Visual Studio's restore; `nuget.exe` hung under Mono)
   - `dotnet msbuild <Project>.csproj -p:Configuration=Release -p:Platform=x64
     -p:FrameworkPathOverride=/usr/lib/mono/4.8-api -p:PostBuildEvent=` for AccountCacher, LauncherServer,
     LobbyServer, WorldServer (pulls in Common and FrameWork). The projects are classic .NET Framework 4.8,
     x64, packages.config. `PostBuildEvent` is Windows `cmd` (`if not exist ... mkdir`), so it is skipped and
     the two folders are made by the script. Not built: ServerLauncher (WinForms GUI, replaced by the units),
     ClientDataMatrix, LosBuilder (tools).
   - `build-warzone.sh`: builds the native line-of-sight library (`src/WarZone`, C++, normally the
     Windows-only `WarZone64.dll`) as `bin/Release/libWarZone64.so` with g++, using Win32 shim headers in
     `scripts/warzone-compat/` and removing 2 dead, uncompilable `tagA/tagB` initialisers from the bundled
     glm 0.9.3 (in a build copy, not in git)
   - `configure.py`: DB user/password into `bin/Release/Configs/{Account,World}.xml` (Ross: "check
     bin\Release\Configs\*.xml"), a Mono `<dllmap>` in `WorldServer.exe.config` so
     `[DllImport("WarZone64.dll")]` loads `libWarZone64.so`, and a `Zones -> zones` symlink
     (WorldServer checks `Directory.Exists("Zones")`; Linux is case-sensitive)
   - Note: a rebuild re-copies `Configs/*.xml` from `src/WorldServer/Configs/LocalDevelopment/` and
     `configure.py` re-applies the DB login. Other edits to `bin/Release/Configs` are overwritten by a rebuild.
5. `scripts/build-launcher.sh`: the client-side Launcher (see below).
6. Units: `scripts/gen-units.sh` writes them; copy `units/*` to `~/.config/systemd/user/`, `daemon-reload`.

### Linux source patch (`scripts/patches/linux.patch`)

- `FrameWork/NetWork/TCPManager.cs`: game listeners bound to `IPAddress.Any` (0.0.0.0) on Windows; they now
  take the address from env `PROJECTWAR_BIND_ADDRESS` (default unchanged). `scripts/run-server.sh` sets
  `127.0.0.1`, so 8000/8048/10300 are loopback-only. (RPC ports were already loopback in this commit.)
  It also avoids clashing with anything else on the machine that listens on port 8000.
- `LobbyServer/NetWork/Client.cs`: the lobby assumed a packet's header and body arrive in two separate
  reads, which is what Windows happens to do. On Linux they arrive together, the header was parsed as part of
  the protobuf body and every login failed (`Protocol message tag had invalid wire type`, the client shows
  "Authentication failed"). The handler now gets the body only.
- `WorldServer/Program.cs`: the kernel32 calls (`SetDllDirectory`, `SetConsoleCtrlHandler`) run only on
  Windows (Mono throws `DllNotFoundException`). On Linux a SIGTERM/SIGINT handler (Mono.Posix
  `UnixSignal`, loaded by reflection) calls the same stop routine as the Windows console-close handler.

## Deviations from Ross's Windows procedure

| Ross (Windows) | hal (Linux) |
|---|---|
| Visual Studio, build ProjectWAR.sln Release x64 | `scripts/build-server.sh` (.NET 10 SDK MSBuild/Roslyn against Mono's 4.8 reference assemblies); 4 server projects, no ServerLauncher |
| run `serverlauncher.exe`, launch all | systemd user units under Mono (`warctl start`) |
| MySQL 8.0.46 + Workbench, root/password | MySQL 8.4.13, user `war`/`warlocal`, `lower_case_table_names=1` |
| `ALTER TABLE mythic_src_item_infos ADD COLUMN TokUnlock3 ...` | skipped automatically: the current dump already has `TokUnlock3` (`smallint unsigned NOT NULL DEFAULT 0`); the ALTER would fail with "Duplicate column" |
| `incremental.ps1` (from the mod folder) | not in the mod repo or in ProjectWAR. Per ProjectWAR's README/AGENTS.md ("apply incremental scripts 00–13 in order") it is replaced by a shell loop in `db-setup.sh`: `Database/00_*.sql` … `13_*.sql` in order with the `mysql` client (each script has its own `USE`). No PowerShell installed. |
| WarZone64.dll (prebuilt Windows DLL) | `libWarZone64.so` built from the same C++ source |
| "open firewall" | nothing opened; all listeners on 127.0.0.1 (the client runs on hal under Wine) |
| Account.xml `RpcClientStartingPort` → 16000 if the lobby fails | left at 6000 (RPC clients use 6001-6003; the lobby starts fine) |
| accounts via the Launcher's "Create Account" | `scripts/create-account.py` sends the same CL_CREATE packet; the Launcher works too once running |

## Client launcher (for the Wine test, not verified)

- Built on hal: `src/bin/Release/Launcher/` (`Launcher.exe` + `libs/` + `Launcher.exe.config`,
  `mythloginserviceconfig.xml` → 127.0.0.1:8048). Ross: set `<add key="PatchMYP" value="true" />` in
  `Launcher.exe.config`, copy the whole `Launcher` folder into the client folder, run it from there.
  The Launcher has `127.0.0.1:8000` built in (`LauncherForm.cs`); our LauncherServer listens there.
- Linux build differences: its csproj calls the Windows SDK `resgen.exe` by absolute `C:\` path. The Linux
  build uses a generated `Launcher/Launcher.linux.csproj` (untracked) without that step, so the resources are
  in the preserialized format read by `System.Resources.Extensions.dll`. The 4 assemblies that reader needs
  (System.Memory 4.0.1.1, System.Buffers 4.0.3.0, System.Runtime.CompilerServices.Unsafe 4.0.4.1,
  System.Numerics.Vectors 4.1.4.0) were added to `libs/`.
- Under Mono on Linux (xvfb test) it loads its resources and then dies on the window icon:
  `System.ComponentModel.Win32Exception (0x80004005): No valid icon entry were found.` at
  `System.Drawing.Icon.InitFromStreamWithSize` (Mono's System.Drawing can't read that .ico). **wine-mono is
  Mono too, so expect the same error under Wine unless the prefix has real .NET Framework 4.8
  (`winetricks dotnet48`)**. Fallback: build the Launcher on Windows with Visual Studio as Ross did, and
  copy that folder over.
- What the Launcher does after login (Client.cs): patches WAR.exe / data.myp (PatchMYP) and starts
  `WAR.exe --acctname=<base64(user)> --sesstoken=<base64(token)>`, the token coming from LCR_START
  (`scripts/test-login.py` prints it).

## Reset the databases

```
~/ross-vr/server/scripts/warctl stop
sudo mysql -e "DROP DATABASE war_accounts; DROP DATABASE war_characters; DROP DATABASE war_world;"
~/ross-vr/server/scripts/db-setup.sh
~/ross-vr/server/scripts/warctl start      # first start adds the tables/columns the code expects
python3 -I ~/ross-vr/server/scripts/create-account.py ross ross   # then the GmLevel UPDATE + warctl restart
```

## Known problems / notes

- First WorldServer start alters the imported schema itself (normal for this emulator; it also happens on
  Windows): creates `bot_gear_overrides`, `bot_template_profiles`, `characters_action_counters`,
  `characters_tome_tactics`, adds `characters.FirstConnect`, drops `characters.honorpoints/honorrank` and
  `accounts.corelevel`.
- Harmless warnings at WorldServer start: `BotEditorAPI icons.xml not found at C:\Users\Admin\Downloads\myps\...`
  and `itemdata.csv not found` (developer-only bot editor data). Build warning
  `MSB3245: Could not resolve this reference ... "System.ComponentModel"` (FrameWork.csproj): unused, harmless.
- `RegionOcclusionFolder` = `los/` (249 zone .bin files from the repo's `deps/los`); occlusion loads per
  zone on demand.
- Not yet exercised with a real client: lobby login, character creation, entering the world, LOS in play.
