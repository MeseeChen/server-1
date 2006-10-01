//////////////////////////////////////////////////////////////////////
// OpenTibia - an opensource roleplaying game
//////////////////////////////////////////////////////////////////////
// 
//////////////////////////////////////////////////////////////////////
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software Foundation,
// Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
//////////////////////////////////////////////////////////////////////

#include "creature.h"

#include "game.h"
#include "otsystem.h"
#include "player.h"
#include "npc.h"
#include "monster.h"
#include "tile.h"
#include "container.h"
#include "condition.h"
#include "combat.h"

#include <string>
#include <sstream>
#include <algorithm>
#include <vector>

OTSYS_THREAD_LOCKVAR AutoID::autoIDLock;
unsigned long AutoID::count = 1000;
AutoID::list_type AutoID::list;

extern Game g_game;

Creature::Creature() :
  isInternalRemoved(false)
{
	direction  = NORTH;
	master = NULL;

	health     = 1000;
	healthMax  = 1000;

	level = 0;
	mana = 0;
	manaMax = 0;
	lastMove = 0;
	speed = 220;

	followCreature = NULL;
	eventWalk = 0;
	internalUpdateFollow = false;
	followDistance = 1;

	eventCheck = 0;
	eventCheckAttacking = 0;
	attackedCreature = NULL;
	lastHitCreature = 0;
	internalDefense = true;
	internalArmor = true;
}

Creature::~Creature()
{
	std::list<Creature*>::iterator cit;
	for(cit = summons.begin(); cit != summons.end(); ++cit) {
		(*cit)->setAttackedCreature(NULL);
		(*cit)->setMaster(NULL);
		(*cit)->releaseThing2();
	}

	summons.clear();

	for(ConditionList::iterator it = conditions.begin(); it != conditions.end(); ++it){
		(*it)->endCondition(this, REASON_ABORT);
		delete *it;
	}

	conditions.clear();

	attackedCreature = NULL;

	//std::cout << "Creature destructor " << this->getID() << std::endl;
}

void Creature::setRemoved()
{
	isInternalRemoved = true;
}

void Creature::onThink(uint32_t interval)
{
	eventCheck = g_game.addEvent(makeTask(interval, boost::bind(&Game::checkCreature,
		&g_game, getID(), interval)));

	if(internalUpdateFollow && followCreature){
		internalUpdateFollow = false;
		internalFollowCreature(followCreature);
	}

	internalDefense = true;
	internalArmor = true;
}

void Creature::onWalk()
{
	Direction dir;
	if(getNextStep(dir)){
		ReturnValue ret = g_game.internalMoveCreature(this, dir);

		if(ret != RET_NOERROR){
			internalUpdateFollow = true;
		}
	}

	eventWalk = 0;
	addWalkEvent();
}

bool Creature::getNextStep(Direction& dir)
{
	bool result = false;

	if(!listWalkDir.empty()){
		Position pos = getPosition();
		dir = listWalkDir.front();
		listWalkDir.pop_front();
		result = true;
	}

	if(hasCondition(CONDITION_DRUNK)){
		switch(random_range(0, 4)){
			case 0: dir = NORTH; break;
			case 1: dir = WEST;  break;
			case 3: dir = SOUTH; break;
			case 4: dir = EAST;  break;

			default:
				break;
		}
	}

	return result;
}

void Creature::addWalk(std::list<Direction>& listDir)
{
	listWalkDir = listDir;
	addWalkEvent();
}

void Creature::addWalkEvent()
{
	if(eventWalk == 0){
		int64_t ticks = getEventStepTicks();
		eventWalk = g_game.addEvent(makeTask(ticks, std::bind2nd(std::mem_fun(&Game::checkWalk), getID())));
	}
}

void Creature::validateWalkPath()
{
	if(!internalUpdateFollow && followCreature){
		if(!g_game.isPathValid(this, listWalkDir, followCreature->getPosition())){
			internalUpdateFollow = true;
		}
	}
}

void Creature::stopWalkEvent()
{
	if(eventWalk != 0){
		g_game.stopEvent(eventWalk);
		eventWalk = 0;
	}
}

void Creature::onAddTileItem(const Position& pos, const Item* item)
{
	validateWalkPath();
}

void Creature::onUpdateTileItem(const Position& pos, uint32_t stackpos, const Item* oldItem, const Item* newItem)
{
	validateWalkPath();
}

void Creature::onRemoveTileItem(const Position& pos, uint32_t stackpos, const Item* item)
{
	validateWalkPath();
}

void Creature::onUpdateTile(const Position& pos)
{
	validateWalkPath();
}

