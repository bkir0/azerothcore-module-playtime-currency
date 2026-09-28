/*
 * mod-playtime-currency
 *
 * An AzerothCore module that pays a token item to online accounts on a fixed
 * interval, scaled by playtime, with a daily cap and optional milestone gifts.
 *
 * Copyright (C) 2026 z4d3s
 *
 * This module is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * This module is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Affero General Public License along
 * with this module. If not, see <https://www.gnu.org/licenses/>.
 *
 * World of Warcraft and AzerothCore are the property of Blizzard Entertainment
 * and are used here under their own terms. This module is an unofficial project
 * and is not affiliated with nor endorsed by Blizzard Entertainment.
 */

#include "mod_playtime_currency.h"

#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "Log.h"
#include "Mail.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "StringFormat.h"
#include "Util.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#include <algorithm>
#include <ctime>
#include <sstream>

namespace
{
    // AzerothCore keeps its prepared statements in a core side enum a module
    // cannot extend, so these statements are plain SQL. That is why the payout
    // path is built so that it almost never reads: the ledger and the milestone
    // claims are cached in memory, and the two queries that fill those caches
    // run through AsyncQuery, off the world update thread. Every interpolated
    // value is an integer this module produced, so there is nothing to escape.
    constexpr char const* REWARDS_TABLE = "playtime_currency_rewards";
    constexpr char const* MILESTONES_TABLE = "playtime_currency_milestones";

    // Milestones are scanned on their own timer instead of on the payout tick,
    // so a character that just crossed a threshold gets the gift within a
    // minute rather than waiting for the next interval.
    constexpr uint32 MILESTONE_CHECK_INTERVAL_MS = 60 * 1000;

    // Appends a mail to a transaction. Returns false when the item does not
    // exist, so the caller can skip recording a reward that was never sent.
    bool AppendMail(CharacterDatabaseTransaction trans, ObjectGuid::LowType characterGuid, Player* player,
        uint32 itemEntry, uint32 amount, std::string const& subject, std::string const& body)
    {
        Item* item = Item::CreateItem(itemEntry, amount, player);
        if (!item)
        {
            LOG_ERROR("module.playtime_currency", "Item {} is missing from item_template, no mail was created.",
                itemEntry);
            return false;
        }

        // A mail with a body is a normal mail: the client already lets the
        // player take the items straight from it, no extra flag needed.
        MailDraft(subject, body)
            .AddItem(item)
            .SendMailTo(trans, player ? MailReceiver(player) : MailReceiver(characterGuid),
                player ? MailSender(player) : MailSender(static_cast<uint32>(0)), MAIL_CHECK_MASK_HAS_BODY);
        return true;
    }

    void AppendTokens(CharacterDatabaseTransaction trans, uint32 accountId, uint32 amount)
    {
        trans->Append(Acore::StringFormat(
            "INSERT INTO `{}` (`account_id`, `reward_date`, `tokens`) VALUES ({}, CURDATE(), {}) "
            "ON DUPLICATE KEY UPDATE `tokens` = `tokens` + VALUES(`tokens`)",
            REWARDS_TABLE, accountId, amount));
    }

    void AppendMilestoneClaim(CharacterDatabaseTransaction trans, uint32 characterGuid, uint32 hours)
    {
        trans->Append(Acore::StringFormat(
            "INSERT INTO `{}` (`character_guid`, `milestone_hours`) VALUES ({}, {}) "
            "ON DUPLICATE KEY UPDATE `milestone_hours` = `milestone_hours`",
            MILESTONES_TABLE, characterGuid, hours));
    }
}

namespace PlaytimeCurrency
{
    Manager& Manager::Instance()
    {
        static Manager instance;
        return instance;
    }

    uint64 Manager::MakeMilestoneKey(uint32 characterGuid, uint32 hours)
    {
        return (static_cast<uint64>(characterGuid) << 32) | hours;
    }

    std::string Manager::GetCurrentDay()
    {
        // Has to match the timezone of the database, because the ledger is
        // keyed by CURDATE() on the MySQL side. Keep the worldserver and the
        // database in the same timezone.
        return secsToTimeString(static_cast<uint64>(time(nullptr))).substr(0, 10);
    }

