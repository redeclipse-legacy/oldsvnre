#include "game.h"
namespace ai
{
    avoidset obstacles, wpavoid;
    int updatemillis = 0, iteration = 0, itermillis = 0;
    vec aitarget(0, 0, 0);
    const char *stnames[AI_S_MAX] = {
        "wait", "defend", "pursue", "interest"
    }, *sttypes[AI_T_MAX+1] = {
        "none", "node", "actor", "affinity", "entity", "drop"
    };


    VAR(0, aidebug, 0, 0, 6);
    VAR(0, aidebugfocus, 0, 1, 2);
    VAR(0, aiforcegun, -1, -1, WEAP_MAX-1);
    VAR(0, aicampaign, 0, 0, 1);
    VAR(0, aipassive, 0, 0, 1);

    VAR(0, showwaypoints, 0, 0, 1);
    VAR(0, showwaypointsradius, 0, 256, VAR_MAX);

    VAR(IDF_PERSIST, aideadfade, 0, 10000, VAR_MAX);
    VAR(IDF_PERSIST, showaiinfo, 0, 0, 2); // 0/1 = shows/hides bot join/parts, 2 = show more verbose info

    bool dbgfocus(gameent *d)  { return d->ai && (!aidebugfocus || d == game::focus || (aidebugfocus != 2 && !game::focus->ai)); }

    float viewdist(int x) { return x <= 100 ? clamp((SIGHTMIN+(SIGHTMAX-SIGHTMIN))/100.f*float(x), float(SIGHTMIN), max(float(fog), SIGHTMIN)) : max(float(fog), SIGHTMIN); }
    float viewfieldx(int x) { return x <= 100 ? clamp((VIEWMIN+(VIEWMAX-VIEWMIN))/100.f*float(x), float(VIEWMIN), float(VIEWMAX)) : float(VIEWMAX); }
    float viewfieldy(int x) { return viewfieldx(x)*3.f/4.f; }

    int owner(gameent *d)
    {
        if(d->aitype >= AI_START)
        {
            if(entities::ents.inrange(d->aientity))
            {
                if(m_capture(game::gamemode)) return capture::aiowner(d);
                else if(m_defend(game::gamemode)) return defend::aiowner(d);
                else if(m_bomber(game::gamemode)) return bomber::aiowner(d);
            }
        }
        return d->team;
    }

    float weapmindist(int weap, bool alt)
    {
        return WEAPEX(weap, alt, game::gamemode, game::mutators, 1.f) && WEAP2(weap, collide, alt)&COLLIDE_OWNER ? WEAPEX(weap, alt, game::gamemode, game::mutators, 1.f) : 2;
    }

    float weapmaxdist(int weap, bool alt)
    {
        return WEAP2(weap, aidist, alt) ? WEAP2(weap, aidist, alt) : hdr.worldsize;
    }

    bool weaprange(gameent *d, int weap, bool alt, float dist)
    {
        if(!isweap(weap) || (WEAP2(weap, extinguish, alt) && d->inliquid)) return false;
        float mindist = weapmindist(weap, alt), maxdist = weapmaxdist(weap, alt);
        return dist >= mindist*mindist && dist <= maxdist*maxdist;
    }

    bool targetable(gameent *d, gameent *e, bool solid)
    {
        if(e && d != e && !m_edit(game::gamemode) && e->state == CS_ALIVE && (!solid || physics::issolid(e, d)))
        {
            int dt = owner(d), et = owner(e);
            if(dt == TEAM_ENEMY && et == TEAM_ENEMY) return false;
            if(!m_team(game::gamemode, game::mutators) || dt != et) return true;
        }
        return false;
    }

    bool cansee(gameent *d, vec &x, vec &y, bool force, vec &targ)
    {
        if(force) return raycubelos(x, y, targ);
        return getsight(x, d->yaw, d->pitch, y, targ, d->ai->views[2], d->ai->views[0], d->ai->views[1]);
    }

    bool badhealth(gameent *d)
    {
        if(d->skill <= 100) return d->health <= (111-d->skill)/4;
        return false;
    }

    bool canshoot(gameent *d, gameent *e, bool alt = true)
    {
        if(isweap(d->weapselect) && weaprange(d, d->weapselect, alt, e->o.squaredist(d->o)))
        {
            int prot = m_protect(game::gamemode, game::mutators);
            if((d->aitype >= AI_START || !d->protect(lastmillis, prot)) && targetable(d, e, true))
                return d->canshoot(d->weapselect, alt ? HIT_ALT : 0, m_weapon(game::gamemode, game::mutators), lastmillis, (1<<WEAP_S_RELOAD));
        }
        return false;
    }

    bool altfire(gameent *d, gameent *e)
    {
        if(e && !WEAP(d->weapselect, zooms) && canshoot(d, e, true))
        {
            if(d->weapstate[d->weapselect] == WEAP_S_POWER)
            {
                if(d->action[AC_ALTERNATE] && (!d->action[AC_ATTACK] || d->actiontime[AC_ALTERNATE] > d->actiontime[AC_ATTACK]))
                    return true;
            }
            switch(d->weapselect)
            {
                case WEAP_PISTOL: return true; break;
                case WEAP_MELEE: case WEAP_ROCKET: default: return false; break;
                case WEAP_SWORD: case WEAP_SHOTGUN: case WEAP_SMG: case WEAP_PLASMA: case WEAP_GRENADE: if(rnd(d->skill*3) <= d->skill) return false; break;
                case WEAP_RIFLE: if(weaprange(d, d->weapselect, false, e->o.squaredist(d->o))) return false; break;
            }
            return true;
        }
        return false;
    }

    bool hastarget(gameent *d, aistate &b, gameent *e, bool alt, float yaw, float pitch, float dist)
    { // add margins of error
        if(weaprange(d, d->weapselect, alt, dist) || (d->skill <= 100 && !rnd(d->skill)))
        {
            if(weaptype[d->weapselect].melee) return true;
            float skew = clamp(float(lastmillis-d->ai->enemymillis)/float((d->skill*WEAP(d->weapselect, rdelay)/5000.f)+(d->skill*WEAP2(d->weapselect, adelay, alt)/500.f)), 0.f, d->weapselect >= WEAP_ITEM ? 0.25f : 1e16f),
                offy = yaw-d->yaw, offp = pitch-d->pitch;
            if(offy > 180) offy -= 360;
            else if(offy < -180) offy += 360;
            if(fabs(offy) <= d->ai->views[0]*skew && fabs(offp) <= d->ai->views[1]*skew) return true;
        }
        return false;
    }

    vec getaimpos(gameent *d, gameent *e, bool alt)
    {
        vec o = e->headpos();
        if(WEAP2(d->weapselect, radial, alt)) o.z -= e->height;
        if(d->skill <= 100)
        {
            if(lastmillis >= d->ai->lastaimrnd)
            {
                #define rndaioffset(r) ((rnd(int(r*WEAP2(d->weapselect, aiskew, alt)*2)+1)-(r*WEAP2(d->weapselect, aiskew, alt)))*(1.f/float(max(d->skill, 1))))
                loopk(3) d->ai->aimrnd[k] = rndaioffset(e->radius);
                int dur = (d->skill+10)*10;
                d->ai->lastaimrnd = lastmillis+dur+rnd(dur);
            }
            loopk(3) o[k] += d->ai->aimrnd[k];
        }
        return o;
    }

    bool hasweap(gameent *d, int weap)
    {
        if(!isweap(weap)) return false;
        if(w_carry(weap, m_weapon(game::gamemode, game::mutators)))
            return d->hasweap(weap, m_weapon(game::gamemode, game::mutators));
        return d->ammo[weap] >= WEAP(weap, max);
    }

    bool wantsweap(gameent *d, int weap)
    {
        if(!isweap(weap) || hasweap(d, weap)) return false;
        if(d->carry(m_weapon(game::gamemode, game::mutators)) >= maxcarry && (hasweap(d, d->ai->weappref) || weap != d->ai->weappref))
            return false;
        return true;
    }

    void create(gameent *d)
    {
        if(!d->ai && !(d->ai = new aiinfo()))
        {
            fatal("could not create ai");
            return;
        }
    }

    void destroy(gameent *d)
    {
        if(d->ai) DELETEP(d->ai);
    }