void Creature::onCreatureAppear(const Creature* creature, bool isLogin)
{
	validateWalkPath();
}

void Creature::onCreatureDisappear(const Creature* creature)
{
	if(attackedCreature == creature){
		setAttackedCreature(NULL);
	}

	if(followCreature == creature){
		setFollowCreature(NULL);
	}
}

void Creature::onCreatureDisappear(const Creature* creature, uint32_t stackpos, bool isLogout)
{
	validateWalkPath();

	onCreatureDisappear(creature);
}

void Creature::onCreatureMove(const Creature* creature, const Position& oldPos, uint32_t oldStackPos, bool teleport)
{
	if(followCreature == creature || (creature == this && followCreature)){
		if(!creature->isInRange(followCreature->getPosition())){
			onCreatureDisappear(followCreature);	
		}
		
		validateWalkPath();
	}
	if(attackedCreature == creature || (creature == this && attackedCreature)){
		if(!creature->isInRange(attackedCreature->getPosition())){
			onCreatureDisappear(attackedCreature);	
		}
	}
}

void Creature::die()
{
	Creature* lastHitCreature = NULL;
	Creature* mostDamageCreature = NULL;

	if(getKillers(&lastHitCreature, &mostDamageCreature)){
		if(lastHitCreature){
			lastHitCreature->onKilledCreature(this);
		}

		if(mostDamageCreature && mostDamageCreature != lastHitCreature){
			mostDamageCreature->onKilledCreature(this);
		}
	}

	for(DamageMap::iterator it = damageMap.begin(); it != damageMap.end(); ++it){
		if(Creature* attacker = g_game.getCreatureByID((*it).first)){
			attacker->onAttackedCreatureKilled(this);
		}
	}
}

bool Creature::getKillers(Creature** _lastHitCreature, Creature** _mostDamageCreature)
{
	*_lastHitCreature = g_game.getCreatureByID(lastHitCreature);

	int32_t mostDamage = 0;
	for(DamageMap::iterator it = damageMap.begin(); it != damageMap.end(); ++it){
		if((*it).second > mostDamage){
			if(*_mostDamageCreature = g_game.getCreatureByID((*it).first)){
				mostDamage = (*it).second;
			}
		}
	}

	return (*_lastHitCreature || *_mostDamageCreature);
}

Item* Creature::getCorpse()
{
	Item* corpse = Item::CreateItem(getLookCorpse());

	if(corpse){
		if(Container* corpseContainer = corpse->getContainer()){
			dropLoot(corpseContainer);
		}
	}

	return corpse;
}

void Creature::changeHealth(int32_t healthChange)
{
	if(healthChange > 0){
		health += std::min(healthChange, healthMax - health);
	}
	else{
		health = std::max((int32_t)0, health + healthChange);
	}
}

void Creature::changeMana(int32_t manaChange)
{
	if(manaChange > 0){
		mana += std::min(manaChange, manaMax - mana);
	}
	else{
		mana = std::max((int32_t)0, mana + manaChange);
	}
}

/*
void Creature::changeSoul(int32_t soulChange)
{
	if(soulChange > 0){
		soul += std::min(soulChange, soulMax - mana);
	}
	else{
		soul = std::max((int32_t)0, soul + soulChange);
	}
}
*/

void Creature::drainHealth(Creature* attacker, DamageType_t damageType, int32_t damage)
{
	changeHealth(-damage);

	if(attacker){
		attacker->onAttackedCreatureDrainHealth(this, damage);
	}
}

void Creature::drainMana(Creature* attacker, int32_t manaLoss)
{
	onAttacked();
	changeMana(-manaLoss);
}

void Creature::setAttackedCreature(Creature* creature)
{
	attackedCreature = creature;

	if(attackedCreature){
		onAttackedCreature(attackedCreature);
		attackedCreature->onAttacked();
	}

	std::list<Creature*>::iterator cit;
	for(cit = summons.begin(); cit != summons.end(); ++cit) {
		(*cit)->setAttackedCreature(creature);
	}
}

