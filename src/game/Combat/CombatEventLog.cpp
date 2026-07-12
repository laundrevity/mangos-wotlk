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

#include "Combat/CombatEventLog.h"

#include "Config/Config.h"
#include "Entities/Player.h"
#include "Entities/Unit.h"
#include "Globals/ObjectAccessor.h"
#include "Groups/Group.h"
#include "Log/Log.h"
#include "Server/DBCStores.h"
#include "Server/WorldSession.h"
#include "Spells/SpellAuras.h"
#include "Util/Timer.h"

#include <chrono>

namespace
{
    constexpr size_t BUFFER_SOFT_CAP = 64 * 1024;   // flush when the buffer grows past this
    constexpr uint32 FLUSH_INTERVAL_MS = 2000;      // ... or at latest this often
    constexpr uint32 SNAPSHOT_INTERVAL_MS = 1000;   // real-player power snapshot cadence

    uint64 WallClockMs()
    {
        using namespace std::chrono;
        return uint64(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
    }
}

CombatEventLog* CombatEventLog::instance()
{
    static CombatEventLog log;
    return &log;
}

CombatEventLog::~CombatEventLog()
{
    Shutdown();
}

void CombatEventLog::Initialize()
{
    m_enabled = sConfig.GetBoolDefault("CombatEventLog.Enable", false);
    m_scope = sConfig.GetIntDefault("CombatEventLog.Scope", 0);
    if (!m_enabled)
        return;

    std::string base = sConfig.GetStringDefault("CombatEventLog.File", "combat_events.jsonl");
    // relative names live in LogsDir, same as the other server logs
    if (!base.empty() && base[0] != '/')
    {
        std::string logsDir = sConfig.GetStringDefault("LogsDir", "");
        if (!logsDir.empty() && logsDir[logsDir.length() - 1] != '/' && logsDir[logsDir.length() - 1] != '\\')
            logsDir.push_back('/');
        base = logsDir + base;
    }

    // one file per boot: insert the boot timestamp before the extension
    std::string stamp = Log::GetTimestampStr();
    size_t slash = base.find_last_of("/\\");
    size_t dot = base.find_last_of('.');
    std::string path;
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        path = base.substr(0, dot) + "_" + stamp + base.substr(dot);
    else
        path = base + "_" + stamp;

    m_file = fopen(path.c_str(), "w");
    if (!m_file)
    {
        sLog.outError("CombatEventLog: cannot open '%s', disabling", path.c_str());
        m_enabled = false;
        return;
    }

    m_buffer.reserve(BUFFER_SOFT_CAP + 4096);
    sLog.outString("CombatEventLog: enabled (scope %u), writing %s", m_scope, path.c_str());
}

void CombatEventLog::Shutdown()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_enabled = false;
    if (!m_file)
        return;
    FlushLocked();
    fclose(m_file);
    m_file = nullptr;
}

bool CombatEventLog::Update(uint32 diff)
{
    if (!m_enabled)
        return false;

    m_flushAccum += diff;
    if (m_flushAccum >= FLUSH_INTERVAL_MS)
    {
        m_flushAccum = 0;
        std::lock_guard<std::mutex> lock(m_mutex);
        FlushLocked();
    }

    m_snapshotAccum += diff;
    if (m_snapshotAccum >= SNAPSHOT_INTERVAL_MS)
    {
        m_snapshotAccum = 0;
        return true;
    }
    return false;
}

// A real player is a session with a client socket behind it. Bot sessions never
// have one (WorldSession::GetRemoteAddress says "disconnected/bot"), and unlike
// GetPlayerbotAI() this already holds while a bot is still logging in — the AI
// attaches late, which otherwise leaks a login window of bot events into scope 0
// and mislabels them "player".
bool CombatEventLog::IsRealPlayer(Player* player)
{
    if (!player)
        return false;
    WorldSession* session = player->GetSession();
    return session && session->HasSocket();
}