    void init(gameent *d, int at, int et, int on, int sk, int bn, char *name, int tm, int cl)
    {
        if(!loadwaypoints()) importwaypoints();

        gameent *o = game::newclient(on);
        bool resetthisguy = false;

        string m;
        if(o) copystring(m, game::colorname(o));
        else formatstring(m)("\fs\faunknown [\fs\fr%d\fS]\fS", on);

        if(!d->name[0])
        {
            if(showaiinfo && game::showplayerinfo)
            {
                if(showaiinfo > 1) conoutft(CON_EVENT, "\fg%s assigned to %s at skill %d", game::colorname(d, name), m, sk);
                else conoutft(CON_EVENT, "\fg%s joined the game", game::colorname(d, name), m, sk);
            }
            game::cameras.deletecontents();
            resetthisguy = true;
        }
        else
        {
            if(d->ownernum != on)
            {
                if(showaiinfo && game::showplayerinfo)
                    conoutft(CON_EVENT, "\fg%s reassigned to %s", game::colorname(d, name), m);
                resetthisguy = true;
            }
            if(d->skill != sk && showaiinfo > 1 && game::showplayerinfo)
                conoutft(CON_EVENT, "\fg%s changed skill to %d", game::colorname(d, name), sk);
        }

        copystring(d->name, name, MAXNAMELEN); // don't use setname so we keep our random colour
        if((d->aitype = at) >= AI_START) d->type = ENT_AI;
        d->aientity = et;
        d->ownernum = on;
        d->skill = sk;
        d->team = tm;
        d->colour = cl;

        if(resetthisguy) projs::remove(d);
        if(d->ownernum >= 0 && game::player1->clientnum == d->ownernum)
        {
            create(d);
            if(d->ai)
            {
                d->ai->views[0] = viewfieldx(d->skill);
                d->ai->views[1] = viewfieldy(d->skill);
                d->ai->views[2] = viewdist(d->skill);
            }
        }
        else if(d->ai) destroy(d);
    }

    void update()
    {
        if(game::intermission) { loopv(game::players) if(game::players[i] && game::players[i]->ai) game::players[i]->stopmoving(true); }
        else // fixed rate logic done out-of-sequence at 1 frame per second for each ai
        {
            if(totalmillis-updatemillis > 1000)
            {
                avoid();
                if(multiplayer(false)) { aiforcegun = -1; aicampaign = aipassive = 0; }
                updatemillis = totalmillis;
            }
            if(!iteration && totalmillis-itermillis > 1000)
            {
                iteration = 1;
                itermillis = totalmillis;
            }
            int c = 0;
            loopv(game::players) if(game::players[i] && game::players[i]->ai)
                think(game::players[i], ++c == iteration ? true : false);
            if(++iteration > c) iteration = 0;
        }
    }

    int checkothers(vector<int> &targets, gameent *d, int state, int targtype, int target, bool teams, int *members)
    { // checks the states of other ai for a match
        targets.setsize(0);
        gameent *e = NULL;
        int numdyns = game::numdynents();
        loopi(numdyns) if((e = (gameent *)game::iterdynents(i)))
        {
            if(targets.find(e->clientnum) >= 0) continue;
            if(teams)
            {
                int dt = owner(d), et = owner(e);
                if(dt != TEAM_ENEMY || et != TEAM_ENEMY)
                {
                    if(m_team(game::gamemode, game::mutators) && dt != et) continue;
                }
            }
            if(members) (*members)++;
            if(e == d || !e->ai || e->state != CS_ALIVE || e->aitype != d->aitype) continue;
            aistate &b = e->ai->getstate();
            if(state >= 0 && b.type != state) continue;
            if(target >= 0 && b.target != target) continue;
            if(targtype >=0 && b.targtype != targtype) continue;
            targets.add(e->clientnum);
        }
        return targets.length();
    }

    bool makeroute(gameent *d, aistate &b, int node, bool changed, int retries)
    {
        if(d->lastnode < 0) return false;
        if(changed && !d->ai->route.empty() && d->ai->route[0] == node) return true;
        if(route(d, d->lastnode, node, d->ai->route, obstacles, retries))
        {
            b.override = false;
            return true;
        }
        // retry fails: 0 = first attempt, 1 = try ignoring obstacles, 2 = try ignoring prevnodes too
        if(retries <= 1) return makeroute(d, b, node, false, retries+1);
        return false;
    }

    bool makeroute(gameent *d, aistate &b, const vec &pos, bool changed, int retries)
    {
        int node = closestwaypoint(pos, CLOSEDIST, true);
        return makeroute(d, b, node, changed, retries);
    }

    bool randomnode(gameent *d, aistate &b, const vec &pos, float guard, float wander)
    {
        static vector<int> candidates;
        candidates.setsize(0);
        findwaypointswithin(pos, guard, wander, candidates);

        while(!candidates.empty())
        {
            int w = rnd(candidates.length()), n = candidates.removeunordered(w);
            if(n != d->lastnode && !d->ai->hasprevnode(n) && makeroute(d, b, n)) return true;
        }
        return false;
    }

    bool randomnode(gameent *d, aistate &b, float guard, float wander)
    {
        return randomnode(d, b, d->feetpos(), guard, wander);
    }

    bool enemy(gameent *d, aistate &b, const vec &pos, float guard, int pursue, bool force)
    {
        gameent *t = NULL, *e = NULL;
        vec dp = d->headpos();
        float mindist = guard*guard, bestdist = 1e16f;
        int numdyns = game::numdynents();
        loopi(numdyns) if((e = (gameent *)game::iterdynents(i)) && e != d && targetable(d, e))
        {
            vec ep = getaimpos(d, e, altfire(d, e));
            float dist = ep.squaredist(dp);
            if(dist < bestdist && (cansee(d, dp, ep, d->aitype >= AI_START) || dist <= mindist))
            {
                t = e;
                bestdist = dist;
            }
        }
        if(t && violence(d, b, t, pursue)) return true;
        return false;
    }

    bool patrol(gameent *d, aistate &b, const vec &pos, float guard, float wander, int walk, bool retry)
    {
        if(aistyle[d->aitype].canmove)
        {
            vec feet = d->feetpos();
            float dist = feet.squaredist(pos), fardist = guard*4;
            if(walk == 2 || b.override || (walk && dist <= guard*guard) || !makeroute(d, b, pos))
            { // run away and back to keep ourselves busy
                if(!b.override && randomnode(d, b, pos, guard, wander))
                {
                    b.override = true;
                    return true;
                }
                else if(d->ai->route.empty())
                {
                    b.override = false;
                    if(!retry) return patrol(d, b, pos, guard, wander, walk, true);
                    return false;
                }
            }
            if(walk && dist >= fardist*fardist) b.idle = -1;
        }
        b.override = false;
        return true;
    }

    bool defense(gameent *d, aistate &b, const vec &pos, float guard, float wander, int walk)
    {
        bool hasenemy = enemy(d, b, pos, wander, weaptype[d->weapselect].melee ? 1 : 0, false);
        if(!aistyle[d->aitype].canmove) { b.idle = hasenemy ? 2 : 1; return true; }
        else if(!walk)
        {
            if(pos.squaredist(d->feetpos()) <= guard*guard)
            {
                b.idle = hasenemy ? 2 : 1;
                return true;
            }
            walk++;
        }
        return patrol(d, b, pos, guard, wander, walk);
    }

    bool violence(gameent *d, aistate &b, gameent *e, int pursue)
    {
        if(e && !aipassive && targetable(d, e))
        {
            if(pursue)
            {
                if((b.targtype != AI_T_AFFINITY || !(pursue%2)) && makeroute(d, b, e->lastnode))
                    d->ai->switchstate(b, AI_S_PURSUE, AI_T_ACTOR, e->clientnum);
                else if(pursue >= 3) return false; // can't pursue
            }
            if(d->ai->enemy != e->clientnum)
            {
                d->ai->enemyseen = d->ai->enemymillis = lastmillis;
                d->ai->enemy = e->clientnum;
            }
            return true;
        }
        return false;
    }

    bool target(gameent *d, aistate &b, int pursue = 0, bool force = false, float mindist = 0.f)
    {
        if(aipassive) return false;
        static vector<gameent *> hastried; hastried.setsize(0);
        vec dp = d->headpos();
        int numdyns = game::numdynents();
        while(true)
        {
            float dist = 1e16f;
            gameent *t = NULL, *e = NULL;
            loopi(numdyns) if((e = (gameent *)game::iterdynents(i)) && e != d && hastried.find(e) < 0 && targetable(d, e))
            {
                vec ep = getaimpos(d, e, altfire(d, e));
                float v = ep.squaredist(dp);
                if(d->dominating.find(e)) v *= 0.5f; // REVENGE
                if((!t || v < dist) && (mindist <= 0 || v <= mindist) && (force || cansee(d, dp, ep, d->aitype >= AI_START)))
                {
                    t = e;
                    dist = v;
                }
            }
            if(t)
            {
                if(violence(d, b, t, pursue)) return true;
                defformatstring(s)("%s", game::colorname(t));
                hastried.add(t);
            }
            else break;
        }
        return false;
    }