    std::vector<Tier> Manager::ParseTiers(std::string const& raw)
    {
        std::vector<Tier> tiers;
        std::stringstream stream(raw);
        std::string entry;

        while (std::getline(stream, entry, ','))
        {
            std::stringstream pair(entry);
            uint32 hours = 0;
            uint32 tokens = 0;
            char separator = 0;

            if (pair >> hours >> separator >> tokens && separator == ':' && hours > 0 && tokens > 0)
                tiers.push_back({ hours, tokens });
            else if (!entry.empty())
                LOG_ERROR("module.playtime_currency", "Ignoring malformed tier '{}', expected 'hours:tokens'.", entry);
        }

        // Highest threshold has to win when a player passes several tiers.
        std::sort(tiers.begin(), tiers.end(), [](Tier const& left, Tier const& right) { return left.Hours < right.Hours; });
        return tiers;
    }

    std::vector<Milestone> Manager::ParseMilestones(std::string const& raw)
    {
        std::vector<Milestone> milestones;
        std::stringstream stream(raw);
        std::string entry;

        while (std::getline(stream, entry, ','))
        {
            std::stringstream pair(entry);
            uint32 hours = 0;
            uint32 itemEntry = 0;
            char separator = 0;

            if (pair >> hours >> separator >> itemEntry && separator == ':' && hours > 0 && itemEntry > 0)
                milestones.push_back({ hours, itemEntry });
            else if (!entry.empty())
                LOG_ERROR("module.playtime_currency", "Ignoring malformed milestone '{}', expected 'hours:itemEntry'.",
                    entry);
        }

        std::sort(milestones.begin(), milestones.end(),
            [](Milestone const& left, Milestone const& right) { return left.Hours < right.Hours; });
        return milestones;
    }

    void Manager::LoadConfig(bool validateItems)
    {
        _enabled = sConfigMgr->GetOption<bool>("PlaytimeCurrency.Enable", true);
        _debug = sConfigMgr->GetOption<bool>("PlaytimeCurrency.Debug", false);
        _tokenEntry = sConfigMgr->GetOption<uint32>("PlaytimeCurrency.TokenItemEntry", 0);
        uint32 intervalMinutes = sConfigMgr->GetOption<uint32>("PlaytimeCurrency.IntervalMinutes", 15);
        _dailyCap = sConfigMgr->GetOption<uint32>("PlaytimeCurrency.DailyCap", 0);
        _rewardSubject = sConfigMgr->GetOption<std::string>("PlaytimeCurrency.RewardMailSubject", "Playtime reward");
        _rewardBody = sConfigMgr->GetOption<std::string>("PlaytimeCurrency.RewardMailBody",
            "Thank you for playing. Here are your tokens.");
        _milestoneSubject = sConfigMgr->GetOption<std::string>("PlaytimeCurrency.MilestoneMailSubject",
            "Playtime milestone reached");
        _milestoneBody = sConfigMgr->GetOption<std::string>("PlaytimeCurrency.MilestoneMailBody",
            "Thank you for sticking around. Here is a gift for your dedication.");
        _tiers = ParseTiers(sConfigMgr->GetOption<std::string>("PlaytimeCurrency.ProgressiveTiers", "10:2,50:3,120:4"));
        _milestones = ParseMilestones(sConfigMgr->GetOption<std::string>("PlaytimeCurrency.Milestones", ""));

        // A zero interval would tick forever, a zero cap would pay nothing and
        // a zero item entry cannot be created: disable the module instead of
        // misbehaving.
        if (_tokenEntry == 0)
        {
            LOG_ERROR("module.playtime_currency", "PlaytimeCurrency.TokenItemEntry is 0, disabling the module.");
            _enabled = false;
        }

        if (intervalMinutes == 0)
        {
            LOG_ERROR("module.playtime_currency", "PlaytimeCurrency.IntervalMinutes is 0, disabling the module.");
            _enabled = false;
        }

        if (_dailyCap == 0)
        {
            LOG_ERROR("module.playtime_currency", "PlaytimeCurrency.DailyCap is 0, disabling the module.");
            _enabled = false;
        }

        // The interval is stored in milliseconds, so the multiplication below
        // has to stay inside a uint32. A big configured value would wrap
        // around and leave a nonsense interval, either paying on every tick or
        // never paying at all, so cap it at a day.
        constexpr uint32 MAX_INTERVAL_MINUTES = 24 * 60;
        if (intervalMinutes > MAX_INTERVAL_MINUTES)
        {
            LOG_ERROR("module.playtime_currency",
                "PlaytimeCurrency.IntervalMinutes {} is above the maximum of {}, clamping it.", intervalMinutes,
                MAX_INTERVAL_MINUTES);
            intervalMinutes = MAX_INTERVAL_MINUTES;
        }

        // Catch a bad item entry at startup instead of silently never paying.
        // item_template is not loaded when the config is read, so the config
        // reload pass skips this and OnStartup does the real check.
        if (validateItems && _enabled && !sObjectMgr->GetItemTemplate(_tokenEntry))
        {
            LOG_ERROR("module.playtime_currency",
                "PlaytimeCurrency.TokenItemEntry {} does not exist in item_template, disabling the module.",
                _tokenEntry);
            _enabled = false;
        }

        for (Milestone const& milestone : _milestones)
            if (validateItems && !sObjectMgr->GetItemTemplate(milestone.ItemEntry))
                LOG_ERROR("module.playtime_currency",
                    "Milestone {}h points at item {}, which does not exist in item_template, it will never be granted.",
                    milestone.Hours, milestone.ItemEntry);

        _intervalMs = intervalMinutes * 60 * 1000;
        _msSinceLastTick = 0;

        // A reload rebuilds the caches, they have to be fetched again.
        _ledgerLoaded = false;
        _ledgerDay.clear();
        _tokensToday.clear();
        _claimedMilestones.clear();
        _loadedCharacters.clear();

        LOG_INFO("module.playtime_currency",
            "Playtime currency: enabled {}, item {}, interval {} min, daily cap {} per account, {} tier(s), {} milestone(s)",
            _enabled, _tokenEntry, intervalMinutes, _dailyCap, _tiers.size(), _milestones.size());
    }