// A unit matters for scope 0 if the player behind it (through pet/totem/charm
// chains) is a real player or shares a group with one.
bool CombatEventLog::IsRelevantUnit(Unit* unit)
{
    if (!unit || !unit->IsInWorld())
        return false;

    Player* player = unit->GetBeneficiaryPlayer();
    if (!player)
        return false;

    if (IsRealPlayer(player))
        return true;

    if (Group* group = player->GetGroup())
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            if (IsRealPlayer(itr->getSource()))
                return true;

    return false;
}

bool CombatEventLog::ShouldLog(Unit* a, Unit* b)
{
    if (!m_enabled)
        return false;

    switch (m_scope)
    {
        case 2:
            return true;
        case 1:
        {
            WorldObject const* ref = a ? static_cast<WorldObject const*>(a) : static_cast<WorldObject const*>(b);
            if (!ref)
                return false;
            MapEntry const* entry = sMapStore.LookupEntry(ref->GetMapId());
            return entry && entry->Instanceable();
        }
        default:
            if (IsRelevantUnit(a) || IsRelevantUnit(b))
                return true;
            // NPC-only events (e.g. a mob's self-cast mid-fight) stay visible
            // through whoever the mob is currently fighting
            if (a && a->IsInWorld() && IsRelevantUnit(a->GetVictim()))
                return true;
            if (b && b->IsInWorld() && IsRelevantUnit(b->GetVictim()))
                return true;
            return false;
    }
}

Unit* CombatEventLog::ResolveTarget(Unit* caster, ObjectGuid targetGuid) const
{
    if (!caster || !targetGuid || !caster->IsInWorld())
        return nullptr;
    if (caster->GetObjectGuid() == targetGuid)
        return caster;
    return ObjectAccessor::GetUnit(*caster, targetGuid);
}

void CombatEventLog::AppendEscaped(std::string& out, char const* text)
{
    if (!text)
        return;
    for (char const* c = text; *c; ++c)
    {
        unsigned char ch = static_cast<unsigned char>(*c);
        if (ch == '"' || ch == '\\')
        {
            out += '\\';
            out += char(ch);
        }
        else if (ch < 0x20)
        {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", ch);
            out += buf;
        }
        else
            out += char(ch);
    }
}

void CombatEventLog::AppendUnit(std::string& out, char const* key, Unit* unit)
{
    out += '"';
    out += key;
    out += "\":";
    if (!unit)
    {
        out += "null";
        return;
    }

    char buf[80];
    snprintf(buf, sizeof(buf), "{\"g\":\"0x%llx\",\"n\":\"", (unsigned long long)unit->GetObjectGuid().GetRawValue());
    out += buf;
    AppendEscaped(out, unit->GetName());

    char const* kind = "npc";
    ObjectGuid master;
    if (unit->GetTypeId() == TYPEID_PLAYER)
        kind = IsRealPlayer(static_cast<Player*>(unit)) ? "player" : "bot";
    else
    {
        master = unit->GetMasterGuid();
        // only player-controlled summons are "pet": boss totems etc. also
        // carry a master guid but must stay plain npcs for the harness
        if (master && unit->IsInWorld() && unit->GetBeneficiaryPlayer())
            kind = "pet";
        else
            master = ObjectGuid();
    }

    snprintf(buf, sizeof(buf), "\",\"k\":\"%s\",\"c\":%u", kind, uint32(unit->getClass()));
    out += buf;
    if (master)
    {
        snprintf(buf, sizeof(buf), ",\"o\":\"0x%llx\"", (unsigned long long)master.GetRawValue());
        out += buf;
    }
    out += '}';
}

void CombatEventLog::AppendSpell(std::string& out, SpellEntry const* spellInfo)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "\"sp\":%u,\"spn\":\"", spellInfo ? spellInfo->Id : 0);
    out += buf;
    if (spellInfo)
        AppendEscaped(out, spellInfo->SpellName[0]);
    out += '"';
}