    void assist(gameent *d, aistate &b, vector<interest> &interests, bool all = false, bool force = false)
    {
        gameent *e = NULL;
        int numdyns = game::numdynents();
        loopi(numdyns) if((e = (gameent *)game::iterdynents(i)) && e != d && (all || e->aitype == AI_NONE) && owner(d) == owner(e))
        {
            interest &n = interests.add();
            n.state = AI_S_DEFEND;
            n.node = e->lastnode;
            n.target = e->clientnum;
            n.targtype = AI_T_ACTOR;
            n.score = e->o.squaredist(d->o)/(force ? 1e8f : (hasweap(d, d->ai->weappref) ? 1.f : 0.5f));
            n.tolerance = 0.25f;
            n.team = true;
        }
    }

    void items(gameent *d, aistate &b, vector<interest> &interests, bool force = false)
    {
        vec pos = d->feetpos();
        int sweap = m_weapon(game::gamemode, game::mutators);
        loopj(entities::lastusetype[EU_ITEM])
        {
            gameentity &e = *(gameentity *)entities::ents[j];
            if(enttype[e.type].usetype != EU_ITEM) continue;
            switch(e.type)
            {
                case WEAPON:
                {
                    int attr = w_attr(game::gamemode, e.attrs[0], sweap);
                    if(e.spawned && isweap(attr) && wantsweap(d, attr))
                    { // go get a weapon upgrade
                        interest &n = interests.add();
                        n.state = AI_S_INTEREST;
                        n.node = closestwaypoint(e.o, CLOSEDIST, true);
                        n.target = j;
                        n.targtype = AI_T_ENTITY;
                        n.score =  pos.squaredist(e.o)/(attr == d->ai->weappref ? 1e8f : (force ? 1e4f : 1.f));
                        n.tolerance = 0;
                    }
                    break;
                }
                default: break;
            }
        }

        loopvj(projs::projs) if(projs::projs[j]->projtype == PRJ_ENT && projs::projs[j]->ready())
        {
            projent &proj = *projs::projs[j];
            if(!entities::ents.inrange(proj.id) || enttype[entities::ents[proj.id]->type].usetype != EU_ITEM) continue;
            gameentity &e = *(gameentity *)entities::ents[proj.id];
            switch(e.type)
            {
                case WEAPON:
                {
                    int attr = w_attr(game::gamemode, e.attrs[0], sweap);
                    if(isweap(attr) && wantsweap(d, attr) && proj.owner != d)
                    { // go get a weapon upgrade
                        interest &n = interests.add();
                        n.state = AI_S_INTEREST;
                        n.node = closestwaypoint(proj.o, CLOSEDIST, true);
                        n.target = proj.id;
                        n.targtype = AI_T_DROP;
                        n.score = pos.squaredist(proj.o)/(attr == d->ai->weappref ? 1e8f : (force ? 1e4f : 1.f));
                        n.tolerance = 0;
                    }
                    break;
                }
                default: break;
            }
        }
    }

    bool find(gameent *d, aistate &b)
    {
        static vector<interest> interests; interests.setsize(0);
        if(d->aitype == AI_BOT)
        {
            if(!aipassive)
            {
                int sweap = m_weapon(game::gamemode, game::mutators);
                if(!hasweap(d, d->ai->weappref) || d->carry(sweap) == 0) items(d, b, interests, d->carry(sweap) == 0);
                if(m_team(game::gamemode, game::mutators)) assist(d, b, interests, false, m_campaign(game::gamemode));
            }
            if(m_fight(game::gamemode))
            {
                if(m_capture(game::gamemode)) capture::aifind(d, b, interests);
                else if(m_defend(game::gamemode)) defend::aifind(d, b, interests);
                else if(m_bomber(game::gamemode)) bomber::aifind(d, b, interests);
            }
            if(m_campaign(game::gamemode) && aicampaign)
            {
                loopi(entities::lastent(TRIGGER)) if(entities::ents[i]->type == TRIGGER && entities::ents[i]->attrs[1] == TR_EXIT)
                {
                    interest &n = interests.add();
                    n.state = AI_S_PURSUE;
                    n.target = i;
                    n.node = closestwaypoint(entities::ents[i]->o, CLOSEDIST, true);
                    n.targtype = AI_T_AFFINITY;
                    n.score = -1;
                    n.tolerance = 1;
                }
            }
        }
        else if(entities::ents.inrange(d->aientity)) loopv(entities::ents[d->aientity]->links)
        {
            interest &n = interests.add();
            n.state = AI_S_DEFEND;
            n.target = n.node = entities::ents[d->aientity]->links[i];
            n.targtype = AI_T_NODE;
            n.score = -1;
            n.tolerance = 1;
        }
        while(!interests.empty())
        {
            int q = interests.length()-1;
            loopi(interests.length()-1) if(interests[i].score < interests[q].score) q = i;
            interest n = interests.removeunordered(q);
            if(d->aitype == AI_BOT && m_fight(game::gamemode) && m_team(game::gamemode, game::mutators))
            {
                int members = 0;
                static vector<int> targets; targets.setsize(0);
                int others = checkothers(targets, d, n.state, n.targtype, n.target, n.team, &members);
                if(others >= int(ceilf(members*n.tolerance))) continue;
            }
            if(!aistyle[d->aitype].canmove || makeroute(d, b, n.node))
            {
                d->ai->switchstate(b, n.state, n.targtype, n.target);
                return true;
            }
        }
        return false;
    }

    void damaged(gameent *d, gameent *e, int weap, int flags, int damage)
    {
        if(d != e)
        {
            if(d->ai && (d->aitype >= AI_START || hithurts(flags) || !game::getclient(d->ai->enemy))) // see if this ai is interested in a grudge
            {
                d->ai->unsuspend();
                aistate &b = d->ai->getstate();
                violence(d, b, e, d->aitype != AI_BOT || weaptype[d->weapselect].melee ? 1 : 0);
            }
            if(d->aitype >= AI_START && e->aitype < AI_START) // alert the horde
            {
                gameent *t = NULL;
                vec dp = d->headpos();
                float maxdist = ALERTMAX*ALERTMAX;
                int numdyns = game::numdynents();
                loopi(numdyns) if((t = (gameent *)game::iterdynents(i)) && t != d && t->ai && t->state == CS_ALIVE && t->aitype >= AI_START && t->ai->suspended && targetable(t, e))
                {
                    vec tp = t->headpos();
                    if(cansee(t, tp, dp, d->aitype >= AI_START) || tp.squaredist(dp) <= maxdist)
                    {
                        aistate &c = t->ai->getstate();
                        violence(t, c, e, 1);
                    }
                }
            }
            else
            {
                static vector<int> targets; // check if one of our ai is defending them
                targets.setsize(0);
                if(checkothers(targets, d, AI_S_DEFEND, AI_T_ACTOR, d->clientnum, true))
                {
                    gameent *t;
                    loopv(targets) if((t = game::getclient(targets[i])) && t->ai && t->aitype == AI_BOT && (hithurts(flags) || !game::getclient(t->ai->enemy)) && !t->ai->suspended)
                    {
                        aistate &c = t->ai->getstate();
                        violence(t, c, e, weaptype[d->weapselect].melee ? 1 : 0);
                    }
                }
            }
        }
    }

    void setup(gameent *d, int ent = -1)
    {
        d->aientity = ent;
        if(d->ai)
        {
            d->ai->clearsetup();
            d->ai->reset(true);
            d->ai->lastrun = lastmillis;
            if(d->aitype >= AI_START)
            {
                if(entities::ents.inrange(d->aientity) && entities::ents[d->aientity]->type == ACTOR && entities::ents[d->aientity]->attrs[5] > 0)
                    d->ai->weappref = entities::ents[d->aientity]->attrs[5]-1;
                else d->ai->weappref = aistyle[d->aitype].weap;
                if(!isweap(d->ai->weappref)) d->ai->weappref = rnd(WEAP_MAX-1)+1;
            }
            else
            {
                if(m_limited(game::gamemode, game::mutators)) d->ai->weappref = m_weapon(game::gamemode, game::mutators);
                else if(aiforcegun >= 0 && aiforcegun < WEAP_MAX) d->ai->weappref = aiforcegun;
                else d->ai->weappref = rnd(WEAP_MAX-WEAP_OFFSET)+WEAP_OFFSET;
            }
            d->ai->suspended = true;
            vec dp = d->headpos();
            findorientation(dp, d->yaw, d->pitch, d->ai->target);
        }
    }

    void spawned(gameent *d, int ent)
    {
        if(d->ai) setup(d, ent);
    }

