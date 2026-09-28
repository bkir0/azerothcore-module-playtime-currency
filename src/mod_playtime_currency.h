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

#ifndef MOD_PLAYTIME_CURRENCY_H_
#define MOD_PLAYTIME_CURRENCY_H_

#include "CommandScript.h"
#include "ObjectGuid.h"
#include "WorldScript.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class ChatHandler;
class Player;

namespace PlaytimeCurrency
{
    // One "hoursPlayed:tokensPerInterval" step of the progressive payout.
    struct Tier
    {
        uint32 Hours;
        uint32 Tokens;
    };

    // One "hoursPlayed:itemEntry" gift, granted once per character.
    struct Milestone
    {
        uint32 Hours;
        uint32 ItemEntry;
    };

    // Holds the configuration and pays the rewards. Single instance, created on
    // first use, so the world script and the command script share one state.
    //
    // Everything the payout path needs to read is cached in memory, so the
    // world update thread never waits on the database. The database is only
    // touched asynchronously: one query to load the daily ledger when the day
    // changes, one per character the first time that character is seen, and
    // non-blocking transactions to write.
    class Manager
    {
    public:
        static Manager& Instance();

        // validateItems checks the configured items against item_template, which
        // is only loaded once the world is up, so the config reload pass has to
        // pass false.
        void LoadConfig(bool validateItems = true);

        // Drives the interval timer and pays whatever became due.
        void Tick(uint32 diff);

        bool IsEnabled() const { return _enabled; }
        uint32 GetIntervalMs() const { return _intervalMs; }
        uint32 GetMsUntilNextReward() const;
        uint32 GetTokenEntry() const { return _tokenEntry; }
        uint32 GetDailyCap() const { return _dailyCap; }
        std::vector<Tier> const& GetTiers() const { return _tiers; }
        std::vector<Milestone> const& GetMilestones() const { return _milestones; }

        uint32 GetTokensPerInterval(uint32 playedHours) const;
        uint32 GetNextTierHours(uint32 playedHours) const;
        uint32 GetTokensGrantedToday(uint32 accountId) const;

        // Mails tokens to a player, ignoring the daily cap. Used by the
        // interval payout, the milestone gifts and the GM command.
        void MailTokens(Player* player, uint32 amount);
        // Mails an item to a character that is online (player != nullptr) or
        // only known by its low guid (offline). Returns false when the item
        // does not exist, so callers do not record a reward that was never
        // delivered.
        bool MailItem(ObjectGuid::LowType characterGuid, Player* player, uint32 itemEntry, uint32 amount,
            std::string const& subject, std::string const& body);

        // Grants every milestone gift a character reached but has not claimed.
        void CheckMilestones(Player* player);
        bool IsMilestoneClaimed(uint32 characterGuid, uint32 hours) const;

        // Player facing summary behind ".playtime status".
        std::string BuildStatus(Player* player) const;

    private:
        Manager() = default;

        // Pays one interval to every account with online characters.
        void PayDueRewards();
        // Walks the online characters looking for unclaimed milestones.
        void CheckOnlineMilestones();
        // Loads today's ledger with a single query when the day rolls over.
        void RefreshDailyLedger();
        // Loads the claims of a character the first time it is seen.
        void LoadCharacterClaims(uint32 characterGuid);

        static std::string GetCurrentDay();
        static uint64 MakeMilestoneKey(uint32 characterGuid, uint32 hours);
        static std::vector<Tier> ParseTiers(std::string const& raw);
        static std::vector<Milestone> ParseMilestones(std::string const& raw);

        bool _enabled = false;
        bool _debug = false;
        uint32 _tokenEntry = 0;
        uint32 _intervalMs = 0;
        uint32 _dailyCap = 0;
        uint32 _msSinceLastTick = 0;
        uint32 _msSinceMilestoneCheck = 0;
        std::string _rewardSubject;
        std::string _rewardBody;
        std::string _milestoneSubject;
        std::string _milestoneBody;
        std::vector<Tier> _tiers;
        std::vector<Milestone> _milestones;

        // account -> tokens already paid today.
        std::unordered_map<uint32, uint32> _tokensToday;
        // (character, hours) pairs already granted.
        std::unordered_set<uint64> _claimedMilestones;
        // Characters whose claims are cached.
        std::unordered_set<uint32> _loadedCharacters;
        std::string _ledgerDay;
        bool _ledgerLoaded = false;
    };
}

class playtime_currency_WorldScript : public WorldScript
{
public:
    playtime_currency_WorldScript() : WorldScript("playtime_currency_WorldScript")
    {
    }

    void OnAfterConfigLoad(bool reload) override;
    void OnStartup() override;
    void OnUpdate(uint32 diff) override;
};

class playtime_currency_commandscript : public CommandScript
{
public:
    playtime_currency_commandscript() : CommandScript("playtime_currency_commandscript")
    {
    }

    // Declared with the exact type of the base class so the header does not
    // need the chat command definitions.
    std::vector<Acore::ChatCommands::ChatCommandBuilder> GetCommands() const override;
};

#endif