void CombatEventLog::BeginEvent(std::string& out, char const* ev, WorldObject const* ref)
{
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"t\":%llu,\"ms\":%u,\"ev\":\"%s\",\"map\":%u,\"inst\":%u,",
             (unsigned long long)WallClockMs(), WorldTimer::getMSTime(), ev,
             ref ? ref->GetMapId() : 0, ref ? ref->GetInstanceId() : 0);
    out += buf;
}

void CombatEventLog::Commit(std::string& line)
{
    line += '\n';
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_file)
        return;
    m_buffer += line;
    if (m_buffer.size() >= BUFFER_SOFT_CAP)
        FlushLocked();
}

void CombatEventLog::FlushLocked()
{
    if (!m_file || m_buffer.empty())
        return;
    fwrite(m_buffer.data(), 1, m_buffer.size(), m_file);
    fflush(m_file);   // deliberately no fsync: buffered is the whole point
    m_buffer.clear();
}

void CombatEventLog::LogDamage(Unit* dealer, Unit* victim, uint32 damage, uint32 schoolMask, SpellEntry const* spellInfo, uint32 damagetype, bool crit)
{
    if (!ShouldLog(dealer, victim))
        return;

    std::string line;
    line.reserve(384);
    BeginEvent(line, "dmg", victim);
    AppendUnit(line, "src", dealer);
    line += ',';
    AppendUnit(line, "tgt", victim);
    line += ',';

    char buf[96];
    snprintf(buf, sizeof(buf), "\"amt\":%u,\"sch\":%u,", damage, schoolMask);
    line += buf;
    AppendSpell(line, spellInfo);
    snprintf(buf, sizeof(buf), ",\"crit\":%u,\"per\":%u}", crit ? 1 : 0, damagetype == DOT ? 1 : 0);
    line += buf;
    Commit(line);
}

void CombatEventLog::LogHeal(Unit* healer, Unit* victim, uint32 raw, int32 effective, SpellEntry const* spellInfo, bool crit)
{
    if (!ShouldLog(healer, victim))
        return;

    std::string line;
    line.reserve(384);
    BeginEvent(line, "heal", victim);
    AppendUnit(line, "src", healer);
    line += ',';
    AppendUnit(line, "tgt", victim);
    line += ',';

    char buf[96];
    snprintf(buf, sizeof(buf), "\"raw\":%u,\"eff\":%d,", raw, effective > 0 ? effective : 0);
    line += buf;
    AppendSpell(line, spellInfo);
    snprintf(buf, sizeof(buf), ",\"crit\":%u}", crit ? 1 : 0);
    line += buf;
    Commit(line);
}

void CombatEventLog::LogCastStart(Unit* caster, Unit* target, SpellEntry const* spellInfo, int32 castTime)
{
    if (!ShouldLog(caster, target))
        return;

    std::string line;
    line.reserve(384);
    BeginEvent(line, "cast_start", caster);
    AppendUnit(line, "src", caster);
    line += ',';
    AppendUnit(line, "tgt", target);
    line += ',';
    AppendSpell(line, spellInfo);

    char buf[48];
    snprintf(buf, sizeof(buf), ",\"ct\":%d}", castTime);
    line += buf;
    Commit(line);
}

void CombatEventLog::LogCastSuccess(Unit* caster, ObjectGuid targetGuid, SpellEntry const* spellInfo)
{
    Unit* target = ResolveTarget(caster, targetGuid);
    if (!ShouldLog(caster, target))
        return;

    std::string line;
    line.reserve(384);
    BeginEvent(line, "cast_ok", caster);
    AppendUnit(line, "src", caster);
    line += ',';
    AppendUnit(line, "tgt", target);
    line += ',';
    AppendSpell(line, spellInfo);
    line += '}';
    Commit(line);
}