    void killed(gameent *d, gameent *e)
    {
        if(d->ai)
        {
            d->ai->reset();
            if(d->aitype >= AI_START) d->ai->suspended = true;
        }
    }

    void itemspawned(int ent, int spawned)
    {
        if(!aipassive && m_fight(game::gamemode) && entities::ents.inrange(ent) && entities::ents[ent]->type == WEAPON && spawned > 0)
        {
            int sweap = m_weapon(game::gamemode, game::mutators), attr = w_attr(game::gamemode, entities::ents[ent]->attrs[0], sweap);
            loopv(game::players) if(game::players[i] && game::players[i]->ai && game::players[i]->aitype == AI_BOT)
            {
                gameent *d = game::players[i];
                aistate &b = d->ai->getstate();
                if(b.targtype == AI_T_AFFINITY) continue; // don't override any affinity states
                if(!hasweap(d, attr) && (!hasweap(d, d->ai->weappref) || d->carry(sweap) == 0) && wantsweap(d, attr))
                {
                    if(b.type == AI_S_INTEREST && (b.targtype == AI_T_ENTITY || b.targtype == AI_T_DROP))
                    {
                        if(entities::ents.inrange(b.target))
                        {
                            int weap = w_attr(game::gamemode, entities::ents[b.target]->attrs[0], sweap);
                            if((attr == d->ai->weappref && weap != d->ai->weappref) || d->o.squaredist(entities::ents[ent]->o) < d->o.squaredist(entities::ents[b.target]->o))
                                d->ai->switchstate(b, AI_S_INTEREST, AI_T_ENTITY, ent);
                        }
                        continue;
                    }
                    d->ai->switchstate(b, AI_S_INTEREST, AI_T_ENTITY, ent);
                }
            }
        }
    }

    bool check(gameent *d, aistate &b)
    {
        if(d->aitype == AI_BOT)
        {
            if(m_capture(game::gamemode) && capture::aicheck(d, b)) return true;
            else if(m_defend(game::gamemode) && defend::aicheck(d, b)) return true;
            else if(m_bomber(game::gamemode) && bomber::aicheck(d, b)) return true;
        }
        return false;
    }

    int dowait(gameent *d, aistate &b)
    {
        d->ai->clear(true); // ensure they're clean
        if(d->ai->suspended)
        {
            if(!m_campaign(game::gamemode) && d->aitype == AI_BOT) d->ai->unsuspend();
            else // bots idle until a human is around
            {
                gameent *t = NULL;
                int numdyns = game::numdynents();
                loopi(numdyns) if((t = (gameent *)game::iterdynents(i)) && t != d && t->aitype < AI_START)
                {
                    if(d->aitype == AI_BOT ? (t->state != CS_SPECTATOR && t->aitype == AI_NONE) : (t->state == CS_ALIVE && d->o.squaredist(t->o) <= ALERTMAX*ALERTMAX))
                    {
                        d->ai->unsuspend();
                        break;
                    }
                }
            }
        }
        if(check(d, b) || find(d, b)) return 1;
        if(target(d, b, 4, false, d->ai->suspended && d->aitype >= AI_START ? ALERTMAX*ALERTMAX : 0.f)) return 1;
        if(!d->ai->suspended)
        {
            if(target(d, b, 4, true)) return 1;
            if(aistyle[d->aitype].canmove && randomnode(d, b, CLOSEDIST, 1e16f))
            {
                d->ai->switchstate(b, AI_S_INTEREST, AI_T_NODE, d->ai->route[0]);
                return 1;
            }
            d->ai->suspended = true; // fine then..
            d->ai->clean(true);
        }
        return 0; // but don't pop the state
    }

    int dodefense(gameent *d, aistate &b)
    {
        if(!d->ai->suspended && d->state == CS_ALIVE)
        {
            switch(b.targtype)
            {
                case AI_T_NODE:
                {
                    if(check(d, b)) return 1;
                    if(waypoints.inrange(b.target))
                        return defense(d, b, waypoints[b.target].o) ? 1 : 0;
                    break;
                }
                case AI_T_ENTITY:
                {
                    if(check(d, b)) return 1;
                    if(entities::ents.inrange(b.target))
                    {
                        gameentity &e = *(gameentity *)entities::ents[b.target];
                        return defense(d, b, e.o) ? 1 : 0;
                    }
                    break;
                }
                case AI_T_AFFINITY:
                {
                    if(m_campaign(game::gamemode))
                    {
                        if(aicampaign && entities::ents.inrange(b.target)) return defense(d, b, entities::ents[b.target]->o) ? 1 : 0;
                    }
                    else if(m_capture(game::gamemode)) return capture::aidefense(d, b) ? 1 : 0;
                    else if(m_defend(game::gamemode)) return defend::aidefense(d, b) ? 1 : 0;
                    else if(m_bomber(game::gamemode)) return bomber::aidefense(d, b) ? 1 : 0;
                    break;
                }
                case AI_T_ACTOR:
                {
                    if(check(d, b)) return 1;
                    gameent *e = game::getclient(b.target);
                    if(e && e->state == CS_ALIVE) return defense(d, b, e->feetpos()) ? 1 : 0;
                    break;
                }
                default: break;
            }
        }
        return 0;
    }

    int dointerest(gameent *d, aistate &b)
    {
        if(!d->ai->suspended && d->state == CS_ALIVE && aistyle[d->aitype].canmove)
        {
            switch(b.targtype)
            {
                case AI_T_NODE: // this is like a wait state without sitting still..
                {
                    if(check(d, b) || find(d, b)) return 1;
                    if(target(d, b, 4, true)) return 1;
                    if(waypoints.inrange(b.target) && vec(waypoints[b.target].o).sub(d->feetpos()).magnitude() > CLOSEDIST)
                        return makeroute(d, b, waypoints[b.target].o) ? 1 : 0;
                    break;
                }
                case AI_T_ENTITY:
                {
                    if(entities::ents.inrange(b.target))
                    {
                        gameentity &e = *(gameentity *)entities::ents[b.target];
                        if(enttype[e.type].usetype != EU_ITEM) return 0;
                        int sweap = m_weapon(game::gamemode, game::mutators),
                            attr = w_attr(game::gamemode, e.attrs[0], sweap);
                        switch(e.type)
                        {
                            case WEAPON:
                            {
                                if(!e.spawned || !wantsweap(d, attr)) return 0;
                                //float guard = enttype[e.type].radius;
                                //if(d->feetpos().squaredist(e.o) <= guard*guard)
                                //    b.idle = enemy(d, b, e.o, guard*4, weaptype[d->weapselect].melee ? 1 : 0, false) ? 2 : 1;
                                //else b.idle = -1;
                                break;
                            }
                            default: break;
                        }
                        return makeroute(d, b, e.o) ? 1 : 0;
                    }
                    break;
                }
                case AI_T_DROP:
                {
                    loopvj(projs::projs) if(projs::projs[j]->projtype == PRJ_ENT && projs::projs[j]->ready() && projs::projs[j]->id == b.target)
                    {
                        projent &proj = *projs::projs[j];
                        if(!entities::ents.inrange(proj.id) || enttype[entities::ents[proj.id]->type].usetype != EU_ITEM) return 0;
                        gameentity &e = *(gameentity *)entities::ents[proj.id];
                        int sweap = m_weapon(game::gamemode, game::mutators),
                            attr = w_attr(game::gamemode, e.attrs[0], sweap);
                        switch(e.type)
                        {
                            case WEAPON:
                            {
                                if(!wantsweap(d, attr) || proj.owner == d) return 0;
                                //float guard = enttype[e.type].radius;
                                //if(d->feetpos().squaredist(e.o) <= guard*guard)
                                //    b.idle = enemy(d, b, e.o, guard*4, weaptype[d->weapselect].melee ? 1 : 0, false) ? 2 : 1;
                                //else b.idle = -1;
                                break;
                            }
                            default: break;
                        }
                        return makeroute(d, b, proj.o) ? 1 : 0;
                        break;
                    }
                    break;
                }
                default: break;
            }
        }
        return 0;
    }

