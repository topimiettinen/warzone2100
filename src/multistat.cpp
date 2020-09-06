/*
	This file is part of Warzone 2100.
	Copyright (C) 1999-2004  Eidos Interactive
	Copyright (C) 2005-2020  Warzone 2100 Project

	Warzone 2100 is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	Warzone 2100 is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Warzone 2100; if not, write to the Free Software
	Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
*/
/*
 * MultiStat.c
 *
 * Alex Lee , pumpkin studios, EIDOS
 *
 * load / update / store multiplayer statistics for league tables etc...
 */

#if defined(__GNUC__) && !defined(__INTEL_COMPILER) && !defined(__clang__) && (9 <= __GNUC__)
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wdeprecated-copy" // Workaround Qt < 5.13 `deprecated-copy` issues with GCC 9
#endif

#include <QtCore/QSettings> // **NOTE: Qt headers _must_ be before platform specific headers so we don't get conflicts.

#if defined(__GNUC__) && !defined(__INTEL_COMPILER) && !defined(__clang__) && (9 <= __GNUC__)
# pragma GCC diagnostic pop // Workaround Qt < 5.13 `deprecated-copy` issues with GCC 9
#endif

#include "lib/framework/frame.h"
#include "lib/framework/file.h"
#include "lib/netplay/nettypes.h"

#include "main.h"
#include "mission.h" // for cheats
#include "multistat.h"
#include "activity.h"
#include <utility>


// ////////////////////////////////////////////////////////////////////////////
// STATS STUFF
// ////////////////////////////////////////////////////////////////////////////
static PLAYERSTATS playerStats[MAX_PLAYERS];


// ////////////////////////////////////////////////////////////////////////////
// Get Player's stats
PLAYERSTATS const &getMultiStats(UDWORD player)
{
	return playerStats[player];
}

static void NETauto(PLAYERSTATS::Autorating &ar)
{
	NETauto(ar.valid);
	if (ar.valid)
	{
		NETauto(ar.dummy);
		NETauto(ar.star);
		NETauto(ar.medal);
		NETauto(ar.level);
		NETauto(ar.elo);
	}
}

// ////////////////////////////////////////////////////////////////////////////
// Set Player's stats
// send stats to all players when bLocal is false
bool setMultiStats(uint32_t playerIndex, PLAYERSTATS plStats, bool bLocal)
{
	if (playerIndex >= MAX_PLAYERS)
	{
		return true;
	}

	// First copy over the data into our local array
	playerStats[playerIndex] = std::move(plStats);

	if (!bLocal)
	{
		// Now send it to all other players
		NETbeginEncode(NETbroadcastQueue(), NET_PLAYER_STATS);
		// Send the ID of the player's stats we're updating
		NETuint32_t(&playerIndex);

		playerStats[playerIndex].autorating.valid = false;

		/*playerStats[playerIndex].autorating.valid = true;
		playerStats[playerIndex].autorating.dummy = false;
		playerStats[playerIndex].autorating.star[0] = 1;
		playerStats[playerIndex].autorating.star[1] = 2;
		playerStats[playerIndex].autorating.star[2] = 3;
		playerStats[playerIndex].autorating.medal = 3;
		playerStats[playerIndex].autorating.level = 3;
		playerStats[playerIndex].autorating.elo = "Elo: 1433 — W/L: 30/2";*/

		NETauto(playerStats[playerIndex].autorating);

		// Send over the actual stats
		NETuint32_t(&playerStats[playerIndex].played);
		NETuint32_t(&playerStats[playerIndex].wins);
		NETuint32_t(&playerStats[playerIndex].losses);
		NETuint32_t(&playerStats[playerIndex].totalKills);
		NETuint32_t(&playerStats[playerIndex].totalScore);
		NETuint32_t(&playerStats[playerIndex].recentKills);
		NETuint32_t(&playerStats[playerIndex].recentScore);

		EcKey::Key identity;
		if (!playerStats[playerIndex].identity.empty())
		{
			identity = playerStats[playerIndex].identity.toBytes(EcKey::Public);
		}
		NETbytes(&identity);
		NETend();
	}

	return true;
}

