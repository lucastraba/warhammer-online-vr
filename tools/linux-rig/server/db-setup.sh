#!/bin/bash
# Creates the MySQL user and the three ProjectWAR databases, imports the base dumps,
# then applies the numbered incremental scripts (Linux replacement for incremental.ps1).
set -euo pipefail
SRC=${SRC:-$HOME/ross-vr/server/src}
DBUSER=${WAR_DB_USER:-war}
DBPASS=${WAR_DB_PASS:-warlocal}   # loopback-only test database; set WAR_DB_PASS to use your own
sudo mysql <<SQL
CREATE USER IF NOT EXISTS '$DBUSER'@'localhost' IDENTIFIED BY '$DBPASS';
CREATE USER IF NOT EXISTS '$DBUSER'@'127.0.0.1' IDENTIFIED BY '$DBPASS';
CREATE DATABASE IF NOT EXISTS war_accounts;
CREATE DATABASE IF NOT EXISTS war_characters;
CREATE DATABASE IF NOT EXISTS war_world;
GRANT ALL PRIVILEGES ON war_accounts.* TO '$DBUSER'@'localhost', '$DBUSER'@'127.0.0.1';
GRANT ALL PRIVILEGES ON war_characters.* TO '$DBUSER'@'localhost', '$DBUSER'@'127.0.0.1';
GRANT ALL PRIVILEGES ON war_world.* TO '$DBUSER'@'localhost', '$DBUSER'@'127.0.0.1';
FLUSH PRIVILEGES;
SQL
export MYSQL_PWD=$DBPASS
M="mysql -u $DBUSER -h 127.0.0.1 --default-character-set=utf8mb4"
cd "$SRC/Database"
[ -f war_world.sql ] || 7z x -y war_world.7z
for db in war_accounts war_characters war_world; do
  echo "importing $db.sql"; $M $db < $db.sql
done
# Ross's manual step: only add TokUnlock3 if the dump does not already have it
has=$($M -N -e "SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA='war_world' AND TABLE_NAME='mythic_src_item_infos' AND COLUMN_NAME='TokUnlock3'")
if [ "$has" = 0 ]; then
  $M war_world -e "ALTER TABLE mythic_src_item_infos ADD COLUMN TokUnlock3 int(11) unsigned NOT NULL DEFAULT 0;"
  echo "added TokUnlock3"
else
  echo "TokUnlock3 already present in dump, ALTER skipped"
fi
# incremental.ps1 equivalent: numbered scripts in order (each has its own USE)
for f in $(ls [0-9][0-9]_*.sql | sort); do
  echo "applying $f"; $M < "$f"
done
echo DONE