    uint32 Manager::GetMsUntilNextReward() const
    {
        if (!_enabled || _intervalMs == 0)
            return 0;

        return _intervalMs - std::min(_msSinceLastTick, _intervalMs);
    }

    void Manager::RefreshDailyLedger()
    {
        std::string day = GetCurrentDay();
        if (_ledgerDay == day && _ledgerLoaded)
            return;

        _ledgerDay = day;
        _tokensToday.clear();
        _ledgerLoaded = false;

        // One query for the whole realm, on the database thread.
        CharacterDatabase.AsyncQuery(Acore::StringFormat(
            "SELECT `account_id`, `tokens` FROM `{}` WHERE `reward_date` = CURDATE()", REWARDS_TABLE))
            .WithCallback([this](QueryResult result)
            {
                if (result)
                {
                    do
                    {
                        Field* fields = result->Fetch();
                        _tokensToday[fields[0].Get<uint32>()] = fields[1].Get<uint32>();
                    } while (result->NextRow());
                }

                _ledgerLoaded = true;

                if (_debug)
                    LOG_DEBUG("module.playtime_currency", "Daily ledger loaded: {} account(s) with a payout today.",
                        _tokensToday.size());
            });
    }

    void Manager::LoadCharacterClaims(uint32 characterGuid)
    {
        _loadedCharacters.insert(characterGuid);

        CharacterDatabase.AsyncQuery(Acore::StringFormat(
            "SELECT `milestone_hours` FROM `{}` WHERE `character_guid` = {}", MILESTONES_TABLE, characterGuid))
            .WithCallback([this, characterGuid](QueryResult result)
            {
                if (result)
                {
                    do
                    {
                        Field* fields = result->Fetch();
                        _claimedMilestones.insert(MakeMilestoneKey(characterGuid, fields[0].Get<uint32>()));
                    } while (result->NextRow());
                }

                if (_debug)
                    LOG_DEBUG("module.playtime_currency", "Loaded {} milestone claim(s) for character {}.",
                        result ? result->GetRowCount() : 0, characterGuid);
            });
    }

