/*
 * mod-playtime-currency
 *
 * Chat commands for the playtime currency module.
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
#include "CommandScript.h"
#include "Player.h"
#include "StringFormat.h"
#include "WorldSession.h"

using namespace Acore::ChatCommands;

namespace
{
    // ".playtime status"
    bool HandleStatusCommand(ChatHandler* handler)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
        {
            handler->SendErrorMessage("Playtime currency: this command needs a player session.");
            return false;
        }

        handler->SendSysMessage(PlaytimeCurrency::Manager::Instance().BuildStatus(player));
        return true;
    }

    // ".playtime grant [name] <amount>", game masters only.
    bool HandleGrantCommand(ChatHandler* handler, Optional<PlayerIdentifier> player, uint32 amount)
    {
        if (!player)
            player = PlayerIdentifier::FromTargetOrSelf(handler);

        if (!player)
        {
            handler->SendErrorMessage("Playtime currency: no player given.");
            return false;
        }

        if (!amount)
        {
            handler->SendErrorMessage("Playtime currency: the amount must be greater than 0.");
            return false;
        }

        PlaytimeCurrency::Manager& manager = PlaytimeCurrency::Manager::Instance();
        if (!manager.IsEnabled())
        {
            handler->SendErrorMessage("Playtime currency: the module is disabled.");
            return false;
        }

        // Works for offline characters too: the mail is addressed by low guid.
        Player* online = player->IsConnected() ? player->GetConnectedPlayer() : nullptr;
        if (!manager.MailItem(player->GetGUID().GetCounter(), online, manager.GetTokenEntry(), amount,
            "Playtime currency granted", "Granted by a game master."))
        {
            handler->SendSysMessage(Acore::StringFormat(
                "Playtime currency: item {} does not exist, nothing was mailed to {}.", manager.GetTokenEntry(),
                player->GetName()));
            return false;
        }

        handler->SendSysMessage(Acore::StringFormat("Playtime currency: granted {} token(s) to {}.", amount,
            player->GetName()));

        return true;
    }
}

Acore::ChatCommands::ChatCommandTable playtime_currency_commandscript::GetCommands() const
{
    // Handlers are passed as plain names: the table stores function lvalues.
    static ChatCommandTable playtimeCommandTable =
    {
        { "status", HandleStatusCommand, SEC_PLAYER, Console::Yes },
        { "grant", HandleGrantCommand, SEC_ADMINISTRATOR, Console::Yes }
    };

    static ChatCommandTable commandTable =
    {
        { "playtime", playtimeCommandTable }
    };

    return commandTable;
}