BlockType_t Creature::blockHit(Creature* attacker, DamageType_t damageType, int32_t& damage,
	bool checkDefense /* = false */, bool checkArmor /* = false */)
{
	BlockType_t blockType = BLOCK_NONE;

	if(blockType == BLOCK_NONE && isImmune(damageType)){
		damage = 0;
		blockType = BLOCK_IMMUNITY;
	}

	if(blockType == BLOCK_NONE && internalDefense && checkDefense){
		internalDefense = false;
		int32_t defense = getDefense();

		//TODO: change formulas
		int32_t probability = rand() % 100;
		if(probability < defense){
			damage = 0;
			blockType = BLOCK_DEFENSE;
		}
	}
	
	if(blockType == BLOCK_NONE && internalArmor && checkArmor){
		internalArmor = false;
		int32_t armor = getArmor();

		//TODO: change formulas
		int32_t probability = rand() % 100;
		int32_t reduceDamage = (probability * armor * damage) / 3000;
		if(reduceDamage >= damage){
			damage = 0;
			blockType = BLOCK_ARMOR;
		}
		else{
			damage -= reduceDamage;
		}
	}

	if(attacker){
		attacker->onAttackedCreature(this);
		attacker->onAttackedCreatureBlockHit(this, blockType);
	}

	onAttacked();

	return blockType;
}

void Creature::setFollowCreature(const Creature* creature)
{
	if(followCreature != creature){
		followCreature = creature;

		onFollowCreature(creature);
	}
}

bool Creature::internalFollowCreature(const Creature* creature)
{
	if(creature){
		listWalkDir.clear();
		if(!g_game.getPathToEx(this, creature->getPosition(), followDistance, followDistance, true, listWalkDir)){
			setFollowCreature(NULL);
			return false;
		}

		startAutoWalk(listWalkDir);
	}
	
	setFollowCreature(creature);
	return true;
}

double Creature::getDamageRatio(Creature* attacker) const
{
	int32_t totalDamage = 0;
	int32_t attackerDamage = 0;

	for(DamageMap::const_iterator it = damageMap.begin(); it != damageMap.end(); ++it){
		totalDamage += (*it).second;

		if((*it).first == attacker->getID()){
			attackerDamage += (*it).second;
		}
	}

	return ((double)attackerDamage / totalDamage);
}

int32_t Creature::getGainedExperience(Creature* attacker) const
{
	int32_t lostExperience = getLostExperience();
	return (int32_t)std::floor(getDamageRatio(attacker) * lostExperience);
}

bool Creature::addDamagePoints(Creature* attacker, int32_t damagePoints)
{
	if(damagePoints > 0){
		uint32_t attackerId = (attacker ? attacker->getID() : 0);

		damageMap[attackerId] += damagePoints;
		lastHitCreature = attackerId;
	}

	return true;
}

void Creature::onAddCondition(ConditionType_t type)
{
	if(type == CONDITION_PARALYZE && hasCondition(CONDITION_HASTE)){
		removeCondition(CONDITION_HASTE);
	}
}

void Creature::onEndCondition(ConditionType_t type)
{
	//
}

void Creature::onTickCondition(ConditionType_t type, bool& bRemove)
{
	if(const MagicField* field = getTile()->getFieldItem()){
		switch(type){
			case CONDITION_FIRE: bRemove = (field->getDamageType() != DAMAGE_FIRE); break;
			case CONDITION_ENERGY: bRemove = (field->getDamageType() != DAMAGE_ENERGY); break;
			case CONDITION_POISON: bRemove = (field->getDamageType() != DAMAGE_POISON); break;
			default: 
				break;
		}
	}
}

void Creature::onAttackedCreature(Creature* target)
{
	//
}

void Creature::onAttacked()
{
	//
}

void Creature::onAttackedCreatureDrainHealth(Creature* target, int32_t points)
{
	target->addDamagePoints(this, points);
}

void Creature::onAttackedCreatureKilled(Creature* target)
{
	if(target != this){
		int32_t gainedExperience = target->getGainedExperience(this);
		onGainExperience(gainedExperience);
	}
}

void Creature::onKilledCreature(Creature* target)
{
	//
}

void Creature::onGainExperience(int32_t gainExperience)
{
	if(gainExperience > 0){
		std::stringstream strExp;
		strExp << gainExperience;

		g_game.addAnimatedText(getPosition(), TEXTCOLOR_WHITE_EXP, strExp.str());
	}
}

void Creature::onAttackedCreatureBlockHit(Creature* target, BlockType_t blockType)
{
	//
}

void Creature::setMaster(Creature* creature)
{
	//std::cout << "setMaster: " << this << " master=" << creature << std::endl;
	master = creature;
}

void Creature::addSummon(Creature* creature)
{
	//std::cout << "addSummon: " << this << " summon=" << creature << std::endl;
	creature->setMaster(this);
	creature->useThing2();
	summons.push_back(creature);
	
}

void Creature::removeSummon(Creature* creature)
{
	//std::cout << "removeSummon: " << this << " summon=" << creature << std::endl;
	std::list<Creature*>::iterator cit = std::find(summons.begin(), summons.end(), creature);
	if(cit != summons.end()) {
		(*cit)->setMaster(NULL);
		(*cit)->releaseThing2();
		summons.erase(cit);
	}
}