    void Manager::Tick(uint32 diff)
    {
        if (!_enabled)
            return;

        // Counting elapsed intervals instead of resetting the timer keeps the
        // schedule stable after a lag spike, without unbounded loops. The sum
        // is done in a uint64 so a long stall cannot wrap the accumulator and
        // leave the timer stuck short of the interval forever.
        uint64 elapsed = static_cast<uint64>(_msSinceLastTick) + diff;
        if (elapsed < _intervalMs)
        {
            _msSinceLastTick = static_cast<uint32>(elapsed);
            return;
        }

        uint32 dueIntervals = static_cast<uint32>(elapsed / _intervalMs);
        _msSinceLastTick = static_cast<uint32>(elapsed % _intervalMs);

        for (uint32 interval = 0; interval < dueIntervals; ++interval)
            PayDueRewards();

        // Milestones are checked on their own timer, not on the payout tick, so
        // a character that just crossed a threshold does not wait a full
        // interval for the gift.
        uint64 elapsedCheck = static_cast<uint64>(_msSinceMilestoneCheck) + diff;
        if (elapsedCheck >= MILESTONE_CHECK_INTERVAL_MS)
        {
            _msSinceMilestoneCheck = static_cast<uint32>(elapsedCheck % MILESTONE_CHECK_INTERVAL_MS);
            CheckOnlineMilestones();
        }
        else
            _msSinceMilestoneCheck = static_cast<uint32>(elapsedCheck);
    }

    uint32 Manager::GetTokensPerInterval(uint32 playedHours) const
    {
        uint32 tokens = 1;

        for (Tier const& tier : _tiers)
            if (playedHours >= tier.Hours)
                tokens = tier.Tokens;

        return tokens;
    }

    uint32 Manager::GetNextTierHours(uint32 playedHours) const
    {
        for (Tier const& tier : _tiers)
            if (tier.Hours > playedHours)
                return tier.Hours;

        return 0;
    }

    uint32 Manager::GetTokensGrantedToday(uint32 accountId) const
    {
        auto const itr = _tokensToday.find(accountId);
        return itr != _tokensToday.end() ? itr->second : 0;
    }

    bool Manager::IsMilestoneClaimed(uint32 characterGuid, uint32 hours) const
    {
        return _claimedMilestones.contains(MakeMilestoneKey(characterGuid, hours));
    }

    void Manager::PayDueRewards()
    {
        // The cache fills asynchronously, and paying without it would blow
        // through the daily cap, so wait for the first load.
        RefreshDailyLedger();
        if (!_ledgerLoaded)
        {
            if (_debug)
                LOG_DEBUG("module.playtime_currency", "Interval tick: waiting for the daily ledger, nothing paid.");
            return;
        }

        // One payout per account per interval, no matter how many of its
        // characters are online: the per character tier rewards are summed and
        // delivered as a single mail.
        std::unordered_map<uint32, uint32> payout;
        std::unordered_map<uint32, Player*> mailbox;

        for (auto const& [accountId, session] : sWorldSessionMgr->GetAllSessions())
        {
            Player* player = session->GetPlayer();
            if (!player || !player->IsInWorld())
                continue;

            payout[accountId] += GetTokensPerInterval(player->GetTotalPlayedTime() / 3600);
            mailbox.emplace(accountId, player);
        }

        uint32 paidAccounts = 0;

        for (auto const& [accountId, amount] : payout)
        {
            uint32 grantedToday = GetTokensGrantedToday(accountId);
            if (grantedToday >= _dailyCap)
                continue;

            uint32 count = std::min(amount, _dailyCap - grantedToday);
            if (!count)
                continue;

            Player* receiver = mailbox[accountId];
            if (!receiver)
                continue;

            // Mail and ledger move together, so a crash can never pay a
            // reward that was not sent, nor send one that was not counted.
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            if (!AppendMail(trans, receiver->GetGUID().GetCounter(), receiver, _tokenEntry, count, _rewardSubject,
                _rewardBody))
                continue;

            AppendTokens(trans, accountId, count);
            CharacterDatabase.CommitTransaction(trans);

            _tokensToday[accountId] = grantedToday + count;
            ++paidAccounts;
        }

        if (_debug)
            LOG_DEBUG("module.playtime_currency", "Interval tick: {} account(s) online, {} paid.", payout.size(),
                paidAccounts);
    }