    int dopursue(gameent *d, aistate &b)
    {
        if(!d->ai->suspended && d->state == CS_ALIVE)
        {
            switch(b.targtype)
            {
                case AI_T_NODE:
                {
                    if(check(d, b)) return 1;
                    if(waypoints.inrange(b.target))
                        return defense(d, b, waypoints[b.target].o) ? 1 : 0;
                    break;
                }

                case AI_T_AFFINITY:
                {
                    if(m_campaign(game::gamemode))
                    {
                        if(aicampaign && entities::ents.inrange(b.target)) return defense(d, b, entities::ents[b.target]->o) ? 1 : 0;
                    }
                    else if(m_capture(game::gamemode)) return capture::aipursue(d, b) ? 1 : 0;
                    else if(m_defend(game::gamemode)) return defend::aipursue(d, b) ? 1 : 0;
                    else if(m_bomber(game::gamemode)) return bomber::aipursue(d, b) ? 1 : 0;
                    break;
                }

                case AI_T_ACTOR:
                {
                    if(aipassive) return 0;
                    //if(check(d, b)) return 1;
                    gameent *e = game::getclient(b.target);
                    if(e && e->state == CS_ALIVE)
                    {
                        bool alt = altfire(d, e);
                        if(aistyle[d->aitype].canmove)
                        {
                            float mindist = weapmindist(d->weapselect, alt);
                            if(!weaptype[d->weapselect].melee) mindist = min(mindist, CLOSEDIST);
                            return patrol(d, b, e->feetpos(), mindist, d->ai->views[2]) ? 1 : 0;
                        }
                        else
                        {
                            vec dp = d->headpos(), ep = getaimpos(d, e, alt);
                            if(cansee(d, dp, ep, d->aitype >= AI_START) || (e->clientnum == d->ai->enemy && d->ai->enemyseen && lastmillis-d->ai->enemyseen <= (d->skill*10)+1000))
                            {
                                b.idle = -1;
                                return 1;
                            }
                            return 0;
                        }
                    }
                    break;
                }
                default: break;
            }
        }
        return 0;
    }

    int closenode(gameent *d)
    {
        vec pos = d->feetpos();
        int node = -1;
        float mindist = CLOSEDIST*CLOSEDIST;
        loopk(2)
        {
            loopv(d->ai->route) if(waypoints.inrange(d->ai->route[i]))
            {
                vec epos = waypoints[d->ai->route[i]].o;
                int entid = obstacles.remap(d, d->ai->route[i], epos, k!=0);
                if(waypoints.inrange(entid))
                {
                    float dist = epos.squaredist(pos);
                    if(dist < mindist)
                    {
                        node = i;
                        mindist = dist;
                    }
                }
            }
            if(node >= 0) break;
        }
        return node;
    }

    int wpspot(gameent *d, int n)
    {
        if(waypoints.inrange(n)) loopk(2)
        {
            vec epos = waypoints[n].o;
            int entid = obstacles.remap(d, n, epos, k!=0);
            if(waypoints.inrange(entid))
            {
                vec feet = d->feetpos();
                if(!aistyle[d->aitype].canjump && epos.z-d->feetpos().z >= JUMPMIN) epos.z = feet.z;
                d->ai->spot = epos;
                d->ai->targnode = entid;
                return feet.squaredist(epos) > MINWPDIST*MINWPDIST ? 1 : 2;
            }
        }
        return 0;
    }

    int randomlink(gameent *d, int n, bool any)
    {
        if(waypoints.inrange(n) && waypoints[n].haslinks())
        {
            waypoint &w = waypoints[n];
            static vector<int> linkmap; linkmap.setsize(0);
            loopi(MAXWAYPOINTLINKS) if(w.links[i] && w.links[i] != n && waypoints.inrange(w.links[i]) && (any || !d->ai->hasprevnode(w.links[i])))
                linkmap.add(w.links[i]);
            if(!linkmap.empty()) return linkmap[rnd(linkmap.length())];
        }
        return -1;
    }

    bool anynode(gameent *d, aistate &b)
    {
        if(waypoints.inrange(d->lastnode)) loopk(2)
        {
            d->ai->clear(k ? true : false);
            int n = randomlink(d, d->lastnode, k!=0);
            if(wpspot(d, n))
            {
                int t = randomlink(d, n, k!=0);
                if(waypoints.inrange(t)) d->ai->route.add(t);
                d->ai->route.add(n);
                d->ai->route.add(d->lastnode);
                return true;
            }
        }
        return false;
    }

    bool checkroute(gameent *d, int n)
    {
        if(d->ai->route.empty() || !d->ai->route.inrange(n)) return false;
        if(n < 3) return false; // route length is too short
        int w = waypoints.inrange(d->lastnode) ? d->lastnode : d->ai->route[n], c = min(n-1, NUMPREVNODES);
        loopj(c) // check ahead to see if we need to go around something
        {
            int p = n-j-1, v = d->ai->route[p];
            if(d->ai->hasprevnode(v) || obstacles.find(v, d)) // something is in the way, try to remap around it
            {
                int m = p-1;
                if(m < 3) return false; // route length is too short from this point
                loopirev(m)
                {
                    int t = d->ai->route[i];
                    if(!d->ai->hasprevnode(t) && !obstacles.find(t, d))
                    {
                        static vector<int> remap; remap.setsize(0);
                        if(route(d, w, t, remap, obstacles))
                        { // kill what we don't want and put the remap in
                            while(d->ai->route.length() > i) d->ai->route.pop();
                            loopvk(remap) d->ai->route.add(remap[k]);
                            return true;
                        }
                        return false; // we failed
                    }
                }
                return false;
            }
        }
        return false;
    }

    bool hunt(gameent *d, aistate &b)
    {
        if(!d->ai->route.empty())
        {
            int n = closenode(d);
            if(d->ai->route.inrange(n) && checkroute(d, n)) n = closenode(d);
            if(d->ai->route.inrange(n))
            {
                if(!n)
                {
                    switch(wpspot(d, d->ai->route[n]))
                    {
                        case 2: d->ai->clear(false);
                        case 1: return true;
                        case 0: default: break;
                    }
                }
                else
                {
                    while(d->ai->route.length() > n+1) d->ai->route.pop(); // waka-waka-waka-waka
                    int m = n-1;
                    if(d->ai->route.inrange(m) && wpspot(d, d->ai->route[m])) return true;
                }
            }
        }
        b.override = false;
        return anynode(d, b);
    }

    void jumpto(gameent *d, aistate &b, const vec &pos, bool locked)
    {
        vec off = vec(pos).sub(d->feetpos());
        bool sequenced = d->ai->blockseq || d->ai->targseq, offground = d->timeinair && !physics::liquidcheck(d) && !d->onladder,
            impulse = d->timeinair > 500 && !d->turnside && physics::canimpulse(d, 1, false), jet = d->timeinair > 250 && !d->turnside && physics::allowhover(d),
            jumper = !offground && (locked || sequenced || off.z >= JUMPMIN || (d->aitype == AI_BOT && lastmillis >= d->ai->jumprand)),
            jump = (impulse || jet || jumper) && (jet || lastmillis >= d->ai->jumpseed);
        if(jump)
        {
            vec old = d->o;
            d->o = vec(pos).add(vec(0, 0, d->height));
            if(!collide(d, vec(0, 0, 1))) jump = false;
            d->o = old;
            if(jump)
            {
                loopi(entities::lastenttype[PUSHER]) if(entities::ents[i]->type == PUSHER)
                {
                    gameentity &e = *(gameentity *)entities::ents[i];
                    float radius = (e.attrs[3] ? e.attrs[3] : enttype[e.type].radius)*1.5f; radius *= radius;
                    if(e.o.squaredist(pos) <= radius) { jump = false; break; }
                }
            }
        }
        if(jump)
        {
            if((d->action[AC_JUMP] = jump) != false) d->actiontime[AC_JUMP] = lastmillis;
            int seed = (111-d->skill)*(locked || impulse || jet ? 1 : (d->onladder || d->inliquid ? 3 : 5));
            d->ai->jumpseed = lastmillis+seed+rnd(seed);
            seed *= 100; if(b.idle) seed *= 10;
            d->ai->jumprand = lastmillis+seed+rnd(seed);
        }
        if(!sequenced)
        {
            if(d->timeinair > 250 && !d->turnside && (d->skill >= 100 || !rnd(101-d->skill)) && physics::canimpulse(d, 3, true))
                d->action[AC_SPECIAL] = true;
            else if(!aipassive && !weaptype[d->weapselect].melee && locked && lastmillis-d->ai->lastmelee >= (201-d->skill)*5)
            {
                d->action[AC_SPECIAL] = true;
                d->ai->lastmelee = lastmillis;
            }
        }
    }

    bool lockon(gameent *d, gameent *e, float maxdist, bool check)
    {
        if(!aipassive && check && !d->blocked)
        {
            vec dir = vec(e->o).sub(d->o);
            float xydist = dir.x*dir.x+dir.y*dir.y, zdist = dir.z*dir.z, mdist = maxdist*maxdist, ddist = d->radius*d->radius+e->radius*e->radius;
            if(zdist <= ddist && xydist >= ddist+4 && xydist <= mdist+ddist) return true;
        }
        return false;
    }