void recvMultiStats(NETQUEUE queue)
{
	uint32_t playerIndex;

	NETbeginDecode(queue, NET_PLAYER_STATS);
	// Retrieve the ID number of the player for which we need to
	// update the stats
	NETuint32_t(&playerIndex);

	if (playerIndex >= MAX_PLAYERS)
	{
		NETend();
		return;
	}


	if (playerIndex != queue.index && queue.index != NET_HOST_ONLY)
	{
		HandleBadParam("NET_PLAYER_STATS given incorrect params.", playerIndex, queue.index);
		NETend();
		return;
	}

	NETauto(playerStats[playerIndex].autorating);

	// we don't what to update ourselves, we already know our score (FIXME: rewrite setMultiStats())
	if (!myResponsibility(playerIndex))
	{
		// Retrieve the actual stats
		NETuint32_t(&playerStats[playerIndex].played);
		NETuint32_t(&playerStats[playerIndex].wins);
		NETuint32_t(&playerStats[playerIndex].losses);
		NETuint32_t(&playerStats[playerIndex].totalKills);
		NETuint32_t(&playerStats[playerIndex].totalScore);
		NETuint32_t(&playerStats[playerIndex].recentKills);
		NETuint32_t(&playerStats[playerIndex].recentScore);

		EcKey::Key identity;
		NETbytes(&identity);
		EcKey::Key prevIdentity;
		if (!playerStats[playerIndex].identity.empty())
		{
			prevIdentity = playerStats[playerIndex].identity.toBytes(EcKey::Public);
		}
		playerStats[playerIndex].identity.clear();
		if (!identity.empty())
		{
			playerStats[playerIndex].identity.fromBytes(identity, EcKey::Public);
		}
		if (identity != prevIdentity)
		{
			ingame.PingTimes[playerIndex] = PING_LIMIT;
		}
	}
	NETend();
}

// ////////////////////////////////////////////////////////////////////////////
// Load Player Stats
bool loadMultiStats(char *sPlayerName, PLAYERSTATS *st)
{
	char				fileName[255];
	UDWORD				size;
	char				*pFileData;

	*st = PLAYERSTATS();  // clear in case we don't get to load

	// Prevent an empty player name (where the first byte is a 0x0 terminating char already)
	if (!*sPlayerName)
	{
		strcpy(sPlayerName, _("Player"));
	}

	snprintf(fileName, sizeof(fileName), "%s%s.sta", MultiPlayersPath, sPlayerName);

	debug(LOG_WZ, "loadMultiStats: %s", fileName);

	// check player already exists
	if (PHYSFS_exists(fileName))
	{
		if (loadFile(fileName, &pFileData, &size))
		{
			if (strncmp(pFileData, "WZ.STA.v3", 9) != 0)
			{
				free(pFileData);
				pFileData = nullptr;
				return false; // wrong version or not a stats file
			}

			char identity[1001];
			identity[0] = '\0';
			sscanf(pFileData, "WZ.STA.v3\n%u %u %u %u %u\n%1000[A-Za-z0-9+/=]",
				   &st->wins, &st->losses, &st->totalKills, &st->totalScore, &st->played, identity);
			free(pFileData);
			if (identity[0] != '\0')
			{
				st->identity.fromBytes(base64Decode(identity), EcKey::Private);
			}
		}
	}

	if (st->identity.empty())
	{
		st->identity = EcKey::generate();  // Generate new identity.
		saveMultiStats(sPlayerName, sPlayerName, st);  // Save new identity.
	}

	// reset recent scores
	st->recentKills = 0;
	st->recentScore = 0;

	// clear any skirmish stats.
	for (size = 0; size < MAX_PLAYERS; size++)
	{
		ingame.skScores[size][0] = 0;
		ingame.skScores[size][1] = 0;
	}

	return true;
}

// ////////////////////////////////////////////////////////////////////////////
// Save Player Stats
bool saveMultiStats(const char *sFileName, const char *sPlayerName, const PLAYERSTATS *st)
{
	if (Cheated)
	{
	    return false;
	}
	char buffer[1000];
	char fileName[255] = "";

	ssprintf(buffer, "WZ.STA.v3\n%u %u %u %u %u\n%s\n",
	         st->wins, st->losses, st->totalKills, st->totalScore, st->played, base64Encode(st->identity.toBytes(EcKey::Private)).c_str());

	snprintf(fileName, sizeof(fileName), "%s%s.sta", MultiPlayersPath, sFileName);

	saveFile(fileName, buffer, strlen(buffer));

	return true;
}

