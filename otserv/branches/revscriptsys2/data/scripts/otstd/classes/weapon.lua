otstd.weapons = {}

Weapon = {}
Weapon_mt = {__index = Weapon}

function Weapon:new(weaponID)
	local weapon = {
		-- Common for all weapons
		id = weaponID,
		vocation = "any",
		level = 0,
		magicLevel = 0,
		mana = 0,
		manapercent = 0,
		soul = 0,
		exhaustion = false,
		premium = false,
		unproperly = false,
		--
		range = 1,
		damageType = COMBAT_PHYSICALDAMAGE,
		blockedByDefense = true,
		blockedByArmor = true,

		-- damage formula
		damageFormula = nil,

		-- Distance weapons
		hitChange = 0,
		maxHitChange = 0,
		breakChange = 0,
		ammuAttackValue = 0,

		-- Wands
		minChange = 0,
		maxChange = 0,

		-- Callbacks
		onUseWeapon = nil,
		onUseFist = nil
	}
	setmetatable(weapon, Weapon_mt)
	return weapon
end

-- Used to handle default weapons
otstd.defaultWeapon = Weapon:new(0)

-- Formula to get weapon max damage based on player's level, skill and attack factor
function otstd.getWeaponMaxDamage(level, attackSkill, attackValue, attackFactor)
	return math.ceil(2 * (attackValue * (attackSkill + 5.8) / 25 + level / 10 - 0.1) / attackFactor)
end

-- Default damage formula callback
function otstd.damageFormula(player, target, weapon)
	local attackValue = weapon:getAttack()
	if weapon:getWeaponType() == WEAPON_AMMO then
		local bow = player:getWeapon(true)
		if bow then
			attackValue = attackValue + bow:getAttack()
		end
	end

	local maxDamage = otstd.getWeaponMaxDamage(player:getLevel(), player:getWeaponSkill(weapon), attackValue, player:getAttackFactor())
	local minDamage = 0

	if weapon:getWeaponType() == WEAPON_DIST or
		weapon:getWeaponType() == WEAPON_AMMO then
		if typeof(target, "Player") then
			minDamage = math.ceil(player:getLevel() * 0.1)
		else
			minDamage = math.ceil(player:getLevel() * 0.2)
		end
	end

	return -math.random(minDamage, maxDamage)
end

function otstd.onUseWeapon(event)
	local weapon = event.weapon

	if not typeof(event.player, "Player") then
		error("onUseWeapon event triggered by an actor?")
	end

	if not weapon then
		otstd.internalUseFist(event)
	else
		local internalWeapon = otstd.weapons[weapon:getItemID()]
		if not internalWeapon then
			internalWeapon = otstd.defaultWeapon
		end

		event.internalWeapon = internalWeapon
		event.target = event.attacked
		event.damageModifier = 100

		if otstd.onWeaponCheck(event) then
			if internalWeapon.onUseWeapon then
				internalWeapon:onUseWeapon(event)
			else
				otstd.internalUseWeapon(event)
			end
		end
	end

	return true
end

function otstd.onUsedWeapon(event)
	local player = event.player
	local weapon = event.weapon
	local internalWeapon = event.internalWeapon

	-- Add skill points
	if not player:notGainSkill() and
		player:getAddAttackSkill() then
		local skillType = nil
		local weaponType = weapon:getWeaponType()
		if weaponType == WEAPON_SWORD then
			skillType = SKILL_SWORD
		elseif weaponType == WEAPON_CLUB then
			skillType = SKILL_CLUB
		elseif weaponType == WEAPON_AXE then
			skillType = skill_AXE
		elseif weaponType == WEAPON_DIST or
			weaponType == WEAPON_AMMO then
			skillType = SKILL_DIST
		end

		local blockType = player:getLastAttackBlockType()
		local skillPoints = 0
		if skillType == SKILL_DIST and
			blockType == BLOCK_NONE then
			skillPoints = 2
		elseif blockType == BLOCK_DEFENSE or
				blockType == BLOCK_ARMOR or
				blockType == BLOCK_NONE then
			skillPoints = 1
		end

		player:advanceSkill(skillType, skillPoints)
	end

	if not player:hasInfiniteMana() then
		player:spendMana(internalWeapon.mana)
	end

	if not player:hasInfiniteSoul() then
		player:removeSoul(internalWeapon.soul)
	end

	if not player:canGetExhausted() and
		internalWeapon.exhausted then
		player:addCombatExhaustion(config["fight_exhausted"])
	end

	if not player:cannotGainInFight() then
		player:addInFight(config["in_fight_duration"])
	end

	return true
end

function otstd.onWeaponCheck(event)
	local player = event.player
	local target = event.target
	local internalWeapon = event.internalWeapon

	if not areInRange(player:getPosition(), target:getPosition(), internalWeapon.range) then
		event.damageModifier = 0
		return false
	end

	if not player:ignoreWeaponCheck() then
		if (internalWeapon.premium and not player:isPremium()) or
			(internalWeapon.mana > player:getMana()) or
			(internalWeapon.soul > player:getSoulPoints()) or
			(not checkVocation(player:getVocationName(), internalWeapon.vocation)) then
			event.damageModifier = 0
			return false
		end

		-- Wielded unproperly

		-- level
		if internalWeapon.level > player:getLevel() then
			if internalWeapon.unproperly then
				local penalty = (internalWeapon.level - player:getLevel()) * 0.02
				if penalty > 0.5 then
					penalty = 0.5
				end

				event.damageModifier = event.damageModifier - (event.damageModifier * penalty)
			else
				event.damageModifier = 0
				return false
			end
		end

		-- magic level
		if internalWeapon.magicLevel > player:getMagicLevel() then
			event.damageModifier = internalWeapon.unproperly and event.damageModifier/2 or 0
		end
	end

	return true
end

function otstd.internalUseWeapon(event)
	event:skip()

	local player = event.player
	local target = event.target
	local weapon = event.weapon
	local internalWeapon = event.internalWeapon

	-- at this point, weapon shouldn't and won't be nil
	-- get max min damages
	local damage = 0
	if internalWeapon.damageFormula then
		damage = internalWeapon:damageFormula(player, target, weapon)
	else
		damage = otstd.damageFormula(player, target, weapon)
	end

	damage = (damage * event.damageModifier) / 100

	-- do the damage
	internalCastSpell(internalWeapon.damageType, player, target, damage,
		internalWeapon.blockedByShield, internalWeapon.blockedByArmor)

	-- call finish handler
	if internalWeapon.onUsedWeapon then
		internalWeapon:onUsedWeapon(event)
	else
		otstd.onUsedWeapon(event)
	end
end

function otstd.internalUseFist(event)
	event:skip()
	print("use fist triggered")
end

function Weapon:register()
	self:unregister()

	if otstd.weapons[self.id] ~= nil then
		error("Duplicate weapon id \"" .. self.id .. "\"")
	end

	self.onUseWeaponHandler = registerOnUseWeapon(self.id, "weaponid", otstd.onUseWeapon)

	otstd.weapons[self.id] = self
end

function Weapon:unregister()
	if self.onUseWeaponHandler ~= nil then
		stopListener(self.onUseWeaponHandler)
		self.onUseWeaponHandler = nil
	end

	otstd.weapons[self.id] = nil
end