    int process(gameent *d, aistate &b)
    {
        int result = 0, stupify = d->skill <= 10+rnd(15) ? rnd(d->skill*1000) : 0, skmod = 101-d->skill;
        float frame = d->skill <= 100 ? float(lastmillis-d->ai->lastrun)/float(max(skmod,1)*10) : 1;
        if(!aistyle[d->aitype].canstrafe && d->skill <= 100) frame *= 2;
        vec dp = d->headpos();

        bool idle = b.idle == 1 || (stupify && stupify <= skmod) || d->ai->suspended;
        d->action[AC_SPECIAL] = d->ai->dontmove = false;
        if(idle || !aistyle[d->aitype].canmove)
        {
            d->ai->lasthunt = lastmillis;
            d->ai->dontmove = true;
            d->ai->spot = vec(0, 0, 0);
        }
        else if(hunt(d, b))
        {
            game::getyawpitch(dp, vec(d->ai->spot).add(vec(0, 0, d->height)), d->ai->targyaw, d->ai->targpitch);
            d->ai->lasthunt = lastmillis;
        }
        else
        {
            idle = d->ai->dontmove = true;
            d->ai->spot = vec(0, 0, 0);
        }

        bool enemyok = false, locked = false, melee = weaptype[d->weapselect].melee || d->aitype == AI_BOT;
        gameent *e = game::getclient(d->ai->enemy);
        if(!aipassive)
        {
            if(!(enemyok = e && targetable(d, e, true)) || d->skill >= 50)
            {
                gameent *f = game::intersectclosest(dp, d->ai->target, d);
                if(f)
                {
                    if(targetable(d, f, true))
                    {
                        if(!enemyok) violence(d, b, f, weaptype[d->weapselect].melee ? 1 : 0);
                        enemyok = true;
                        e = f;
                    }
                    else enemyok = false;
                }
                else if(!enemyok && target(d, b, weaptype[d->weapselect].melee ? 1 : 0))
                    enemyok = (e = game::getclient(d->ai->enemy)) != NULL;
            }
            if(enemyok)
            {
                bool alt = altfire(d, e);
                vec ep = getaimpos(d, e, alt);
                float yaw, pitch;
                game::getyawpitch(dp, ep, yaw, pitch);
                game::fixrange(yaw, pitch);
                bool insight = cansee(d, dp, ep), hasseen = d->ai->enemyseen && lastmillis-d->ai->enemyseen <= (d->skill*10)+3000,
                    quick = d->ai->enemyseen && lastmillis-d->ai->enemyseen <= (WEAP2(d->weapselect, fullauto, alt) ? WEAP2(d->weapselect, adelay, alt)*3 : skmod)+30;
                if(insight) d->ai->enemyseen = lastmillis;
                if(idle || insight || hasseen || quick)
                {
                    float sskew = insight || d->skill > 100 ? 1.5f : (hasseen ? 1.f : 0.5f);
                    if(insight && lockon(d, e, aistyle[d->aitype].canstrafe ? 32 : 16, melee))
                    {
                        d->ai->targyaw = yaw;
                        d->ai->targpitch = pitch;
                        frame *= 2;
                        if(d->aitype == AI_BOT) locked = true;
                    }
                    game::scaleyawpitch(d->yaw, d->pitch, yaw, pitch, frame, frame*sskew);
                    if(insight || quick)
                    {
                        bool shoot = canshoot(d, e, alt);
                        if(d->action[alt ? AC_ALTERNATE : AC_ATTACK] && WEAP2(d->weapselect, power, alt) && WEAP2(d->weapselect, cooked, alt))
                        { // TODO: make AI more aware of what they're shooting
                            int cooked = WEAP2(d->weapselect, cooked, alt);
                            if(cooked&8) shoot = false; // inverted life
                        }
                        if(shoot && hastarget(d, b, e, alt, yaw, pitch, dp.squaredist(ep)))
                        {
                            d->action[alt ? AC_ALTERNATE : AC_ATTACK] = true;
                            d->actiontime[alt ? AC_ALTERNATE : AC_ATTACK] = lastmillis;
                            result = 3;
                        }
                        else result = 2;
                    }
                    else result = 1;
                }
                else
                {
                    if(!d->ai->enemyseen || lastmillis-d->ai->enemyseen > (d->skill*50)+3000)
                    {
                        d->ai->enemy = -1;
                        d->ai->enemyseen = d->ai->enemymillis = 0;
                    }
                    enemyok = false;
                    result = 0;
                }
            }
            else
            {
                if(!enemyok)
                {
                    d->ai->enemy = -1;
                    d->ai->enemyseen = d->ai->enemymillis = 0;
                }
                enemyok = false;
                result = 0;
            }
        }
        else
        {
            d->ai->enemy = -1;
            d->ai->enemyseen = d->ai->enemymillis = 0;
            result = 0;
        }
        if(result < 3) d->action[AC_ATTACK] = d->action[AC_ALTERNATE] = false;

        game::fixrange(d->ai->targyaw, d->ai->targpitch);
        d->aimyaw = d->ai->targyaw;
        d->aimpitch = d->ai->targpitch;
        if(!result) game::scaleyawpitch(d->yaw, d->pitch, d->ai->targyaw, d->ai->targpitch, frame*0.25f, frame*0.25f);

        if(aistyle[d->aitype].canjump && (!d->ai->dontmove || b.idle)) jumpto(d, b, d->ai->spot, locked);
        if(d->aitype == AI_BOT)
        {
            bool wantsrun = false;
            if(physics::allowimpulse(d))
            {
                if(!impulsemeter || impulsesprint == 0 || impulseregensprint > 0) wantsrun = true;
                else if(b.idle == -1 && !d->ai->dontmove)
                    wantsrun = (d->action[AC_SPRINT] || !d->actiontime[AC_SPRINT] || lastmillis-d->actiontime[AC_SPRINT] > PHYSMILLIS*2);
            }

            if(d->ai->lastpusher >= 0 && d->physstate == PHYS_FALL && lastmillis-d->ai->lastpushtime < (skmod/10)*250)
            {
                float offyaw, offpitch;
                vec v = vec(d->vel).normalize();
                vectoyawpitch(v, offyaw, offpitch);
                offyaw -= d->aimyaw;
                if(offyaw > 180) offyaw -= 360;
                else if(offyaw < -180) offyaw += 360;
                offpitch -= d->aimpitch;
                if(fabs(offyaw)+fabs(offpitch) >= 135)
                {
                    wantsrun = false;
                    d->ai->lastpusher = -1;
                    d->ai->lastpushtime = 0;
                }
                else
                {
                    d->ai->dontmove = true;
                    wantsrun = false;
                }
            }
            else
            {
                d->ai->lastpusher = -1;
                d->ai->lastpushtime = 0;
            }

            if(d->action[AC_SPRINT] != wantsrun)
                if((d->action[AC_SPRINT] = !d->action[AC_SPRINT]) == true) d->actiontime[AC_SPRINT] = lastmillis;
        }

        if(d->ai->dontmove || (d->aitype >= AI_START && lastmillis-d->lastpain <= PHYSMILLIS/3)) d->move = d->strafe = 0;
        else if(!aistyle[d->aitype].canmove || !aistyle[d->aitype].canstrafe)
        {
            d->aimyaw = d->yaw;
            d->aimpitch = d->pitch;
            d->move = aistyle[d->aitype].canmove ? 1 : 0;
            d->strafe = 0;
        }
        else
        { // our guys move one way.. but turn another?! :)
            const struct aimdir { int move, strafe, offset; } aimdirs[8] =
            {
                {  1,  0,   0 },
                {  1,  -1,  45 },
                {  0,  -1,  90 },
                { -1,  -1,  135 },
                { -1,  0,   180 },
                { -1,  1,   225 },
                {  0,  1,   270 },
                {  1,  1,   315 }
            };
            float yaw = d->aimyaw-d->yaw;
            while(yaw < 0.0f) yaw += 360.0f;
            while(yaw >= 360.0f) yaw -= 360.0f;
            const aimdir &ad = aimdirs[clamp(((int)floor((yaw+22.5f)/45.0f))&7, 0, 7)];
            d->move = ad.move;
            d->strafe = ad.strafe;
            d->aimyaw -= ad.offset;
        }
        if(!aistyle[d->aitype].canstrafe && d->move && enemyok && lockon(d, e, 8, melee)) d->move = 0;
        game::fixrange(d->aimyaw, d->aimpitch);
        findorientation(dp, d->yaw, d->pitch, d->ai->target);
        return result;
    }

