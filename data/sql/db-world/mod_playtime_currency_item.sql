-- Playtime currency - default token item (item 95501).
--
-- Mirrors the layout of the WotLK emblem items (BagFamily = currency token) so
-- the client treats it like the Badge/Emblem family: stackable, tradable and
-- usable straight from the bags.
--
-- displayid 32278 is a client-known gold coin icon (INV_Misc_Coin_09, used by
-- the Hakkari Coin), so no custom client icon is required. Point the row at
-- your own ItemDisplayInfo row once you ship a custom BLP in your patch.
--
-- Note: the item only shows in the currency tab with a client CurrencyTypes.dbc
-- patch; without it, it works as a regular stackable item.
--
-- Re-applicable: DELETE + INSERT.

DELETE FROM `item_template`
WHERE `entry` = 95501;

INSERT INTO `item_template`
    (`entry`, `class`, `subclass`, `SoundOverrideSubclass`, `name`, `displayid`,
     `Quality`, `BuyCount`, `AllowableClass`, `AllowableRace`, `ItemLevel`,
     `RequiredLevel`, `maxcount`, `stackable`, `BagFamily`, `Flags`,
     `description`, `VerifiedBuild`)
VALUES
    (95501, 12, 0, -1, 'Playtime Token', 32278,
     4, 1, -1, -1, 1,
     1, 0, 2147483647, 8192, 0,
     'Earned by playing on this server.', 12340);