bool Creature::addCondition(Condition* condition)
{
	if(condition == NULL){
		return false;
	}
	
	Condition* prevCond = getCondition(condition->getType());
	
	if(prevCond){
		prevCond->addCondition(this, condition);
		delete condition;
	}
	else{
		if(condition->startCondition(this)){
			conditions.push_back(condition);
		}

		onAddCondition(condition->getType());
	}

	return true;
}

void Creature::removeCondition(ConditionType_t type)
{
	for(ConditionList::iterator it = conditions.begin(); it != conditions.end(); ++it){
		if((*it)->getType() == type){
			(*it)->endCondition(this, REASON_ABORT);
			delete *it;
			conditions.erase(it);

			onEndCondition(type);
			break;
		}
	}
}

void Creature::executeConditions(int32_t newticks)
{
	for(ConditionList::iterator it = conditions.begin(); it != conditions.end();){
		//(*it)->setTicks((*it)->getTicks() - newticks);

		(*it)->executeCondition(this, newticks);

		if((*it)->getTicks() <= 0){
			ConditionType_t type = (*it)->getType();

			(*it)->endCondition(this, REASON_ENDTICKS);
			delete *it;
			it = conditions.erase(it);

			onEndCondition(type);
		}
		else{
			++it;
		}
	}
}

Condition* Creature::getCondition(ConditionType_t type)
{
	if(conditions.empty())
		return NULL;
	
	for(ConditionList::iterator it = conditions.begin(); it != conditions.end(); ++it){
		if((*it)->getType() == type)
			return *it;
	}

	return NULL;
}

bool Creature::hasCondition(ConditionType_t type) const
{
	for(ConditionList::const_iterator it = conditions.begin(); it != conditions.end(); ++it){
		if((*it)->getType() == type)
			return true;
	}

	return false;
}

bool Creature::isImmune(DamageType_t type) const
{
	return ((getImmunities() & (uint32_t)type) == (uint32_t)type);
}

std::string Creature::getDescription(int32_t lookDistance) const
{
	std::string str = "a creature";
	return str;
}

int Creature::getStepDuration() const
{
	OTSYS_THREAD_LOCK_CLASS lockClass(g_game.gameLock, "Creature::getStepDuration()");

	int duration = 500;

	if(!isRemoved()){
		const Position& tilePos = getPosition();
		Tile* tile = g_game.getTile(tilePos.x, tilePos.y, tilePos.z);
		if(tile && tile->ground){
			int groundid = tile->ground->getID();
			uint16_t stepspeed = Item::items[groundid].speed;
			if(stepspeed != 0) {
				duration =  (1000 * stepspeed) / (getSpeed() != 0 ? getSpeed() : 220);
			}
		}
	}

	return duration;
};

int64_t Creature::getSleepTicks() const
{
	int64_t delay = 0;
	int stepDuration = getStepDuration();
	
	if(lastMove != 0) {
		delay = (((int64_t)(lastMove)) + ((int64_t)(stepDuration))) - ((int64_t)(OTSYS_TIME()));
	}
	
	return delay;
}

int64_t Creature::getEventStepTicks() const
{
	int64_t ret = getSleepTicks();

	if(ret <=0){
		ret = getStepDuration();
	}

	return ret;
}

bool Creature::startAutoWalk(std::list<Direction>& listDir)
{
	if(eventWalk == 0){
		//start a new event
		listWalkDir = listDir;
		return addEventWalk();
	}
	else{
		//event already running
		listWalkDir = listDir;
	}

	return true;
}

bool Creature::addEventWalk()
{
	if(isRemoved()){
		eventWalk = 0;
		return false;
	}

	int64_t ticks = getEventStepTicks();
	eventWalk = g_game.addEvent(makeTask(ticks, std::bind2nd(std::mem_fun(&Game::checkWalk), getID())));
	return true;
}

bool Creature::stopAutoWalk()
{
	if(eventWalk != 0){
		g_game.stopEvent(eventWalk);
		eventWalk = 0;

		if(!listWalkDir.empty()){
			listWalkDir.clear();
			onWalkAborted();
		}
	}

	return true;
}

void Creature::getCreatureLight(LightInfo& light) const
{
	light = internalLight;
}

void Creature::setNormalCreatureLight()
{
	internalLight.level = 0;
	internalLight.color = 0;
}

void Creature::setCreatureLight(LightInfo& light)
{
	internalLight = light;
}
