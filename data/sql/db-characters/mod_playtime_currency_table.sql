-- Playtime currency - ledger tables (characters DB).
--
-- playtime_currency_rewards: how many tokens each account has already been
-- paid today, used to enforce the daily cap.
-- playtime_currency_milestones: which milestone gifts each character has
-- already claimed, so they are only granted once.
--
-- Re-applicable: CREATE TABLE IF NOT EXISTS.

CREATE TABLE IF NOT EXISTS `playtime_currency_rewards` (
    `account_id` INT UNSIGNED NOT NULL DEFAULT 0,
    `reward_date` DATE NOT NULL DEFAULT '1970-01-01',
    `tokens` INT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`account_id`, `reward_date`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4 COLLATE = utf8mb4_general_ci;

CREATE TABLE IF NOT EXISTS `playtime_currency_milestones` (
    `character_guid` INT UNSIGNED NOT NULL DEFAULT 0,
    `milestone_hours` INT UNSIGNED NOT NULL DEFAULT 0,
    `claimed_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`character_guid`, `milestone_hours`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4 COLLATE = utf8mb4_general_ci;