    bool hasrange(gameent *d, gameent *e, int weap)
    {
        if(!e) return true;
        if(targetable(d, e))
        {
            loopk(2)
            {
                vec ep = getaimpos(d, e, k!=0);
                float dist = ep.squaredist(d->headpos());
                if(weaprange(d, weap, k!=0, dist)) return true;
            }
        }
        return false;
    }

    bool request(gameent *d, aistate &b)
    {
        int busy = process(d, b), sweap = m_weapon(game::gamemode, game::mutators);
        bool haswaited = d->weapwaited(d->weapselect, lastmillis, d->skipwait(d->weapselect, 0, lastmillis, (1<<WEAP_S_RELOAD)));
        if(d->aitype == AI_BOT)
        {
            if(b.idle && busy <= 1 && d->carry(sweap, 1) > 1 && d->weapstate[d->weapselect] != WEAP_S_WAIT)
            {
                loopirev(WEAP_ITEM) if(i >= WEAP_OFFSET && i != d->ai->weappref && i != d->weapselect)
                {
                    client::addmsg(N_DROP, "ri3", d->clientnum, lastmillis-game::maptime, i);
                    d->setweapstate(d->weapselect, WEAP_S_WAIT, weaponswitchdelay, lastmillis);
                    d->ai->lastaction = lastmillis;
                    return true;
                }
            }
            if(busy <= 2 && !d->action[AC_USE] && haswaited)
            {
                static vector<actitem> actitems;
                actitems.setsize(0);
                if(entities::collateitems(d, actitems))
                {
                    while(!actitems.empty())
                    {
                        actitem &t = actitems.last();
                        int ent = -1;
                        switch(t.type)
                        {
                            case ITEM_ENT:
                            {
                                if(!entities::ents.inrange(t.target)) break;
                                extentity &e = *entities::ents[t.target];
                                if(enttype[e.type].usetype != EU_ITEM) break;
                                ent = t.target;
                                break;
                            }
                            case ITEM_PROJ:
                            {
                                if(!projs::projs.inrange(t.target)) break;
                                projent &proj = *projs::projs[t.target];
                                if(!entities::ents.inrange(proj.id)) break;
                                extentity &e = *entities::ents[proj.id];
                                if(enttype[e.type].usetype != EU_ITEM || proj.owner == d) break;
                                ent = proj.id;
                                break;
                            }
                            default: break;
                        }
                        if(entities::ents.inrange(ent))
                        {
                            extentity &e = *entities::ents[ent];
                            int attr = e.type == WEAPON ? w_attr(game::gamemode, e.attrs[0], sweap) : e.attrs[0];
                            if(d->canuse(e.type, attr, e.attrs, sweap, lastmillis, WEAP_S_FILTER)) switch(e.type)
                            {
                                case WEAPON:
                                {
                                    if(!wantsweap(d, attr)) break;
                                    d->action[AC_USE] = true;
                                    d->ai->lastaction = d->actiontime[AC_USE] = lastmillis;
                                    return true;
                                }
                                default: break;
                            }
                        }
                        actitems.pop();
                    }
                }
            }
        }

        bool timepassed = d->weapstate[d->weapselect] == WEAP_S_IDLE && (!d->ammo[d->weapselect] || lastmillis-d->weaplast[d->weapselect] >= 6000-(d->skill*50));
        if(busy <= 2 && haswaited && timepassed)
        {
            int weap = d->ai->weappref;
            gameent *e = game::getclient(d->ai->enemy);
            if(!isweap(weap) || !d->hasweap(weap, sweap) || !hasrange(d, e, weap))
            {
                loopirev(WEAP_MAX) if(i >= WEAP_MELEE && d->hasweap(i, sweap) && hasrange(d, e, i)) { weap = i; break; }
            }
            if(isweap(weap) && weap != d->weapselect && weapons::weapselect(d, weap))
            {
                d->ai->lastaction = lastmillis;
                return true;
            }
        }

        if(d->hasweap(d->weapselect, sweap) && busy <= (!d->ammo[d->weapselect] ? 2 : 0) && timepassed)
        {
            if(weapons::weapreload(d, d->weapselect))
            {
                d->ai->lastaction = lastmillis;
                return true;
            }
        }

        return busy >= 1;
    }

    void timeouts(gameent *d, aistate &b)
    {
        if(d->blocked)
        {
            d->ai->blocktime += lastmillis-d->ai->lastrun;
            if(d->ai->blocktime > (d->ai->blockseq+1)*1000)
            {
                d->ai->blockseq++;
                switch(d->ai->blockseq)
                {
                    case 1: case 2: case 3:
                        if(waypoints.inrange(d->ai->targnode)) d->ai->addprevnode(d->ai->targnode);
                        d->ai->clear(false);
                        break;
                    case 4: d->ai->reset(false); break;
                    case 5: default: game::suicide(d, HIT_LOST); return; break; // this is our last resort..
                }
                if(aidebug >= 5 && dbgfocus(d))
                    conoutf("%s blocked %dms sequence %d", game::colorname(d), d->ai->blocktime, d->ai->blockseq);
            }
        }
        else d->ai->blocktime = d->ai->blockseq = 0;

        if(d->ai->targnode == d->ai->targlast)
        {
            d->ai->targtime += lastmillis-d->ai->lastrun;
            if(d->ai->targtime > (d->ai->targseq+1)*1000)
            {
                d->ai->targseq++;
                switch(d->ai->targseq)
                {
                    case 1: case 2: case 3:
                        if(waypoints.inrange(d->ai->targnode)) d->ai->addprevnode(d->ai->targnode);
                        d->ai->clear(false);
                        break;
                    case 4: d->ai->reset(false); break;
                    case 5: default: game::suicide(d, HIT_LOST); return; break; // this is our last resort..
                }
                if(aidebug >= 5 && dbgfocus(d))
                    conoutf("%s targeted %d too long %dms sequence %d", game::colorname(d), d->ai->targnode, d->ai->targtime, d->ai->targseq);
            }
        }
        else
        {
            d->ai->targtime = d->ai->targseq = 0;
            d->ai->targlast = d->ai->targnode;
        }

        if(d->ai->lasthunt)
        {
            int millis = lastmillis-d->ai->lasthunt;
            if(millis <= 1000) { d->ai->tryreset = false; d->ai->huntseq = 0; }
            else if(millis > (d->ai->huntseq+1)*1000)
            {
                d->ai->huntseq++;
                switch(d->ai->huntseq)
                {
                    case 1: d->ai->clear(false); break;
                    case 2: d->ai->reset(false); break;
                    case 3: default: game::suicide(d, HIT_LOST); return; break; // this is our last resort..
                }
                if(aidebug >= 5 && dbgfocus(d))
                    conoutf("%s hunting %dms sequence %d", game::colorname(d), millis, d->ai->huntseq);
            }
        }
    }

    #define issuspended(a) (a->ai->suspended || a->speedscale == 0)

    void logic(gameent *d, aistate &b)
    {
        if(!issuspended(d))
        {
            if(d->state != CS_ALIVE || !game::allowmove(d)) d->stopmoving(true);
            else
            {
                if(!request(d, b)) target(d, b, weaptype[d->weapselect].melee ? 1 : 0);
                weapons::shoot(d, d->ai->target);
            }
        }
        if(d->state == CS_DEAD || d->state == CS_WAITING)
        {
            if(d->ragdoll) moveragdoll(d, true);
            else if(lastmillis-d->lastpain < 5000)
                physics::move(d, 1, false);
        }
        else
        {
            if(d->ragdoll) cleanragdoll(d);
            if(d->state == CS_ALIVE && !game::intermission)
            {
                if(!issuspended(d))
                {
                    bool ladder = d->onladder;
                    physics::move(d, 1, true);
                    if(aistyle[d->aitype].canmove && !b.idle) timeouts(d, b);
                    if(!ladder && d->onladder) d->ai->jumpseed = lastmillis;
                    entities::checkitems(d);
                }
                else
                {
                    d->move = d->strafe = 0;
                    physics::move(d, 1, true);
                }
            }
        }
    }

