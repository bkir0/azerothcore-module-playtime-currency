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
#include "Player.h"
#include "StringFormat.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#include <algorithm>
#include <sstream>
#include <unordered_map>

namespace
{
    // AzerothCore keeps its prepared statements in a core side enum a module
    // cannot extend, so the two ledger statements below are the only SQL built
    // at runtime. Every interpolated value is an integer this module produced
    // itself, so there is nothing user supplied to escape.
    constexpr char const* REWARDS_TABLE = "playtime_currency_rewards";
    constexpr char const* MILESTONES_TABLE = "playtime_currency_milestones";
}

namespace PlaytimeCurrency
{
    Manager& Manager::Instance()
    {
        static Manager instance;
        return instance;
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

    void Manager::LoadConfig()
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

        _intervalMs = intervalMinutes * 60 * 1000;
        _msSinceLastTick = 0;

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

    void Manager::Tick(uint32 diff)
    {
        if (!_enabled)
            return;

        // Counting elapsed intervals instead of resetting the timer keeps the
        // schedule stable after a lag spike, without unbounded loops.
        _msSinceLastTick += diff;
        if (_msSinceLastTick < _intervalMs)
            return;

        uint32 dueIntervals = _msSinceLastTick / _intervalMs;
        _msSinceLastTick %= _intervalMs;

        for (uint32 interval = 0; interval < dueIntervals; ++interval)
            PayDueRewards();
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
        QueryResult result = CharacterDatabase.Query(Acore::StringFormat(
            "SELECT `tokens` FROM `{}` WHERE `account_id` = {} AND `reward_date` = CURDATE()", REWARDS_TABLE, accountId));
        if (!result)
            return 0;

        return (*result)[0].Get<uint32>();
    }

    void Manager::PayDueRewards()
    {
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

            MailItem(receiver->GetGUID().GetCounter(), receiver, _tokenEntry, count, _rewardSubject, _rewardBody);
            RecordTokens(accountId, count);
            ++paidAccounts;
        }

        // Milestones are checked on every tick too, so a player that just
        // crossed a threshold does not wait a full interval for the gift.
        for (auto const& [accountId, session] : sWorldSessionMgr->GetAllSessions())
        {
            Player* player = session->GetPlayer();
            if (player && player->IsInWorld())
                CheckMilestones(player);
        }

        if (_debug)
            LOG_DEBUG("module.playtime_currency", "Interval tick: {} account(s) online, {} paid.", payout.size(),
                paidAccounts);
    }

    bool Manager::IsMilestoneClaimed(uint32 characterGuid, uint32 hours) const
    {
        QueryResult result = CharacterDatabase.Query(Acore::StringFormat(
            "SELECT 1 FROM `{}` WHERE `character_guid` = {} AND `milestone_hours` = {} LIMIT 1", MILESTONES_TABLE,
            characterGuid, hours));

        return result && result->Fetch()[0].Get<uint32>() == 1;
    }

    void Manager::CheckMilestones(Player* player)
    {
        if (_milestones.empty() || !player)
            return;

        uint32 playedHours = player->GetTotalPlayedTime() / 3600;
        uint32 characterGuid = player->GetGUID().GetCounter();

        for (Milestone const& milestone : _milestones)
        {
            if (playedHours < milestone.Hours)
                break;

            if (IsMilestoneClaimed(characterGuid, milestone.Hours))
                continue;

            MailItem(characterGuid, player, milestone.ItemEntry, 1, _milestoneSubject, _milestoneBody);
            RecordMilestone(characterGuid, milestone.Hours);

            if (_debug)
                LOG_DEBUG("module.playtime_currency", "Milestone {}h claimed by character {}.", milestone.Hours,
                    characterGuid);
        }
    }

    void Manager::RecordTokens(uint32 accountId, uint32 amount) const
    {
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        trans->Append(Acore::StringFormat(
            "INSERT INTO `{}` (`account_id`, `reward_date`, `tokens`) VALUES ({}, CURDATE(), {}) "
            "ON DUPLICATE KEY UPDATE `tokens` = `tokens` + VALUES(`tokens`)",
            REWARDS_TABLE, accountId, amount));
        CharacterDatabase.CommitTransaction(trans);
    }

    void Manager::RecordMilestone(uint32 characterGuid, uint32 hours) const
    {
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        trans->Append(Acore::StringFormat(
            "INSERT INTO `{}` (`character_guid`, `milestone_hours`) VALUES ({}, {}) "
            "ON DUPLICATE KEY UPDATE `milestone_hours` = `milestone_hours`",
            MILESTONES_TABLE, characterGuid, hours));
        CharacterDatabase.CommitTransaction(trans);
    }

    void Manager::MailTokens(Player* player, uint32 amount)
    {
        if (!player || !amount)
            return;

        MailItem(player->GetGUID().GetCounter(), player, _tokenEntry, amount, _rewardSubject, _rewardBody);
    }

    void Manager::MailItem(ObjectGuid::LowType characterGuid, Player* player, uint32 itemEntry, uint32 amount,
        std::string const& subject, std::string const& body)
    {
        Item* item = Item::CreateItem(itemEntry, amount, player);
        if (!item)
        {
            LOG_ERROR("module.playtime_currency", "Item {} is missing from item_template, nothing was mailed.", itemEntry);
            return;
        }

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        // A mail with a body is a normal mail: the client already lets the
        // player take the items straight from it, no extra flag needed.
        MailDraft(subject, body)
            .AddItem(item)
            .SendMailTo(trans, player ? MailReceiver(player) : MailReceiver(characterGuid),
                player ? MailSender(player) : MailSender(static_cast<uint32>(0)), MAIL_CHECK_MASK_HAS_BODY);
        CharacterDatabase.CommitTransaction(trans);
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
    PlaytimeCurrency::Manager::Instance().LoadConfig();
}

void playtime_currency_WorldScript::OnStartup()
{
    PlaytimeCurrency::Manager::Instance().LoadConfig();
}

void playtime_currency_WorldScript::OnUpdate(uint32 diff)
{
    PlaytimeCurrency::Manager::Instance().Tick(diff);
}
