# Time of day on your own server

The ProjectWAR server sends the game its own clock, in UTC, so on a private server it is night or day in the game
whenever it is night or day in UTC. There is no setting for it in the server as it comes. This two-line change adds one,
`TimeOfDayOffsetHours`, which WARVRConfig.exe (or you, by hand) can then set in the server's `World.xml`.

It is a change to the server, not to the mod: it only matters if you run your own ProjectWAR server, and you build it
once more after making it.

## The change

1. In `WorldServer\Configs\WorldConfigs.cs`, under the line `public byte RealmId = 1;`, add:

   ```csharp
   public double TimeOfDayOffsetHours = 0;   // shifts the in-game time of day; 12 swaps day and night
   ```

2. In `WorldServer\NetWork\Handler\MovementHandlers.cs`, find this line (in the code that sends `F_SET_TIME`):

   ```csharp
   Out.WriteUInt16((ushort)(DateTime.UtcNow.TimeOfDay.TotalSeconds / 65.5d));
   ```

   and change it to:

   ```csharp
   Out.WriteUInt16((ushort)(DateTime.UtcNow.AddHours(Program.Config.TimeOfDayOffsetHours).TimeOfDay.TotalSeconds / 65.5d));
   ```

With git you can apply both at once from the ProjectWAR folder: `git apply path\to\extras\server-time-of-day.patch`.

## Build it, and keep your logins

Build the server as when you set it up (Visual Studio: Build / Build Solution), with the servers stopped.

Building copies the default XML files from `WorldServer\Configs\LocalDevelopment\` over the ones in
`bin\Release\Configs\`, which resets your MySQL logins. Either back up `bin\Release\Configs` and copy it back after
building, or put your logins into the files in `WorldServer\Configs\LocalDevelopment\` once, so every build copies the
right ones.

## Set the time

- With the settings window: run WARVRConfig.exe, pick the server's `bin\Release` folder under "Your own server", choose
  the time of day and save. It writes `bin\Release\Configs\World.xml` and, if it finds it,
  `WorldServer\Configs\LocalDevelopment\World.xml` too, so a later build keeps the setting.
- By hand: in `bin\Release\Configs\World.xml`, under `<RealmId>1</RealmId>`, add
  `<TimeOfDayOffsetHours>12</TimeOfDayOffsetHours>` (any number of hours).

Then restart the WorldServer. The new time shows when you log in or change zones.

Tested on a local ProjectWAR server (under Mono on Linux): with 12, a night scene turned into day. The window shows
whether your WorldServer.exe already has the change, and only lets you pick a time when it does.