    void avoid()
    {
        obstacles.clear();
        int numdyns = game::numdynents();
        loopi(numdyns)
        {
            gameent *d = (gameent *)game::iterdynents(i);
            if(!d) continue; // || d->aitype >= AI_START) continue;
            if(d->state != CS_ALIVE || !physics::issolid(d)) continue;
            obstacles.avoidnear(d, d->feetpos(), d->radius+2);
        }
        obstacles.add(wpavoid);
        loopv(projs::projs)
        {
            projent *p = projs::projs[i];
            if(p && p->state == CS_ALIVE && p->projtype == PRJ_SHOT && WEAPEX(p->weap, p->flags&HIT_ALT, game::gamemode, game::mutators, p->curscale))
                obstacles.avoidnear(p, p->o, (WEAPEX(p->weap, p->flags&HIT_ALT, game::gamemode, game::mutators, p->curscale)*p->lifesize)*1.25f+2);
        }
        loopi(entities::lastenttype[MAPMODEL]) if(entities::ents[i]->type == MAPMODEL && !entities::ents[i]->links.empty() && !entities::ents[i]->spawned)
        {
            mapmodelinfo &mmi = getmminfo(entities::ents[i]->attrs[0]);
            if(!&mmi) continue;
            vec center, radius;
            mmi.m->collisionbox(0, center, radius);
            if(entities::ents[i]->attrs[4]) { center.mul(entities::ents[i]->attrs[4]/100.f); radius.mul(entities::ents[i]->attrs[4]/100.f); }
            if(!mmi.m->ellipsecollide) rotatebb(center, radius, int(entities::ents[i]->attrs[1]));
            float limit = WAYPOINTRADIUS+(max(radius.x, max(radius.y, radius.z))*mmi.m->height);
            vec pos = entities::ents[i]->o; pos.z += limit*0.5f;
            obstacles.avoidnear(NULL, pos, limit);
        }
    }

    void think(gameent *d, bool run)
    {
        // the state stack works like a chain of commands, certain commands simply replace each other
        // others spawn new commands to the stack the ai reads the top command from the stack and executes
        // it or pops the stack and goes back along the history until it finds a suitable command to execute
        bool cleannext = false;
        if(d->ai->state.empty()) d->ai->addstate(AI_S_WAIT);
        loopvrev(d->ai->state)
        {
            aistate &c = d->ai->state[i];
            if(cleannext)
            {
                c.millis = lastmillis;
                c.override = false;
                cleannext = false;
            }
            if(d->state == CS_DEAD)
            {
                if(d->respawned < 0 && (!d->lastdeath || lastmillis-d->lastdeath > (d->aitype == AI_BOT ? 500 : enemyspawntime)))
                {
                    if(d->aitype == AI_BOT && m_arena(game::gamemode, game::mutators))
                    {
                        d->loadweap[0] = d->ai->weappref; d->loadweap[1] = WEAP_MELEE;
                        client::addmsg(N_LOADWEAP, "ri3", d->clientnum, d->loadweap[0], d->loadweap[1]);
                    }
                    client::addmsg(N_TRYSPAWN, "ri", d->clientnum);
                    d->respawned = lastmillis;
                }
            }
            else if(d->state == CS_ALIVE && run)
            {
                int result = 0;
                c.idle = 0;
                switch(c.type)
                {
                    case AI_S_WAIT: result = dowait(d, c); break;
                    case AI_S_DEFEND: result = dodefense(d, c); break;
                    case AI_S_PURSUE: result = dopursue(d, c); break;
                    case AI_S_INTEREST: result = dointerest(d, c); break;
                    default: result = 0; break;
                }
                if(result <= 0)
                {
                    if(c.type != AI_S_WAIT)
                    {
                        switch(result)
                        {
                            case 0: default: d->ai->removestate(i); cleannext = true; break;
                            case -1: i = d->ai->state.length()-1; break;
                        }
                        continue; // shouldn't interfere
                    }
                }
                else if(d->ai->suspended) d->ai->unsuspend();
            }
            logic(d, c);
            break;
        }
        if(d->ai->trywipe) d->ai->wipe();
        d->ai->lastrun = lastmillis;
    }

    void drawroute(gameent *d, float amt)
    {
        int colour = game::getcolour(d, game::playerdisplaytone), last = -1;
        loopvrev(d->ai->route)
        {
            if(d->ai->route.inrange(last))
            {
                int index = d->ai->route[i], prev = d->ai->route[last];
                if(waypoints.inrange(index) && waypoints.inrange(prev))
                {
                    waypoint &w = waypoints[index], &v = waypoints[prev];
                    vec fr = v.o, dr = w.o;
                    fr.z += amt; dr.z += amt;
                    part_trace(fr, dr, 1, 1, 1, colour);
                }
            }
            last = i;
        }
        if(aidebug > 4)
        {
            vec fr = vec(d->feetpos()).add(vec(0, 0, amt));
            if(d->ai->spot != vec(0, 0, 0)) part_trace(fr, vec(d->ai->spot).add(vec(0, 0, 0.1f)), 1, 1, 1, 0x00FFFF);
            if(waypoints.inrange(d->ai->targnode))
            {
                vec dr = vec(waypoints[d->ai->targnode].o).add(vec(0, 0, amt));
                part_trace(fr, dr, 1, 1, 1, 0xFF00FF);
            }
            if(waypoints.inrange(d->lastnode))
            {
                vec dr = vec(waypoints[d->lastnode].o).add(vec(0, 0, amt));
                part_trace(fr, dr, 1, 1, 1, 0xFFFF00);
                fr = dr;
            }
            loopi(NUMPREVNODES)
            {
                if(waypoints.inrange(d->ai->prevnodes[i]))
                {
                    vec dr = vec(waypoints[d->ai->prevnodes[i]].o).add(vec(0, 0, amt));
                    part_trace(dr, fr, 1, 1, 1, 0x884400);
                    fr = dr;
                }
            }
        }
    }

    void render()
    {
        if(aidebug > 1)
        {
            int amt[2] = { 0, 0 };
            loopv(game::players) if(game::players[i] && dbgfocus(game::players[i])) amt[0]++;
            loopv(game::players) if(game::players[i] && game::players[i]->state == CS_ALIVE && dbgfocus(game::players[i]))
            {
                gameent *d = game::players[i];
                vec pos = d->abovehead();
                pos.z += 3;
                bool top = true;
                amt[1]++;
                if(aidebug > 3 && rendernormally && aistyle[d->aitype].canmove)
                    drawroute(d, 4.f*(float(amt[1])/float(amt[0])));
                if(aidebug > 2)
                {
                    defformatstring(q)("node: %d route: %d (%d)",
                        d->lastnode,
                        !d->ai->route.empty() ? d->ai->route[0] : -1,
                        d->ai->route.length()
                    );
                    part_textcopy(pos, q);
                    pos.z += 2;
                }
                loopvrev(d->ai->state)
                {
                    aistate &b = d->ai->state[i];
                    defformatstring(s)("%s%s (%s) %s:%d",
                        top ? "<default>\fg" : "<sub>\fy",
                        stnames[b.type],
                        hud::timetostr(lastmillis-b.millis),
                        sttypes[b.targtype+1], b.target
                    );
                    part_textcopy(pos, s);
                    pos.z += 2;
                    if(top)
                    {
                        if(aidebug > 2) top = false;
                        else break;
                    }
                }
                if(aidebug > 2)
                {
                    if(isweap(d->ai->weappref))
                    {
                        part_textcopy(pos, WEAP(d->ai->weappref, name));
                        pos.z += 2;
                    }
                    gameent *e = game::getclient(d->ai->enemy);
                    if(e)
                    {
                        part_textcopy(pos, game::colorname(e, NULL, "<default>"));
                        pos.z += 2;
                    }
                }
            }
            if(aidebug >= 4 && !m_edit(game::gamemode))
            {
                int cur = 0;
                loopv(obstacles.obstacles)
                {
                    const avoidset::obstacle &ob = obstacles.obstacles[i];
                    int next = cur + ob.numwaypoints;
                    for(; cur < next; cur++)
                    {
                        int ent = obstacles.waypoints[cur];
                        if(waypoints.inrange(ent))
                            part_create(PART_EDIT, 1, waypoints[ent].o, 0xFF6600, 1.5f);
                    }
                    cur = next;
                }
            }
        }
        if(showwaypoints || aidebug > 5)
        {
            vector<int> close;
            int len = waypoints.length();
            if(showwaypointsradius)
            {
                findwaypointswithin(camera1->o, 0, showwaypointsradius, close);
                len = close.length();
            }
            loopi(len)
            {
                int idx = showwaypointsradius ? close[i] : i;
                waypoint &w = waypoints[idx];
                loopj(MAXWAYPOINTLINKS)
                {
                     int link = w.links[j];
                     if(!link) break;
                     waypoint &v = waypoints[link];
                     bool both = false;
                     loopk(MAXWAYPOINTLINKS) if(v.links[k] == idx) { both = true; break; }
                     part_trace(w.o, v.o, 1, 1, 1, both ? 0xAA44CC : 0x660088);
                }
            }
        }
    }

    void preload()
    {
        loopi(AI_TOTAL)
        {
            loadmodel(aistyle[i+AI_START].tpmdl, -1, true);
            loadmodel(aistyle[i+AI_START].fpmdl, -1, true);
        }
    }
}