void CombatEventLog::LogCastCancel(Unit* caster, ObjectGuid targetGuid, SpellEntry const* spellInfo, uint32 stateAtCancel)
{
    Unit* target = ResolveTarget(caster, targetGuid);
    if (!ShouldLog(caster, target))
        return;

    std::string line;
    line.reserve(384);
    BeginEvent(line, "cast_cancel", caster);
    AppendUnit(line, "src", caster);
    line += ',';
    AppendUnit(line, "tgt", target);
    line += ',';
    AppendSpell(line, spellInfo);

    char buf[48];
    snprintf(buf, sizeof(buf), ",\"st\":%u}", stateAtCancel);
    line += buf;
    Commit(line);
}

void CombatEventLog::LogAura(SpellAuraHolder* holder, bool apply, uint32 removeMode)
{
    if (!m_enabled || !holder || holder->IsPassive())
        return;

    Unit* target = holder->GetTarget();
    if (!target || !target->IsInWorld())
        return;

    Unit* caster = holder->GetCaster();   // may already be gone
    if (!ShouldLog(caster, target))
        return;

    std::string line;
    line.reserve(384);
    BeginEvent(line, apply ? "aura_on" : "aura_off", target);
    AppendUnit(line, "src", caster);
    line += ',';
    AppendUnit(line, "tgt", target);
    line += ',';
    AppendSpell(line, holder->GetSpellProto());
    if (!apply)
    {
        char buf[32];
        snprintf(buf, sizeof(buf), ",\"mode\":%u", removeMode);
        line += buf;
    }
    line += '}';
    Commit(line);
}

void CombatEventLog::LogDeath(Unit* killer, Unit* victim)
{
    if (!ShouldLog(killer, victim))
        return;

    std::string line;
    line.reserve(384);
    BeginEvent(line, "death", victim);
    AppendUnit(line, "src", killer);
    line += ',';
    AppendUnit(line, "tgt", victim);
    line += '}';
    Commit(line);
}

void CombatEventLog::LogCombatState(Unit* unit, bool enter, Unit* enemy)
{
    if (!ShouldLog(unit, enemy))
        return;

    std::string line;
    line.reserve(384);
    BeginEvent(line, enter ? "combat_on" : "combat_off", unit);
    AppendUnit(line, "src", unit);
    line += ',';
    AppendUnit(line, "tgt", enemy);
    line += '}';
    Commit(line);
}

void CombatEventLog::LogDirective(Player* bot, char const* id, char const* brainSource, bool accepted, char const* reason)
{
    if (!m_enabled || !bot)
        return;

    // directives only exist for a real player's party, same relevance rule
    if (m_scope == 0 && !IsRelevantUnit(bot))
        return;

    std::string line;
    line.reserve(384);
    BeginEvent(line, "directive", bot);
    AppendUnit(line, "src", nullptr);
    line += ',';
    AppendUnit(line, "tgt", bot);
    line += ",\"did\":\"";
    AppendEscaped(line, id);
    line += "\",\"bsrc\":\"";
    AppendEscaped(line, brainSource);
    char buf[32];
    snprintf(buf, sizeof(buf), "\",\"ok\":%u,\"why\":\"", accepted ? 1 : 0);
    line += buf;
    AppendEscaped(line, reason);
    line += "\"}";
    Commit(line);
}

void CombatEventLog::LogPower(Player* player)
{
    // power snapshots are only for real players, and bypass the scope filter
    if (!m_enabled || !player || !player->IsInWorld() || !IsRealPlayer(player))
        return;

    std::string line;
    line.reserve(256);
    BeginEvent(line, "power", player);
    AppendUnit(line, "src", player);

    Powers pt = player->GetPowerType();
    char buf[128];
    snprintf(buf, sizeof(buf), ",\"hp\":%u,\"hpm\":%u,\"pt\":%u,\"pw\":%u,\"pwm\":%u,\"cb\":%u}",
             player->GetHealth(), player->GetMaxHealth(), uint32(pt),
             player->GetPower(pt), player->GetMaxPower(pt), player->IsInCombat() ? 1 : 0);
    line += buf;
    Commit(line);
}