// ////////////////////////////////////////////////////////////////////////////
// score update functions

// update players damage stats.
void updateMultiStatsDamage(UDWORD attacker, UDWORD defender, UDWORD inflicted)
{
	// damaging features like skyscrapers does not count
	if (defender != PLAYER_FEATURE)
	{
		if (NetPlay.bComms)
		{
			// killing and getting killed by scavengers does not influence scores in MP games
			if (attacker != scavengerSlot() && defender != scavengerSlot())
			{
				// FIXME: Why in the world are we using two different structs for stats when we can use only one?
				playerStats[attacker].totalScore  += 2 * inflicted;
				playerStats[attacker].recentScore += 2 * inflicted;
				playerStats[defender].totalScore  -= inflicted;
				playerStats[defender].recentScore -= inflicted;
			}
		}
		else
		{
			ingame.skScores[attacker][0] += 2 * inflicted;  // increment skirmish players rough score.
			ingame.skScores[defender][0] -= inflicted;  // increment skirmish players rough score.
		}
	}
}

// update games played.
void updateMultiStatsGames()
{
	if (selectedPlayer >= MAX_PLAYERS)
	{
		return;
	}
	++playerStats[selectedPlayer].played;
}

// games won
void updateMultiStatsWins()
{
	if (selectedPlayer >= MAX_PLAYERS)
	{
		return;
	}
	++playerStats[selectedPlayer].wins;
}

//games lost.
void updateMultiStatsLoses()
{
	if (selectedPlayer >= MAX_PLAYERS)
	{
		return;
	}
	++playerStats[selectedPlayer].losses;
}

// update kills
void updateMultiStatsKills(BASE_OBJECT *psKilled, UDWORD player)
{
	if (player < MAX_PLAYERS)
	{
		if (NetPlay.bComms)
		{
			// killing scavengers does not count in MP games
			if (psKilled != nullptr && psKilled->player != scavengerSlot())
			{
				// FIXME: Why in the world are we using two different structs for stats when we can use only one?
				++playerStats[player].totalKills;
				++playerStats[player].recentKills;
			}
		}
		else
		{
			ingame.skScores[player][1]++;
		}
	}
}

static std::map<std::string, EcKey::Key> knownPlayers;
static QSettings *knownPlayersIni = nullptr;

std::map<std::string, EcKey::Key> const &getKnownPlayers()
{
	if (knownPlayersIni == nullptr)
	{
		knownPlayersIni = new QSettings(PHYSFS_getWriteDir() + QString("/") + "knownPlayers.ini", QSettings::IniFormat);
		QStringList names = knownPlayersIni->allKeys();
		for (int i = 0; i < names.size(); ++i)
		{
			knownPlayers[names[i].toUtf8().constData()] = base64Decode(knownPlayersIni->value(names[i]).toString().toStdString());
		}
	}
	return knownPlayers;
}

void addKnownPlayer(std::string const &name, EcKey const &key, bool override)
{
	if (key.empty())
	{
		return;
	}

	if (!override && knownPlayers.find(name) != knownPlayers.end())
	{
		return;
	}

	getKnownPlayers();  // Init knownPlayersIni.
	knownPlayers[name] = key.toBytes(EcKey::Public);
	knownPlayersIni->setValue(QString::fromUtf8(name.c_str()), base64Encode(key.toBytes(EcKey::Public)).c_str());
	knownPlayersIni->sync();
}

uint32_t getSelectedPlayerUnitsKilled()
{
	if (ActivityManager::instance().getCurrentGameMode() != ActivitySink::GameMode::CAMPAIGN)
	{
		// Let's use the real score for MP games
		// FIXME: Why in the world are we using two different structs for stats when we can use only one?
		if (NetPlay.bComms)
		{
			return getMultiStats(selectedPlayer).recentKills;
		}
		else
		{
			// estimated kills
			return static_cast<uint32_t>(ingame.skScores[selectedPlayer][1]);
		}
	}
	else
	{
		return missionData.unitsKilled;
	}
}

