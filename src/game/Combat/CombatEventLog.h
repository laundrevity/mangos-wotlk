/*
 * This file is part of the CMaNGOS Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef MANGOS_COMBATEVENTLOG_H
#define MANGOS_COMBATEVENTLOG_H

#include "Common.h"
#include "Entities/ObjectGuid.h"

#include <cstdio>
#include <mutex>
#include <string>

class Player;
class SpellAuraHolder;
class Unit;
class WorldObject;
struct SpellEntry;

/**
 * CombatEventLog — config-gated JSONL combat event stream for offline analysis.
 *
 * One JSON object per line, one file per server boot (timestamped name),
 * buffered writes (flush on size or interval, never per event). Scope-filtered
 * so ambient random bots do not turn the log into a firehose: by default only
 * events touching a real (non-bot) player, their group (playerbots included),
 * their pets, or units currently engaged with that group are kept.
 *
 * Thread safety: emit hooks run on map-update worker threads
 * (MapUpdate.Threads); buffer access is mutex-guarded. Config is read once at
 * world init, before the map threads start.
 */
class CombatEventLog
{
    public:
        static CombatEventLog* instance();

        void Initialize();                  // read config and open the per-boot file; call once at world init
        void Shutdown();                    // final flush + close
        bool Update(uint32 diff);           // interval flush; returns true on the 1 Hz power-snapshot tick

        bool IsEnabled() const { return m_enabled; }

        void LogDamage(Unit* dealer, Unit* victim, uint32 damage, uint32 schoolMask, SpellEntry const* spellInfo, uint32 damagetype, bool crit);
        void LogHeal(Unit* healer, Unit* victim, uint32 raw, int32 effective, SpellEntry const* spellInfo, bool crit);
        void LogCastStart(Unit* caster, Unit* target, SpellEntry const* spellInfo, int32 castTime);
        void LogCastSuccess(Unit* caster, ObjectGuid targetGuid, SpellEntry const* spellInfo);
        void LogCastCancel(Unit* caster, ObjectGuid targetGuid, SpellEntry const* spellInfo, uint32 stateAtCancel);
        void LogAura(SpellAuraHolder* holder, bool apply, uint32 removeMode);
        void LogDeath(Unit* killer, Unit* victim);
        void LogCombatState(Unit* unit, bool enter, Unit* enemy);
        void LogPower(Player* player);
        // bot-brains directive seam instrumentation: one line per directive
        // accept/reject so the harness can join brain output against behavior
        void LogDirective(Player* bot, char const* id, char const* brainSource, bool accepted, char const* reason);

    private:
        CombatEventLog() = default;
        ~CombatEventLog();

        static bool IsRealPlayer(Player* player);
        bool IsRelevantUnit(Unit* unit);
        bool ShouldLog(Unit* a, Unit* b);
        Unit* ResolveTarget(Unit* caster, ObjectGuid targetGuid) const;

        static void AppendEscaped(std::string& out, char const* text);
        static void AppendUnit(std::string& out, char const* key, Unit* unit);
        static void AppendSpell(std::string& out, SpellEntry const* spellInfo);
        void BeginEvent(std::string& out, char const* ev, WorldObject const* ref);
        void Commit(std::string& line);
        void FlushLocked();

        std::mutex m_mutex;
        std::string m_buffer;
        FILE* m_file = nullptr;
        bool m_enabled = false;
        uint32 m_scope = 0;
        uint32 m_flushAccum = 0;
        uint32 m_snapshotAccum = 0;
};

#define sCombatEventLog (*CombatEventLog::instance())

#endif
