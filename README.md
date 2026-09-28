# mod-playtime-currency

Pays an item to your players for being online, on a timer, with a daily cap so
idle accounts cannot farm it. Rewards go out by mail, so a full bag never eats
a payout.

The amount grows with total playtime, so a character with a hundred hours earns
more per tick than a fresh one. You can also hand out a one-off item at
playtime milestones, if you want something to chase.

## Requirements

- AzerothCore (WotLK, 3.3.5a) with the module loader enabled
- An item to hand out. The module ships a default one, entry 95501, in
  `data/sql/db-world`, but you can use any item you already have.

## Installing

```
git clone <this repo> modules/mod-playtime-currency
```

Then import the two SQL files:

```bash
mysql -u <user> -p acore_characters < modules/mod-playtime-currency/data/sql/db-characters/mod_playtime_currency_table.sql
mysql -u <user> -p acore_world       < modules/mod-playtime-currency/data/sql/db-world/mod_playtime_currency_item.sql
```

Skip the second one if you already have a token item.

Copy the config to your server config directory and edit it:

```
cp modules/mod-playtime-currency/conf/mod_playtime_currency.conf.dist \
   env/dist/etc/modules/mod_playtime_currency.conf
```

Build and restart the worldserver. That is it.

## Configuration

```ini
[Worldserver]
PlaytimeCurrency.Enable = 1
PlaytimeCurrency.TokenItemEntry = 95501

# Minutes between payouts. 0 disables the module.
PlaytimeCurrency.IntervalMinutes = 15

# Tokens per account per day. 0 disables the module.
PlaytimeCurrency.DailyCap = 40

# "hoursPlayed:tokensPerInterval,..."
# Empty means a flat 1 token per interval.
PlaytimeCurrency.ProgressiveTiers = "10:2,50:3,120:4"

# "hoursPlayed:itemEntry,..." one-off gifts, once per character.
# Example: "10:45577,50:45574,120:45575"
PlaytimeCurrency.Milestones = ""

PlaytimeCurrency.RewardMailSubject = "Playtime reward"
PlaytimeCurrency.RewardMailBody = "Thank you for playing. Here are your tokens."

PlaytimeCurrency.MilestoneMailSubject = "Playtime milestone reached"
PlaytimeCurrency.MilestoneMailBody = "Thank you for sticking around. Here is a gift for your dedication."

# One log line per tick and per milestone.
PlaytimeCurrency.Debug = 0
```

The daily cap is per account, not per character, and the day rolls over at
server midnight. Tiers are checked against the character's total playtime, and
the module pays an account once per interval no matter how many of its
characters are online: the per-character tier amounts are added up and arrive as
a single mail.

If the token entry, the interval or the cap are set to 0 the module refuses to
start and says so in the log, rather than silently doing nothing.

Malformed entries in the tier or milestone lists are skipped with a warning, so
one typo does not take the whole config down.

Milestones are scanned on their own one minute timer, so a character that just
crossed a threshold gets the gift within a minute instead of waiting for the
next payout. The interval itself is capped at 24 hours, because it is stored in
milliseconds and a larger value would overflow.

## A note on the SQL

AzerothCore has a prepared statement system, but the statement lists live in
core side enums that a module cannot extend without patching core, and patching
core would break the "clone it into `modules/` and build" promise. So the two
statements here are plain SQL, and the module is built so the payout path
almost never reads:

- one query to load the day's ledger, when the day changes
- one query the first time a character is seen, to cache its milestone claims
- everything else is an in-memory lookup, and the writes go through
  `CommitTransaction`, which is queued on the database thread

That is also why the world update thread never waits on the database here. The
elapsed time is compared in a `uint64` so a lag spike cannot wrap the timer.

Keep the worldserver and the database in the same timezone: the day rollover is
detected from the server clock while the ledger is keyed by `CURDATE()`.

## Commands

`.playtime status` shows the player what they have earned today, what the next
tier is and how long until the next payout.

`.playtime grant <name> <amount>` is for game masters. It mails the tokens and
works on offline characters, which is handy for compensating someone who is not
around.

## Tables

Two tables in the characters database:

- `playtime_currency_rewards` - account id, day, tokens already paid
- `playtime_currency_milestones` - character guid, milestone hours, when claimed

Nothing else is written, and nothing is touched in the world database apart from
the item you configure.

## Notes

- If you want your own icon for the token, that is a client side job: a BLP in
  your patch under `Interface\Icons\` plus a row in `ItemDisplayInfo.dbc`. The
  shipped item uses a coin icon the client already knows, so the module works
  without any patch.
- To put the token in the currency tab you need a `CurrencyTypes.dbc` patch on
  top of that. Without it, it behaves as a normal stackable item.
- The reward mail is sent from the player to themselves, which is how
  AzerothCore handles mail without a real sender NPC. Change `MailItem` if you
  want a proper sender creature.

## License

Copyright (C) 2026 z4d3s. AGPL-3.0, the same as AzerothCore. See `LICENSE`.

Blizzard owns World of Warcraft; this is an unofficial project and nobody at
Blizzard had anything to do with it.
