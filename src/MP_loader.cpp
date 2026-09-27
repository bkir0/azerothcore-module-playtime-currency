/*
 * mod-playtime-currency
 *
 * Module entry point: registers the world script and the chat commands.
 *
 * Copyright (C) 2026 z4d3s
 *
 * This module is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * World of Warcraft and AzerothCore are the property of Blizzard Entertainment
 * and are used here under their own terms. This module is an unofficial project
 * and is not affiliated with nor endorsed by Blizzard Entertainment.
 */

#include "mod_playtime_currency.h"

void Addmod_playtime_currencyScripts()
{
    new playtime_currency_WorldScript();
    new playtime_currency_commandscript();
}