    void Manager::CheckOnlineMilestones()
    {
        if (_milestones.empty())
            return;

        for (auto const& [accountId, session] : sWorldSessionMgr->GetAllSessions())
        {
            Player* player = session->GetPlayer();
            if (player && player->IsInWorld())
                CheckMilestones(player);
        }
    }

    void Manager::CheckMilestones(Player* player)
    {
        if (_milestones.empty() || !player)
            return;

        uint32 characterGuid = player->GetGUID().GetCounter();

        // The claims of this character are still being fetched: ask once and
        // look again on the next tick.
        if (!_loadedCharacters.contains(characterGuid))
        {
            LoadCharacterClaims(characterGuid);
            return;
        }

        uint32 playedHours = player->GetTotalPlayedTime() / 3600;

        for (Milestone const& milestone : _milestones)
        {
            if (playedHours < milestone.Hours)
                break;

            if (IsMilestoneClaimed(characterGuid, milestone.Hours))
                continue;

            // Gift and claim share one transaction, and the claim is only
            // recorded when the mail really was created.
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            if (!AppendMail(trans, characterGuid, player, milestone.ItemEntry, 1, _milestoneSubject, _milestoneBody))
                continue;

            AppendMilestoneClaim(trans, characterGuid, milestone.Hours);
            CharacterDatabase.CommitTransaction(trans);
            _claimedMilestones.insert(MakeMilestoneKey(characterGuid, milestone.Hours));

            if (_debug)
                LOG_DEBUG("module.playtime_currency", "Milestone {}h granted to character {}.", milestone.Hours,
                    characterGuid);
        }
    }

    void Manager::MailTokens(Player* player, uint32 amount)
    {
        if (!player || !amount)
            return;

        MailItem(player->GetGUID().GetCounter(), player, _tokenEntry, amount, _rewardSubject, _rewardBody);
    }

    bool Manager::MailItem(ObjectGuid::LowType characterGuid, Player* player, uint32 itemEntry, uint32 amount,
        std::string const& subject, std::string const& body)
    {
        if (!characterGuid)
        {
            LOG_ERROR("module.playtime_currency", "Refusing to mail item {} to an invalid character guid.", itemEntry);
            return false;
        }

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        if (!AppendMail(trans, characterGuid, player, itemEntry, amount, subject, body))
            return false;

        CharacterDatabase.CommitTransaction(trans);
        return true;
    }

    std::string Manager::BuildStatus(Player* player) const
    {
        if (!player)
            return "No player.";

        uint32 accountId = player->GetSession()->GetAccountId();
        uint32 playedHours = player->GetTotalPlayedTime() / 3600;
        uint32 grantedToday = GetTokensGrantedToday(accountId);
        uint32 perInterval = GetTokensPerInterval(playedHours);
        uint32 nextTierHours = GetNextTierHours(playedHours);
        uint32 msLeft = GetMsUntilNextReward();

        std::string status = Acore::StringFormat("Playtime currency: {} | today {}/{} | per interval {} | playtime {}h",
            _enabled ? "enabled" : "disabled", grantedToday, _dailyCap, perInterval, playedHours);

        if (nextTierHours)
            status += Acore::StringFormat(" | next tier at {}h", nextTierHours);

        if (_enabled)
            status += Acore::StringFormat(" | next reward in {}m {}s", msLeft / 60000, (msLeft % 60000) / 1000);

        status += Acore::StringFormat(" | milestones {}", _milestones.size());

        return status;
    }
}

void playtime_currency_WorldScript::OnAfterConfigLoad(bool /*reload*/)
{
    // item_template is not loaded yet at this point, so the item check waits
    // for OnStartup.
    PlaytimeCurrency::Manager::Instance().LoadConfig(false);
}

void playtime_currency_WorldScript::OnStartup()
{
    PlaytimeCurrency::Manager::Instance().LoadConfig();
}

void playtime_currency_WorldScript::OnUpdate(uint32 diff)
{
    PlaytimeCurrency::Manager::Instance().Tick(diff);
}
